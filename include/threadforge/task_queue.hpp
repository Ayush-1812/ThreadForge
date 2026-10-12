#pragma once

#include "threadforge/task.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace threadforge {

class TaskQueue {
public:
    TaskQueue() = default;
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    [[nodiscard]] bool push(Task task) {
        if (!task.valid()) {
            throw std::invalid_argument("cannot enqueue an empty task");
        }
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return false;
            }
            tasks_.push_back(std::move(task));
        }
        available_.notify_one();
        return true;
    }

    [[nodiscard]] std::optional<Task> wait_pop() {
        std::unique_lock lock(mutex_);
        available_.wait(lock, [this] { return closed_ || !tasks_.empty(); });
        if (tasks_.empty()) {
            return std::nullopt;
        }
        auto task = std::move(tasks_.front());
        tasks_.pop_front();
        return task;
    }

    [[nodiscard]] std::optional<Task> try_pop() {
        std::lock_guard lock(mutex_);
        if (tasks_.empty()) {
            return std::nullopt;
        }
        auto task = std::move(tasks_.front());
        tasks_.pop_front();
        return task;
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            // Closing and submission share this lock: every accepted task is drained.
            closed_ = true;
        }
        available_.notify_all();
    }

    [[nodiscard]] bool is_closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<Task> tasks_;
    bool closed_ = false;
};

} // namespace threadforge
