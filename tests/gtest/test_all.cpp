#include <gtest/gtest.h>
#include "checksum.h"
#include "format.h"
#include "progress.h"
#include "thread_pool.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif
#include <cstdio>
#include <fstream>

using namespace multidow;

TEST(FormatBytes, Bytes) {
    EXPECT_EQ(format_bytes(512), "512.0 B");
}

TEST(FormatBytes, OneKB) {
    EXPECT_EQ(format_bytes(1024), "1.0 KB");
}

TEST(FormatBytes, OneMB) {
    EXPECT_EQ(format_bytes(1048576), "1.0 MB");
}

TEST(FormatBytes, OneGB) {
    EXPECT_EQ(format_bytes(1073741824), "1.000 GB");
}

TEST(FormatBytes, FractionalKB) {
    EXPECT_EQ(format_bytes(1536), "1.5 KB");
}

TEST(FormatBytes, FractionalMB) {
    EXPECT_EQ(format_bytes(2621440), "2.5 MB");
}

TEST(FormatBytes, LargeGB) {
    EXPECT_EQ(format_bytes(5368709120ULL), "5.000 GB");
}

TEST(FormatSpeed, Basic) {
    EXPECT_EQ(format_speed(1048576), "1.0 MB/s");
}

TEST(FormatSpeed, KB) {
    EXPECT_EQ(format_speed(2048), "2.0 KB/s");
}

TEST(FormatSpeed, Zero) {
    EXPECT_EQ(format_speed(0), "0.0 B/s");
}

TEST(MakeBar, Empty) {
    EXPECT_EQ(make_bar(10, 0.0), "[..........]");
}

TEST(MakeBar, Full) {
    EXPECT_EQ(make_bar(10, 1.0), "[##########]");
}

TEST(MakeBar, Half) {
    EXPECT_EQ(make_bar(10, 0.5), "[#####.....]");
}

TEST(MakeBar, Quarter) {
    EXPECT_EQ(make_bar(8, 0.25), "[##......]");
}

TEST(MakeBar, ClampOverflow) {
    EXPECT_EQ(make_bar(5, 2.0), "[#####]");
}

TEST(MakeBar, ClampNegative) {
    EXPECT_EQ(make_bar(5, -1.0), "[.....]");
}

class ChecksumTest : public ::testing::Test {
   protected:
    std::string path_;

