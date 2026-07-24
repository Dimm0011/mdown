#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "progress.h"
#include "transport.h"

namespace multidow {

class ThreadPool;

struct DownloadConfig {
    std::string url;
    std::string output_path;
    int num_threads = 4;
    bool verify = false;
    std::string expected_checksum;
    int max_retries = 3;
    int timeout = 300;
    bool output_explicit = false;
};

class Downloader {
   public:
    Downloader(const DownloadConfig& config, ProgressManager& pm, ThreadPool& pool,
               ITransport& transport);
    ~Downloader() = default;

    bool run();

   private:
    DownloadConfig config_;
    ProgressManager& pm_;
    ThreadPool& pool_;
    ITransport& transport_;
    int file_id_ = -1;
    uint64_t file_size_ = 0;
    bool range_supported_ = false;

    uint64_t probe_server();
    bool start_download();
    bool download_single();
    std::string extract_filename(const std::string& url) const;
    std::string meta_path() const;
    void save_metadata();
};

}  // namespace multidow
