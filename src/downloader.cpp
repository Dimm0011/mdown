#include "downloader.h"
#include "checksum.h"
#include "file_handle.h"
#include "thread_pool.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stop_token>
#include <thread>

namespace fs = std::filesystem;

namespace multidow {

static std::string sanitize_filename(const std::string& name) {
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        if (c == '/' || c == '\\' || c == '\0' || c == ':') continue;
        if (c == '.' && (result.empty() || result.back() == '.')) continue;
        result.push_back(c);
    }
    while (!result.empty() && result.back() == '.') result.pop_back();
    return result.empty() ? "download" : result;
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
    {
        std::lock_guard<std::mutex> lock(*c->fp_mtx);
        fseek(c->fp, c->write_offset, SEEK_SET);
        size_t written = fwrite(ptr, 1, bytes, c->fp);
        c->write_offset += written;
        c->bytes_written += written;
    }
    c->pm->update_thread(c->file_id, c->thread_id, c->bytes_written, c->chunk_total);
    return bytes;
}

static int chunk_progress_cb(void* ud, long long, long long, long long, long long) {
    auto* c = static_cast<ChunkCtx*>(ud);
    if (g_stop.stop_requested()) return 1;
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

static int single_progress_cb(void* ud, long long dltotal, long long dlnow, long long, long long) {
    auto* c = static_cast<SingleCtx*>(ud);
    if (g_stop.stop_requested()) return 1;
    uint64_t total = (dltotal > 0) ? (uint64_t)dltotal : c->expected_total;
    uint64_t now = (dlnow > 0) ? (uint64_t)dlnow : c->bytes_written;
    c->pm->update_thread(c->file_id, 0, now, total);
    return 0;
}

Downloader::Downloader(const DownloadConfig& config, ProgressManager& pm, ThreadPool& pool,
                       ITransport& transport)
    : config_(config), pm_(pm), pool_(pool), transport_(transport) {
    if (config_.output_path.empty()) config_.output_path = extract_filename(config_.url);
}

std::string Downloader::extract_filename(const std::string& url) const {
    std::string p = url;
    auto q = p.find('?');
    if (q != std::string::npos) p.erase(q);
    auto pos = p.find_last_of('/');
    if (pos != std::string::npos) p = p.substr(pos + 1);
    return p.empty() ? "download" : p;
}

std::string Downloader::meta_path() const {
    return config_.output_path + ".mdow";
}

uint64_t Downloader::probe_server() {
    for (int attempt = 0; attempt <= config_.max_retries; attempt++) {
        ProbeResult result = transport_.head(config_.url);

        if (result.ok) {
            file_size_ = result.file_size;
            range_supported_ = result.range_supported;
            if (!result.filename.empty() && !config_.output_explicit)
                config_.output_path = sanitize_filename(result.filename);
            return file_size_;
        }

        if (attempt < config_.max_retries) {
            int delay = 1 << attempt;
            std::cerr << std::format("Probe failed, retrying in {}s...\r", delay) << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(delay));
        }
    }
    return 0;
}

bool Downloader::run() {
    try {
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
    } catch (const std::exception& e) {
        if (file_id_ >= 0) {
            pm_.set_file_done(file_id_, false, std::string("Error: ") + e.what());
            pm_.redraw();
        }
        return false;
    } catch (...) {
        if (file_id_ >= 0) {
            pm_.set_file_done(file_id_, false, "Unknown error");
            pm_.redraw();
        }
        return false;
    }
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

    if (config_.num_threads <= 1 || !range_supported_ || file_size_ == 0) return download_single();

    FileHandle fp(output, resume ? "r+b" : "wb");
    if (!fp) {
        pm_.set_file_done(file_id_, false, "Cannot open file");
        return false;
    }

    if (file_size_ > 0) {
        fseek(fp.get(), file_size_ - 1, SEEK_SET);
        fwrite("", 1, 1, fp.get());
    }

    uint64_t chunk_size = file_size_ / config_.num_threads;
    std::mutex fp_mtx;
    std::atomic<int> success_count{0};
    int total_chunks = config_.num_threads;
    std::vector<std::future<bool>> futures;
    futures.reserve(total_chunks);

    for (int i = 0; i < config_.num_threads; i++) {
        uint64_t base_start = (uint64_t)i * chunk_size;
        uint64_t end =
            (i == config_.num_threads - 1) ? file_size_ - 1 : (uint64_t)(i + 1) * chunk_size - 1;
        uint64_t chunk_total = end - base_start + 1;

        futures.push_back(pool_.submit([this, i, base_start, chunk_total, fp_raw = fp.get(),
                                        &fp_mtx, &success_count]() -> bool {
            pm_.mark_thread_active(file_id_, i);
            pm_.redraw();

            bool success = false;
            for (int retry = 0; retry <= config_.max_retries; retry++) {
                if (g_stop.stop_requested()) break;

                uint64_t bytes_already = 0;
                {
                    std::lock_guard lock(fp_mtx);
                    long pos = ftell(fp_raw);
                    if (pos >= (long)base_start && pos < (long)(base_start + chunk_total))
                        bytes_already = (uint64_t)pos - base_start;
                }

                uint64_t resume_from = base_start + bytes_already;
                uint64_t remaining = chunk_total - bytes_already;

                if (remaining == 0) {
                    success = true;
                    break;
                }

                ChunkCtx ctx{&pm_,          file_id_, i,           chunk_total,
                             bytes_already, fp_raw,   resume_from, &fp_mtx};

                std::optional<std::pair<uint64_t, uint64_t>> range =
                    std::make_pair(resume_from, base_start + chunk_total - 1);

                bool ok = transport_.download(config_.url, range, &ctx, chunk_write_cb, &ctx,
                                              chunk_progress_cb);

                if (ok) {
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
            return success;
        }));
    }

    for (auto& f : futures) f.get();

    if (g_stop.stop_requested()) {
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
    if (fs::exists(output) && fs::exists(meta_path())) resume_offset = fs::file_size(output);

    for (int retry = 0; retry <= config_.max_retries; retry++) {
        if (g_stop.stop_requested()) return false;

        FileHandle fp(output, retry == 0 && resume_offset == 0 ? "wb" : "r+b");
        if (!fp) {
            pm_.set_file_done(file_id_, false, "Cannot open file");
            return false;
        }

        if (resume_offset > 0) fseek(fp.get(), resume_offset, SEEK_SET);

        pm_.mark_thread_active(file_id_, 0);
        pm_.redraw();

        SingleCtx ctx{fp.get(), &pm_, file_id_, resume_offset, file_size_};

        std::optional<std::pair<uint64_t, uint64_t>> range;
        if (resume_offset > 0 && file_size_ > 0)
            range = std::make_pair(resume_offset, file_size_ - 1);

        bool ok = transport_.download(config_.url, range, &ctx, single_write_cb, &ctx,
                                      single_progress_cb);

        if (g_stop.stop_requested()) {
            save_metadata();
            return false;
        }

        if (ok) {
            pm_.mark_thread_finished(file_id_, 0);
            fs::remove(meta_path());
            return true;
        }

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

}  // namespace multidow
