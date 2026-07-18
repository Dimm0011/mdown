#include "downloader.h"
#include "progress.h"
#include "thread_pool.h"
#include <curl/curl.h>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <stop_token>
#include <cstring>
#include <cstdlib>
#include <csignal>

static void usage() {
    std::cout << "MultiDow - multi-threaded file downloader with resume\n\n"
              << "Usage: multidow [options] <URL...>\n\n"
              << "Options:\n"
              << "  -f, --file <path>        File with URLs (one per line)\n"
              << "  -o, --output <path>      Output file (single URL only)\n"
              << "  -t, --threads <N>        Threads per file (default: 4)\n"
              << "  -c, --checksum <SHA256>  Expected SHA-256 (single URL only)\n"
              << "  -r, --retries <N>        Max retries per chunk (default: 3)\n"
              << "  -T, --timeout <sec>      Transfer timeout (default: 300)\n"
              << "  -h, --help               Show this help\n\n"
              << "Examples:\n"
              << "  multidow https://example.com/file.zip\n"
              << "  multidow https://url1.zip https://url2.zip -t 8\n"
              << "  multidow -f urls.txt -t 4\n"
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
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        while (!line.empty() && line.front() == ' ')
            line.erase(line.begin());
        if (!line.empty() && line[0] != '#')
            urls.push_back(line);
    }
    return urls;
}

static void sigint_handler(int) {
    multidow::g_signal_received = 1;
}

int main(int argc, char* argv[]) {
    if (argc < 2) { usage(); return 1; }

    struct sigaction sa{};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    multidow::DownloadConfig cfg;
    std::vector<std::string> urls;
    std::string url_file;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") { usage(); return 0; }
        else if (arg == "-o" || arg == "--output") { if (i + 1 < argc) cfg.output_path = argv[++i]; }
        else if (arg == "-t" || arg == "--threads") { if (i + 1 < argc) cfg.num_threads = std::atoi(argv[++i]); }
        else if (arg == "-c" || arg == "--checksum") { if (i + 1 < argc) { cfg.expected_checksum = argv[++i]; cfg.verify = true; } }
        else if (arg == "-r" || arg == "--retries") { if (i + 1 < argc) cfg.max_retries = std::atoi(argv[++i]); }
        else if (arg == "-T" || arg == "--timeout") { if (i + 1 < argc) cfg.timeout = std::atoi(argv[++i]); }
        else if (arg == "-f" || arg == "--file") { if (i + 1 < argc) url_file = argv[++i]; }
        else if (arg[0] != '-') { urls.push_back(arg); }
        else { std::cerr << "Unknown option: " << arg << std::endl; return 1; }
    }

    if (!url_file.empty()) {
        auto file_urls = read_urls_from_file(url_file);
        urls.insert(urls.end(), file_urls.begin(), file_urls.end());
    }

    if (urls.empty()) {
        std::cerr << "No URLs specified." << std::endl;
        return 1;
    }

    if (cfg.num_threads < 1) cfg.num_threads = 1;
    if (cfg.num_threads > 32) cfg.num_threads = 32;

    curl_global_init(CURL_GLOBAL_ALL);

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

    for (auto& url : urls) {
        auto config = [&]() {
            multidow::DownloadConfig c = cfg;
            c.url = url;
            if (urls.size() > 1) c.output_path = "";
            return c;
        }();
        downloaders.push_back(std::make_unique<multidow::Downloader>(std::move(config), pm, pool));
    }

    for (auto& dl : downloaders) {
        file_threads.emplace_back([&dl]() { dl->run(); });
    }

    file_threads.clear();
    timer.request_stop();

    curl_global_cleanup();

    return 0;
}
