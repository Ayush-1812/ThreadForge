#include "threadforge/scheduler.hpp"
#include "threadforge/worker.hpp"

#include <new>
#include <algorithm>
#include <chrono>
#include <unordered_set>

namespace threadforge::detail {

struct SchedulerIdentity {};

struct DependencyState {
    std::weak_ptr<SchedulerIdentity> owner;
    Task ready_task;
    Priority priority = Priority::Normal;
    // Registration, completion, counters, and ready links share state_mutex_.
    std::size_t remaining = 0;
    bool completed = false;
    std::vector<std::weak_ptr<DependencyState>> dependents;
    DependencyState* next_ready = nullptr;
    std::uint64_t admitted_ns = 0;
    std::uint64_t ready_ns = 0;
    std::uint64_t started_ns = 0;
};

} // namespace threadforge::detail

namespace threadforge {

namespace {
std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void record_duration(std::atomic<std::uint64_t>& total,
                     std::atomic<std::uint64_t>& maximum, std::uint64_t duration) noexcept {
    total.fetch_add(duration, std::memory_order_relaxed);
    // Only the owning worker writes its counters; concurrent readers use atomics.
    if (duration > maximum.load(std::memory_order_relaxed)) {
        maximum.store(duration, std::memory_order_relaxed);
    }
}
} // namespace

bool TaskHandle::valid() const noexcept {
    return state_ && !state_->owner.expired();
}

Scheduler::Scheduler() : Scheduler(Config{}) {}

Scheduler::Scheduler(std::size_t worker_count) : Scheduler(Config{worker_count}) {}

Scheduler::Scheduler(Config config)
    : config_(config), identity_(std::make_shared<detail::SchedulerIdentity>()) {
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

TaskHandle Scheduler::admit(Task task, Priority priority,
                            std::span<const TaskHandle> prerequisites) {
    auto node = std::make_shared<detail::DependencyState>();
    node->owner = identity_;
    node->priority = priority;
    std::weak_ptr<detail::DependencyState> weak_node = node;
    node->ready_task = Task([this, weak_node, inner = std::move(task)]() mutable {
        const auto executing = weak_node.lock();
        start_task(*executing);
        // The typed future is ready before the completion event releases children.
        inner();
        complete(executing);
    });
    std::vector<std::shared_ptr<detail::DependencyState>> parents;
    parents.reserve(prerequisites.size());
    std::unordered_set<detail::DependencyState*> seen;
    seen.reserve(prerequisites.size());
    for (const auto& prerequisite : prerequisites) {
        if (!prerequisite.state_ || prerequisite.state_->owner.lock() != identity_) {
            throw std::invalid_argument("dependency must belong to this ThreadForge scheduler");
        }
        if (seen.insert(prerequisite.state_.get()).second) {
            parents.push_back(prerequisite.state_);
        }
    }
    {
        // Holding strong references outside the lock defers callable destruction
        // until after unlocking, including failed admission and rollback paths.
        std::lock_guard lock(state_mutex_);
        if (!accepting_) {
            throw std::runtime_error("ThreadForge scheduler is shut down");
        }
        for (const auto& parent : parents) {
            if (!parent->completed) {
                // Reserve every edge before committing any registration.
                auto& dependents = parent->dependents;
                if (dependents.size() == dependents.capacity()) {
                    const auto limit = dependents.max_size();
                    if (dependents.size() == limit) {
                        throw std::length_error("too many task dependents");
                    }
                    const auto capacity = dependents.capacity();
                    const auto grown = capacity == 0 ? std::size_t{1}
                        : (capacity > limit / 2 ? limit : capacity * 2);
                    dependents.reserve(grown);
                }
                ++node->remaining;
            }
        }
        active_nodes_.emplace(node.get(), node);
        if (node->remaining == 0) {
            try {
                auto* worker = detail::Worker::current();
                if (worker && &worker->scheduler_ == this) {
                    worker->queue_.push_preserving(node->ready_task, priority);
                } else if (!queue_.push_preserving(node->ready_task, priority)) {
                    throw std::runtime_error("ThreadForge scheduler is shut down");
                }
            } catch (const std::bad_alloc&) {
                // Accepted work still has storage and can be published without
                // another allocation if an ordinary deque cannot grow.
                publish_ready_locked(*node);
            } catch (...) {
                active_nodes_.erase(node.get());
                throw;
            }
        } else {
            for (const auto& parent : parents) {
                if (!parent->completed) {
                    parent->dependents.emplace_back(node);
                }
            }
        }
        // Queue consumers cannot start until this admission lock is released.
        if (config_.enable_timing) {
            node->admitted_ns = now_ns();
            if (node->remaining == 0) {
                node->ready_ns = node->admitted_ns;
            }
        }
        if (node->remaining == 0) {
            ++tasks_ready_;
        } else {
            ++tasks_waiting_dependencies_;
        }
        peak_tasks_pending_ = std::max(peak_tasks_pending_,
            static_cast<std::uint64_t>(active_nodes_.size()));
        ++submitted_by_priority_[detail::priority_index(priority)];
        tasks_submitted_.fetch_add(1, std::memory_order_relaxed);
        ++wake_generation_;
    }
    work_available_.notify_one();
    return TaskHandle(std::move(node));
}

void Scheduler::publish_ready_locked(detail::DependencyState& node) noexcept {
    // Nodes are already owned by active_nodes_: linking an accepted descendant
    // never allocates, and remains legal after new submissions have closed.
    const auto lane = detail::priority_index(node.priority);
    if (ready_tails_[lane]) {
        ready_tails_[lane]->next_ready = &node;
    } else {
        ready_heads_[lane] = &node;
    }
    ready_tails_[lane] = &node;
}

void Scheduler::start_task(detail::DependencyState& node) noexcept {
    auto& metrics = detail::Worker::current()->metrics_;
    {
        std::lock_guard lock(state_mutex_);
        --tasks_ready_;
        ++tasks_running_;
        if (config_.enable_timing) {
            node.started_ns = now_ns();
            record_duration(metrics.total_queue_wait_ns, metrics.max_queue_wait_ns,
                            node.started_ns - node.ready_ns);
        }
    }
    metrics.tasks_started.fetch_add(1, std::memory_order_relaxed);
}

void Scheduler::record_failure() noexcept {
    detail::Worker::current()->metrics_.tasks_failed.fetch_add(1, std::memory_order_relaxed);
}

void Scheduler::complete(const std::shared_ptr<detail::DependencyState>& node) noexcept {
    auto& metrics = detail::Worker::current()->metrics_;
    if (config_.enable_timing) {
        record_duration(metrics.total_execution_ns, metrics.max_execution_ns,
                        now_ns() - node->started_ns);
    }
    bool wake = false;
    {
        std::lock_guard lock(state_mutex_);
        node->completed = true;
        --tasks_running_;
        for (const auto& dependent : node->dependents) {
            if (auto child = dependent.lock()) {
                if (--child->remaining == 0) {
                    --tasks_waiting_dependencies_;
                    ++tasks_ready_;
                    if (config_.enable_timing) {
                        child->ready_ns = now_ns();
                        total_dependency_wait_ns_ += child->ready_ns - child->admitted_ns;
                    }
                    publish_ready_locked(*child);
                    wake = true;
                }
            }
        }
        node->dependents.clear();
        active_nodes_.erase(node.get());
        metrics.tasks_by_priority[detail::priority_index(node->priority)].fetch_add(
            1, std::memory_order_relaxed);
        metrics.tasks_completed.fetch_add(1, std::memory_order_relaxed);
        if (wake || (!accepting_ && active_nodes_.empty())) {
            ++wake_generation_;
            wake = true;
        }
    }
    if (wake) {
        work_available_.notify_all();
    }
}

std::optional<Task> Scheduler::take_dependency_ready(Priority priority) {
    const auto lane = detail::priority_index(priority);
    std::lock_guard lock(state_mutex_);
    auto* node = ready_heads_[lane];
    if (!node) {
        return std::nullopt;
    }
    ready_heads_[lane] = node->next_ready;
    if (!ready_heads_[lane]) {
        ready_tails_[lane] = nullptr;
    }
    node->next_ready = nullptr;
    return std::move(node->ready_task);
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
        if (!accepting_ && active_nodes_.empty()) {
            return std::nullopt;
        }
        worker.metrics_.waits.fetch_add(1, std::memory_order_relaxed);
        const auto wait_started = config_.enable_timing ? now_ns() : 0;
        work_available_.wait(lock, [&] {
            return wake_generation_ != observed_generation || (!accepting_ && active_nodes_.empty());
        });
        if (config_.enable_timing) {
            worker.metrics_.total_idle_wait_ns.fetch_add(now_ns() - wait_started,
                                                        std::memory_order_relaxed);
        }
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
    if (auto task = take_dependency_ready(priority)) {
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
    result.timing_enabled = config_.enable_timing;
    {
        std::lock_guard lock(state_mutex_);
        result.tasks_submitted = tasks_submitted_.load(std::memory_order_relaxed);
        result.submitted_by_priority = submitted_by_priority_;
        result.tasks_ready = tasks_ready_;
        result.tasks_running = tasks_running_;
        result.tasks_pending = active_nodes_.size();
        result.tasks_waiting_dependencies = tasks_waiting_dependencies_;
        result.peak_tasks_pending = peak_tasks_pending_;
        result.total_dependency_wait_ns = total_dependency_wait_ns_;
    }
    result.workers.reserve(workers_.size());
    for (const auto& worker : workers_) {
        const auto snapshot = worker->metrics_.snapshot();
        result.local_tasks += snapshot.local_tasks;
        result.global_tasks += snapshot.global_tasks;
        result.tasks_stolen += snapshot.tasks_stolen;
        result.failed_steal_attempts += snapshot.failed_steal_attempts;
        result.waits += snapshot.waits;
        result.tasks_started += snapshot.tasks_started;
        result.tasks_completed += snapshot.tasks_completed;
        result.tasks_failed += snapshot.tasks_failed;
        result.total_execution_ns += snapshot.total_execution_ns;
        result.max_execution_ns = std::max(result.max_execution_ns, snapshot.max_execution_ns);
        result.total_queue_wait_ns += snapshot.total_queue_wait_ns;
        result.max_queue_wait_ns = std::max(result.max_queue_wait_ns, snapshot.max_queue_wait_ns);
        result.total_idle_wait_ns += snapshot.total_idle_wait_ns;
        for (std::size_t lane = 0; lane < detail::priority_count; ++lane) {
            result.tasks_by_priority[lane] += snapshot.tasks_by_priority[lane];
        }
        result.workers.push_back(snapshot);
    }
    return result;
}

} // namespace threadforge
