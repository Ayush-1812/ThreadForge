# Scheduler API: Phase 3

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
scheduler publishes to its local deque; an external thread or a worker belonging
to a different scheduler publishes to the receiving scheduler's global queue.
A task submitted during graceful draining is rejected, including nested tasks.

Each queue has three priority lanes. Workers choose a class before checking its
local lane, global lane, then other workers' lanes. Within that class, owners take
newer local tasks first, global tasks are FIFO, and thieves take older victim
tasks first. Each worker uses a weighted ring of 8 High, 4 Normal, and 1 Low slot,
skips empty classes, and advances only on a successful claim. This provides
conditional class service opportunities rather than strict priority ordering or
fairness for individual tasks. Running tasks are not preempted. See
[Priority scheduling](priorities.md) for the cursor rules, fairness bounds, and
deterministic example.

Calling `future.get()` from a scheduler worker can deadlock if every worker waits
for queued child tasks; priority scheduling does not add cooperative waiting.

An allocation failure during submission throws without accepting that task.
Submission after closure throws `std::runtime_error`. Exceptions thrown by an
accepted callable propagate through `future.get()`. Ignoring or destroying a
future neither blocks the submitting thread nor cancels queued work.

## Graceful shutdown

`shutdown()` closes submissions and waits for all accepted work and workers to
finish. External callers may invoke it concurrently or repeatedly. A worker of
the same scheduler receives `std::logic_error` if it invokes this method.

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
| `tasks_submitted` | Tasks successfully published to any scheduler queue |
| `local_tasks` | Tasks claimed from the executing worker's own deque |
| `global_tasks` | Tasks claimed from the global injection queue |
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

No immediate shutdown, cancellation, or dependency API exists in Phase 3.
