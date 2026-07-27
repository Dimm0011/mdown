#include <curl/curl.h>
#include "curl_transport.h"
#include "downloader.h"
#include "progress.h"
#include "thread_pool.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

static long parse_int(const char* s) {
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0') return -1;
    return v;
}

static void usage() {
    std::cout << "mdown - multi-threaded file downloader with resume\n\n"
              << "Usage: mdown [options] <URL...>\n\n"
              << "Options:\n"
              << "  -f, --file <path>        File with URLs (one per line)\n"
              << "  -o, --output <path>      Output file (single URL only)\n"
              << "  -t, --threads <N>        Threads per file (default: 4)\n"
              << "  -c, --checksum <SHA256>  Expected SHA-256 (single URL only)\n"
              << "  -r, --retries <N>        Max retries per chunk (default: 3)\n"
              << "  -T, --timeout <sec>      Transfer timeout (default: 300)\n"
              << "  -h, --help               Show this help\n\n"
              << "Examples:\n"
              << "  mdown https://example.com/file.zip\n"
              << "  mdown https://url1.zip https://url2.zip -t 8\n"
              << "  mdown -f urls.txt -t 4\n"
              << std::endl;
}

static std::vector<std::string> read_urls_from_file(const std::string& path) {
    std::vector<std::string> urls;
    std::ifstream f(path);
    if (!f) {
        std::cerr << "Cannot open URL file: " << path << std::endl;
        return urls;
    }
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        while (!line.empty() && line.front() == ' ') line.erase(line.begin());
        if (!line.empty() && line[0] != '#') urls.push_back(line);
    }
    return urls;
}

static void sigint_handler(int) {
    multidow::g_signal_received = 1;
}

#ifdef _WIN32
static BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        multidow::g_signal_received = 1;
        return TRUE;
    }
    return FALSE;
}
#endif

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage();
        return 1;
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    struct sigaction sa {};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif

    multidow::DownloadConfig cfg;
    std::vector<std::string> urls;
    std::string url_file;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) {
                cfg.output_path = argv[++i];
                cfg.output_explicit = true;
            }
        } else if (arg == "-t" || arg == "--threads") {
            if (i + 1 < argc) {
                long v = parse_int(argv[++i]);
                if (v < 0) {
                    std::cerr << "Invalid threads value: " << argv[i] << std::endl;
                    return 1;
                }
                cfg.num_threads = (int)v;
            }
        } else if (arg == "-c" || arg == "--checksum") {
            if (i + 1 < argc) {
                cfg.expected_checksum = argv[++i];
                cfg.verify = true;
            }
        } else if (arg == "-r" || arg == "--retries") {
            if (i + 1 < argc) {
                long v = parse_int(argv[++i]);
                if (v < 0) {
                    std::cerr << "Invalid retries value: " << argv[i] << std::endl;
                    return 1;
                }
                cfg.max_retries = (int)v;
            }
        } else if (arg == "-T" || arg == "--timeout") {
            if (i + 1 < argc) {
                long v = parse_int(argv[++i]);
                if (v <= 0) {
                    std::cerr << "Invalid timeout value: " << argv[i] << std::endl;
                    return 1;
                }
                cfg.timeout = (int)v;
            }
        } else if (arg == "-f" || arg == "--file") {
            if (i + 1 < argc) url_file = argv[++i];
        } else if (arg[0] != '-') {
            urls.push_back(arg);
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            return 1;
        }
    }

    if (!url_file.empty()) {
        auto file_urls = read_urls_from_file(url_file);
        urls.insert(urls.end(), file_urls.begin(), file_urls.end());
    }

    if (urls.empty()) {
        std::cerr << "No URLs specified." << std::endl;
        return 1;
    }

    if (urls.size() > 1) {
        if (!cfg.expected_checksum.empty()) {
            std::cerr << "-c/--checksum is only supported for a single URL, ignoring\n";
            cfg.expected_checksum.clear();
            cfg.verify = false;
        }
        if (cfg.output_explicit) {
            std::cerr << "-o/--output is only supported for a single URL, ignoring\n";
            cfg.output_explicit = false;
            cfg.output_path.clear();
        }
    }

    if (cfg.num_threads < 1) cfg.num_threads = 1;
    unsigned hw = std::thread::hardware_concurrency();
    int max_threads = (hw > 0) ? std::min((int)hw, 16) : 16;
    if (cfg.num_threads > max_threads) cfg.num_threads = max_threads;

    if (curl_global_init(CURL_GLOBAL_ALL) != 0) {
        std::cerr << "curl_global_init failed" << std::endl;
        return 1;
    }

    multidow::CurlTransportConfig tc;
    tc.timeout_sec = cfg.timeout;
    multidow::CurlTransport transport(tc);

    multidow::ProgressManager pm;
    multidow::ThreadPool pool;
    std::vector<std::unique_ptr<multidow::Downloader>> downloaders;
    std::vector<std::jthread> file_threads;

    std::jthread timer([&pm](std::stop_token st) {
        while (!st.stop_requested()) {
            if (multidow::g_signal_received) {
                multidow::g_stop.request_stop();
                break;
            }
            pm.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    std::jthread watchdog([&pm](std::stop_token st) {
        std::mutex mtx;
        std::condition_variable_any cv;
        std::unique_lock lk(mtx);
        while (!cv.wait_for(lk, st, std::chrono::seconds(5), [&] { return st.stop_requested(); })) {
            if (pm.all_done()) break;
            if (pm.any_stalled(std::chrono::seconds(120))) {
                multidow::g_stop.request_stop();
                break;
            }
        }
    });

    for (auto& url : urls) {
        auto config = [&]() {
            multidow::DownloadConfig c = cfg;
            c.url = url;
            if (urls.size() > 1) c.output_path = "";
            return c;
        }();
        downloaders.push_back(
            std::make_unique<multidow::Downloader>(std::move(config), pm, pool, transport));
    }

    for (auto& dl : downloaders) {
        auto* ptr = dl.get();
        file_threads.emplace_back([ptr]() { ptr->run(); });
    }

    file_threads.clear();
    timer.request_stop();
    watchdog.request_stop();

    curl_global_cleanup();

    return 0;
}
