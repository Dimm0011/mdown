#include "format.h"
#include "checksum.h"
#include "progress.h"

#include <cassert>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { tests_run++; std::cout << "  " << #name << "... "; } while(0)

#define PASS() \
    do { tests_passed++; std::cout << "ok" << std::endl; } while(0)

#define ASSERT_EQ(a, b) \
    do { if ((a) != (b)) { std::cout << "FAIL (" << (a) << " != " << (b) << ")" << std::endl; return; } } while(0)

#define ASSERT_TRUE(x) \
    do { if (!(x)) { std::cout << "FAIL" << std::endl; return; } } while(0)

static std::string temp_file(const std::string& content) {
    char path[] = "/tmp/multidow_test_XXXXXX";
    int fd = mkstemp(path);
    write(fd, content.c_str(), content.size());
    close(fd);
    return path;
}

// --- format.h tests ---

static void test_format_bytes_zero() {
    TEST(format_bytes(0));
    ASSERT_EQ(multidow::format_bytes(0), "0.0 B");
    PASS();
}

static void test_format_bytes_bytes() {
    TEST(format_bytes(512));
    ASSERT_EQ(multidow::format_bytes(512), "512.0 B");
    PASS();
}

static void test_format_bytes_kb() {
    TEST(format_bytes(1024));
    ASSERT_EQ(multidow::format_bytes(1024), "1.0 KB");
    PASS();
}

static void test_format_bytes_mb() {
    TEST(format_bytes(1048576));
    ASSERT_EQ(multidow::format_bytes(1048576), "1.0 MB");
    PASS();
}

static void test_format_bytes_gb() {
    TEST(format_bytes(1073741824));
    ASSERT_EQ(multidow::format_bytes(1073741824), "1.0 GB");
    PASS();
}

static void test_format_bytes_fractional() {
    TEST(format_bytes(1536));
    ASSERT_EQ(multidow::format_bytes(1536), "1.5 KB");
    PASS();
}

static void test_format_speed() {
    TEST(format_speed);
    ASSERT_EQ(multidow::format_speed(1048576), "1.0 MB/s");
    PASS();
}

static void test_make_bar_empty() {
    TEST(make_bar 0%);
    ASSERT_EQ(multidow::make_bar(10, 0.0), "[..........]");
    PASS();
}

static void test_make_bar_full() {
    TEST(make_bar 100%);
    ASSERT_EQ(multidow::make_bar(10, 1.0), "[##########]");
    PASS();
}

static void test_make_bar_half() {
    TEST(make_bar 50%);
    ASSERT_EQ(multidow::make_bar(10, 0.5), "[#####.....]");
    PASS();
}

static void test_make_bar_clamp() {
    TEST(make_bar overflow);
    ASSERT_EQ(multidow::make_bar(5, 2.0), "[#####]");
    PASS();
}

// --- checksum tests ---

static void test_sha256_empty() {
    TEST(sha256 empty file);
    auto path = temp_file("");
    std::string hash = multidow::sha256_file(path);
    ASSERT_EQ(hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::remove(path.c_str());
    PASS();
}

static void test_sha256_hello() {
    TEST(sha256 "hello");
    auto path = temp_file("hello");
    std::string hash = multidow::sha256_file(path);
    ASSERT_EQ(hash, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    std::remove(path.c_str());
    PASS();
}

static void test_verify_checksum_ok() {
    TEST(verify_checksum match);
    auto path = temp_file("hello");
    ASSERT_TRUE(multidow::verify_checksum(path, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"));
    std::remove(path.c_str());
    PASS();
}

static void test_verify_checksum_fail() {
    TEST(verify_checksum mismatch);
    auto path = temp_file("hello");
    ASSERT_TRUE(!multidow::verify_checksum(path, "0000000000000000000000000000000000000000000000000000000000000000"));
    std::remove(path.c_str());
    PASS();
}

// --- ProgressManager tests ---

static void test_pm_add_file() {
    TEST(pm add_file);
    multidow::ProgressManager pm;
    int id = pm.add_file("test.zip", 1024, 4);
    ASSERT_EQ(id, 0);
    ASSERT_TRUE(!pm.all_done());
    PASS();
}

static void test_pm_single_file_done() {
    TEST(pm single file lifecycle);
    multidow::ProgressManager pm;
    int id = pm.add_file("test.zip", 1024, 1);
    pm.mark_thread_active(id, 0);
    pm.update_thread(id, 0, 1024, 1024);
    pm.mark_thread_finished(id, 0);
    pm.set_file_done(id, true, "Done");
    ASSERT_TRUE(pm.all_done());
    PASS();
}

static void test_pm_multi_file() {
    TEST(pm multi file lifecycle);
    multidow::ProgressManager pm;
    int f0 = pm.add_file("a.zip", 1000, 2);
    int f1 = pm.add_file("b.zip", 2000, 2);

    pm.mark_thread_active(f0, 0);
    pm.mark_thread_active(f0, 1);
    pm.mark_thread_finished(f0, 0);
    pm.mark_thread_finished(f0, 1);
    pm.set_file_done(f0, true, "Done");
    ASSERT_TRUE(!pm.all_done());

    pm.mark_thread_active(f1, 0);
    pm.mark_thread_active(f1, 1);
    pm.mark_thread_finished(f1, 0);
    pm.mark_thread_finished(f1, 1);
    pm.set_file_done(f1, true, "Done");
    ASSERT_TRUE(pm.all_done());
    PASS();
}

static void test_pm_error_state() {
    TEST(pm error state);
    multidow::ProgressManager pm;
    int id = pm.add_file("test.zip", 1000, 1);
    pm.mark_thread_active(id, 0);
    pm.mark_thread_error(id, 0);
    pm.set_file_done(id, false, "Failed");
    ASSERT_TRUE(pm.all_done());
    PASS();
}

static void test_pm_invalid_ids() {
    TEST(pm invalid file/thread ids);
    multidow::ProgressManager pm;
    pm.add_file("test.zip", 1000, 2);
    pm.update_thread(99, 0, 100, 100);
    pm.mark_thread_active(0, 99);
    pm.mark_thread_finished(-1, 0);
    ASSERT_TRUE(!pm.all_done());
    PASS();
}

int main() {
    std::cout << "=== format ===" << std::endl;
    test_format_bytes_zero();
    test_format_bytes_bytes();
    test_format_bytes_kb();
    test_format_bytes_mb();
    test_format_bytes_gb();
    test_format_bytes_fractional();
    test_format_speed();
    test_make_bar_empty();
    test_make_bar_full();
    test_make_bar_half();
    test_make_bar_clamp();

    std::cout << "=== checksum ===" << std::endl;
    test_sha256_empty();
    test_sha256_hello();
    test_verify_checksum_ok();
    test_verify_checksum_fail();

    std::cout << "=== progress ===" << std::endl;
    test_pm_add_file();
    test_pm_single_file_done();
    test_pm_multi_file();
    test_pm_error_state();
    test_pm_invalid_ids();

    std::cout << "\n" << tests_passed << "/" << tests_run << " tests passed" << std::endl;
    return (tests_passed == tests_run) ? 0 : 1;
}
