#pragma once

#include "threadforge/config.hpp"
#include "threadforge/task_queue.hpp"

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace threadforge {

namespace detail { class Worker; }

class Scheduler {
public:
    Scheduler();
    explicit Scheduler(std::size_t worker_count);
    explicit Scheduler(Config config);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

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
        Task task([inner = std::move(result_task)]() mutable { inner(); });
        if (!queue_.push(std::move(task))) {
            throw std::runtime_error("ThreadForge scheduler is shut down");
        }
        return result;
    }

    // Idempotent and safe for concurrent callers outside this scheduler's workers.
    void shutdown();
    [[nodiscard]] bool is_shutdown() const;
    [[nodiscard]] std::size_t worker_count() const noexcept;

private:
    Config config_;
    TaskQueue queue_;
    // Members are destroyed in reverse order; the queue outlives every worker.
    std::vector<std::unique_ptr<detail::Worker>> workers_;
    std::mutex shutdown_mutex_;
};

} // namespace threadforge
