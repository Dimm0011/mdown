#pragma once

#include <chrono>
#include <csignal>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace multidow {

extern std::stop_source g_stop;
extern volatile sig_atomic_t g_signal_received;

struct SpeedSample {
    std::chrono::steady_clock::time_point time;
    uint64_t bytes;
};

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
    std::chrono::steady_clock::time_point last_active;
    uint64_t last_bytes = 0;
    uint64_t total_bytes_received = 0;
    std::chrono::steady_clock::time_point last_progress_time;
    std::chrono::steady_clock::time_point last_sample_time;
    std::deque<SpeedSample> speed_history;
};

class ProgressManager {
   public:
    ProgressManager();
    ~ProgressManager() = default;

    int add_file(const std::string& filename, uint64_t file_size, int num_threads);
    void update_file_size(int file_id, uint64_t file_size);
    void update_thread(int file_id, int thread_id, uint64_t downloaded, uint64_t total);
    void mark_thread_active(int file_id, int thread_id);
    void mark_thread_finished(int file_id, int thread_id);
    void mark_thread_error(int file_id, int thread_id);
    void set_file_done(int file_id, bool success, const std::string& msg = "");
    void set_file_status(int file_id, const std::string& msg);
    void rename_file(int file_id, const std::string& new_name);
    void reset_file_threads(int file_id, int num_threads);
    void redraw();
    void poll();

    bool all_done() const;
    bool any_stalled(std::chrono::seconds timeout) const;

   private:
    std::vector<FileState> files_;
    mutable std::mutex mtx_;
    int prev_lines_ = 0;
    std::chrono::steady_clock::time_point last_redraw_;
    bool terminal_supported_ = true;
    bool dirty_ = false;

    void do_redraw();
};

}  // namespace multidow
