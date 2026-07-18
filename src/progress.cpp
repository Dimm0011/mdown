#include "progress.h"
#include "format.h"
#include <iostream>
#include <format>
#include <cmath>
#include <unistd.h>

namespace multidow {

std::stop_source g_stop;
volatile sig_atomic_t g_signal_received = 0;

static const int MIN_REDRAW_MS = 100;
static const int SPEED_WINDOW_SEC = 2;

ProgressManager::ProgressManager()
    : last_redraw_(std::chrono::steady_clock::now()) {
    terminal_supported_ = isatty(STDERR_FILENO);
}

int ProgressManager::add_file(const std::string& filename, uint64_t file_size, int num_threads) {
    std::lock_guard<std::mutex> lock(mtx_);
    int id = (int)files_.size();
    FileState fs;
    fs.id = id;
    fs.filename = filename;
    fs.file_size = file_size;
    fs.threads.resize(num_threads);
    fs.start_time = std::chrono::steady_clock::now();
    files_.push_back(std::move(fs));
    return id;
}

void ProgressManager::update_thread(int file_id, int thread_id, uint64_t downloaded, uint64_t total) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (file_id < 0 || file_id >= (int)files_.size()) return;
    auto& f = files_[file_id];
    auto& t = f.threads;
    if (thread_id >= 0 && thread_id < (int)t.size()) {
        t[thread_id].bytes_downloaded = downloaded;
        t[thread_id].total_bytes = total;
    }

    uint64_t total_down = 0;
    for (auto& th : f.threads)
        total_down += th.bytes_downloaded;

    auto now = std::chrono::steady_clock::now();
    f.speed_history.push_back({now, total_down});

    while (f.speed_history.size() > 20)
        f.speed_history.pop_front();

    dirty_ = true;
}

void ProgressManager::mark_thread_active(int file_id, int thread_id) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (file_id < 0 || file_id >= (int)files_.size()) return;
        auto& t = files_[file_id].threads;
        if (thread_id >= 0 && thread_id < (int)t.size())
            t[thread_id].active = true;
    }
    do_redraw();
}

void ProgressManager::mark_thread_finished(int file_id, int thread_id) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (file_id < 0 || file_id >= (int)files_.size()) return;
        auto& t = files_[file_id].threads;
        if (thread_id >= 0 && thread_id < (int)t.size()) {
            t[thread_id].finished = true;
            t[thread_id].active = false;
            if (t[thread_id].total_bytes > 0)
                t[thread_id].bytes_downloaded = t[thread_id].total_bytes;
        }
    }
    do_redraw();
}

void ProgressManager::mark_thread_error(int file_id, int thread_id) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (file_id < 0 || file_id >= (int)files_.size()) return;
        auto& t = files_[file_id].threads;
        if (thread_id >= 0 && thread_id < (int)t.size()) {
            t[thread_id].error = true;
            t[thread_id].active = false;
        }
    }
    do_redraw();
}

void ProgressManager::set_file_done(int file_id, bool success, const std::string& msg) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (file_id < 0 || file_id >= (int)files_.size()) return;
    files_[file_id].done = true;
    files_[file_id].success = success;
    files_[file_id].status_text = msg;
}

void ProgressManager::redraw() {
    do_redraw();
}

void ProgressManager::poll() {
    do_redraw();
}

static double compute_speed(const std::deque<SpeedSample>& history) {
    if (history.size() < 2) return 0;

    auto now = history.back().time;
    uint64_t latest_bytes = history.back().bytes;

    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        double dt = std::chrono::duration<double>(now - it->time).count();
        if (dt >= SPEED_WINDOW_SEC) {
            double elapsed = std::chrono::duration<double>(now - it->time).count();
            if (elapsed > 0)
                return (double)(latest_bytes - it->bytes) / elapsed;
            return 0;
        }
    }

    double elapsed = std::chrono::duration<double>(now - history.front().time).count();
    if (elapsed > 0.1)
        return (double)(latest_bytes - history.front().bytes) / elapsed;
    return 0;
}

void ProgressManager::do_redraw() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!terminal_supported_) return;

    auto now = std::chrono::steady_clock::now();
    auto ms_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_redraw_).count();

    bool is_terminal_event = false;
    for (auto& f : files_) {
        if (f.done) is_terminal_event = true;
    }
    if (!is_terminal_event && (!dirty_ || ms_since_last < MIN_REDRAW_MS)) return;
    dirty_ = false;
    last_redraw_ = now;

    std::string out;

    if (prev_lines_ > 0)
        out = std::format("\r\033[{}A", prev_lines_);

    int lines = 0;

    for (auto& f : files_) {
        if (f.done) {
            out += std::format("\r\033[2K{} {}\n", f.filename, f.status_text);
            lines++;
            continue;
        }

        uint64_t file_down = 0, file_total = 0;
        bool any_active = false;
        bool any_error = false;
        for (auto& t : f.threads) {
            file_down += t.bytes_downloaded;
            file_total += t.total_bytes;
            if (t.active) any_active = true;
            if (t.error) any_error = true;
        }

        out += "\r\033[2K";
        out += f.filename;

        if (file_total > 0) {
            out += std::format(" ({}) ", format_bytes(f.file_size > 0 ? f.file_size : file_total));

            double pct = (double)file_down / file_total * 100.0;
            out += make_bar(30, pct / 100.0);
            out += std::format(" {:.1f}%", pct);

            double speed = compute_speed(f.speed_history);
            if (speed > 0) {
                out += std::format("  {}", format_speed((uint64_t)speed));
                uint64_t remaining = file_total - file_down;
                uint64_t eta = (uint64_t)(remaining / speed);
                out += std::format("  ETA {}:{:02}", eta / 60, eta % 60);
            }
        } else if (any_active) {
            out += std::format("  downloading... {}", format_bytes(file_down));
        } else if (any_error) {
            out += "  error, retrying...";
        } else {
            out += "  connecting...";
        }

        out += "\n";
        lines++;
    }

    if (lines < prev_lines_) {
        for (int i = 0; i < prev_lines_ - lines; i++)
            out += "\r\033[2K\n";
    }

    std::cerr << out << std::flush;
    prev_lines_ = (lines > prev_lines_) ? lines : prev_lines_;
}

bool ProgressManager::all_done() const {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& f : files_)
        if (!f.done) return false;
    return true;
}

}
