#pragma once

#include <thread>

namespace threadforge {

class TaskQueue;

namespace detail {

class Worker {
public:
    explicit Worker(TaskQueue& queue);
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void join();
    [[nodiscard]] static bool is_current_worker(const TaskQueue& queue) noexcept;

private:
    static thread_local const TaskQueue* current_queue_;
    std::jthread thread_;
};

} // namespace detail
} // namespace threadforge
