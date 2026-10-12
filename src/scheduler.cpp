#include "threadforge/scheduler.hpp"
#include "threadforge/worker.hpp"

#include <new>
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
};

} // namespace threadforge::detail

namespace threadforge {

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
        // The typed future is ready before the completion event releases children.
        inner();
        complete(weak_node.lock());
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
                parent->dependents.reserve(parent->dependents.size() + 1);
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

void Scheduler::complete(const std::shared_ptr<detail::DependencyState>& node) noexcept {
    bool wake = false;
    {
        std::lock_guard lock(state_mutex_);
        node->completed = true;
        for (const auto& dependent : node->dependents) {
            if (auto child = dependent.lock()) {
                if (--child->remaining == 0) {
                    publish_ready_locked(*child);
                    wake = true;
                }
            }
        }
        node->dependents.clear();
        active_nodes_.erase(node.get());
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
        work_available_.wait(lock, [&] {
            return wake_generation_ != observed_generation || (!accepting_ && active_nodes_.empty());
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
