#include "threadforge/work_stealing_queue.hpp"

#include <stdexcept>
#include <utility>

namespace threadforge {

void WorkStealingQueue::push(Task task) {
    if (!task.valid()) {
        throw std::invalid_argument("cannot enqueue an empty task");
    }
    std::lock_guard lock(mutex_);
    tasks_.push_front(std::move(task));
}

std::optional<Task> WorkStealingQueue::try_pop() {
    std::lock_guard lock(mutex_);
    if (tasks_.empty()) {
        return std::nullopt;
    }
    auto task = std::move(tasks_.front());
    tasks_.pop_front();
    return task;
}

std::optional<Task> WorkStealingQueue::try_steal() {
    std::lock_guard lock(mutex_);
    if (tasks_.empty()) {
        return std::nullopt;
    }
    auto task = std::move(tasks_.back());
    tasks_.pop_back();
    return task;
}

} // namespace threadforge
