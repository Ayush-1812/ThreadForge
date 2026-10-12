# Scheduler API: Phase 4

Include `<threadforge/scheduler.hpp>` and link `threadforge::threadforge`.

## Construction

```cpp
threadforge::Scheduler automatic;
threadforge::Scheduler four_workers(4);
threadforge::Scheduler configured(threadforge::Config{4});
```

The default configuration uses the reported hardware concurrency with a one-worker
fallback. Zero workers throws `std::invalid_argument`. Thread/resource allocation
failures propagate, with already-created workers shut down before construction
unwinds. Scheduler objects are neither copyable nor movable.

`worker_count()` returns the configured count, including after shutdown.

## Submission and futures

`submit(callable, args...)` and `submit(Priority, callable, args...)` return
`std::future<Result>`, including `void` and reference result types. The first
overload uses `Priority::Normal`; the second accepts `Priority::High`, `Normal`,
or `Low`. Nested submissions default to Normal and do not inherit their parent's
priority. Invalid priority enum values throw `std::invalid_argument` before the
task is accepted. Callable and argument storage follows decayed value
semantics. The callable and arguments are moved for their single invocation;
explicit `std::ref` arguments preserve references. Callables must be invocable
with the stored argument types and their intended move/reference semantics.

```cpp
auto value = scheduler.submit([](int a, int b) { return a + b; }, 10, 20);
auto moved = scheduler.submit(
    [](std::unique_ptr<int> p) { return *p; }, std::make_unique<int>(42));
auto urgent = scheduler.submit(threadforge::Priority::High, [] { return 7; });
```

Keep captured pointers, reference arguments, and reference results valid for their
required lifetime. Submission is safe from multiple external threads and from
worker threads while the scheduler accepts work. A worker submitting to its own
scheduler normally publishes a ready task to its local deque; an external thread
or a worker belonging to a different scheduler publishes ready work to the
receiving scheduler's global injection queue. Admission can fall back to an
intrusive global ready list if a ready-task deque cannot allocate.
A task submitted during graceful draining is rejected, including nested tasks.

Each queue has three priority lanes. Workers choose a class before checking its
local lane, global injection lane, intrusive ready list, then other workers'
lanes. Within that class, owners take newer local tasks first, each global source
is FIFO independently, and thieves take older victim tasks first. There is no
FIFO guarantee spanning injection and intrusive sources. Each worker uses a
weighted ring of 8 High, 4 Normal, and 1 Low slot,
skips empty classes, and advances only on a successful claim. This provides
conditional class service opportunities rather than strict priority ordering or
fairness for individual tasks. Running tasks are not preempted. See
[Priority scheduling](priorities.md) for the cursor rules, fairness bounds, and
deterministic example.

Calling `future.get()` from a scheduler worker can deadlock if every worker waits
for queued child tasks; priority scheduling does not add cooperative waiting.

Allocation failure while preparing result state, nodes, or registration can
throw without accepting that task. Failure to grow an ordinary ready deque uses
the already-allocated node's intrusive ready link instead.
Submission after closure throws `std::runtime_error`. Exceptions thrown by an
accepted callable propagate through `future.get()`. Ignoring or destroying a
future neither blocks the submitting thread nor cancels queued work.

## Dependency submission

`submit_task(callable, args...)` and `submit_task(Priority, callable, args...)`
return `TaskSubmission<Result>` with a `std::future<Result> future` and a
`TaskHandle handle`. Existing `submit()` calls still return only a future.

`submit_after(dependencies, callable, args...)` and
`submit_after(dependencies, Priority, callable, args...)` return the same
submission structure and delay readiness until all unique prerequisites finish.
Dependencies accept `std::span<const TaskHandle>` or an initializer list.
Omitted priorities default to Normal, and empty dependencies submit ready work.

```cpp
auto root = scheduler.submit_task([] { return 6; });
auto root_result = root.future.share();
auto child = scheduler.submit_after({root.handle}, threadforge::Priority::High,
                                    [root_result] { return root_result.get() * 2; });
auto value = child.future.get();
```

Handles are copyable completion tokens. `valid()` requires a nonempty token and
a live scheduler owner; it does not require an unfinished task or open admission.
Default and stale handles are invalid, while foreign handles may be valid but
are rejected by the receiving scheduler. Dependencies must belong to that
scheduler; invalid ownership throws `std::invalid_argument` without accepting
the child. Duplicate prerequisites are deduplicated, and completed handles remain
usable while their scheduler lives. Dropping handles or futures does not cancel
accepted work.

Prerequisite completion releases children even when the prerequisite throws.
A child can read a declared prerequisite's already-ready future and choose
whether to propagate or handle its exception. The runtime does not wait on result
futures to schedule dependencies. Creation-only edges to already-accepted tasks
prevent graph cycles through this API; no dependency mutation API exists. See
[Dependency scheduling](dependencies.md) and the
[diamond example](../examples/dependency_graph.cpp).

## Graceful shutdown

`shutdown()` closes submissions and waits for all accepted work and workers to
finish. External callers may invoke it concurrently or repeatedly. A worker of
the same scheduler receives `std::logic_error` if it invokes this method.

Accepted blocked children are drained as their prerequisites finish, even after
admission closes. Idle workers wait while accepted tasks remain and exit when
the active graph is empty; closure alone does not cause spinning. Futures and
handles may outlive shutdown. Handles remain valid while the scheduler lives,
although a closed scheduler rejects new dependents; destruction makes handles
stale. Result futures retain their standard ownership of values and exceptions.

`is_shutdown()` means the submission boundary has closed; it can become true
before all running tasks or an external `shutdown()` call have returned. It does
not provide a separate termination barrier. Calling `shutdown()` does.

The destructor drains and joins automatically. The owner must destroy the object
outside its own workers, after other API users have stopped using it. Calling an
API concurrently with destruction violates the object's lifetime contract.

## Metrics

`metrics()` returns a `MetricsSnapshot`. It is safe concurrently with submission,
execution, and external shutdown, provided the scheduler object remains alive.
Its `workers` vector is indexed by stable worker index, with one element per
configured worker. Reading metrics does not synchronize user task data.

| Field | Meaning |
| --- | --- |
| `tasks_submitted` | Accepted tasks, including children waiting on prerequisites |
| `local_tasks` | Tasks claimed from the executing worker's own deque |
| `global_tasks` | Tasks claimed from injection or intrusive global ready lists |
| `tasks_stolen` | Tasks claimed from another worker's deque |
| `failed_steal_attempts` | Victim-priority-lane probes that returned no task |
| `waits` | Entries into the worker condition-variable wait |

Per-worker fields use the same definitions; steals belong to the executing thief,
and a worker never counts probing itself. A one-worker scheduler has no steal
attempts. An empty victim can produce three failed probes in a full scan, one per
priority lane. Claims are counted before invocation, so these counters classify
started tasks rather than successful completions. Throwing tasks still count as claims.

Individual counters use relaxed atomic reads. Live snapshots can combine different
instants and are unsuitable for a synchronization or completion barrier. After
`shutdown()` returns, totals are stable and
`tasks_submitted == local_tasks + global_tasks + tasks_stolen`.

No immediate shutdown, cancellation, or dependency mutation API exists in Phase 4.
