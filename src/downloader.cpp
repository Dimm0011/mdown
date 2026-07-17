#include "downloader.h"
#include "checksum.h"

#include <curl/curl.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

namespace multidow {

static const char* USER_AGENT = "Mozilla/5.0 (X11; Linux x86_64) MultiDow/1.0";

struct CurlHeader {
    uint64_t file_size = 0;
    bool range_supported = false;
    std::string filename;
};

static size_t header_cb(char* buf, size_t size, size_t nitems, void* userp) {
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

struct ChunkCtx {
    ProgressManager* pm;
    int file_id;
    int thread_id;
    uint64_t chunk_total;
    uint64_t bytes_written;
    FILE* fp;
    uint64_t write_offset;
    std::mutex* fp_mtx;
};

static size_t chunk_write_cb(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* c = static_cast<ChunkCtx*>(ud);
    size_t bytes = sz * nm;
    std::lock_guard<std::mutex> lock(*c->fp_mtx);
    fseek(c->fp, c->write_offset, SEEK_SET);
    size_t written = fwrite(ptr, 1, bytes, c->fp);
    c->write_offset += written;
    c->bytes_written += written;
    c->pm->update_thread(c->file_id, c->thread_id, c->bytes_written, c->chunk_total);
    return written;
}

static int chunk_xfer_cb(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* c = static_cast<ChunkCtx*>(ud);
    if (g_cancelled) return 1;
    c->pm->update_thread(c->file_id, c->thread_id, c->bytes_written, c->chunk_total);
    return 0;
}

struct SingleCtx {
    FILE* fp;
    ProgressManager* pm;
    int file_id;
    uint64_t bytes_written;
    uint64_t expected_total;
};

static size_t single_write_cb(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* c = static_cast<SingleCtx*>(ud);
    size_t bytes = sz * nm;
    size_t written = fwrite(ptr, 1, bytes, c->fp);
    c->bytes_written += written;
    c->pm->update_thread(c->file_id, 0, c->bytes_written, c->expected_total);
    return written;
}

static int single_xfer_cb(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* c = static_cast<SingleCtx*>(ud);
    if (g_cancelled) return 1;
    uint64_t total = (dltotal > 0) ? (uint64_t)dltotal : c->expected_total;
    uint64_t now = (dlnow > 0) ? (uint64_t)dlnow : c->bytes_written;
    c->pm->update_thread(c->file_id, 0, now, total);
    return 0;
}

void Downloader::configure_curl(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)config_.timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
}

Downloader::Downloader(const DownloadConfig& config, ProgressManager& pm)
    : config_(config), pm_(pm) {
    if (config_.output_path.empty())
        config_.output_path = extract_filename(config_.url);
}

std::string Downloader::extract_filename(const std::string& url) const {
    std::string p = url;
    auto q = p.find('?');
    if (q != std::string::npos) p.erase(q);
    auto pos = p.find_last_of('/');
    if (pos != std::string::npos) p = p.substr(pos + 1);
    return p.empty() ? "download" : p;
}

std::string Downloader::meta_path() const { return config_.output_path + ".mdow"; }

uint64_t Downloader::probe_server() {
    for (int attempt = 0; attempt <= config_.max_retries; attempt++) {
        CurlHeader hdr;
        CURL* curl = curl_easy_init();
        if (!curl) return 0;

        configure_curl(curl);
        curl_easy_setopt(curl, CURLOPT_URL, config_.url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hdr);

        CURLcode res = curl_easy_perform(curl);
        long code = 0;
        if (res == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(curl);

        if (res == CURLE_OK && code == 206) hdr.range_supported = true;

        if (res == CURLE_OK && (code == 200 || code == 206)) {
            file_size_ = hdr.file_size;
            range_supported_ = hdr.range_supported;
            if (!hdr.filename.empty())
                config_.output_path = hdr.filename;
            return file_size_;
        }

        if (attempt < config_.max_retries) {
            int delay = 1 << attempt;
            pm_.set_file_done(file_id_ >= 0 ? file_id_ : pm_.add_file(config_.output_path, 0, 1),
                              false, "Probe failed, retrying in " + std::to_string(delay) + "s...");
            std::this_thread::sleep_for(std::chrono::seconds(delay));
        }
    }
    return 0;
}

bool Downloader::run() {
    probe_server();

    if (file_size_ == 0) {
        range_supported_ = false;
        config_.num_threads = 1;
    }
    if (!range_supported_) config_.num_threads = 1;

    file_id_ = pm_.add_file(config_.output_path, file_size_, config_.num_threads);

    bool ok = start_download();

    if (ok && config_.verify && !config_.expected_checksum.empty()) {
        pm_.set_file_done(file_id_, false, "Verifying...");
        if (verify_checksum(config_.output_path, config_.expected_checksum))
            pm_.set_file_done(file_id_, true, "Done (checksum OK)");
        else
            pm_.set_file_done(file_id_, false, "FAILED (checksum mismatch)");
    } else if (ok) {
        pm_.set_file_done(file_id_, true, "Done");
    }
    pm_.redraw();
    return ok;
}

static int backoff_seconds(int attempt) {
    int delay = 1 << attempt;
    return delay > 16 ? 16 : delay;
}

bool Downloader::start_download() {
    std::string output = config_.output_path;
    bool resume = fs::exists(meta_path()) && fs::exists(output);

    if (resume) {
        uint64_t sz = fs::file_size(output);
        if (file_size_ > 0 && sz >= file_size_) {
            pm_.set_file_done(file_id_, true, "Already downloaded");
            return true;
        }
    } else if (fs::exists(meta_path())) {
        fs::remove(meta_path());
    }

    if (config_.num_threads <= 1 || !range_supported_ || file_size_ == 0)
        return download_single();

    FILE* fp = fopen(output.c_str(), resume ? "r+b" : "wb");
    if (!fp) {
        pm_.set_file_done(file_id_, false, "Cannot open file");
        return false;
    }

    if (file_size_ > 0) {
        fseek(fp, file_size_ - 1, SEEK_SET);
        fwrite("", 1, 1, fp);
    }

    uint64_t chunk_size = file_size_ / config_.num_threads;
    std::mutex fp_mtx;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    int total_chunks = config_.num_threads;

    for (int i = 0; i < config_.num_threads; i++) {
        uint64_t base_start = (uint64_t)i * chunk_size;
        uint64_t end = (i == config_.num_threads - 1) ? file_size_ - 1 : (uint64_t)(i + 1) * chunk_size - 1;
        uint64_t chunk_total = end - base_start + 1;

        threads.emplace_back([this, i, base_start, chunk_total, fp, &fp_mtx, &success_count]() {
            pm_.mark_thread_active(file_id_, i);
            pm_.redraw();

            bool success = false;
            for (int retry = 0; retry <= config_.max_retries; retry++) {
                if (g_cancelled) break;

                CURL* curl = curl_easy_init();
                if (!curl) { std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }

                uint64_t bytes_already = 0;
                {
                    std::lock_guard<std::mutex> lock(fp_mtx);
                    long pos = ftell(fp);
                    if (pos >= (long)base_start && pos < (long)(base_start + chunk_total))
                        bytes_already = (uint64_t)pos - base_start;
                }

                uint64_t resume_from = base_start + bytes_already;
                uint64_t remaining = chunk_total - bytes_already;

                if (remaining == 0) { success = true; curl_easy_cleanup(curl); break; }

                ChunkCtx ctx{&pm_, file_id_, i, chunk_total, bytes_already, fp, resume_from, &fp_mtx};

                configure_curl(curl);
                curl_easy_setopt(curl, CURLOPT_URL, config_.url.c_str());
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, chunk_write_cb);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
                curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, chunk_xfer_cb);
                curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

                std::string range = std::to_string(resume_from) + "-" + std::to_string(base_start + chunk_total - 1);
                curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());

                CURLcode res = curl_easy_perform(curl);
                curl_easy_cleanup(curl);

                if (res == CURLE_OK) {
                    success = true;
                    break;
                }

                if (retry < config_.max_retries) {
                    int delay = backoff_seconds(retry);
                    pm_.mark_thread_error(file_id_, i);
                    pm_.redraw();
                    std::this_thread::sleep_for(std::chrono::seconds(delay));
                    pm_.mark_thread_active(file_id_, i);
                }
            }

            if (success) {
                pm_.mark_thread_finished(file_id_, i);
                success_count++;
            } else {
                pm_.mark_thread_error(file_id_, i);
            }
            pm_.redraw();
        });
    }

    for (auto& t : threads) t.join();
    fclose(fp);

    if (g_cancelled) {
        save_metadata();
        return false;
    }

    if (success_count < total_chunks) {
        save_metadata();
        return false;
    }

    fs::remove(meta_path());
    return true;
}

