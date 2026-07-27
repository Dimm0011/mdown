#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>
#include "compat.h"

namespace multidow {

class ThreadPool {
   public:
    explicit ThreadPool(size_t threads = std::thread::hardware_concurrency()) {
        if (threads == 0) threads = 1;
        for (size_t i = 0; i < threads; i++)
            workers_.emplace_back([this](multidow::stop_token) { worker_loop(); });
    }

    ~ThreadPool() {
        shutdown();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<R()>>(
            [fn = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
                return fn(std::move(args)...);
            });
        auto future = task->get_future();
        {
            std::lock_guard lock(mtx_);
            if (stop_) throw std::runtime_error("submit on stopped ThreadPool");
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return future;
    }

    void shutdown() {
        {
            std::lock_guard lock(mtx_);
            if (stop_.exchange(true)) return;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    [[nodiscard]] size_t size() const {
        return workers_.size();
    }

   private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(mtx_);
                cv_.wait(lock, [this] { return stop_.load() || !tasks_.empty(); });
                if (stop_.load() && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<multidow::jthread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
};

}  // namespace multidow
