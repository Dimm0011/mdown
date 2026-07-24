#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace multidow {

struct ProbeResult {
    bool ok = false;
    long status = 0;
    uint64_t file_size = 0;
    bool range_supported = false;
    std::string filename;
};

class ITransport {
   public:
    virtual ~ITransport() = default;

    virtual ProbeResult head(const std::string& url) = 0;

    virtual bool download(const std::string& url,
                          std::optional<std::pair<uint64_t, uint64_t>> range, void* write_userp,
                          size_t (*write_cb)(char*, size_t, size_t, void*), void* progress_userp,
                          int (*progress_cb)(void*, long long, long long, long long,
                                             long long)) = 0;
};

}  // namespace multidow