bool Downloader::download_single() {
    std::string output = config_.output_path;

    uint64_t resume_offset = 0;
    if (fs::exists(output) && fs::exists(meta_path()))
        resume_offset = fs::file_size(output);

    for (int retry = 0; retry <= config_.max_retries; retry++) {
        if (g_cancelled) return false;

        FILE* fp = fopen(output.c_str(), retry == 0 && resume_offset == 0 ? "wb" : "r+b");
        if (!fp) {
            pm_.set_file_done(file_id_, false, "Cannot open file");
            return false;
        }

        if (resume_offset > 0)
            fseek(fp, resume_offset, SEEK_SET);

        pm_.mark_thread_active(file_id_, 0);
        pm_.redraw();

        SingleCtx ctx{fp, &pm_, file_id_, resume_offset, file_size_};

        CURL* curl = curl_easy_init();
        configure_curl(curl);
        curl_easy_setopt(curl, CURLOPT_URL, config_.url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, single_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, single_xfer_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

        if (resume_offset > 0 && file_size_ > 0) {
            std::string range = std::to_string(resume_offset) + "-" + std::to_string(file_size_ - 1);
            curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
        }

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (g_cancelled) {
            fclose(fp);
            save_metadata();
            return false;
        }

        if (res == CURLE_OK) {
            fclose(fp);
            pm_.mark_thread_finished(file_id_, 0);
            fs::remove(meta_path());
            return true;
        }

        fclose(fp);
        resume_offset = fs::file_size(output);

        if (retry < config_.max_retries) {
            pm_.mark_thread_error(file_id_, 0);
            pm_.redraw();
            int delay = backoff_seconds(retry);
            std::this_thread::sleep_for(std::chrono::seconds(delay));
        }
    }

    save_metadata();
    pm_.mark_thread_error(file_id_, 0);
    return false;
}

void Downloader::save_metadata() {
    std::ofstream f(meta_path());
    f << config_.url << "\n" << file_size_ << "\n" << config_.output_path << "\n";
}

}
