#include "threadforge/scheduler.hpp"
#include "threadforge/worker.hpp"

namespace threadforge {

Scheduler::Scheduler() : Scheduler(Config{}) {}

Scheduler::Scheduler(std::size_t worker_count) : Scheduler(Config{worker_count}) {}

Scheduler::Scheduler(Config config) : config_(config) {
    if (config_.worker_count == 0) {
        throw std::invalid_argument("ThreadForge requires at least one worker");
    }
    workers_.reserve(config_.worker_count);
    try {
        for (std::size_t i = 0; i < config_.worker_count; ++i) {
            workers_.push_back(std::make_unique<detail::Worker>(queue_));
        }
    } catch (...) {
        // A failed thread allocation must wake and join workers already created.
        queue_.close();
        throw;
    }
}

Scheduler::~Scheduler() {
    shutdown();
}

void Scheduler::shutdown() {
    // Check before acquiring the join lock: an external caller may already be
    // holding it while waiting for this very task to return.
    if (detail::Worker::is_current_worker(queue_)) {
        throw std::logic_error("a ThreadForge worker cannot shut down its own scheduler");
    }
    std::lock_guard lock(shutdown_mutex_);
    queue_.close();
    for (auto& worker : workers_) {
        worker->join();
    }
}

bool Scheduler::is_shutdown() const {
    return queue_.is_closed();
}

std::size_t Scheduler::worker_count() const noexcept {
    return config_.worker_count;
}

} // namespace threadforge
