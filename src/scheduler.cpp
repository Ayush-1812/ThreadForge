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
    // Finish the worker/queue registry before any thread can scan it for steals.
    for (std::size_t i = 0; i < config_.worker_count; ++i) {
        workers_.push_back(std::make_unique<detail::Worker>(*this, i));
    }
    try {
        for (auto& worker : workers_) {
            worker->start();
        }
    } catch (...) {
        // A failed thread allocation must wake and join workers already created.
        close_submission();
        throw;
    }
}

Scheduler::~Scheduler() {
    shutdown();
}

void Scheduler::shutdown() {
    // Check before acquiring the join lock: an external caller may already be
    // holding it while waiting for this very task to return.
    if (auto* worker = detail::Worker::current(); worker && &worker->scheduler_ == this) {
        throw std::logic_error("a ThreadForge worker cannot shut down its own scheduler");
    }
    std::lock_guard lock(shutdown_mutex_);
    close_submission();
    for (auto& worker : workers_) {
        worker->join();
    }
}

bool Scheduler::is_shutdown() const {
    std::lock_guard lock(state_mutex_);
    return !accepting_;
}

std::size_t Scheduler::worker_count() const noexcept {
    return config_.worker_count;
}

void Scheduler::close_submission() {
    {
        std::lock_guard lock(state_mutex_);
        accepting_ = false;
        ++wake_generation_;
        queue_.close();
    }
    work_available_.notify_all();
}

void Scheduler::enqueue(Task task, Priority priority) {
    {
        // Admission and queue publication are atomic with respect to closure.
        // Queue consumers never take this lock while holding a queue lock.
        std::lock_guard lock(state_mutex_);
        if (!accepting_) {
            throw std::runtime_error("ThreadForge scheduler is shut down");
        }
        auto* worker = detail::Worker::current();
        if (worker && &worker->scheduler_ == this) {
            worker->queue_.push(std::move(task), priority);
        } else if (!queue_.push(std::move(task), priority)) {
            throw std::runtime_error("ThreadForge scheduler is shut down");
        }
        tasks_submitted_.fetch_add(1, std::memory_order_relaxed);
        ++wake_generation_;
    }
    // Local submissions must also wake an idle thief while the owner is busy.
    work_available_.notify_one();
}

std::optional<Task> Scheduler::take_task(detail::Worker& worker) {
    for (;;) {
        std::uint64_t observed_generation;
        {
            std::lock_guard lock(state_mutex_);
            observed_generation = wake_generation_;
        }
        std::array<bool, detail::priority_count> checked{};
        for (std::size_t offset = 0; offset < detail::priority_cycle.size(); ++offset) {
            const auto slot = (worker.priority_cursor_ + offset) % detail::priority_cycle.size();
            const auto priority = detail::priority_cycle[slot];
            const auto lane = detail::priority_index(priority);
            if (checked[lane]) {
                continue;
            }
            checked[lane] = true;
            // Class selection precedes source selection: when High is preferred,
            // local Low cannot mask global or stealable High. Only claims consume slots.
            if (auto task = try_take_task(worker, priority)) {
                worker.priority_cursor_ = (slot + 1) % detail::priority_cycle.size();
                return task;
            }
        }

        std::unique_lock lock(state_mutex_);
        // If publication raced any part of the scan, rescan before sleeping or
        // exiting. Notifications alone cannot protect the scan-to-wait gap.
        if (wake_generation_ != observed_generation) {
            continue;
        }
        if (!accepting_) {
            return std::nullopt;
        }
        worker.metrics_.waits.fetch_add(1, std::memory_order_relaxed);
        work_available_.wait(lock, [&] {
            return wake_generation_ != observed_generation || !accepting_;
        });
    }
}

std::optional<Task> Scheduler::try_take_task(detail::Worker& worker, Priority priority) {
    if (auto task = worker.queue_.try_pop(priority)) {
        worker.metrics_.local_tasks.fetch_add(1, std::memory_order_relaxed);
        return task;
    }
    if (auto task = queue_.try_pop(priority)) {
        worker.metrics_.global_tasks.fetch_add(1, std::memory_order_relaxed);
        return task;
    }
    const auto count = workers_.size();
    const auto first_victim = worker.next_victim_ % count;
    for (std::size_t offset = 0; offset < count; ++offset) {
        const auto victim = (first_victim + offset) % count;
        if (victim == worker.index_) {
            continue;
        }
        if (auto task = workers_[victim]->queue_.try_steal(priority)) {
            worker.next_victim_ = (victim + 1) % count;
            worker.metrics_.tasks_stolen.fetch_add(1, std::memory_order_relaxed);
            return task;
        }
        worker.metrics_.failed_steal_attempts.fetch_add(1, std::memory_order_relaxed);
    }
    worker.next_victim_ = (first_victim + 1) % count;
    return std::nullopt;
}

MetricsSnapshot Scheduler::metrics() const {
    MetricsSnapshot result;
    result.tasks_submitted = tasks_submitted_.load(std::memory_order_relaxed);
    result.workers.reserve(workers_.size());
    for (const auto& worker : workers_) {
        const auto snapshot = worker->metrics_.snapshot();
        result.local_tasks += snapshot.local_tasks;
        result.global_tasks += snapshot.global_tasks;
        result.tasks_stolen += snapshot.tasks_stolen;
        result.failed_steal_attempts += snapshot.failed_steal_attempts;
        result.waits += snapshot.waits;
        result.workers.push_back(snapshot);
    }
    return result;
}

} // namespace threadforge
