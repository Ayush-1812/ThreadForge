#pragma once

#include "threadforge/metrics.hpp"
#include "threadforge/work_stealing_queue.hpp"

#include <cstddef>
#include <thread>

namespace threadforge {

class Scheduler;

namespace detail {

class Worker {
public:
    Worker(Scheduler& scheduler, std::size_t index);
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void start();
    void join();
    [[nodiscard]] static Worker* current() noexcept;

private:
    friend class threadforge::Scheduler;
    static thread_local Worker* current_worker_;
    Scheduler& scheduler_;
    const std::size_t index_;
    std::size_t next_victim_;
    WorkStealingQueue queue_;
    WorkerMetrics metrics_;
    // A worker's queues and counters must outlive its thread.
    std::jthread thread_;
};

} // namespace detail
} // namespace threadforge
