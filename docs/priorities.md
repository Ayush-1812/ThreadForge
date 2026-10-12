# Priority scheduling: Phase 4

ThreadForge supports `threadforge::Priority::High`, `Normal`, and `Low`. Include
`<threadforge/scheduler.hpp>` and pass the priority before the callable:

```cpp
auto urgent = scheduler.submit(threadforge::Priority::High,
                               [](int value) { return value * 2; }, 21);
auto ordinary = scheduler.submit([] { return 42; });
auto background = scheduler.submit(threadforge::Priority::Low, [] {});
```

The overload without a priority uses `Normal`. Nested submissions also default
to `Normal`, even when their parent is `High` or `Low`; priorities are explicit
and are not inherited. Invalid enum values, such as `static_cast<Priority>(99)`,
throw `std::invalid_argument` without accepting the task. Callable arguments,
futures, exception propagation, and graceful shutdown retain their existing
contracts.

`submit_task(Priority, callable, args...)` accepts the same classes and returns a
future plus a completion handle. For dependency submissions the explicit form is
`submit_after(dependencies, Priority, callable, args...)`. Their omitted
priorities also default to Normal. A waiting dependent is not eligible for
priority selection until its prerequisites complete; priority does not bypass
dependencies. See [Dependency scheduling](dependencies.md).

## Queue selection

The global queue and every worker-local queue each have three priority lanes.
Within a lane, the global queue is FIFO, the owner takes its newest local task
first, and thieves take the oldest task from a victim. External submissions use
the global injection queue; ready submissions from a worker of this scheduler
normally use its local queue. Dependency completion publishes newly ready tasks
to separate intrusive priority lists. Ready admission also uses these lists as
an allocation-free fallback if an ordinary deque cannot grow.

For direct queue users, no-argument `try_pop()`, `try_steal()`, and global
`wait_pop()` select the highest available lane strictly. The overloads taking a
`Priority` probe only that lane. The weighted service ring belongs to `Scheduler`,
which uses the lane-specific methods; direct queue helpers do not provide fairness.

A worker first chooses which priority class to try. For that class it checks its
local lane, the global injection lane, the intrusive ready list, then every
other worker's lane. The injection lane and intrusive list are individually
FIFO, with no FIFO ordering spanning the two sources. Only after
failing to claim any task of that class does it try the next class. Therefore,
when `High` is the current preferred class, a global or stolen `High` task takes
precedence over a local `Low` task. Source preference applies within each class.

These scans use individual queue locks rather than one lock covering all queues.
A concurrent publication or removal can change what is available during a scan.
The policy describes task claims, not an atomic ordering over simultaneous
submissions, execution starts, or completions.

## Weighted service and fairness

Each worker maintains its own cursor in this fixed ring:

```text
High High High High High High High High Normal Normal Normal Normal Low
```

The initial cursor points to the first `High`. Starting at the current cursor,
the worker tries at most three distinct classes, ordered by their first
occurrence around the ring. Empty classes are skipped without idling. On a
successful claim, the cursor advances to the slot immediately after the first
occurrence of the chosen class. An unsuccessful scan leaves the cursor unchanged;
idle scans do not consume service slots. Skipped slots are not saved as quotas
for later submissions, and a new batch does not reset the cursor to `High`.

When all three classes remain claimable throughout selection, each worker claims
8 `High`, 4 `Normal`, and 1 `Low` task per 13 claims. Under that condition, the
maximum intervening claims of other classes between two claims of a given class
are:

| Class | Claims per 13 | Maximum intervening claims of other classes |
| --- | --- | --- |
| `High` | 8 | 5 |
| `Normal` | 4 | 9 |
| `Low` | 1 | 12 |

These are conditional service opportunities measured in one worker's claims.
They do not bound elapsed time, task duration, or completion order. With multiple
workers, other workers can remove work before a probe; readiness must remain
observable for a service bound to apply. Running tasks are never preempted, and
a blocked task occupies its worker until it returns.

The ring deliberately relaxes strict `High`-before-`Normal`-before-`Low` ordering:
a worker services `Normal` and `Low` slots even while `High` tasks are queued.
This gives lower classes opportunities under a sustained higher-class load.
Conversely, priority does not guarantee fairness to every individual task. Local
LIFO selection can postpone an old local task indefinitely, and continuous local
work can postpone global or victim work of the same class. Continuous injection
work can also postpone intrusive-ready tasks of that class. FIFO orders tasks
within each global source and lane but does not overcome earlier-source
preference. No aging or deadline scheduling is implemented.

Blocking on a child future from a scheduler worker can still deadlock if all
workers wait for queued work. Priorities do not add cooperative future waiting.
Return child futures to an external caller, or otherwise keep workers available
to execute them. Declared handle dependencies let the scheduler release children
without waiting on futures; a dependent can read its declared prerequisites'
already-ready results. Arbitrary waits still have the same deadlock risk.

## Deterministic example and metrics

[`examples/priorities.cpp`](../examples/priorities.cpp) uses one worker and a
promise gate. A `High` task holds the worker while the main thread queues `Low`,
`Normal`, then `High`. The gate consumes one `High` slot, so the queued tasks are
claimed in `High`, `Normal`, `Low` order. The gate is released before waiting on
futures or calling shutdown. No timing assumptions or sleeps are needed.

Priority lanes retain Phase 2 metrics: each claimed task counts as local, global,
or stolen, regardless of its priority. Intrusive ready-list claims count as global
work. `failed_steal_attempts` counts unsuccessful
victim-priority-lane probes; an empty victim can account for three failed probes
in a full scan. Live metrics remain observational snapshots, and no counter
provides a per-priority completion barrier.

The design adds lanes to the existing mutex-protected queues and keeps the
scheduler's generation-based waiting protocol. It does not introduce a lock-free
queue or task preemption. The weighted ring is a fixed policy rather than a
configurable priority or fairness guarantee for individual tasks.
