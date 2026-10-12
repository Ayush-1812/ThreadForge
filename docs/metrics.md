# Runtime metrics: Phase 5

`scheduler.metrics()` returns a `threadforge::MetricsSnapshot` with aggregate
counters and one `WorkerMetricsSnapshot` per configured worker. The `workers`
vector uses stable worker indices from zero to `worker_count() - 1`.

Counters are available by default. Duration collection is optional because clock
reads add overhead to small tasks:

```cpp
threadforge::Config config;
config.worker_count = 4;
config.enable_timing = true;
threadforge::Scheduler scheduler(config);

auto result = scheduler.submit([] { return 42; });
result.get();
scheduler.shutdown();
threadforge::write_metrics(std::cout, scheduler.metrics());
```

Include `<threadforge/scheduler.hpp>`, `<threadforge/metrics.hpp>`, and
`<iostream>` for this example. Timing is disabled by default and cannot be
changed after constructing the scheduler. `timing_enabled` identifies the
snapshot's configuration; duration fields remain zero when timing is disabled.
[`examples/metrics_report.cpp`](../examples/metrics_report.cpp) provides a runnable report.

## Task counts and current work

| Aggregate field | Meaning |
| --- | --- |
| `tasks_submitted` | Tasks accepted once, including tasks waiting on dependencies |
| `tasks_started` | Tasks whose execution has begun |
| `tasks_completed` | Executions that finished, including executions that threw |
| `tasks_failed` | Completed executions with an exception escaping callable invocation |
| `submitted_by_priority` | Accepted task counts indexed High, Normal, Low |
| `tasks_by_priority` | Completed execution counts indexed High, Normal, Low |
| `tasks_pending` | All accepted tasks that have not finished |
| `tasks_ready` | Pending tasks eligible to execute |
| `tasks_running` | Pending tasks currently executing |
| `tasks_waiting_dependencies` | Pending tasks whose prerequisites are not all complete |
| `peak_tasks_pending` | Largest number of accepted unfinished tasks observed by this scheduler |

A throwing callable counts as both completed and failed; in settled totals,
`tasks_failed` is a subset of `tasks_completed`. Its exception is delivered
through its result future. A failed prerequisite releases completion-order dependents, which get
their own execution and failure classification. Rejected submissions do not add
to task counts, and making an accepted descendant ready does not count as a second
submission.

Failure accounting surrounds callable invocation. An exception while storing a
returned value in the packaged task's result state can make the future
exceptional without incrementing `tasks_failed`.

These state fields describe scheduler work, rather than the number of nodes
retained by application handles. A handle can outlive its completed task without
contributing to pending work. Dependency waiters do not occupy workers.

## Worker distribution and waiting

| Aggregate and per-worker field | Meaning |
| --- | --- |
| `local_tasks` | Claims from the executing worker's own deque |
| `global_tasks` | Claims from the injection queue or intrusive dependency-ready lists |
| `tasks_stolen` | Claims from another worker's deque, credited to the thief |
| `failed_steal_attempts` | Victim-priority-lane probes that found no task |
| `waits` | Entries into the worker condition-variable wait |
| `tasks_started`, `tasks_completed`, `tasks_failed` | The executing worker's task lifecycle counts |
| `tasks_by_priority` | The executing worker's completed counts indexed High, Normal, Low |

Each executed task has one local, global, or stolen classification. Dependency
promotion uses the intrusive global ready lists, so a dependency-ready task may
run on any available worker without being classified as stolen. Steal counts
therefore do not measure all redistribution of work.

A failed steal counts one unsuccessful lane probe. An empty victim can contribute
three failures to a complete scan, and the current priority policy can probe a
higher lane before finding work in a lower lane. A one-worker scheduler has no
victim probes. `waits` counts wait episodes; it does not measure elapsed idle time
or spin iterations.

The per-worker distribution helps identify uneven completed work. Equal task
counts do not imply equal CPU work when task durations differ, and a larger steal
count does not itself establish better throughput.

## Optional durations

When enabled, timing uses a monotonic clock and stores integer nanoseconds.

| Aggregate field | Interval and aggregation |
| --- | --- |
| `total_execution_ns` | Sum of execution intervals across completed tasks |
| `max_execution_ns` | Largest completed task execution interval |
| `total_queue_wait_ns` | Sum of intervals from task readiness to execution start |
| `max_queue_wait_ns` | Largest readiness-to-start interval |
| `total_idle_wait_ns` | Sum of worker condition-variable wait intervals |
| `total_dependency_wait_ns` | Sum of admission-to-readiness intervals for tasks initially waiting on prerequisites |

Workers also expose execution, queue-wait, and idle-wait duration fields. Aggregate
totals sum worker totals, while aggregate maxima select the largest worker
maximum. Dependency wait belongs to the scheduler because a waiting task has not
yet been assigned to an executing worker.

Queue wait excludes time waiting for prerequisites. A task submitted with no
unsatisfied prerequisites starts its ready interval at admission; a waiting
descendant starts that interval when its final prerequisite completes. Dependency
wait and queue wait therefore describe different delays.

Execution duration runs from the scheduler's execution-start timestamp through
typed-future publication, including wrapper overhead and the callable's own
blocking waits. It excludes the subsequent dependency-release lock and
bookkeeping. Idle duration covers condition-variable waits, including waking and
reacquiring the state mutex, rather than every period when a worker is outside a
task. It is not an operating-system CPU utilization measurement. Totals across
concurrent workers can exceed the program's wall time, and clock resolution
limits very short intervals.

For a stable final snapshot with timing enabled and completed work, an average
execution interval can be calculated as `total_execution_ns / tasks_completed`.
Maximum intervals identify outliers; averages alone do not describe a latency
distribution. No percentile histogram is collected.

## Reading a snapshot or report

`metrics()` is safe during submission, execution, and external shutdown while the
scheduler remains alive. Worker counters are observational reads, so a live
snapshot can combine different instants. It is not a synchronization barrier for
task data. A result future may become ready just before the scheduler finishes
the corresponding completion bookkeeping.

The ready, running, pending, and dependency-waiting gauges are sampled together
under the state mutex, so `tasks_pending == tasks_ready + tasks_running +
tasks_waiting_dependencies` holds within a snapshot. Worker lifecycle counters
are sampled separately and may not describe that same instant. A task that has
been claimed but has not reached its execution-start bookkeeping still belongs
to the ready gauge.

Call `shutdown()` before relying on settled totals. After it returns:

```text
tasks_submitted == tasks_started == tasks_completed
tasks_completed == local_tasks + global_tasks + tasks_stolen
tasks_pending == tasks_ready == tasks_running == tasks_waiting_dependencies == 0
sum(submitted_by_priority) == sum(tasks_by_priority) == tasks_submitted
```

`write_metrics(output, snapshot)` writes a readable aggregate and per-worker
report to an `std::ostream`; taking or printing a live snapshot does not wait for
tasks. Reports are telemetry, not performance benchmarks. Reproducible wall-time
comparisons and their timing boundaries are documented in
[benchmarks.md](benchmarks.md).

The metrics example measures one batch from submission through `shutdown()`,
excluding pool construction. Its throughput includes the fan-out parent as a
completed task. The Google Benchmark suite instead excludes final shutdown and
reports child-task throughput. Those different boundaries must be kept with the
reported values.
