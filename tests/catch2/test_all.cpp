#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

#include "format.h"
#include "checksum.h"
#include "progress.h"

#include <cstdio>
#include <fstream>
#include <unistd.h>

using namespace multidow;

// ==================== format.h ====================

TEST_CASE("format_bytes zero", "[format]") {
    REQUIRE(format_bytes(0) == "0.0 B");
}

TEST_CASE("format_bytes bytes", "[format]") {
    REQUIRE(format_bytes(512) == "512.0 B");
}

TEST_CASE("format_bytes KB", "[format]") {
    REQUIRE(format_bytes(1024) == "1.0 KB");
}

TEST_CASE("format_bytes MB", "[format]") {
    REQUIRE(format_bytes(1048576) == "1.0 MB");
}

TEST_CASE("format_bytes GB", "[format]") {
    REQUIRE(format_bytes(1073741824) == "1.0 GB");
}

TEST_CASE("format_bytes fractional", "[format]") {
    REQUIRE(format_bytes(1536) == "1.5 KB");
    REQUIRE(format_bytes(2621440) == "2.5 MB");
}

TEST_CASE("format_bytes large GB", "[format]") {
    REQUIRE(format_bytes(5368709120ULL) == "5.0 GB");
}

TEST_CASE("format_speed", "[format]") {
    REQUIRE(format_speed(1048576) == "1.0 MB/s");
    REQUIRE(format_speed(2048) == "2.0 KB/s");
    REQUIRE(format_speed(0) == "0.0 B/s");
}

// ==================== make_bar ====================

TEST_CASE("make_bar empty", "[bar]") {
    REQUIRE(make_bar(10, 0.0) == "[..........]");
}

TEST_CASE("make_bar full", "[bar]") {
    REQUIRE(make_bar(10, 1.0) == "[##########]");
}

TEST_CASE("make_bar half", "[bar]") {
    REQUIRE(make_bar(10, 0.5) == "[#####.....]");
}

TEST_CASE("make_bar quarter", "[bar]") {
    REQUIRE(make_bar(8, 0.25) == "[##......]");
}

TEST_CASE("make_bar clamp overflow", "[bar]") {
    REQUIRE(make_bar(5, 2.0) == "[#####]");
}

TEST_CASE("make_bar clamp negative", "[bar]") {
    REQUIRE(make_bar(5, -1.0) == "[.....]");
}

TEST_CASE("make_bar width 1", "[bar]") {
    REQUIRE(make_bar(1, 1.0) == "[#]");
    REQUIRE(make_bar(1, 0.0) == "[.]");
}

// ==================== checksum ====================

static std::string make_temp(const std::string& content) {
    char path[] = "/tmp/multidow_catch2_XXXXXX";
    int fd = mkstemp(path);
    if (!content.empty())
        write(fd, content.c_str(), content.size());
    close(fd);
    return path;
}

