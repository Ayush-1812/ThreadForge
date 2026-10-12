#include "threadforge/work_stealing_queue.hpp"

#include <stdexcept>
#include <utility>

namespace threadforge {

void WorkStealingQueue::push(Task task, Priority priority) {
    const auto lane = detail::priority_index(priority);
    if (!task.valid()) {
        throw std::invalid_argument("cannot enqueue an empty task");
    }
    std::lock_guard lock(mutex_);
    tasks_[lane].push_front(std::move(task));
}

std::optional<Task> WorkStealingQueue::try_pop() {
    std::lock_guard lock(mutex_);
    return pop_highest_locked(false);
}

std::optional<Task> WorkStealingQueue::try_pop(Priority priority) {
    const auto lane = detail::priority_index(priority);
    std::lock_guard lock(mutex_);
    return pop_locked(lane, false);
}

std::optional<Task> WorkStealingQueue::try_steal() {
    std::lock_guard lock(mutex_);
    return pop_highest_locked(true);
}

std::optional<Task> WorkStealingQueue::try_steal(Priority priority) {
    const auto lane = detail::priority_index(priority);
    std::lock_guard lock(mutex_);
    return pop_locked(lane, true);
}

std::optional<Task> WorkStealingQueue::pop_locked(std::size_t lane, bool steal) {
    auto& tasks = tasks_[lane];
    if (tasks.empty()) {
        return std::nullopt;
    }
    auto task = std::move(steal ? tasks.back() : tasks.front());
    if (steal) {
        tasks.pop_back();
    } else {
        tasks.pop_front();
    }
    return task;
}

std::optional<Task> WorkStealingQueue::pop_highest_locked(bool steal) {
    for (std::size_t lane = 0; lane < detail::priority_count; ++lane) {
        if (auto task = pop_locked(lane, steal)) {
            return task;
        }
    }
    return std::nullopt;
}

} // namespace threadforge
