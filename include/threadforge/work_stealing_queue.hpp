#pragma once

#include "threadforge/task.hpp"
#include "threadforge/priority.hpp"

#include <array>
#include <deque>
#include <mutex>
#include <optional>

namespace threadforge {

// The owner pushes/pops the front; thieves remove the oldest task from the back.
class WorkStealingQueue {
public:
    WorkStealingQueue() = default;
    WorkStealingQueue(const WorkStealingQueue&) = delete;
    WorkStealingQueue& operator=(const WorkStealingQueue&) = delete;

    void push(Task task, Priority priority = Priority::Normal);
    [[nodiscard]] std::optional<Task> try_pop();
    [[nodiscard]] std::optional<Task> try_pop(Priority priority);
    [[nodiscard]] std::optional<Task> try_steal();
    [[nodiscard]] std::optional<Task> try_steal(Priority priority);

private:
    [[nodiscard]] std::optional<Task> pop_locked(std::size_t lane, bool steal);
    [[nodiscard]] std::optional<Task> pop_highest_locked(bool steal);
    std::mutex mutex_;
    std::array<std::deque<Task>, detail::priority_count> tasks_;
};

} // namespace threadforge
