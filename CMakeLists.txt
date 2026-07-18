cmake_minimum_required(VERSION 3.14)
project(MultiDow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Werror)

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_executable(multidow
    src/main.cpp
    src/downloader.cpp
    src/progress.cpp
    src/checksum.cpp
)

target_include_directories(multidow PRIVATE src)
target_link_libraries(multidow PRIVATE
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

install(TARGETS multidow RUNTIME DESTINATION bin)

# --- Tests ---

option(BUILD_TESTS "Build tests (requires Google Test / Catch2)" OFF)

if(BUILD_TESTS)
    enable_testing()

    include(FetchContent)

    # --- Simple unit tests (no framework) ---
    add_executable(test_all
        tests/unit/test_all.cpp
        src/progress.cpp
        src/checksum.cpp
    )
    target_include_directories(test_all PRIVATE src)
    target_link_libraries(test_all PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )
    add_test(NAME multidow_unit COMMAND test_all)

    # --- Google Test ---
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(gtest_all
        tests/gtest/test_all.cpp
        src/progress.cpp
        src/checksum.cpp
    )
    target_include_directories(gtest_all PRIVATE src)
    target_link_libraries(gtest_all PRIVATE
        gtest
        gtest_main
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )
    add_test(NAME multidow_gtest COMMAND gtest_all)

    # --- Catch2 ---
    FetchContent_Declare(
        catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.2
    )
    set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(catch2)

    add_executable(catch2_all
        tests/catch2/test_all.cpp
        src/progress.cpp
        src/checksum.cpp
    )
    target_include_directories(catch2_all PRIVATE src)
    target_link_libraries(catch2_all PRIVATE
        Catch2::Catch2WithMain
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )
    add_test(NAME multidow_catch2 COMMAND catch2_all)
endif()
