#include "curl_transport.h"

#include <cstring>

namespace multidow {

static int curl_debug_cb(CURL*, curl_infotype type, char*, size_t size, void*) {
    if (type == CURLINFO_TEXT)
        return 0;
    if (type == CURLINFO_HEADER_IN || type == CURLINFO_HEADER_OUT)
        return 0;
    (void)size;
    return 0;
}

struct CurlHeader {
    uint64_t file_size = 0;
    bool range_supported = false;
    std::string filename;
};

static size_t header_cb(char* buf, size_t /*size*/, size_t nitems, void* userp) {
    auto* hdr = static_cast<CurlHeader*>(userp);
    std::string h(buf, nitems);
    if (h.find("Content-Length:") != std::string::npos) {
        std::string v = h.substr(h.find(":") + 1);
        v.erase(0, v.find_first_not_of(" "));
        v.erase(v.find_last_not_of(" \r\n") + 1);
        try { hdr->file_size = std::stoull(v); } catch (...) {}
    }
    if (h.find("Accept-Ranges: bytes") != std::string::npos)
        hdr->range_supported = true;
    if (h.find("Content-Disposition:") != std::string::npos) {
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

static int bridge_progress_cb(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    auto* b = static_cast<DownloadBridge*>(ud);
    return b->progress_cb(b->progress_userp,
        static_cast<long long>(dltotal), static_cast<long long>(dlnow),
        static_cast<long long>(ultotal), static_cast<long long>(ulnow));
}

CurlTransport::CurlTransport(const CurlTransportConfig& config)
    : config_(config) {}

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
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_debug_cb);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
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

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
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

bool CurlTransport::download(
    const std::string& url,
    std::optional<std::pair<uint64_t, uint64_t>> range,
    void* write_userp,
    size_t (*write_cb)(char*, size_t, size_t, void*),
    void* progress_userp,
    int (*progress_cb)(void*, long long, long long, long long, long long)
) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

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
    curl_easy_cleanup(curl);
    return res == CURLE_OK;
}

}
