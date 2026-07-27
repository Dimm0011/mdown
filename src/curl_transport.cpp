#include "curl_transport.h"

#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace multidow {

static std::mutex g_curlsh_mutex;

static void curlsh_lock_cb(CURL*, curl_lock_data, curl_lock_access, void*) {
    g_curlsh_mutex.lock();
}

static void curlsh_unlock_cb(CURL*, curl_lock_data, void*) {
    g_curlsh_mutex.unlock();
}

struct CurlHeader {
    uint64_t file_size = 0;
    bool range_supported = false;
    std::string filename;
};

static bool header_contains_ci(const std::string& h, const std::string& name) {
    if (name.size() > h.size()) return false;
    for (size_t i = 0; i <= h.size() - name.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < name.size(); ++j) {
            if (std::tolower((unsigned char)h[i + j]) != std::tolower((unsigned char)name[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

static size_t header_cb(char* buf, size_t, size_t nitems, void* userp) {
    auto* hdr = static_cast<CurlHeader*>(userp);
    std::string h(buf, nitems);
    if (header_contains_ci(h, "content-length:")) {
        auto pos = h.find(':');
        if (pos != std::string::npos) {
            std::string v = h.substr(pos + 1);
            v.erase(0, v.find_first_not_of(" "));
            v.erase(v.find_last_not_of(" \r\n") + 1);
            try {
                hdr->file_size = std::stoull(v);
            } catch (...) {
            }
        }
    }
    {
        auto pos = h.find(':');
        if (pos != std::string::npos && header_contains_ci(h.substr(0, pos), "accept-ranges")) {
            std::string v = h.substr(pos + 1);
            v.erase(0, v.find_first_not_of(" "));
            v.erase(v.find_last_not_of(" \r\n") + 1);
            if (header_contains_ci(v, "bytes")) hdr->range_supported = true;
        }
    }
    if (header_contains_ci(h, "content-disposition:")) {
        auto pos = h.find("filename=");
        if (pos != std::string::npos) {
            std::string fn = h.substr(pos + 9);
            fn.erase(0, fn.find_first_not_of(" \""));
            fn.erase(fn.find_last_not_of(" \"\r\n") + 1);
            if (!fn.empty()) hdr->filename = fn;
        }
    }
    return nitems;
}

struct DownloadBridge {
    void* write_userp;
    size_t (*write_cb)(char*, size_t, size_t, void*);
    void* progress_userp;
    int (*progress_cb)(void*, long long, long long, long long, long long);
};

static size_t bridge_write_cb(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* b = static_cast<DownloadBridge*>(ud);
    return b->write_cb(ptr, sz, nm, b->write_userp);
}

static int bridge_progress_cb(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                              curl_off_t ulnow) {
    auto* b = static_cast<DownloadBridge*>(ud);
    return b->progress_cb(b->progress_userp, static_cast<long long>(dltotal),
                          static_cast<long long>(dlnow), static_cast<long long>(ultotal),
                          static_cast<long long>(ulnow));
}

CurlTransport::CurlTransport(const CurlTransportConfig& config) : config_(config) {
    share_ = curl_share_init();
    if (share_) {
        curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
        curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(share_, CURLSHOPT_LOCKFUNC, curlsh_lock_cb);
        curl_share_setopt(share_, CURLSHOPT_UNLOCKFUNC, curlsh_unlock_cb);
    }
}

CurlTransport::~CurlTransport() {
    if (share_) curl_share_cleanup(share_);
}

void CurlTransport::configure(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, config_.user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)config_.timeout_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)config_.connect_timeout_sec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, config_.low_speed_limit);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, config_.low_speed_time_sec);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)config_.max_redirs);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 10L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 5L);
    if (share_) curl_easy_setopt(curl, CURLOPT_SHARE, share_);
}

ProbeResult CurlTransport::head(const std::string& url) {
    CurlHeader hdr;
    ProbeResult result;

    CURL* curl = curl_easy_init();
    if (!curl) return result;

    configure(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hdr);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 10L);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return result;
    if (code != 200 && code != 206) return result;

    result.ok = true;
    result.status = code;
    result.file_size = hdr.file_size;
    result.range_supported = hdr.range_supported || (code == 206);
    result.filename = std::move(hdr.filename);
    return result;
}

DownloadResult CurlTransport::download(
    const std::string& url, std::optional<std::pair<uint64_t, uint64_t>> range, void* write_userp,
    size_t (*write_cb)(char*, size_t, size_t, void*), void* progress_userp,
    int (*progress_cb)(void*, long long, long long, long long, long long)) {
    CURL* curl = curl_easy_init();
    if (!curl) return {false, "curl_easy_init failed"};

    configure(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    DownloadBridge bridge{write_userp, write_cb, progress_userp, progress_cb};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, bridge_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bridge);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, bridge_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &bridge);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    if (range) {
        std::string r = std::to_string(range->first) + "-" + std::to_string(range->second);
        curl_easy_setopt(curl, CURLOPT_RANGE, r.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    DownloadResult result;
    long http_code = 0;
    if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    result.http_code = http_code;
    if (res != CURLE_OK) {
        result.ok = false;
        result.error = std::string(curl_easy_strerror(res));
    } else {
        result.ok = true;
    }
    curl_easy_cleanup(curl);
    return result;
}

}  // namespace multidow
