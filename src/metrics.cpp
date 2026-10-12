#include "threadforge/metrics.hpp"

namespace threadforge::detail {

WorkerMetricsSnapshot WorkerMetrics::snapshot() const noexcept {
    // Counters provide telemetry only, not publication or a synchronization barrier.
    return {
        local_tasks.load(std::memory_order_relaxed),
        global_tasks.load(std::memory_order_relaxed),
        tasks_stolen.load(std::memory_order_relaxed),
        failed_steal_attempts.load(std::memory_order_relaxed),
        waits.load(std::memory_order_relaxed)
    };
}

} // namespace threadforge::detail
