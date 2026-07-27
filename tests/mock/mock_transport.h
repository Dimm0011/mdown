#pragma once

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>
#include "transport.h"

namespace multidow::mock {

class MockTransport : public ITransport {
   public:
    std::vector<ProbeResult> probe_responses;
    std::vector<bool> download_results;
    std::vector<uint8_t> download_data;

    mutable std::vector<std::string> probe_urls;
    mutable std::vector<std::string> download_urls;
    mutable std::mutex mtx;
    mutable std::atomic<int> probe_count{0};
    mutable std::atomic<int> download_count{0};

    ProbeResult head(const std::string& url) override {
        std::lock_guard lock(mtx);
        probe_urls.push_back(url);
        int idx = probe_count.fetch_add(1);
        if (idx < (int)probe_responses.size()) return probe_responses[idx];
        return {};
    }

    DownloadResult download(const std::string& url,
                            std::optional<std::pair<uint64_t, uint64_t>> range,
                            void* write_userp, size_t (*write_cb)(char*, size_t, size_t, void*),
                            void* progress_userp,
                            int (*progress_cb)(void*, long long, long long, long long,
                                               long long)) override {
        {
            std::lock_guard lock(mtx);
            download_urls.push_back(url);
        }
        int idx = download_count.fetch_add(1);

        if (!download_data.empty() && write_cb) {
            size_t total = download_data.size();
            size_t written =
                write_cb(reinterpret_cast<char*>(download_data.data()), 1, total, write_userp);
            (void)written;
        }

        if (progress_cb) {
            progress_cb(progress_userp, (long long)download_data.size(),
                        (long long)download_data.size(), 0, 0);
        }

        DownloadResult result;
        result.ok = (idx < (int)download_results.size()) ? download_results[idx] : true;
        result.http_code = range ? 206 : 200;
        return result;
    }
};

}
