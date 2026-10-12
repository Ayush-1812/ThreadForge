# Architecture: Phase 1

ThreadForge currently uses a centralized queue. This milestone establishes task
ownership, exception safety, worker lifetime, and a reliable shutdown boundary.
Work stealing and priorities are intentionally scheduled for later milestones.

## Components and ownership

`Scheduler` owns one `TaskQueue` and a fixed vector of `detail::Worker` objects.
Each worker owns one `std::jthread`; no thread is detached. The queue is declared
before the workers, so it is destroyed after them. Worker addresses remain stable
because each worker is separately owned by a `std::unique_ptr`.

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
| Queue contents and closed flag | One queue mutex |
| Work arrival and queue closure | Condition variable with a protected predicate |
| Worker joins | Scheduler shutdown mutex serializes external callers |
| Results and task exceptions | Standard future shared state |
| Worker identity | Thread-local queue pointer, accessed only by the current thread |
| Worker count | Immutable configuration after construction |

An idle worker waits on `closed || !tasks.empty()`. This predicate handles spurious
wakeups and notifications arriving before the worker enters its wait. Submission
notifies one waiter; closure notifies every waiter. There is no polling loop in
the runtime.

## Shutdown protocol

1. Reject shutdown from a worker of the same scheduler before taking the join lock.
2. Serialize external shutdown callers using the shutdown mutex.
3. Set the queue's closed flag under its mutex and notify all workers.
4. Workers drain the queue until it is closed and empty, then return.
5. Join every worker before returning from `shutdown()`.

Queue insertion and closure use the same mutex. Insertion before closure succeeds
and its task must be drained. Insertion after closure is rejected. The protocol
does not need a second accepting flag or an unsynchronized pending-work counter.
An empty closed queue can cause an idle worker to exit while another worker is
still running a task; joining all workers guarantees running tasks finish too.

Self-shutdown is checked before taking the shutdown mutex because an external
shutdown caller might already hold that mutex while joining the current worker.
Taking the mutex first would create a circular wait. `is_shutdown()` observes the
closed flag, not completion of all joins.

If thread creation throws during construction, the constructor closes the queue
before existing workers are destroyed and joined. The destructor uses the same
graceful shutdown protocol as an explicit call.

## Boundaries and tradeoffs

There is one global FIFO ready queue. FIFO describes dequeue order; concurrent
execution and completion order are unspecified. The shared mutex can become a
contention point, and unbounded submission can exhaust memory. Admission control,
worker-local queues, and scheduling metrics require separate design work.

Tasks must terminate for graceful shutdown to finish. The runtime does not forcibly
interrupt user code. Blocking all workers on futures for work submitted to their
own scheduler can deadlock. A later DAG scheduler will release dependency-ready
tasks without occupying a waiting worker.

The scheduler must outlive its tasks and any concurrent API users. Destruction
from one of its own tasks is unsupported. Cycles involving tasks, external locks,
or shutdown of multiple schedulers remain the application's responsibility.

## Repository evolution

Phase 1 includes only functioning components and their tests. There are no empty
work-stealing, priority, metrics, or dependency classes. Their headers, source files,
tests, and examples will be introduced with the corresponding milestone. Queue
operations are currently small enough to keep in `task_queue.hpp`; worker and
scheduler lifecycle implementations live in source files.
