#pragma once

#include "progress.h"
#include <curl/curl.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <atomic>

namespace multidow {

struct DownloadConfig {
    std::string url;
    std::string output_path;
    int num_threads = 4;
    bool verify = false;
    std::string expected_checksum;
    int max_retries = 3;
    int timeout = 300;
};

class Downloader {
public:
    Downloader(const DownloadConfig& config, ProgressManager& pm);
    ~Downloader() = default;

    bool run();

private:
    DownloadConfig config_;
    ProgressManager& pm_;
    int file_id_ = -1;
    uint64_t file_size_ = 0;
    bool range_supported_ = false;

    void configure_curl(CURL* curl);
    uint64_t probe_server();
    bool start_download();
    bool download_chunk(int thread_id, uint64_t start, uint64_t chunk_total, FILE* fp, std::mutex& fp_mtx);
    bool download_single();
    std::string extract_filename(const std::string& url) const;
    std::string meta_path() const;
    void save_metadata();
};

}
