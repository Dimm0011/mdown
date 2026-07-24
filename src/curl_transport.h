#pragma once

#include <curl/curl.h>
#include <string>
#include "transport.h"

namespace multidow {

struct CurlTransportConfig {
    std::string user_agent = "Mozilla/5.0 (X11; Linux x86_64) mdown/1.0";
    int timeout_sec = 300;
    int connect_timeout_sec = 15;
    long low_speed_limit = 1024;
    long low_speed_time_sec = 30;
    int max_redirs = 10;
};

class CurlTransport : public ITransport {
   public:
    explicit CurlTransport(const CurlTransportConfig& config = {});

    ProbeResult head(const std::string& url) override;

    bool download(const std::string& url, std::optional<std::pair<uint64_t, uint64_t>> range,
                  void* write_userp, size_t (*write_cb)(char*, size_t, size_t, void*),
                  void* progress_userp,
                  int (*progress_cb)(void*, long long, long long, long long, long long)) override;

   private:
    CurlTransportConfig config_;
    void configure(CURL* curl);
};

}  // namespace multidow
