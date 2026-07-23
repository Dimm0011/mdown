#include <gtest/gtest.h>
#include "downloader.h"
#include "thread_pool.h"
#include "progress.h"
#include "mock_transport.h"

#include <cstdio>
#include <fstream>
#include <unistd.h>
#include <filesystem>

namespace fs = std::filesystem;
using namespace multidow;
using multidow::mock::MockTransport;

class DownloaderTest : public ::testing::Test {
protected:
    ProgressManager pm;
    ThreadPool pool;
    MockTransport transport;
    std::string tmp_path;

    void SetUp() override {
        char buf[] = "/tmp/multidow_dl_test_XXXXXX";
        int fd = mkstemp(buf);
        close(fd);
        tmp_path = buf;
    }

    void TearDown() override {
        std::remove(tmp_path.c_str());
        std::remove((tmp_path + ".mdow").c_str());
    }

    DownloadConfig make_config(const std::string& url = "http://example.com/file.bin") {
        DownloadConfig cfg;
        cfg.url = url;
        cfg.output_path = tmp_path;
        cfg.output_explicit = true;
        cfg.num_threads = 1;
        cfg.max_retries = 2;
        return cfg;
    }
};

TEST_F(DownloaderTest, ProbeSuccess_SingleThread) {
    auto cfg = make_config();
    cfg.num_threads = 1;

    ProbeResult pr;
    pr.ok = true;
    pr.status = 200;
    pr.file_size = 0;
    pr.range_supported = false;
    transport.probe_responses.push_back(pr);

    transport.download_results.push_back(true);
    transport.download_data = {0x41, 0x42, 0x43};

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_TRUE(dl.run());
    EXPECT_EQ(transport.probe_count, 1);
    EXPECT_EQ(transport.download_count, 1);

    std::ifstream f(tmp_path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "ABC");
}

TEST_F(DownloaderTest, ProbeFailure_FallbackToSingleThread) {
    auto cfg = make_config();

    for (int i = 0; i < 3; i++) {
        ProbeResult pr;
        pr.ok = false;
        transport.probe_responses.push_back(pr);
    }

    for (int i = 0; i < 3; i++)
        transport.download_results.push_back(false);

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_FALSE(dl.run());
    EXPECT_EQ(transport.probe_count, 3);
    EXPECT_EQ(transport.download_count, 3);
}

TEST_F(DownloaderTest, ProbeRetryThenSuccess) {
    auto cfg = make_config();

    for (int i = 0; i < 2; i++) {
        ProbeResult pr;
        pr.ok = false;
        transport.probe_responses.push_back(pr);
    }

    ProbeResult pr;
    pr.ok = true;
    pr.status = 200;
    pr.file_size = 100;
    pr.range_supported = false;
    transport.probe_responses.push_back(pr);

    transport.download_results.push_back(true);
    transport.download_data.resize(100, 0x55);

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_TRUE(dl.run());
    EXPECT_EQ(transport.probe_count, 3);
    EXPECT_EQ(transport.download_count, 1);
}

TEST_F(DownloaderTest, OutputExplicit_Respected) {
    auto cfg = make_config();
    cfg.output_explicit = true;

    ProbeResult pr;
    pr.ok = true;
    pr.status = 200;
    pr.file_size = 50;
    pr.range_supported = false;
    pr.filename = "server_file.zip";
    transport.probe_responses.push_back(pr);

    transport.download_results.push_back(true);
    transport.download_data.resize(50, 0xBB);

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_TRUE(dl.run());

    EXPECT_TRUE(fs::exists(tmp_path));

    std::ifstream f(tmp_path, std::ios::binary | std::ios::ate);
    EXPECT_EQ(f.tellg(), 50);
}

TEST_F(DownloaderTest, OutputNotExplicit_ServerFilenameUsed) {
    char buf2[] = "/tmp/multidow_dl_test_notexplicit_XXXXXX";
    int fd2 = mkstemp(buf2);
    close(fd2);
    std::string server_file(buf2);

    DownloadConfig cfg;
    cfg.url = "http://example.com/file.bin";
    cfg.output_path = server_file;
    cfg.output_explicit = false;
    cfg.num_threads = 1;
    cfg.max_retries = 2;

    ProbeResult pr;
    pr.ok = true;
    pr.status = 200;
    pr.file_size = 0;
    pr.range_supported = false;
    pr.filename = "server_file.zip";
    transport.probe_responses.push_back(pr);

    transport.download_results.push_back(true);

    Downloader dl(cfg, pm, pool, transport);
    dl.run();

    std::ifstream f(server_file, std::ios::binary);
    EXPECT_TRUE(f.good());

    std::remove(server_file.c_str());
    std::remove((server_file + ".mdow").c_str());
    std::remove("server_file.zip");
    std::remove("server_file.zip.mdow");
}

TEST_F(DownloaderTest, PathTraversal_Sanitized) {
    char buf2[] = "/tmp/multidow_dl_test_traversal_XXXXXX";
    int fd2 = mkstemp(buf2);
    close(fd2);
    std::string target_file(buf2);

    DownloadConfig cfg;
    cfg.url = "http://example.com/file.bin";
    cfg.output_path = target_file;
    cfg.output_explicit = false;
    cfg.num_threads = 1;
    cfg.max_retries = 2;

    ProbeResult pr;
    pr.ok = true;
    pr.status = 200;
    pr.file_size = 0;
    pr.range_supported = false;
    pr.filename = "../../etc/passwd";
    transport.probe_responses.push_back(pr);

    transport.download_results.push_back(true);

    Downloader dl(cfg, pm, pool, transport);
    dl.run();

    EXPECT_TRUE(fs::exists(target_file));

    std::remove(target_file.c_str());
    std::remove((target_file + ".mdow").c_str());
}

