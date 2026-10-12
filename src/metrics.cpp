#include "threadforge/metrics.hpp"

#include <ostream>

namespace threadforge::detail {

WorkerMetricsSnapshot WorkerMetrics::snapshot() const noexcept {
    // Counters provide telemetry only, not publication or a synchronization barrier.
    WorkerMetricsSnapshot result;
    result.local_tasks = local_tasks.load(std::memory_order_relaxed);
    result.global_tasks = global_tasks.load(std::memory_order_relaxed);
    result.tasks_stolen = tasks_stolen.load(std::memory_order_relaxed);
    result.failed_steal_attempts = failed_steal_attempts.load(std::memory_order_relaxed);
    result.waits = waits.load(std::memory_order_relaxed);
    result.tasks_started = tasks_started.load(std::memory_order_relaxed);
    result.tasks_completed = tasks_completed.load(std::memory_order_relaxed);
    result.tasks_failed = tasks_failed.load(std::memory_order_relaxed);
    result.total_execution_ns = total_execution_ns.load(std::memory_order_relaxed);
    result.max_execution_ns = max_execution_ns.load(std::memory_order_relaxed);
    result.total_queue_wait_ns = total_queue_wait_ns.load(std::memory_order_relaxed);
    result.max_queue_wait_ns = max_queue_wait_ns.load(std::memory_order_relaxed);
    result.total_idle_wait_ns = total_idle_wait_ns.load(std::memory_order_relaxed);
    for (std::size_t lane = 0; lane < result.tasks_by_priority.size(); ++lane) {
        result.tasks_by_priority[lane] = tasks_by_priority[lane].load(std::memory_order_relaxed);
    }
    return result;
}

} // namespace threadforge::detail

namespace threadforge {

void write_metrics(std::ostream& output, const MetricsSnapshot& metrics) {
    output << "Tasks: submitted=" << metrics.tasks_submitted
           << " started=" << metrics.tasks_started << " completed=" << metrics.tasks_completed
           << " failed=" << metrics.tasks_failed << '\n'
           << "Pending: total=" << metrics.tasks_pending << " ready=" << metrics.tasks_ready
           << " running=" << metrics.tasks_running
           << " dependencies=" << metrics.tasks_waiting_dependencies
           << " peak=" << metrics.peak_tasks_pending << '\n'
           << "Sources: local=" << metrics.local_tasks << " global=" << metrics.global_tasks
           << " stolen=" << metrics.tasks_stolen
           << " failed_steals=" << metrics.failed_steal_attempts << " waits=" << metrics.waits << '\n'
           << "Priority submitted H/N/L: " << metrics.submitted_by_priority[0] << '/'
           << metrics.submitted_by_priority[1] << '/' << metrics.submitted_by_priority[2] << '\n'
           << "Priority completed H/N/L: " << metrics.tasks_by_priority[0] << '/'
           << metrics.tasks_by_priority[1] << '/' << metrics.tasks_by_priority[2] << '\n';
    if (metrics.timing_enabled) {
        output << "Timing (ns): execution_total=" << metrics.total_execution_ns
               << " execution_max=" << metrics.max_execution_ns
               << " execution_average=" << (metrics.tasks_completed == 0 ? 0 :
                   metrics.total_execution_ns / metrics.tasks_completed)
               << " queue_total=" << metrics.total_queue_wait_ns
               << " queue_max=" << metrics.max_queue_wait_ns
               << " queue_average=" << (metrics.tasks_started == 0 ? 0 :
                   metrics.total_queue_wait_ns / metrics.tasks_started)
               << " dependency_total=" << metrics.total_dependency_wait_ns
               << " idle_total=" << metrics.total_idle_wait_ns << '\n';
    } else {
        output << "Timing: disabled\n";
    }
    for (std::size_t index = 0; index < metrics.workers.size(); ++index) {
        const auto& worker = metrics.workers[index];
        output << "Worker " << index << ": started=" << worker.tasks_started
               << " completed=" << worker.tasks_completed << " failed=" << worker.tasks_failed
               << " local=" << worker.local_tasks << " global=" << worker.global_tasks
               << " stolen=" << worker.tasks_stolen
               << " failed_steals=" << worker.failed_steal_attempts << " waits=" << worker.waits;
        if (metrics.timing_enabled) {
            output << " execution_ns=" << worker.total_execution_ns
                   << " execution_max_ns=" << worker.max_execution_ns
                   << " execution_average_ns=" << (worker.tasks_completed == 0 ? 0 :
                       worker.total_execution_ns / worker.tasks_completed)
                   << " queue_ns=" << worker.total_queue_wait_ns
                   << " queue_max_ns=" << worker.max_queue_wait_ns
                   << " queue_average_ns=" << (worker.tasks_started == 0 ? 0 :
                       worker.total_queue_wait_ns / worker.tasks_started)
                   << " idle_ns=" << worker.total_idle_wait_ns;
        }
        output << " priority_H/N/L=" << worker.tasks_by_priority[0] << '/'
               << worker.tasks_by_priority[1] << '/' << worker.tasks_by_priority[2] << '\n';
    }
}

} // namespace threadforge
