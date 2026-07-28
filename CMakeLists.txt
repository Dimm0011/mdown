cmake_minimum_required(VERSION 3.15)
project(mdown LANGUAGES CXX)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_executable(mdown
    src/main.cpp
    src/downloader.cpp
    src/curl_transport.cpp
    src/progress.cpp
    src/checksum.cpp
)

target_include_directories(mdown PRIVATE src)
target_link_libraries(mdown PRIVATE
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

install(TARGETS mdown RUNTIME DESTINATION bin)

option(BUILD_TESTS "Build tests" OFF)

if(BUILD_TESTS)
    enable_testing()

    include(FetchContent)

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(gtest_all
        tests/gtest/test_all.cpp
        tests/gtest/test_downloader.cpp
        src/downloader.cpp
        src/progress.cpp
        src/checksum.cpp
    )
    target_include_directories(gtest_all PRIVATE src tests/mock)
    target_link_libraries(gtest_all PRIVATE
        gtest
        gtest_main
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )
    add_test(NAME multidow_gtest COMMAND gtest_all)
endif()
