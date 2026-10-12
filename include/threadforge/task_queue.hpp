#pragma once

#include "threadforge/task.hpp"
#include "threadforge/priority.hpp"

#include <array>
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

    [[nodiscard]] bool push(Task task, Priority priority = Priority::Normal) {
        return push_preserving(task, priority);
    }

private:
    friend class Scheduler;
    // Allocate the slot before moving so a failed allocation retains the task.
    [[nodiscard]] bool push_preserving(Task& task, Priority priority) {
        const auto lane = detail::priority_index(priority);
        if (!task.valid()) {
            throw std::invalid_argument("cannot enqueue an empty task");
        }
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return false;
            }
            tasks_[lane].emplace_back();
            tasks_[lane].back() = std::move(task);
        }
        available_.notify_one();
        return true;
    }

public:
    [[nodiscard]] std::optional<Task> wait_pop() {
        std::unique_lock lock(mutex_);
        available_.wait(lock, [this] { return closed_ || !empty_locked(); });
        return pop_highest_locked();
    }

    [[nodiscard]] std::optional<Task> try_pop() {
        std::lock_guard lock(mutex_);
        return pop_highest_locked();
    }

    [[nodiscard]] std::optional<Task> try_pop(Priority priority) {
        const auto lane = detail::priority_index(priority);
        std::lock_guard lock(mutex_);
        return pop_locked(lane);
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
    [[nodiscard]] bool empty_locked() const {
        for (const auto& lane : tasks_) {
            if (!lane.empty()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::optional<Task> pop_locked(std::size_t lane) {
        if (tasks_[lane].empty()) {
            return std::nullopt;
        }
        auto task = std::move(tasks_[lane].front());
        tasks_[lane].pop_front();
        return task;
    }

    [[nodiscard]] std::optional<Task> pop_highest_locked() {
        for (std::size_t lane = 0; lane < detail::priority_count; ++lane) {
            if (auto task = pop_locked(lane)) {
                return task;
            }
        }
        return std::nullopt;
    }

    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::array<std::deque<Task>, detail::priority_count> tasks_;
    bool closed_ = false;
};

} // namespace threadforge
