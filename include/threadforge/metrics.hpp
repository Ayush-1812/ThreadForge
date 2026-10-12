#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <iosfwd>
#include <vector>

namespace threadforge {

struct WorkerMetricsSnapshot {
    std::uint64_t local_tasks = 0;
    std::uint64_t global_tasks = 0;
    std::uint64_t tasks_stolen = 0;
    std::uint64_t failed_steal_attempts = 0;
    std::uint64_t waits = 0;
    std::uint64_t tasks_started = 0;
    std::uint64_t tasks_completed = 0;
    std::uint64_t tasks_failed = 0;
    std::uint64_t total_execution_ns = 0;
    std::uint64_t max_execution_ns = 0;
    std::uint64_t total_queue_wait_ns = 0;
    std::uint64_t max_queue_wait_ns = 0;
    std::uint64_t total_idle_wait_ns = 0;
    // Completed tasks, indexed High / Normal / Low.
    std::array<std::uint64_t, 3> tasks_by_priority{};
};

struct MetricsSnapshot {
    std::uint64_t tasks_submitted = 0;
    std::uint64_t local_tasks = 0;
    std::uint64_t global_tasks = 0;
    std::uint64_t tasks_stolen = 0;
    std::uint64_t failed_steal_attempts = 0;
    std::uint64_t waits = 0;
    std::uint64_t tasks_started = 0;
    std::uint64_t tasks_completed = 0;
    std::uint64_t tasks_failed = 0;
    std::uint64_t total_execution_ns = 0;
    std::uint64_t max_execution_ns = 0;
    std::uint64_t total_queue_wait_ns = 0;
    std::uint64_t max_queue_wait_ns = 0;
    std::uint64_t total_idle_wait_ns = 0;
    std::array<std::uint64_t, 3> tasks_by_priority{};
    std::array<std::uint64_t, 3> submitted_by_priority{};
    // These gauges are sampled together under the scheduler state lock.
    std::uint64_t tasks_ready = 0;
    std::uint64_t tasks_running = 0;
    std::uint64_t tasks_pending = 0;
    std::uint64_t tasks_waiting_dependencies = 0;
    std::uint64_t peak_tasks_pending = 0;
    std::uint64_t total_dependency_wait_ns = 0;
    bool timing_enabled = false;
    // Indexed by the stable worker index, from zero to worker_count() - 1.
    std::vector<WorkerMetricsSnapshot> workers;
};

// Human-readable totals and worker distribution. Does not change stream flags.
void write_metrics(std::ostream& output, const MetricsSnapshot& metrics);

namespace detail {

struct WorkerMetrics {
    std::atomic<std::uint64_t> local_tasks{0};
    std::atomic<std::uint64_t> global_tasks{0};
    std::atomic<std::uint64_t> tasks_stolen{0};
    std::atomic<std::uint64_t> failed_steal_attempts{0};
    std::atomic<std::uint64_t> waits{0};
    std::atomic<std::uint64_t> tasks_started{0};
    std::atomic<std::uint64_t> tasks_completed{0};
    std::atomic<std::uint64_t> tasks_failed{0};
    std::atomic<std::uint64_t> total_execution_ns{0};
    std::atomic<std::uint64_t> max_execution_ns{0};
    std::atomic<std::uint64_t> total_queue_wait_ns{0};
    std::atomic<std::uint64_t> max_queue_wait_ns{0};
    std::atomic<std::uint64_t> total_idle_wait_ns{0};
    std::array<std::atomic<std::uint64_t>, 3> tasks_by_priority{};

    [[nodiscard]] WorkerMetricsSnapshot snapshot() const noexcept;
};

} // namespace detail
} // namespace threadforge
