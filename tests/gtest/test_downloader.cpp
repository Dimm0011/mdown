#include <gtest/gtest.h>
#include "downloader.h"
#include "mock_transport.h"
#include "progress.h"
#include "thread_pool.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

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
#ifdef _WIN32
        char buf[] = "C:\\Temp\\multidow_dl_test_XXXXXX";
        _mktemp_s(buf, sizeof(buf));
        int fd = -1;
        _open_s(&fd, buf, _O_CREAT | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (fd >= 0) _close(fd);
#else
        char buf[] = "/tmp/multidow_dl_test_XXXXXX";
        int fd = mkstemp(buf);
        close(fd);
#endif
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
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "ABC");
}

TEST_F(DownloaderTest, ProbeFailure_FallbackToSingleThread) {
    auto cfg = make_config();

    for (int i = 0; i < 3; i++) {
        ProbeResult pr;
        pr.ok = false;
        transport.probe_responses.push_back(pr);
    }

    for (int i = 0; i < 3; i++) transport.download_results.push_back(false);

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
#ifdef _WIN32
    char buf2[] = "C:\\Temp\\multidow_dl_test_notexplicit_XXXXXX";
    _mktemp_s(buf2, sizeof(buf2));
    int fd2 = -1;
    _open_s(&fd2, buf2, _O_CREAT | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd2 >= 0) _close(fd2);
#else
    char buf2[] = "/tmp/multidow_dl_test_notexplicit_XXXXXX";
    int fd2 = mkstemp(buf2);
    close(fd2);
#endif
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
#ifdef _WIN32
    char buf2[] = "C:\\Temp\\multidow_dl_test_traversal_XXXXXX";
    _mktemp_s(buf2, sizeof(buf2));
    int fd2 = -1;
    _open_s(&fd2, buf2, _O_CREAT | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd2 >= 0) _close(fd2);
#else
    char buf2[] = "/tmp/multidow_dl_test_traversal_XXXXXX";
    int fd2 = mkstemp(buf2);
    close(fd2);
#endif
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

    for (int i = 0; i < 3; i++) transport.download_results.push_back(false);

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

    for (int i = 0; i < 4; i++) transport.download_results.push_back(true);

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

    for (int i = 0; i < 9; i++) transport.download_results.push_back(false);

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_FALSE(dl.run());
    EXPECT_EQ(transport.download_count, 9);
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
        DownloadResult download(const std::string&, std::optional<std::pair<uint64_t, uint64_t>>,
                                void*, size_t (*)(char*, size_t, size_t, void*), void*,
                                int (*)(void*, long long, long long, long long,
                                        long long)) override {
            return {false, "network boom"};
        }
    } thrower;

    auto cfg = make_config();
    cfg.num_threads = 1;

    Downloader dl(cfg, pm, pool, thrower);
    EXPECT_NO_THROW({ EXPECT_FALSE(dl.run()); });
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

#ifdef _WIN32
    char buf2[] = "C:\\Temp\\multidow_dl_test2_XXXXXX";
    _mktemp_s(buf2, sizeof(buf2));
    int fd2 = -1;
    _open_s(&fd2, buf2, _O_CREAT | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd2 >= 0) _close(fd2);
#else
    char buf2[] = "/tmp/multidow_dl_test2_XXXXXX";
    int fd2 = mkstemp(buf2);
    close(fd2);
#endif
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

TEST_F(DownloaderTest, MetadataSavedOnFailure) {
    auto cfg = make_config();

    ProbeResult pr;
    pr.ok = true;
    pr.status = 200;
    pr.file_size = 100;
    pr.range_supported = false;
    transport.probe_responses.push_back(pr);

    for (int i = 0; i < 3; i++) transport.download_results.push_back(false);

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_FALSE(dl.run());

    EXPECT_TRUE(fs::exists(tmp_path + ".mdow"));

    std::ifstream meta(tmp_path + ".mdow");
    std::string url, size_str, path;
    std::getline(meta, url);
    std::getline(meta, size_str);
    std::getline(meta, path);
    EXPECT_EQ(url, "http://example.com/file.bin");
    EXPECT_EQ(path, tmp_path);

    std::remove((tmp_path + ".mdow").c_str());
}

TEST_F(DownloaderTest, MetadataRemovedOnSuccess) {
    auto cfg = make_config();

    ProbeResult pr;
    pr.ok = true;
    pr.status = 200;
    pr.file_size = 0;
    pr.range_supported = false;
    transport.probe_responses.push_back(pr);

    transport.download_results.push_back(true);
    transport.download_data = {0x41, 0x42};

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_TRUE(dl.run());
    EXPECT_FALSE(fs::exists(tmp_path + ".mdow"));
}

TEST_F(DownloaderTest, FileDoneCalledOnFailure) {
    auto cfg = make_config();

    ProbeResult pr;
    pr.ok = true;
    pr.status = 200;
    pr.file_size = 100;
    pr.range_supported = false;
    transport.probe_responses.push_back(pr);

    for (int i = 0; i < 3; i++) transport.download_results.push_back(false);

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_FALSE(dl.run());

    EXPECT_TRUE(pm.all_done());
    std::remove((tmp_path + ".mdow").c_str());
}

TEST_F(DownloaderTest, ChecksumVerificationPass) {
    auto cfg = make_config();
    cfg.verify = true;
    cfg.expected_checksum = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";

    ProbeResult pr;
    pr.ok = true;
    pr.status = 200;
    pr.file_size = 0;
    pr.range_supported = false;
    transport.probe_responses.push_back(pr);

    transport.download_results.push_back(true);
    transport.download_data = {'h', 'e', 'l', 'l', 'o'};

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_TRUE(dl.run());
    EXPECT_TRUE(pm.all_done());
}

