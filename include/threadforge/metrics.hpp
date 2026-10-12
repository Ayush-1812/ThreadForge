#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace threadforge {

struct WorkerMetricsSnapshot {
    std::uint64_t local_tasks = 0;
    std::uint64_t global_tasks = 0;
    std::uint64_t tasks_stolen = 0;
    std::uint64_t failed_steal_attempts = 0;
    std::uint64_t waits = 0;
};

struct MetricsSnapshot {
    std::uint64_t tasks_submitted = 0;
    std::uint64_t local_tasks = 0;
    std::uint64_t global_tasks = 0;
    std::uint64_t tasks_stolen = 0;
    std::uint64_t failed_steal_attempts = 0;
    std::uint64_t waits = 0;
    // Indexed by the stable worker index, from zero to worker_count() - 1.
    std::vector<WorkerMetricsSnapshot> workers;
};

namespace detail {

struct WorkerMetrics {
    std::atomic<std::uint64_t> local_tasks{0};
    std::atomic<std::uint64_t> global_tasks{0};
    std::atomic<std::uint64_t> tasks_stolen{0};
    std::atomic<std::uint64_t> failed_steal_attempts{0};
    std::atomic<std::uint64_t> waits{0};

    [[nodiscard]] WorkerMetricsSnapshot snapshot() const noexcept;
};

} // namespace detail
} // namespace threadforge
