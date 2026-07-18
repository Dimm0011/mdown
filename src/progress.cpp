#include "progress.h"
#include "format.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <unistd.h>

namespace multidow {

std::atomic<bool> g_cancelled{false};

static const int MIN_REDRAW_MS = 150;

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
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (file_id < 0 || file_id >= (int)files_.size()) return;
        auto& t = files_[file_id].threads;
        if (thread_id >= 0 && thread_id < (int)t.size()) {
            t[thread_id].bytes_downloaded = downloaded;
            t[thread_id].total_bytes = total;
        }
    }
    do_redraw();
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

void ProgressManager::do_redraw() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!terminal_supported_) return;

    auto now = std::chrono::steady_clock::now();
    auto ms_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_redraw_).count();
    bool is_terminal_event = false;
    for (auto& f : files_) {
        if (f.done) is_terminal_event = true;
    }
    if (!is_terminal_event && ms_since_last < MIN_REDRAW_MS) return;
    last_redraw_ = now;

    std::ostringstream out;

    if (prev_lines_ > 0)
        out << "\033[" << prev_lines_ << "A";

    int lines = 0;

    for (auto& f : files_) {
        if (f.done) {
            out << "\033[2K" << f.filename << " " << f.status_text << "\n";
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

        out << "\033[2K";
        out << f.filename << " ";
        if (f.file_size > 0)
            out << "(" << format_bytes(f.file_size) << ") ";

        if (file_total > 0) {
            double pct = (double)file_down / file_total * 100.0;
            out << make_bar(30, pct / 100.0) << " ";
            out << std::fixed << std::setprecision(1) << pct << "%";

            auto file_elapsed = std::chrono::duration<double>(now - f.start_time).count();
            if (file_elapsed > 0.5) {
                double speed = (double)file_down / file_elapsed;
                out << "  " << format_speed((uint64_t)speed);
                uint64_t remaining = file_total - file_down;
                uint64_t eta = (speed > 0) ? (uint64_t)(remaining / speed) : 0;
                out << "  ETA " << (eta / 60) << ":" << std::setw(2) << std::setfill('0') << (eta % 60);
            }
        } else if (any_active) {
            out << "downloading...";
        } else if (any_error) {
            out << "error, retrying...";
        } else {
            out << "connecting...";
        }

        out << "\n";
        lines++;
    }

    if (lines < prev_lines_) {
        for (int i = 0; i < prev_lines_ - lines; i++)
            out << "\033[2K\n";
    }

    std::cerr << out.str() << std::flush;
    prev_lines_ = (lines > prev_lines_) ? lines : prev_lines_;
}

bool ProgressManager::all_done() const {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& f : files_)
        if (!f.done) return false;
    return true;
}

}
