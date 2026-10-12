#pragma once

#include "threadforge/config.hpp"
#include "threadforge/metrics.hpp"
#include "threadforge/task_queue.hpp"

#include <functional>
#include <condition_variable>
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
        enqueue(std::move(task));
        return result;
    }

    // Idempotent and safe for concurrent callers outside this scheduler's workers.
    void shutdown();
    [[nodiscard]] bool is_shutdown() const;
    [[nodiscard]] std::size_t worker_count() const noexcept;
    [[nodiscard]] MetricsSnapshot metrics() const;

private:
    friend class detail::Worker;
    void enqueue(Task task);
    [[nodiscard]] std::optional<Task> take_task(detail::Worker& worker);
    void close_submission();

    Config config_;
    TaskQueue queue_;
    mutable std::mutex state_mutex_;
    std::condition_variable work_available_;
    bool accepting_ = true;
    std::uint64_t wake_generation_ = 0;
    std::atomic<std::uint64_t> tasks_submitted_{0};
    // All queues and synchronization state outlive the worker threads.
    std::vector<std::unique_ptr<detail::Worker>> workers_;
    std::mutex shutdown_mutex_;
};

} // namespace threadforge
