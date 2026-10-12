# Architecture: Phase 2

ThreadForge combines a global injection queue with one mutex-protected deque per
worker. External submissions enter the global FIFO queue. Submissions from a
worker of the same scheduler enter that worker's local queue. Submitting to a
different scheduler counts as an external submission for the receiving scheduler.

## Components and ownership

`Scheduler` owns one `TaskQueue` and a fixed vector of `detail::Worker` objects.
Each worker owns a `WorkStealingQueue`, counters, and one `std::jthread`; no thread
is detached. The entire worker registry is created before starting any threads,
so concurrent steals never inspect a vector being modified. Queue and waiting
state outlive workers, and each worker's queue outlives its thread. Worker addresses
remain stable because each worker is owned by a `std::unique_ptr`.

`submit()` creates a typed `std::packaged_task<Result()>`, returns its future, and
moves that task inside an erased `std::packaged_task<void()>`. A queued task has
one owner and is invoked once. Move-only captures and arguments need no shared
callable allocation or copyable wrapper. Both packaged tasks have future shared
state, which incurs allocation overhead; this is a correctness baseline and can
be measured before revisiting type erasure.

The worker removes a task while holding the queue lock, then executes it after
releasing the lock. Arbitrary user code never executes under scheduler locks.
Callable exceptions are captured by the typed packaged task and rethrown by the
returned future's `get()`.

## Synchronization

| Shared state | Synchronization |
| --- | --- |
| Global queue contents and closed flag | Global queue mutex |
| Each local deque | Its own queue mutex |
| Admission and wake generation | Scheduler state mutex |
| Sleeping workers | Scheduler condition variable with a generation predicate |
| Worker joins | Scheduler shutdown mutex serializes external callers |
| Results and task exceptions | Standard future shared state |
| Worker identity | Thread-local worker pointer, accessed only by the current thread |
| Counters | Per-worker relaxed atomics; submitted count uses a relaxed atomic |
| Victim cursor | Accessed only by its owning worker |
| Worker count | Immutable configuration after construction |

## Scheduling and waiting

A worker tries its own deque, then the global FIFO queue, then every other worker's
deque before sleeping. The owner pushes and pops the front (LIFO), while a thief
pops the back (oldest first). Victim selection rotates between scans and successful
steals. A stolen task executes directly rather than moving between ready queues.

The worker snapshots a wake generation under the state mutex before scanning.
Every accepted publication increments that generation under the same mutex and
notifies one sleeper, including publications to a local queue. After an unsuccessful
scan the worker takes the state mutex again. If the generation changed, it rescans.
Otherwise it either exits after closure or waits for a generation change/closure.
This protects the gap between the scan and entering a wait, without timed polling
or an indefinite spin loop. Spurious notifications do not satisfy the predicate.

Admission holds the state mutex while inserting into one queue; consumers release
the queue mutex before taking the state mutex. The lock order has no reverse edge.
The shared admission lock is a correctness tradeoff: local queues distribute queue
access, but task publication still has a brief serialized section.

## Shutdown protocol

1. Reject shutdown from a worker of the same scheduler before taking the join lock.
2. Serialize external shutdown callers using the shutdown mutex.
3. Close admission under the state mutex, advance the generation, and notify all workers.
4. Workers drain all queues and exit after a full unsuccessful scan with no generation change.
5. Join every worker before returning from `shutdown()`.

Publication and closure share the state mutex. Publication before closure succeeds
and its task must be drained; publication after closure is rejected, including
nested worker submissions. Generation changes force a rescan if publication raced
an earlier part of a scan. Once admission is closed, no new queue entries can appear
and thieves only remove work, so an unsuccessful scan is sufficient for worker
exit. No running-task counter is required: joining every worker also waits for tasks
already removed from queues to finish.

Self-shutdown is checked before taking the shutdown mutex because an external
shutdown caller might already hold that mutex while joining the current worker.
Taking the mutex first would create a circular wait. `is_shutdown()` observes the
admission state, not completion of all joins.

If thread creation throws during construction, the constructor closes the queue
before existing workers are destroyed and joined. The destructor uses the same
graceful shutdown protocol as an explicit call.

## Boundaries and tradeoffs

The global FIFO and local LIFO policies describe selection order; concurrent
execution and completion order are unspecified. Always checking local work first
can delay global work during continuous nested submissions. Priorities and fairness
remain Phase 3 work. Unbounded admission can exhaust memory, and the shared
publication gate and queue mutexes can become contention points. No lock-free
algorithm or performance advantage is claimed without measurement.

`metrics()` aggregates submitted tasks, local/global claims, successful steals,
failed victim probes, and wait episodes. The per-worker counters use relaxed atomics
and do not establish synchronization. A live snapshot can mix different instants;
after shutdown the counters are stable and every accepted task has exactly one
local/global/stolen classification. Successful steals are charged to the thief.
Failed probes include empty victim queues; no self-probe is counted. Wait episodes
count entries into a condition-variable wait, not elapsed idle time.

Tasks must terminate for graceful shutdown to finish. The runtime does not forcibly
interrupt user code. Blocking all workers on futures for work submitted to their
own scheduler can deadlock. A later DAG scheduler will release dependency-ready
tasks without occupying a waiting worker.

The scheduler must outlive its tasks and any concurrent API users. Destruction
from one of its own tasks is unsupported. Cycles involving tasks, external locks,
or shutdown of multiple schedulers remain the application's responsibility.

## Repository evolution

Phase 2 adds `work_stealing_queue.hpp/.cpp`, `metrics.hpp/.cpp`, and dedicated
`stealing_test.cpp` coverage. The Phase 1 queue retains its blocking API for direct
consumers and tests; runtime workers use its nonblocking `try_pop()` and the
scheduler-wide waiting protocol. No empty priority or dependency classes exist.
