#include "downloader.h"
#include "checksum.h"
#include "file_handle.h"
#include "thread_pool.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <mutex>
#include <stop_token>
#include <thread>

#ifdef _WIN32
#include <io.h>
#define pwrite64 _pwrite
#define fileno _fileno
#else
#include <unistd.h>
#endif

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
    std::atomic<uint64_t>* bytes_written;
    int fd;
    uint64_t base_start;
};

static size_t chunk_write_cb(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* c = static_cast<ChunkCtx*>(ud);
    size_t bytes = sz * nm;
    uint64_t offset = c->base_start + c->bytes_written->load(std::memory_order_relaxed);
    ssize_t written = pwrite64(c->fd, ptr, bytes, offset);
    if (written < 0) return 0;
    *c->bytes_written += (uint64_t)written;
    c->pm->update_thread(c->file_id, c->thread_id,
                         c->bytes_written->load(std::memory_order_relaxed), c->chunk_total);
    return (size_t)written;
}

static int chunk_progress_cb(void* ud, long long, long long, long long, long long) {
    auto* c = static_cast<ChunkCtx*>(ud);
    if (g_stop.stop_requested()) return 1;
    c->pm->update_thread(c->file_id, c->thread_id,
                         c->bytes_written->load(std::memory_order_relaxed), c->chunk_total);
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

static int backoff_seconds(int attempt) {
    int delay = 1 << attempt;
    return delay > 16 ? 16 : delay;
}

uint64_t Downloader::probe_server() {
    if (file_id_ >= 0) pm_.set_file_status(file_id_, "probing...");

    for (int attempt = 0; attempt <= config_.max_retries; attempt++) {
        if (g_stop.stop_requested()) return 0;

        ProbeResult result = transport_.head(config_.url);

        if (result.ok) {
            file_size_ = result.file_size;
            range_supported_ = result.range_supported;
            if (!result.filename.empty() && !config_.output_explicit) {
                config_.output_path = sanitize_filename(result.filename);
                pm_.rename_file(file_id_, config_.output_path);
            }
            return file_size_;
        }

        if (attempt < config_.max_retries) {
            int delay = backoff_seconds(attempt);
            if (file_id_ >= 0)
                pm_.set_file_status(file_id_,
                                    std::format("probe failed ({}/{}), retrying in {}s...",
                                                attempt + 1, config_.max_retries, delay));
            for (int s = 0; s < delay; s++) {
                if (g_stop.stop_requested()) return 0;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }
    return 0;
}

bool Downloader::run() {
    try {
        file_id_ = pm_.add_file(config_.output_path, file_size_, config_.num_threads);

        probe_server();

        pm_.update_file_size(file_id_, file_size_);

        if (file_size_ == 0) {
            range_supported_ = false;
            config_.num_threads = 1;
        }
        if (!range_supported_) config_.num_threads = 1;

        if (file_size_ > 0 && range_supported_ && config_.num_threads > 1)
            pm_.set_file_status(file_id_, "downloading...");

        bool ok = start_download();

        if (!ok && config_.num_threads > 1 && !g_stop.stop_requested()) {
            pm_.set_file_status(file_id_, "Multi-threaded failed, trying single-threaded...");
            pm_.reset_file_threads(file_id_, 1);
            pm_.redraw();
            fs::remove(meta_path());
            fs::remove(config_.output_path);
            config_.num_threads = 1;
            ok = start_download();
        }

        if (ok && config_.verify && !config_.expected_checksum.empty()) {
            pm_.set_file_done(file_id_, false, "Verifying...");
            if (verify_checksum(config_.output_path, config_.expected_checksum))
                pm_.set_file_done(file_id_, true, "Done (checksum OK)");
            else
                pm_.set_file_done(file_id_, false, "FAILED (checksum mismatch)");
        } else if (ok) {
            pm_.set_file_done(file_id_, true, "Done");
        } else {
            pm_.set_file_done(file_id_, false, "Failed");
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

    if (config_.num_threads <= 1 || !range_supported_ || file_size_ == 0) {
        return download_single();
    }

    FileHandle fp(output, resume ? "r+b" : "wb");
    if (!fp) {
        pm_.set_file_done(file_id_, false, "Cannot open file");
        return false;
    }

    int fd = fileno(fp.get());
    return start_download_multi(fd, resume);
}

bool Downloader::start_download_multi(int fd, bool resume) {
    std::vector<std::pair<uint64_t, uint64_t>> saved_chunks;
    if (resume) saved_chunks = load_chunk_progress();

    uint64_t chunk_size = file_size_ / config_.num_threads;
    int total_chunks = config_.num_threads;
    std::atomic<int> success_count{0};
    std::vector<std::future<bool>> futures;
    futures.reserve(total_chunks);

    std::vector<std::atomic<uint64_t>> chunk_bytes(total_chunks);
    for (int i = 0; i < total_chunks; i++) {
        uint64_t base_start = (uint64_t)i * chunk_size;
        uint64_t already = 0;
        if (resume && i < (int)saved_chunks.size() && saved_chunks[i].first == base_start)
            already = saved_chunks[i].second;
        chunk_bytes[i].store(already, std::memory_order_relaxed);
    }

    for (int i = 0; i < total_chunks; i++) {
        uint64_t base_start = (uint64_t)i * chunk_size;
        uint64_t end =
            (i == total_chunks - 1) ? file_size_ - 1 : (uint64_t)(i + 1) * chunk_size - 1;
        uint64_t chunk_total = end - base_start + 1;
        uint64_t already = chunk_bytes[i].load(std::memory_order_relaxed);

        if (already >= chunk_total) {
            pm_.update_thread(file_id_, i, chunk_total, chunk_total);
            pm_.mark_thread_active(file_id_, i);
            pm_.mark_thread_finished(file_id_, i);
            success_count++;
            continue;
        }

        if (i > 0 && already == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

        futures.push_back(pool_.submit([this, i, base_start, chunk_total, fd,
                                        &success_count, &chunk_bytes]() -> bool {
            pm_.mark_thread_active(file_id_, i);

            bool success = false;
            for (int retry = 0; retry <= config_.max_retries; retry++) {
                if (g_stop.stop_requested()) break;

                uint64_t cur = base_start + chunk_bytes[i].load(std::memory_order_relaxed);
                ChunkCtx ctx{&pm_, file_id_, i, chunk_total, &chunk_bytes[i], fd, base_start};

                std::optional<std::pair<uint64_t, uint64_t>> range =
                    std::make_pair(cur, base_start + chunk_total - 1);

                DownloadResult dl_result = transport_.download(config_.url, range, &ctx,
                                                               chunk_write_cb, &ctx,
                                                               chunk_progress_cb);

                if (dl_result.ok) {
                    if (dl_result.http_code != 206) {
                        dl_result.ok = false;
                        dl_result.error =
                            "Server ignored Range (got " + std::to_string(dl_result.http_code) +
                            ")";
                    } else {
                        success = true;
                        break;
                    }
                }

                if (retry < config_.max_retries) {
                    int delay = backoff_seconds(retry);
                    pm_.mark_thread_error(file_id_, i);
                    pm_.set_file_status(file_id_,
                                        std::format("ch{} retry {}s: {}", i, delay, dl_result.error));
                    pm_.redraw();
                    std::this_thread::sleep_for(std::chrono::seconds(delay));
                    pm_.mark_thread_active(file_id_, i);
                } else {
                    pm_.set_file_status(file_id_,
                                        std::format("ch{} failed: {}", i, dl_result.error));
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

    if (g_stop.stop_requested() || success_count < total_chunks) {
        std::vector<std::pair<uint64_t, uint64_t>> progress;
        for (int i = 0; i < total_chunks; i++) {
            uint64_t base_start = (uint64_t)i * chunk_size;
            progress.push_back({base_start, chunk_bytes[i].load(std::memory_order_relaxed)});
        }
        save_metadata_multi(progress);
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

        if (resume_offset > 0) fseeko(fp.get(), resume_offset, SEEK_SET);

        pm_.mark_thread_active(file_id_, 0);
        pm_.redraw();

        SingleCtx ctx{fp.get(), &pm_, file_id_, resume_offset, file_size_};

        std::optional<std::pair<uint64_t, uint64_t>> range;
        if (resume_offset > 0 && file_size_ > 0)
            range = std::make_pair(resume_offset, file_size_ - 1);

        DownloadResult dl_result = transport_.download(config_.url, range, &ctx, single_write_cb,
                                                        &ctx, single_progress_cb);

        if (g_stop.stop_requested()) {
            save_metadata();
            return false;
        }

        bool concatenation = false;
        if (range && dl_result.ok && dl_result.http_code != 206) {
            concatenation = true;
        }

        if (concatenation) {
            dl_result.ok = false;
            dl_result.error = "Server ignored Range (got " + std::to_string(dl_result.http_code) + ")";
        }

        if (dl_result.ok) {
            pm_.mark_thread_finished(file_id_, 0);
            fs::remove(meta_path());
            return true;
        }

        resume_offset = fs::file_size(output);
        if (file_size_ > 0 && resume_offset > file_size_) {
            fs::remove(output);
            fs::remove(meta_path());
            resume_offset = 0;
        }

        if (retry < config_.max_retries) {
            pm_.mark_thread_error(file_id_, 0);
            pm_.set_file_status(file_id_,
                                std::format("retry {} in {}s: {}", retry + 1,
                                            backoff_seconds(retry), dl_result.error));
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

void Downloader::save_metadata_multi(const std::vector<std::pair<uint64_t, uint64_t>>& chunk_progress) {
    std::ofstream f(meta_path());
    if (!f) return;
    f << config_.url << "\n" << file_size_ << "\n" << config_.output_path << "\n";
    f << "MT\n";
    f << chunk_progress.size() << "\n";
    for (auto& [start, done] : chunk_progress) f << start << " " << done << "\n";
}

std::vector<std::pair<uint64_t, uint64_t>> Downloader::load_chunk_progress() {
    std::vector<std::pair<uint64_t, uint64_t>> result;
    std::ifstream f(meta_path());
    if (!f) return result;

    std::string url_line, size_line, path_line;
    std::getline(f, url_line);
    std::getline(f, size_line);
    std::getline(f, path_line);

    std::string marker;
    if (!std::getline(f, marker) || marker != "MT") return result;

    std::string count_str;
    if (!std::getline(f, count_str)) return result;
    int count = 0;
    try { count = std::stoi(count_str); } catch (...) { return result; }

    for (int i = 0; i < count; i++) {
        std::string line;
        if (!std::getline(f, line)) break;
        auto sp = line.find(' ');
        if (sp == std::string::npos) break;
        try {
            uint64_t start = std::stoull(line.substr(0, sp));
            uint64_t done = std::stoull(line.substr(sp + 1));
            result.push_back({start, done});
        } catch (...) { break; }
    }
    return result;
}

}