    void SetUp(const std::string& content) {
#ifdef _WIN32
        char buf[] = "C:\\Temp\\multidow_test_XXXXXX";
        if (_mktemp_s(buf, sizeof(buf)) != 0) return;
        int fd = -1;
        _open_s(&fd, buf, _O_CREAT | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
        ASSERT_NE(fd, -1);
        if (!content.empty()) {
            auto rc = _write(fd, content.c_str(), static_cast<unsigned>(content.size()));
            (void)rc;
        }
        _close(fd);
#else
        char buf[] = "/tmp/multidow_test_XXXXXX";
        int fd = mkstemp(buf);
        ASSERT_NE(fd, -1);
        if (!content.empty()) {
            auto rc = write(fd, content.c_str(), content.size());
            (void)rc;
        }
        close(fd);
#endif
        path_ = buf;
    }

    void TearDown() override {
        if (!path_.empty()) std::remove(path_.c_str());
    }
};

TEST_F(ChecksumTest, SHA256_EmptyFile) {
    SetUp("");
    EXPECT_EQ(sha256_file(path_),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_F(ChecksumTest, SHA256_Hello) {
    SetUp("hello");
    EXPECT_EQ(sha256_file(path_),
              "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

TEST_F(ChecksumTest, SHA256_LargeContent) {
    std::string data(100000, 'A');
    SetUp(data);
    EXPECT_EQ(sha256_file(path_).size(), 64u);
}

TEST_F(ChecksumTest, Verify_Match) {
    SetUp("hello");
    EXPECT_TRUE(
        verify_checksum(path_, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"));
}

TEST_F(ChecksumTest, Verify_Mismatch) {
    SetUp("hello");
    EXPECT_FALSE(
        verify_checksum(path_, "0000000000000000000000000000000000000000000000000000000000000000"));
}

TEST_F(ChecksumTest, NonExistentFile) {
    EXPECT_EQ(sha256_file("/tmp/no_such_file_12345"), "");
}

TEST_F(ChecksumTest, Verify_NonExistent) {
    EXPECT_FALSE(verify_checksum("/tmp/no_such_file_12345", "abc"));
}

class ProgressTest : public ::testing::Test {
   protected:
    ProgressManager pm;
};

TEST_F(ProgressTest, AddFileReturnsZeroId) {
    EXPECT_EQ(pm.add_file("a.zip", 1024, 4), 0);
    EXPECT_FALSE(pm.all_done());
}

TEST_F(ProgressTest, MultipleFilesGetIncrementingIds) {
    EXPECT_EQ(pm.add_file("a.zip", 1000, 2), 0);
    EXPECT_EQ(pm.add_file("b.zip", 2000, 2), 1);
    EXPECT_EQ(pm.add_file("c.zip", 3000, 2), 2);
}

TEST_F(ProgressTest, SingleThreadLifecycle) {
    int id = pm.add_file("test.zip", 1024, 1);
    pm.mark_thread_active(id, 0);
    pm.update_thread(id, 0, 512, 1024);
    pm.mark_thread_finished(id, 0);
    pm.set_file_done(id, true, "Done");
    EXPECT_TRUE(pm.all_done());
}

TEST_F(ProgressTest, MultiThreadLifecycle) {
    int id = pm.add_file("test.zip", 4096, 4);
    for (int i = 0; i < 4; i++) {
        pm.mark_thread_active(id, i);
        pm.update_thread(id, i, 1024, 1024);
        pm.mark_thread_finished(id, i);
    }
    pm.set_file_done(id, true, "Done");
    EXPECT_TRUE(pm.all_done());
}

TEST_F(ProgressTest, MultiFileSequentialCompletion) {
    int f0 = pm.add_file("a.zip", 1000, 2);
    int f1 = pm.add_file("b.zip", 2000, 2);

    pm.mark_thread_active(f0, 0);
    pm.mark_thread_finished(f0, 0);
    pm.mark_thread_active(f0, 1);
    pm.mark_thread_finished(f0, 1);
    pm.set_file_done(f0, true, "Done");
    EXPECT_FALSE(pm.all_done());

    pm.mark_thread_active(f1, 0);
    pm.mark_thread_finished(f1, 0);
    pm.mark_thread_active(f1, 1);
    pm.mark_thread_finished(f1, 1);
    pm.set_file_done(f1, true, "Done");
    EXPECT_TRUE(pm.all_done());
}

TEST_F(ProgressTest, MultiFileParallelCompletion) {
    int f0 = pm.add_file("a.zip", 1000, 1);
    int f1 = pm.add_file("b.zip", 2000, 1);

    pm.mark_thread_active(f0, 0);
    pm.mark_thread_active(f1, 0);
    pm.mark_thread_finished(f0, 0);
    pm.mark_thread_finished(f1, 0);
    pm.set_file_done(f0, true, "Done");
    pm.set_file_done(f1, true, "Done");
    EXPECT_TRUE(pm.all_done());
}

TEST_F(ProgressTest, ErrorState) {
    int id = pm.add_file("test.zip", 1000, 2);
    pm.mark_thread_active(id, 0);
    pm.mark_thread_error(id, 0);
    pm.mark_thread_active(id, 1);
    pm.mark_thread_finished(id, 1);
    pm.set_file_done(id, false, "Partial");
    EXPECT_TRUE(pm.all_done());
}

TEST_F(ProgressTest, UpdateProgress) {
    int id = pm.add_file("test.zip", 10000, 1);
    pm.mark_thread_active(id, 0);
    pm.update_thread(id, 0, 0, 10000);
    pm.update_thread(id, 0, 5000, 10000);
    pm.update_thread(id, 0, 10000, 10000);
    pm.mark_thread_finished(id, 0);
    pm.set_file_done(id, true, "Done");
    EXPECT_TRUE(pm.all_done());
}

TEST_F(ProgressTest, RedrawMultipleFiles) {
    for (int i = 0; i < 5; i++) {
        int id = pm.add_file("file_" + std::to_string(i) + ".zip", 1000 * (i + 1), 3);
        for (int t = 0; t < 3; t++) {
            pm.mark_thread_active(id, t);
            pm.update_thread(id, t, 500 * (i + 1), 1000 * (i + 1));
        }
    }
    pm.redraw();
    for (int i = 0; i < 5; i++) {
        for (int t = 0; t < 3; t++) pm.mark_thread_finished(i, t);
        pm.set_file_done(i, true, "Done");
    }
    pm.redraw();
    EXPECT_TRUE(pm.all_done());
}

TEST(ThreadPool, BasicSubmitAndResult) {
    ThreadPool pool(2);
    auto f = pool.submit([]() { return 42; });
    EXPECT_EQ(f.get(), 42);
}

TEST(ThreadPool, MultipleTasks) {
    ThreadPool pool(4);
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 10; i++) {
        futures.push_back(pool.submit([i]() { return i * i; }));
    }
    for (int i = 0; i < 10; i++) {
        EXPECT_EQ(futures[i].get(), i * i);
    }
}

TEST(ThreadPool, SubmitAfterShutdownThrows) {
    ThreadPool pool(1);
    pool.shutdown();
    EXPECT_THROW(pool.submit([]() { return 1; }), std::runtime_error);
}

TEST(ThreadPool, ShutdownJoinsWorkers) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    for (int i = 0; i < 8; i++) {
        pool.submit([&counter]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            counter++;
        });
    }
    pool.shutdown();
    EXPECT_EQ(counter.load(), 8);
}

TEST(ThreadPool, SizeReturnsWorkerCount) {
    ThreadPool pool(3);
    EXPECT_EQ(pool.size(), 3u);
}

TEST_F(ProgressTest, SetFileStatus) {
    int id = pm.add_file("test.zip", 1000, 1);
    pm.set_file_status(id, "custom message");
    pm.redraw();
    EXPECT_FALSE(pm.all_done());
}

TEST_F(ProgressTest, RenameFile) {
    int id = pm.add_file("old_name.zip", 1000, 1);
    pm.rename_file(id, "new_name.zip");
    pm.redraw();
    EXPECT_FALSE(pm.all_done());
}

TEST_F(ProgressTest, AnyStalled_NoFiles) {
    EXPECT_FALSE(pm.any_stalled(std::chrono::seconds(1)));
}

TEST_F(ProgressTest, AnyStalled_AllDone) {
    int id = pm.add_file("test.zip", 1000, 1);
    pm.set_file_done(id, true, "Done");
    EXPECT_FALSE(pm.any_stalled(std::chrono::seconds(1)));
}

TEST_F(ProgressTest, AnyStalled_RecentlyActive) {
    int id = pm.add_file("test.zip", 1000, 1);
    pm.mark_thread_active(id, 0);
    pm.update_thread(id, 0, 100, 1000);
    EXPECT_FALSE(pm.any_stalled(std::chrono::seconds(10)));
}

TEST_F(ProgressTest, AnyStalled_Stalled) {
    int id = pm.add_file("test.zip", 1000, 1);
    pm.mark_thread_active(id, 0);
    pm.update_thread(id, 0, 100, 1000);
    EXPECT_TRUE(pm.any_stalled(std::chrono::seconds(0)));
}

TEST_F(ProgressTest, ConcurrentUpdateThread) {
    int id = pm.add_file("test.zip", 10000, 4);
    std::vector<multidow::jthread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([this, id, t](multidow::stop_token) {
            for (int i = 0; i < 100; i++) {
                pm.update_thread(id, t, i * 25, 2500);
            }
        });
    }
    for (auto& t : threads) t.join();
    pm.redraw();
}