TEST_F(DownloaderTest, DownloadFailure_RetriesExhausted) {
    auto cfg = make_config();

    ProbeResult pr;
    pr.ok = true;
    pr.status = 200;
    pr.file_size = 100;
    pr.range_supported = false;
    transport.probe_responses.push_back(pr);

    for (int i = 0; i < 3; i++)
        transport.download_results.push_back(false);

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_FALSE(dl.run());
    EXPECT_EQ(transport.download_count, 3);
}

TEST_F(DownloaderTest, DownloadRetryThenSuccess) {
    auto cfg = make_config();

    ProbeResult pr;
    pr.ok = true;
    pr.status = 200;
    pr.file_size = 100;
    pr.range_supported = false;
    transport.probe_responses.push_back(pr);

    transport.download_results.push_back(false);
    transport.download_results.push_back(false);
    transport.download_results.push_back(true);
    transport.download_data = {0x41, 0x42};

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_TRUE(dl.run());
    EXPECT_EQ(transport.download_count, 3);
}

TEST_F(DownloaderTest, RangeSupported_MultiThread) {
    auto cfg = make_config();
    cfg.num_threads = 4;

    ProbeResult pr;
    pr.ok = true;
    pr.status = 206;
    pr.file_size = 1000;
    pr.range_supported = true;
    transport.probe_responses.push_back(pr);

    for (int i = 0; i < 4; i++)
        transport.download_results.push_back(true);

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_TRUE(dl.run());
    EXPECT_EQ(transport.download_count, 4);
}

TEST_F(DownloaderTest, RangeSupported_AllChunksFail) {
    auto cfg = make_config();
    cfg.num_threads = 2;

    ProbeResult pr;
    pr.ok = true;
    pr.status = 206;
    pr.file_size = 100;
    pr.range_supported = true;
    transport.probe_responses.push_back(pr);

    for (int i = 0; i < 6; i++)
        transport.download_results.push_back(false);

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_FALSE(dl.run());
    EXPECT_EQ(transport.download_count, 6);
}

TEST_F(DownloaderTest, ZeroFileSize_SingleThreadFallback) {
    auto cfg = make_config();
    cfg.num_threads = 4;

    ProbeResult pr;
    pr.ok = true;
    pr.status = 200;
    pr.file_size = 0;
    pr.range_supported = false;
    transport.probe_responses.push_back(pr);

    transport.download_results.push_back(true);

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_TRUE(dl.run());
    EXPECT_EQ(transport.download_count, 1);
}

TEST_F(DownloaderTest, AlreadyDownloaded) {
    auto cfg = make_config();
    cfg.num_threads = 1;

    std::ofstream out(tmp_path);
    out << "already done";
    out.close();

    std::ofstream meta(tmp_path + ".mdow");
    meta << cfg.url << "\n100\n" << tmp_path << "\n";
    meta.close();

    ProbeResult pr;
    pr.ok = true;
    pr.status = 200;
    pr.file_size = 11;
    pr.range_supported = false;
    transport.probe_responses.push_back(pr);

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_TRUE(dl.run());
    EXPECT_EQ(transport.download_count, 0);
}

TEST_F(DownloaderTest, ExceptionSafety_TransportThrows) {
    struct ThrowingTransport : ITransport {
        ProbeResult head(const std::string&) override {
            throw std::runtime_error("network boom");
        }
        bool download(const std::string&, std::optional<std::pair<uint64_t, uint64_t>>,
            void*, size_t (*)(char*, size_t, size_t, void*),
            void*, int (*)(void*, long long, long long, long long, long long)) override {
            return false;
        }
    } thrower;

    auto cfg = make_config();
    cfg.num_threads = 1;

    Downloader dl(cfg, pm, pool, thrower);
    EXPECT_NO_THROW({
        EXPECT_FALSE(dl.run());
    });
}

TEST_F(DownloaderTest, MultipleFiles_SameTransport) {
    MockTransport shared_transport;

    ProbeResult pr;
    pr.ok = true;
    pr.status = 200;
    pr.file_size = 0;
    pr.range_supported = false;
    shared_transport.probe_responses.push_back(pr);
    shared_transport.probe_responses.push_back(pr);

    shared_transport.download_results.push_back(true);
    shared_transport.download_results.push_back(true);

    char buf2[] = "/tmp/multidow_dl_test2_XXXXXX";
    int fd2 = mkstemp(buf2);
    close(fd2);
    std::string tmp_path2 = buf2;

    auto cfg1 = make_config();
    cfg1.output_path = tmp_path;
    auto cfg2 = make_config();
    cfg2.output_path = tmp_path2;

    Downloader dl1(cfg1, pm, pool, shared_transport);
    Downloader dl2(cfg2, pm, pool, shared_transport);

    EXPECT_TRUE(dl1.run());
    EXPECT_TRUE(dl2.run());
    EXPECT_EQ(shared_transport.probe_count, 2);
    EXPECT_EQ(shared_transport.download_count, 2);

    std::remove(tmp_path2.c_str());
    std::remove((tmp_path2 + ".mdow").c_str());
}
