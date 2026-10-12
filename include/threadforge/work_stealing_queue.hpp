#pragma once

#include "threadforge/task.hpp"

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

    void push(Task task);
    [[nodiscard]] std::optional<Task> try_pop();
    [[nodiscard]] std::optional<Task> try_steal();

private:
    std::mutex mutex_;
    std::deque<Task> tasks_;
};

} // namespace threadforge
