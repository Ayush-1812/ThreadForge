#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace threadforge::benchmarks {

// A deliberately small FIFO baseline: all submissions share one queue.
class CentralizedPool {
public:
    explicit CentralizedPool(std::size_t worker_count) {
        if (worker_count == 0) {
            throw std::invalid_argument("centralized pool requires at least one worker");
        }
        workers_.reserve(worker_count);
        try {
            for (std::size_t i = 0; i < worker_count; ++i) {
                workers_.emplace_back([this] { run(); });
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~CentralizedPool() { shutdown(); }

    CentralizedPool(const CentralizedPool&) = delete;
    CentralizedPool& operator=(const CentralizedPool&) = delete;
    CentralizedPool(CentralizedPool&&) = delete;
    CentralizedPool& operator=(CentralizedPool&&) = delete;

    template<class Function, class... Args>
    auto submit(Function&& function, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>> {
        using Result = std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>;
        auto invocation = [callable = std::forward<Function>(function),
                           arguments = std::make_tuple(std::forward<Args>(args)...)]() mutable -> Result {
            return std::apply([&callable](auto&&... values) -> Result {
                return std::invoke(std::move(callable), std::forward<decltype(values)>(values)...);
            }, std::move(arguments));
        };
        std::packaged_task<Result()> result_task(std::move(invocation));
        auto result = result_task.get_future();
        std::packaged_task<void()> task([inner = std::move(result_task)]() mutable { inner(); });
        {
            std::lock_guard lock(queue_mutex_);
            if (!accepting_) {
                throw std::runtime_error("centralized pool is shut down");
            }
            tasks_.push_back(std::move(task));
        }
        available_.notify_one();
        return result;
    }

    void shutdown() {
        // Check before taking the join lock, which an external joiner may hold.
        if (current_ == this) {
            throw std::logic_error("centralized pool worker cannot shut down its own pool");
        }
        std::lock_guard join_lock(shutdown_mutex_);
        {
            std::lock_guard queue_lock(queue_mutex_);
            accepting_ = false;
        }
        available_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    [[nodiscard]] bool is_shutdown() const {
        std::lock_guard lock(queue_mutex_);
        return !accepting_;
    }

private:
    void run() {
        current_ = this;
        for (;;) {
            std::packaged_task<void()> task;
            {
                std::unique_lock lock(queue_mutex_);
                available_.wait(lock, [this] { return !accepting_ || !tasks_.empty(); });
                if (tasks_.empty()) {
                    break;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
        current_ = nullptr;
    }

    inline static thread_local CentralizedPool* current_ = nullptr;
    mutable std::mutex queue_mutex_;
    std::condition_variable available_;
    std::deque<std::packaged_task<void()>> tasks_;
    bool accepting_ = true;
    std::vector<std::thread> workers_;
    std::mutex shutdown_mutex_;
};

} // namespace threadforge::benchmarks
