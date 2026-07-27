#include "progress.h"
#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif
#include <cmath>
#include <format>
#include <iostream>
#include "format.h"

namespace multidow {

std::stop_source g_stop;
volatile sig_atomic_t g_signal_received = 0;

static const int MIN_REDRAW_MS = 100;
static const int SPEED_WINDOW_SEC = 2;
static const int SAMPLE_INTERVAL_MS = 200;

ProgressManager::ProgressManager() : last_redraw_(std::chrono::steady_clock::now()) {
    terminal_supported_ = isatty(fileno(stderr));
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
    fs.last_active = fs.start_time;
    fs.last_progress_time = fs.start_time;
    files_.push_back(std::move(fs));
    return id;
}

void ProgressManager::update_file_size(int file_id, uint64_t file_size) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (file_id < 0 || file_id >= (int)files_.size()) return;
    files_[file_id].file_size = file_size;
    dirty_ = true;
}

void ProgressManager::update_thread(int file_id, int thread_id, uint64_t downloaded,
                                    uint64_t total) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (file_id < 0 || file_id >= (int)files_.size()) return;
    auto& f = files_[file_id];
    auto& t = f.threads;
    if (thread_id >= 0 && thread_id < (int)t.size()) {
        uint64_t prev = t[thread_id].bytes_downloaded;
        if (downloaded > prev) {
            f.total_bytes_received += (downloaded - prev);
            t[thread_id].bytes_downloaded = downloaded;
        }
        t[thread_id].total_bytes = total;
    }

    uint64_t total_down = 0;
    for (auto& th : f.threads) total_down += th.bytes_downloaded;

    auto now = std::chrono::steady_clock::now();

    if (total_down > f.last_bytes) {
        f.last_active = now;
        f.last_bytes = total_down;
        f.last_progress_time = now;
    }

    auto ms_since_sample =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - f.last_sample_time).count();
    if (ms_since_sample >= SAMPLE_INTERVAL_MS) {
        f.speed_history.push_back({now, f.total_bytes_received});
        while (f.speed_history.size() > 20) f.speed_history.pop_front();
        f.last_sample_time = now;
    }

    dirty_ = true;
}

void ProgressManager::mark_thread_active(int file_id, int thread_id) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (file_id < 0 || file_id >= (int)files_.size()) return;
        auto& t = files_[file_id].threads;
        if (thread_id >= 0 && thread_id < (int)t.size()) t[thread_id].active = true;
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
    dirty_ = true;
}

void ProgressManager::set_file_status(int file_id, const std::string& msg) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (file_id < 0 || file_id >= (int)files_.size()) return;
    files_[file_id].status_text = msg;
    dirty_ = true;
}

void ProgressManager::rename_file(int file_id, const std::string& new_name) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (file_id < 0 || file_id >= (int)files_.size()) return;
    files_[file_id].filename = new_name;
    dirty_ = true;
}

void ProgressManager::reset_file_threads(int file_id, int num_threads) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (file_id < 0 || file_id >= (int)files_.size()) return;
    auto& f = files_[file_id];
    f.threads.clear();
    f.threads.resize(num_threads);
    f.last_bytes = 0;
    f.total_bytes_received = 0;
    f.speed_history.clear();
    dirty_ = true;
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
            if (dt > 0) return (double)(latest_bytes - it->bytes) / dt;
            return 0;
        }
    }

    double elapsed = std::chrono::duration<double>(now - history.front().time).count();
    if (elapsed > 0.1) return (double)(latest_bytes - history.front().bytes) / elapsed;
    return 0;
}

void ProgressManager::do_redraw() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!terminal_supported_) return;

    auto now = std::chrono::steady_clock::now();
    auto ms_since_last =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_redraw_).count();

    bool is_terminal_event = false;
    for (auto& f : files_) {
        if (f.done) is_terminal_event = true;
    }
    if (!is_terminal_event && (!dirty_ || ms_since_last < MIN_REDRAW_MS)) return;
    dirty_ = false;
    last_redraw_ = now;

    std::string out;

    if (prev_lines_ > 0) out = std::format("\r\033[{}A", prev_lines_);

    int lines = 0;

    for (auto& f : files_) {
        if (f.done) {
            out += std::format("\r\033[2K{} {}\n", f.filename, f.status_text);
            lines++;
            continue;
        }

        uint64_t file_down = 0;
        bool any_active = false;
        bool any_error = false;
        for (auto& t : f.threads) {
            file_down += t.bytes_downloaded;
            if (t.active) any_active = true;
            if (t.error) any_error = true;
        }

        out += "\r\033[2K";
        out += f.filename;

        if (f.file_size > 0) {
            out += std::format(" ({}) ", format_bytes(f.file_size));

            double pct = (double)file_down / f.file_size * 100.0;
            out += make_bar(30, pct / 100.0);
            out += std::format(" {:.1f}%", pct);

            double speed = compute_speed(f.speed_history);
            if (speed > 0) {
                out += std::format("  {}", format_speed((uint64_t)speed));
                uint64_t remaining = f.file_size - file_down;
                uint64_t eta = (uint64_t)(remaining / speed);
                out += std::format("  ETA {}:{:02}", eta / 60, eta % 60);
            }
            if (!f.status_text.empty()) {
                out += std::format("  {}", f.status_text);
            }
        } else if (any_active) {
            out += std::format("  downloading... {}", format_bytes(file_down));
        } else if (any_error) {
            out += "  error, retrying...";
        } else if (!f.status_text.empty()) {
            out += std::format("  {}", f.status_text);
        } else {
            out += "  connecting...";
        }

        out += "\n";
        lines++;
    }

    if (lines < prev_lines_) {
        for (int i = 0; i < prev_lines_ - lines; i++) out += "\r\033[2K\n";
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

bool ProgressManager::any_stalled(std::chrono::seconds timeout) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto now = std::chrono::steady_clock::now();
    bool has_active = false;
    for (auto& f : files_) {
        if (f.done) continue;
        has_active = true;
        if (now - f.last_progress_time <= timeout) return false;
    }
    return has_active;
}

}
