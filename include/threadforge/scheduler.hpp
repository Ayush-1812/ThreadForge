#pragma once

#include "threadforge/config.hpp"
#include "threadforge/metrics.hpp"
#include "threadforge/priority.hpp"
#include "threadforge/task_queue.hpp"
#include "threadforge/task_handle.hpp"

#include <functional>
#include <condition_variable>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <unordered_map>
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
        return submit(Priority::Normal, std::forward<Function>(function),
                      std::forward<Args>(args)...);
    }

    template<class Function, class... Args>
    auto submit(Priority priority, Function&& function, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>> {
        return submit_task(priority, std::forward<Function>(function),
                           std::forward<Args>(args)...).future;
    }

    template<class Function, class... Args>
    auto submit_task(Function&& function, Args&&... args)
        -> TaskSubmission<std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>> {
        return submit_task(Priority::Normal, std::forward<Function>(function),
                           std::forward<Args>(args)...);
    }

    template<class Function, class... Args>
    auto submit_task(Priority priority, Function&& function, Args&&... args)
        -> TaskSubmission<std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>> {
        return submit_after(std::span<const TaskHandle>{}, priority,
                            std::forward<Function>(function), std::forward<Args>(args)...);
    }

    template<class Function, class... Args>
    auto submit_after(std::span<const TaskHandle> prerequisites,
                      Function&& function, Args&&... args)
        -> TaskSubmission<std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>> {
        return submit_after(prerequisites, Priority::Normal,
                            std::forward<Function>(function), std::forward<Args>(args)...);
    }

    template<class Function, class... Args>
    auto submit_after(std::initializer_list<TaskHandle> prerequisites,
                      Function&& function, Args&&... args)
        -> TaskSubmission<std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>> {
        return submit_after(std::span<const TaskHandle>(prerequisites.begin(), prerequisites.size()),
                            Priority::Normal, std::forward<Function>(function), std::forward<Args>(args)...);
    }

    template<class Function, class... Args>
    auto submit_after(std::initializer_list<TaskHandle> prerequisites, Priority priority,
                      Function&& function, Args&&... args)
        -> TaskSubmission<std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>> {
        return submit_after(std::span<const TaskHandle>(prerequisites.begin(), prerequisites.size()),
                            priority, std::forward<Function>(function), std::forward<Args>(args)...);
    }

    template<class Function, class... Args>
    auto submit_after(std::span<const TaskHandle> prerequisites, Priority priority,
                      Function&& function, Args&&... args)
        -> TaskSubmission<std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>> {
        static_cast<void>(detail::priority_index(priority));
        using Result = std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>;
        auto invocation = [this, callable = std::forward<Function>(function),
                           arguments = std::make_tuple(std::forward<Args>(args)...)]() mutable -> Result {
            try {
                return std::apply([&callable](auto&&... values) -> Result {
                    return std::invoke(std::move(callable), std::forward<decltype(values)>(values)...);
                }, std::move(arguments));
            } catch (...) {
                record_failure();
                throw;
            }
        };
        std::packaged_task<Result()> result_task(std::move(invocation));
        auto result = result_task.get_future();
        Task task([inner = std::move(result_task)]() mutable { inner(); });
        auto handle = admit(std::move(task), priority, prerequisites);
        return {std::move(result), std::move(handle)};
    }

    // Idempotent and safe for concurrent callers outside this scheduler's workers.
    void shutdown();
    [[nodiscard]] bool is_shutdown() const;
    [[nodiscard]] std::size_t worker_count() const noexcept;
    [[nodiscard]] MetricsSnapshot metrics() const;

private:
    friend class detail::Worker;
    [[nodiscard]] TaskHandle admit(Task task, Priority priority,
                                   std::span<const TaskHandle> prerequisites);
    void complete(const std::shared_ptr<detail::DependencyState>& node) noexcept;
    void start_task(detail::DependencyState& node) noexcept;
    void record_failure() noexcept;
    void publish_ready_locked(detail::DependencyState& node) noexcept;
    [[nodiscard]] std::optional<Task> take_dependency_ready(Priority priority);
    [[nodiscard]] std::optional<Task> take_task(detail::Worker& worker);
    [[nodiscard]] std::optional<Task> try_take_task(detail::Worker& worker, Priority priority);
    void close_submission();

    Config config_;
    TaskQueue queue_;
    mutable std::mutex state_mutex_;
    std::condition_variable work_available_;
    bool accepting_ = true;
    std::uint64_t wake_generation_ = 0;
    std::atomic<std::uint64_t> tasks_submitted_{0};
    std::array<std::uint64_t, detail::priority_count> submitted_by_priority_{};
    std::uint64_t tasks_ready_ = 0;
    std::uint64_t tasks_running_ = 0;
    std::uint64_t tasks_waiting_dependencies_ = 0;
    std::uint64_t peak_tasks_pending_ = 0;
    std::uint64_t total_dependency_wait_ns_ = 0;
    const std::shared_ptr<detail::SchedulerIdentity> identity_;
    // These nodes own waiting work; intrusive ready links never outlive admission.
    std::unordered_map<detail::DependencyState*, std::shared_ptr<detail::DependencyState>> active_nodes_;
    std::array<detail::DependencyState*, detail::priority_count> ready_heads_{};
    std::array<detail::DependencyState*, detail::priority_count> ready_tails_{};
    // All queues and synchronization state outlive the worker threads.
    std::vector<std::unique_ptr<detail::Worker>> workers_;
    std::mutex shutdown_mutex_;
};

} // namespace threadforge
