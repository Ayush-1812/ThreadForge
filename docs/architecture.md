# Architecture: Phase 5

ThreadForge combines a global injection queue with one mutex-protected queue per
worker. Every queue has separate High, Normal, and Low lanes. External submissions
enter the global queue, which is FIFO within each lane. Submissions from a
worker of the same scheduler enter that worker's local queue. Submitting to a
different scheduler counts as an external submission for the receiving scheduler.
Accepted tasks whose prerequisites have not completed remain outside the ready
queues. Completion releases them into separate intrusive priority ready lists.

## Components and ownership

`Scheduler` owns one `TaskQueue` and a fixed vector of `detail::Worker` objects.
Each worker owns a `WorkStealingQueue`, counters, and one `std::jthread`; no thread
is detached. The entire worker registry is created before starting any threads,
so concurrent steals never inspect a vector being modified. Queue and waiting
state outlive workers, and each worker's queue outlives its thread. Worker addresses
remain stable because each worker is owned by a `std::unique_ptr`.

`submit_task()` creates a typed `std::packaged_task<Result()>`, places it inside
an erased `std::packaged_task<void()>`, and wraps invocation with dependency
completion bookkeeping. It returns `TaskSubmission<Result>` with a result future
and a copyable `TaskHandle`. `submit()` uses the same path and returns only the
future. Move-only captures and arguments retain their single-invocation semantics.
Packaged-task shared states and dependency nodes add allocation costs that can
be measured before revisiting the representation.

The scheduler owns an active-node map for every accepted task, including ordinary
`submit()` calls. It retains blocked, queued, and running tasks independently of
caller-held handles and futures. Handles retain their node and refer weakly to a
scheduler identity; `valid()` is false for empty tokens or an expired owner and
remains true after shutdown while the scheduler lives. Admission validates the
owner identity and deduplicates prerequisite nodes.

Registration and completion share the state mutex. Registration reserves edge
storage before committing acceptance and counts only unfinished prerequisites.
The typed future becomes ready before completion marks the node finished and
decrements its children's remaining counts. Each child is released once when its
count reaches zero, including after a parent throws. Completion removes the
finished node from the active map while retaining a strong reference until after
unlocking; user callable destruction therefore occurs outside scheduler locks.
Dependencies only refer to already-accepted tasks, so creation-only edges cannot
form a cycle. See [Dependency scheduling](dependencies.md) for public contracts.

The worker removes a task while holding the queue lock, then executes it after
releasing the lock. Arbitrary user code never executes under scheduler locks.
Callable exceptions are captured by the typed packaged task and rethrown by the
returned future's `get()`.

## Synchronization

| Shared state | Synchronization |
| --- | --- |
| Global priority lanes and closed flag | Global queue mutex |
| Each local queue's three priority lanes | Its own queue mutex |
| Admission and wake generation | Scheduler state mutex |
| Active nodes, completion flags, remaining counts, dependency registration | Scheduler state mutex |
| Intrusive priority ready lists | Scheduler state mutex |
| Sleeping workers | Scheduler condition variable with a generation predicate |
| Worker joins | Scheduler shutdown mutex serializes external callers |
| Results and task exceptions | Standard future shared state |
| Worker identity | Thread-local worker pointer, accessed only by the current thread |
| Worker counts and duration accumulators | Per-worker relaxed atomics, written by their owning worker |
| Pending/ready/running/dependency gauges, submission priorities, dependency durations | Scheduler state mutex |
| Submitted count | Relaxed atomic, sampled with admission fields under the state mutex |
| Victim and priority cursors | Accessed only by their owning worker |
| Worker count and optional timing configuration | Immutable configuration after construction |

## Scheduling and waiting

A worker chooses a priority class before selecting a source. For that class it
tries its own lane, the global injection lane, the intrusive ready list, then
every other worker's lane. Only after all those sources yield no task does it try
the next class. The owner
pushes and pops the front (LIFO) within a lane, while a thief pops the back (oldest
first). Victim selection rotates between class scans and successful steals. A
stolen task executes directly rather than moving between ready queues.

Ready tasks submitted without unfinished prerequisites normally retain local or
global routing. The queue insertion helpers preserve the task if a deque cannot
allocate; admission then links it to the intrusive ready list instead. Releasing
an already-accepted dependent also uses this allocation-free path. The injection
lane and intrusive lane are individually FIFO, with no FIFO order spanning them.
Intrusive-list claims count as global work.

Each worker has a service cursor over a fixed ring of eight High slots, four
Normal slots, and one Low slot, initially pointing to High. A scan tries the
three distinct classes in first-occurrence order from that cursor. A successful
claim advances to the slot after the chosen class's first occurrence; empty
classes are skipped, and an unsuccessful scan does not advance the cursor.
With all classes continually claimable, each worker serves 8 High, 4 Normal, and
1 Low task per 13 claims. This deliberately allows lower-priority service while
High work is queued. The full policy and its conditional fairness limits are in
[Priority scheduling](priorities.md).

There is no atomic scan across queues. Publication and claims by other workers
can change readiness during selection, so the policy does not impose a total
priority ordering over concurrent execution.

The worker snapshots a wake generation under the state mutex before scanning.
Every accepted submission increments that generation under the same mutex and
notifies one sleeper, including publications to a local queue. Completion that
releases dependents increments the generation and notifies all sleepers, as does
the last completion after closure. After an unsuccessful scan the worker takes
the state mutex again. If the generation changed, it rescans. Otherwise it exits
only after closure with an empty active-node map, or waits for a generation
change or that termination condition. Closed admission alone is not a satisfied
wait predicate while accepted tasks remain, so draining does not spin.
This protects the gap between the scan and entering a wait, without timed polling
or an indefinite spin loop. Spurious notifications do not satisfy the predicate.