TEST_F(DownloaderTest, ChecksumVerificationFail) {
    auto cfg = make_config();
    cfg.verify = true;
    cfg.expected_checksum = "0000000000000000000000000000000000000000000000000000000000000000";

    ProbeResult pr;
    pr.ok = true;
    pr.status = 200;
    pr.file_size = 0;
    pr.range_supported = false;
    transport.probe_responses.push_back(pr);

    transport.download_results.push_back(true);
    transport.download_data = {'h', 'e', 'l', 'l', 'o'};

    Downloader dl(cfg, pm, pool, transport);
    EXPECT_TRUE(dl.run());

    EXPECT_TRUE(pm.all_done());
    std::remove(tmp_path.c_str());
}

struct RangeAwareTransport : ITransport {
    struct ChunkBehavior {
        uint64_t range_end;
        std::vector<std::vector<uint8_t>> attempt_data;
        std::vector<bool> attempt_ok;
        std::vector<long> attempt_http;
    };

    ProbeResult probe_resp;
    std::vector<ChunkBehavior> behaviors;

    mutable std::mutex mtx;
    mutable std::map<uint64_t, int> call_count;
    mutable std::atomic<int> probe_count{0};

    ProbeResult head(const std::string&) override {
        probe_count.fetch_add(1);
        return probe_resp;
    }

    DownloadResult download(const std::string&, std::optional<std::pair<uint64_t, uint64_t>> range,
                            void* write_userp, size_t (*write_cb)(char*, size_t, size_t, void*),
                            void* progress_userp,
                            int (*progress_cb)(void*, long long, long long, long long,
                                               long long)) override {
        uint64_t end = range ? range->second : 0;

        std::lock_guard lock(mtx);
        int attempt = call_count[end]++;
        call_count[end] = attempt + 1;

        DownloadResult result;
        result.ok = false;
        result.http_code = 404;

        for (auto& b : behaviors) {
            if (b.range_end == end && attempt < (int)b.attempt_data.size()) {
                auto& data = b.attempt_data[attempt];
                if (!data.empty() && write_cb) {
                    write_cb(reinterpret_cast<char*>(data.data()), 1, data.size(), write_userp);
                }
                result.ok = (attempt < (int)b.attempt_ok.size()) ? b.attempt_ok[attempt] : true;
                result.http_code =
                    (attempt < (int)b.attempt_http.size()) ? b.attempt_http[attempt] : 206;
                break;
            }
        }

        if (progress_cb) {
            progress_cb(progress_userp, 0, 0, 0, 0);
        }
        return result;
    }
};

TEST_F(DownloaderTest, ChunkRetry_PartialWrite_ResumesCorrectly) {
    RangeAwareTransport t;
    t.probe_resp = {true, 206, 100, true, ""};

    std::vector<uint8_t> first30(30, 0x41);
    std::vector<uint8_t> remaining20(20, 0x42);
    std::vector<uint8_t> chunk1_full(50, 0x43);

    t.behaviors.push_back({49, {first30, remaining20}, {false, true}, {206, 206}});
    t.behaviors.push_back({99, {chunk1_full}, {true}, {206}});

    auto cfg = make_config();
    cfg.num_threads = 2;

    Downloader dl(cfg, pm, pool, t);
    EXPECT_TRUE(dl.run());

    std::ifstream f(tmp_path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ASSERT_EQ(content.size(), 100u);

    for (int i = 0; i < 30; i++) EXPECT_EQ((uint8_t)content[i], 0x41) << "offset " << i;
    for (int i = 30; i < 50; i++) EXPECT_EQ((uint8_t)content[i], 0x42) << "offset " << i;
    for (int i = 50; i < 100; i++) EXPECT_EQ((uint8_t)content[i], 0x43) << "offset " << i;
}

TEST_F(DownloaderTest, MultiThreadedResume_CompletedChunksSkipDownload) {
    uint64_t file_size = 100;

    {
        std::ofstream meta(tmp_path + ".mdow");
        meta << "http://example.com/file.bin\n";
        meta << file_size << "\n";
        meta << tmp_path << "\n";
        meta << "MT\n";
        meta << "4\n";
        meta << "0 25\n";
        meta << "25 25\n";
        meta << "50 10\n";
        meta << "75 0\n";
    }

    {
        std::ofstream f(tmp_path, std::ios::binary);
        for (uint64_t i = 0; i < 35; i++) f.put(static_cast<char>(0x55));
    }

    RangeAwareTransport t;
    t.probe_resp = {true, 206, file_size, true, ""};

    std::vector<uint8_t> ch2_remain(15, 0xAA);
    std::vector<uint8_t> ch3_full(25, 0xBB);

    t.behaviors.push_back({74, {ch2_remain}, {true}, {206}});
    t.behaviors.push_back({99, {ch3_full}, {true}, {206}});

    DownloadConfig cfg;
    cfg.url = "http://example.com/file.bin";
    cfg.output_path = tmp_path;
    cfg.output_explicit = true;
    cfg.num_threads = 4;
    cfg.max_retries = 0;

    Downloader dl(cfg, pm, pool, t);
    EXPECT_TRUE(dl.run());

    std::ifstream f(tmp_path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ASSERT_EQ(content.size(), 100u);

    for (uint64_t i = 0; i < 35; i++)
        EXPECT_EQ((uint8_t)content[i], 0x55) << "prefilled offset " << i;
    for (uint64_t i = 60; i < 75; i++)
        EXPECT_EQ((uint8_t)content[i], 0xAA) << "chunk2 resume offset " << i;
    for (uint64_t i = 75; i < 100; i++)
        EXPECT_EQ((uint8_t)content[i], 0xBB) << "chunk3 offset " << i;
}
