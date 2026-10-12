#pragma once

#include <future>
#include <memory>
#include <utility>

namespace threadforge {

namespace detail {
struct DependencyState;
struct SchedulerIdentity;
}

class Scheduler;

// A completion token; task values and exceptions belong to the result future.
class TaskHandle {
public:
    TaskHandle() = default;
    [[nodiscard]] bool valid() const noexcept;

private:
    friend class Scheduler;
    explicit TaskHandle(std::shared_ptr<detail::DependencyState> state)
        : state_(std::move(state)) {}
    std::shared_ptr<detail::DependencyState> state_;
};

template<class Result>
struct TaskSubmission {
    std::future<Result> future;
    TaskHandle handle;
};

} // namespace threadforge
