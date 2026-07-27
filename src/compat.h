#pragma once

#include <version>

#ifdef __APPLE__
#include <unistd.h>
#define pwrite64 pwrite
#endif

#if __has_include(<stop_token>) && defined(__cpp_lib_jthread)
#define HAS_JTHREAD 1
#else
#define HAS_JTHREAD 0
#endif

#if HAS_JTHREAD
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>
namespace multidow {
using std::jthread;
using std::stop_source;
using std::stop_token;

inline void wait_for(stop_source& src, std::chrono::steady_clock::duration timeout) {
    std::mutex mtx;
    std::condition_variable_any cv;
    std::unique_lock lk(mtx);
    cv.wait_for(lk, src.get_token(), timeout, [&] { return src.stop_requested(); });
}

}  // namespace multidow
#else
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace multidow {

class stop_token {
   public:
    stop_token() = default;
    explicit stop_token(std::shared_ptr<std::atomic<bool> > f, std::shared_ptr<std::mutex> mtx,
                        std::shared_ptr<std::condition_variable> cv)
        : flag_(std::move(f)), mtx_(std::move(mtx)), cv_(std::move(cv)) {}
    [[nodiscard]] bool stop_requested() const {
        return flag_ && flag_->load(std::memory_order_relaxed);
    }

   private:
    std::shared_ptr<std::atomic<bool> > flag_;
    std::shared_ptr<std::mutex> mtx_;
    std::shared_ptr<std::condition_variable> cv_;
};

class stop_source {
   public:
    stop_source()
        : flag_(std::make_shared<std::atomic<bool> >(false)),
          mtx_(std::make_shared<std::mutex>()),
          cv_(std::make_shared<std::condition_variable>()) {}

    [[nodiscard]] stop_token get_token() const {
        return stop_token(flag_, mtx_, cv_);
    }
    [[nodiscard]] bool stop_requested() const {
        return flag_->load(std::memory_order_relaxed);
    }

    void request_stop() {
        bool expected = false;
        if (flag_->compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            std::lock_guard lk(*mtx_);
            cv_->notify_all();
            return;
        }
    }

    void wait_for(std::chrono::steady_clock::duration timeout) {
        std::unique_lock lk(*mtx_);
        cv_->wait_for(lk, timeout, [this] { return flag_->load(std::memory_order_relaxed); });
    }

   private:
    std::shared_ptr<std::atomic<bool> > flag_;
    std::shared_ptr<std::mutex> mtx_;
    std::shared_ptr<std::condition_variable> cv_;
};

class jthread {
   public:
    jthread() = default;

    template <typename F, typename... Args>
    explicit jthread(F&& f, Args&&... args)
        : st_(std::make_shared<stop_source>()),
          t_(std::thread(
              [st = st_, fn = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
                  fn(st->get_token(), std::move(args)...);
              })) {}

    jthread(jthread&& o) noexcept : st_(std::move(o.st_)), t_(std::move(o.t_)) {}
    jthread& operator=(jthread&& o) noexcept {
        if (this != &o) {
            request_stop();
            join();
            st_ = std::move(o.st_);
            t_ = std::move(o.t_);
        }
        return *this;
    }

    ~jthread() {
        request_stop();
        join();
    }

    jthread(const jthread&) = delete;
    jthread& operator=(const jthread&) = delete;

    void request_stop() {
        if (st_) st_->request_stop();
    }

    [[nodiscard]] stop_token get_token() const {
        return st_ ? st_->get_token() : stop_token();
    }

    void join() {
        if (t_.joinable()) t_.join();
    }

    [[nodiscard]] bool joinable() const {
        return t_.joinable();
    }

   private:
    std::shared_ptr<stop_source> st_;
    std::thread t_;
};

inline void wait_for(stop_source& src, std::chrono::steady_clock::duration timeout) {
    src.wait_for(timeout);
}

}  // namespace multidow
#endif
