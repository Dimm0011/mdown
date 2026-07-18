#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <chrono>

namespace multidow {

extern std::atomic<bool> g_cancelled;

struct ThreadState {
    uint64_t bytes_downloaded = 0;
    uint64_t total_bytes = 0;
    bool finished = false;
    bool error = false;
    bool active = false;
};

struct FileState {
    int id;
    std::string filename;
    uint64_t file_size = 0;
    std::vector<ThreadState> threads;
    bool done = false;
    bool success = false;
    std::string status_text;
    std::chrono::steady_clock::time_point start_time;
};

class ProgressManager {
public:
    ProgressManager();
    ~ProgressManager() = default;

    int add_file(const std::string& filename, uint64_t file_size, int num_threads);
    void update_thread(int file_id, int thread_id, uint64_t downloaded, uint64_t total);
    void mark_thread_active(int file_id, int thread_id);
    void mark_thread_finished(int file_id, int thread_id);
    void mark_thread_error(int file_id, int thread_id);
    void set_file_done(int file_id, bool success, const std::string& msg = "");
    void redraw();

    bool all_done() const;

private:
    std::vector<FileState> files_;
    mutable std::mutex mtx_;
    int prev_lines_ = 0;
    std::chrono::steady_clock::time_point last_redraw_;
    bool terminal_supported_ = true;

    void do_redraw();
};

}