TEST_CASE("sha256 empty file", "[checksum]") {
    auto p = make_temp("");
    REQUIRE(sha256_file(p) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::remove(p.c_str());
}

TEST_CASE("sha256 hello", "[checksum]") {
    auto p = make_temp("hello");
    REQUIRE(sha256_file(p) == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    std::remove(p.c_str());
}

TEST_CASE("sha256 large content", "[checksum]") {
    auto p = make_temp(std::string(100000, 'A'));
    REQUIRE(sha256_file(p).size() == 64);
    std::remove(p.c_str());
}

TEST_CASE("verify match", "[checksum]") {
    auto p = make_temp("hello");
    REQUIRE(verify_checksum(p, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"));
    std::remove(p.c_str());
}

TEST_CASE("verify mismatch", "[checksum]") {
    auto p = make_temp("hello");
    REQUIRE_FALSE(verify_checksum(p, "0000000000000000000000000000000000000000000000000000000000000000"));
    std::remove(p.c_str());
}

TEST_CASE("sha256 nonexistent", "[checksum]") {
    REQUIRE(sha256_file("/tmp/no_such_file_12345").empty());
}

TEST_CASE("verify nonexistent", "[checksum]") {
    REQUIRE_FALSE(verify_checksum("/tmp/no_such_file_12345", "abc"));
}

// ==================== ProgressManager ====================

TEST_CASE("add file returns id", "[progress]") {
    ProgressManager pm;
    REQUIRE(pm.add_file("a.zip", 1024, 4) == 0);
    REQUIRE_FALSE(pm.all_done());
}

TEST_CASE("incrementing ids", "[progress]") {
    ProgressManager pm;
    REQUIRE(pm.add_file("a.zip", 1000, 2) == 0);
    REQUIRE(pm.add_file("b.zip", 2000, 2) == 1);
    REQUIRE(pm.add_file("c.zip", 3000, 2) == 2);
}

TEST_CASE("single thread lifecycle", "[progress]") {
    ProgressManager pm;
    int id = pm.add_file("test.zip", 1024, 1);
    pm.mark_thread_active(id, 0);
    pm.update_thread(id, 0, 512, 1024);
    pm.mark_thread_finished(id, 0);
    pm.set_file_done(id, true, "Done");
    REQUIRE(pm.all_done());
}

TEST_CASE("multi thread lifecycle", "[progress]") {
    ProgressManager pm;
    int id = pm.add_file("test.zip", 4096, 4);
    for (int i = 0; i < 4; i++) {
        pm.mark_thread_active(id, i);
        pm.update_thread(id, i, 1024, 1024);
        pm.mark_thread_finished(id, i);
    }
    pm.set_file_done(id, true, "Done");
    REQUIRE(pm.all_done());
}

TEST_CASE("multi file sequential", "[progress]") {
    ProgressManager pm;
    int f0 = pm.add_file("a.zip", 1000, 2);
    int f1 = pm.add_file("b.zip", 2000, 2);

    pm.mark_thread_active(f0, 0);
    pm.mark_thread_finished(f0, 0);
    pm.mark_thread_active(f0, 1);
    pm.mark_thread_finished(f0, 1);
    pm.set_file_done(f0, true, "Done");
    REQUIRE_FALSE(pm.all_done());

    pm.mark_thread_active(f1, 0);
    pm.mark_thread_finished(f1, 0);
    pm.mark_thread_active(f1, 1);
    pm.mark_thread_finished(f1, 1);
    pm.set_file_done(f1, true, "Done");
    REQUIRE(pm.all_done());
}

TEST_CASE("multi file parallel", "[progress]") {
    ProgressManager pm;
    int f0 = pm.add_file("a.zip", 1000, 1);
    int f1 = pm.add_file("b.zip", 2000, 1);

    pm.mark_thread_active(f0, 0);
    pm.mark_thread_active(f1, 0);
    pm.mark_thread_finished(f0, 0);
    pm.mark_thread_finished(f1, 0);
    pm.set_file_done(f0, true, "Done");
    pm.set_file_done(f1, true, "Done");
    REQUIRE(pm.all_done());
}

TEST_CASE("error state", "[progress]") {
    ProgressManager pm;
    int id = pm.add_file("test.zip", 1000, 2);
    pm.mark_thread_active(id, 0);
    pm.mark_thread_error(id, 0);
    pm.mark_thread_active(id, 1);
    pm.mark_thread_finished(id, 1);
    pm.set_file_done(id, false, "Partial");
    REQUIRE(pm.all_done());
}

TEST_CASE("invalid ids no crash", "[progress]") {
    ProgressManager pm;
    pm.add_file("test.zip", 1000, 2);
    REQUIRE_NOTHROW(pm.update_thread(99, 0, 100, 100));
    REQUIRE_NOTHROW(pm.mark_thread_active(99, 0));
    REQUIRE_NOTHROW(pm.mark_thread_finished(99, 0));
    REQUIRE_NOTHROW(pm.mark_thread_error(99, 0));
    REQUIRE_NOTHROW(pm.set_file_done(99, true, "x"));
    REQUIRE_FALSE(pm.all_done());
}

TEST_CASE("negative ids no crash", "[progress]") {
    ProgressManager pm;
    pm.add_file("test.zip", 1000, 2);
    REQUIRE_NOTHROW(pm.update_thread(-1, 0, 100, 100));
    REQUIRE_NOTHROW(pm.mark_thread_active(-1, 0));
    REQUIRE_NOTHROW(pm.mark_thread_finished(-1, -1));
    REQUIRE_NOTHROW(pm.mark_thread_error(-1, 0));
    REQUIRE_FALSE(pm.all_done());
}

TEST_CASE("redraw does not crash", "[progress]") {
    ProgressManager pm;
    int id = pm.add_file("test.zip", 1024, 2);
    pm.mark_thread_active(id, 0);
    pm.update_thread(id, 0, 512, 1024);
    REQUIRE_NOTHROW(pm.redraw());
    pm.mark_thread_finished(id, 0);
    pm.mark_thread_active(id, 1);
    pm.update_thread(id, 1, 1024, 1024);
    REQUIRE_NOTHROW(pm.redraw());
    pm.mark_thread_finished(id, 1);
    pm.set_file_done(id, true, "Done");
    REQUIRE_NOTHROW(pm.redraw());
}

TEST_CASE("redraw multiple files", "[progress]") {
    ProgressManager pm;
    for (int i = 0; i < 5; i++) {
        int id = pm.add_file("file_" + std::to_string(i) + ".zip", 1000 * (i + 1), 3);
        for (int t = 0; t < 3; t++) {
            pm.mark_thread_active(id, t);
            pm.update_thread(id, t, 500 * (i + 1), 1000 * (i + 1));
        }
    }
    REQUIRE_NOTHROW(pm.redraw());
    for (int i = 0; i < 5; i++) {
        for (int t = 0; t < 3; t++)
            pm.mark_thread_finished(i, t);
        pm.set_file_done(i, true, "Done");
    }
    REQUIRE_NOTHROW(pm.redraw());
    REQUIRE(pm.all_done());
}