Admission holds the state mutex while inserting into one queue; consumers release
the queue mutex before taking the state mutex. The lock order has no reverse edge.
The shared state lock is a correctness tradeoff: local queues distribute queue
access, but admission, dependency registration, readiness promotion, and completion
bookkeeping remain serialized.

Each invocation starts by moving one task from the ready gauge to the running
gauge under the state mutex. The executing worker counts its start, then invokes
the typed packaged task. A catch around the callable records an escaping
exception before rethrowing it to the packaged task's result state. Completion
updates the worker's completed and priority counts, removes the running task,
and promotes eligible descendants. Throwing executions are also completed;
failure counts are a subset rather than a separate terminal category.

Optional monotonic-clock timing records admission, readiness, and execution start
on the node. Queue wait measures readiness to execution start, while dependency
wait measures admission to readiness for an initially blocked task. Execution
timing ends before acquiring the dependency-completion lock. Worker wait timing
covers the condition-variable wait interval. With `Config::enable_timing` false,
these clock reads are skipped and counters remain available.

## Shutdown protocol

1. Reject shutdown from a worker of the same scheduler before taking the join lock.
2. Serialize external shutdown callers using the shutdown mutex.
3. Close admission and the injection queue under the state mutex, advance the generation, and notify all workers.
4. Drain ready tasks, release accepted children as prerequisites finish, and wait while active tasks remain.
5. Workers exit after a full unsuccessful scan with no generation change and an empty active-node map.
6. Join every worker before returning from `shutdown()`.

New submission and closure share the state mutex. Admission before closure succeeds
and its task must be drained; new submissions after closure are rejected, including
nested worker submissions. Generation changes force a rescan if publication raced
an earlier part of a scan. Closure rejects new task acceptance but completion can
still publish already-accepted children to the intrusive ready lists. An empty
scan alone is therefore insufficient for worker exit. The active-node map tracks
unfinished accepted work; workers can exit once it is empty and admission is
closed. Joining every worker also waits for invocation and cleanup to return.

Self-shutdown is checked before taking the shutdown mutex because an external
shutdown caller might already hold that mutex while joining the current worker.
Taking the mutex first would create a circular wait. `is_shutdown()` observes the
admission state, not completion of all joins.

If thread creation throws during construction, the constructor closes the queue
before existing workers are destroyed and joined. The destructor uses the same
graceful shutdown protocol as an explicit call.

## Boundaries and tradeoffs

The per-source, per-lane FIFO and local LIFO policies describe selection order;
concurrent execution and completion order are unspecified. Weighted service gives
priority classes opportunities under sustained load, without an elapsed-time
bound or fairness guarantee for individual tasks. Local LIFO can postpone old
local tasks, and local-source preference can delay global or victim tasks of the
same class indefinitely. Priorities are explicit, default to Normal, and are not
inherited by nested submissions. Unbounded admission can exhaust memory, and the
shared state lock and queue mutexes can become contention points. Continuous
injection work can postpone intrusive-ready tasks of the same class. No lock-free
algorithm or performance advantage is claimed without measurement.

`metrics()` aggregates task starts, completions, callable failures, submission and
completion priorities, queue-source claims, victim-priority-lane probes, and wait
episodes. It also samples pending, ready, running, and dependency-waiting gauges
together under the state lock, preserving their partition of unfinished work.
Per-worker counters use relaxed atomic reads and do not establish synchronization.
A live snapshot can combine those gauges with worker counts from different
instants; after shutdown every accepted task has a settled completion and exactly
one local/global/stolen classification. Successful steals are charged to the
thief. Submitted totals include accepted blocked children; global claims include
injection and intrusive ready-list tasks.

An empty victim can produce three failed probes in a full scan, one per priority
lane; no self-probe is counted. Wait episodes count entries into a
condition-variable wait rather than elapsed idle time. Optional durations sum
execution, ready-queue, dependency, and idle-wait intervals; aggregate execution
and queue maxima select the largest worker maximum. Concurrent execution totals
are not program wall time or CPU utilization. Full definitions are in
[Runtime metrics](metrics.md).

Tasks must terminate for graceful shutdown to finish. The runtime does not forcibly
interrupt user code. Blocking all workers on futures for work submitted to their
own scheduler can deadlock. Declared prerequisites let the dependency scheduler
release ready tasks without occupying a waiting worker, but arbitrary future
waits do not gain cooperative execution.

The scheduler must outlive its tasks and any concurrent API users. Destruction
from one of its own tasks is unsupported. Cycles involving tasks, external locks,
or shutdown of multiple schedulers remain the application's responsibility.

## Repository evolution

Phase 5 extends lifecycle counters, priority totals, pending-work gauges, optional
durations, and an ostream report. Dedicated `metrics_test.cpp` coverage verifies
gated transitions, exception accounting, timing configuration, worker aggregates,
and concurrent snapshots. `examples/metrics_report.cpp` prints a settled report.
The optional Google Benchmark target compares sequential work, a centralized
FIFO pool, and ThreadForge's uniform and uneven fan-out workloads; its methodology
is in [Scheduler benchmarks](benchmarks.md).

Dependency handles, active task ownership, intrusive ready lists, priority lanes,
weighted service, and work stealing remain in place. The global queue retains its
blocking API for direct consumers and tests; runtime workers use priority-specific
nonblocking probes and the scheduler-wide waiting protocol.
