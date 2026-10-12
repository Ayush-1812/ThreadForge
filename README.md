# ThreadForge

ThreadForge is a C++20 multicore task scheduling runtime. The project is built in
tested increments, with worker-local queues, a global injection queue, and work
stealing across CPU workers.

## Current milestone: Phase 3

- Configurable, fixed worker pool owned through `std::jthread`.
- Mutex-protected FIFO queue and condition-variable waiting.
- Per-worker deques: owners take newest work, thieves take oldest work.
- Worker-local submissions, global fallback, and rotating steal victims.
- High / Normal / Low priorities with work-conserving 8:4:1 weighted service.
- Thread-safe steal, queue-source, and worker-wait counters.
- Callable submission with arguments, move-only captures, and `std::future` results.
- Task exceptions delivered through futures without terminating workers.
- Graceful shutdown that drains accepted tasks and joins every worker.
- Deterministic priority tests and runnable runtime, stealing, and priority examples.
- Shutdown race regressions and 100,000-task stress workloads.

The runtime design is described in [docs/architecture.md](docs/architecture.md),
with API contracts in [docs/scheduler.md](docs/scheduler.md).
Priority selection and fairness limits are detailed in [docs/priorities.md](docs/priorities.md).

## Build and test

Requirements: a C++20 compiler, CMake 3.20+, and GoogleTest. Linux is the primary
development target; the library uses portable standard C++.

On Ubuntu:

```sh
sudo apt-get install build-essential cmake ninja-build libgtest-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/threadforge_basic
./build/threadforge_work_stealing
./build/threadforge_priorities
```

GoogleTest is resolved using `find_package(GTest REQUIRED)`; builds do not download
dependencies. Use `-DTHREADFORGE_BUILD_TESTS=OFF` to build only the library and
examples. Link consumers against `threadforge::threadforge` after `add_subdirectory`.

The stress suite is included by default and carries the CTest `stress` label.
Use `ctest --test-dir build -LE stress --output-on-failure` for only the focused
tests, or `-L stress` for only the stress suite. Disable building stress tests with
`-DTHREADFORGE_BUILD_STRESS_TESTS=OFF`.

## Sanitizers

Use separate build directories with GCC or Clang:

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DTHREADFORGE_ENABLE_ASAN=ON -DTHREADFORGE_ENABLE_UBSAN=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DTHREADFORGE_ENABLE_TSAN=ON
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

ASan and TSan cannot be combined; configuration rejects that combination.
Sanitizer options propagate to consumers and fail on detected violations.
See [docs/validation.md](docs/validation.md) for measured validation and environment
limitations. Benchmarks are planned for Phase 5; their methodology is outlined in
[docs/benchmarks.md](docs/benchmarks.md).

## Usage

```cpp
#include <threadforge/scheduler.hpp>

threadforge::Scheduler scheduler(4);
auto answer = scheduler.submit([] { return 42; });
auto sum = scheduler.submit([](int a, int b) { return a + b; }, 10, 20);
int value = answer.get();
int total = sum.get();
scheduler.shutdown();
```

Use `scheduler.submit(threadforge::Priority::High, callable, args...)` to select
a priority; the existing overload defaults to `Normal`. Priorities are explicit
and are not inherited by nested submissions.

Within the preferred priority, workers consume local work first, then global work,
then steal from other workers. Each worker cycles through eight High, four Normal,
and one Low claim, skipping empty classes. Running tasks are never preempted.
Nested submissions on the same scheduler are local; external and cross-scheduler
submissions enter the global queue. The [work-stealing example](examples/work_stealing.cpp)
returns child futures to its external caller so workers stay available.

Retrieve a thread-safe telemetry snapshot with `scheduler.metrics()`:

```cpp
scheduler.shutdown();
auto metrics = scheduler.metrics();
// Every accepted task is classified once after the workers have joined.
auto claimed = metrics.local_tasks + metrics.global_tasks + metrics.tasks_stolen;
```

Snapshots include per-worker counts, failed victim-priority-lane probes, and wait episodes.
Live snapshots are approximate across counters; call `shutdown()` before relying
on final totals. Wait counts measure wait entries, not idle time.

The default worker count uses `std::thread::hardware_concurrency()`, falling back
to one worker. An explicitly configured zero worker count is rejected.

## Lifetime and shutdown

`shutdown()` stops accepting work, drains every accepted task, wakes workers, and
joins them. It is idempotent and supports concurrent external callers. A submission
racing shutdown either succeeds and is drained, or throws `std::runtime_error`.
`is_shutdown()` reports that submissions are closed; worker joins may still be in
progress in another caller. The destructor performs graceful shutdown.

Callables and arguments are stored by value and moved for invocation. Use `std::ref`
for explicit reference arguments and keep referenced objects alive until execution
finishes. Discarding a future does not cancel its task.

A worker calling `shutdown()` on its own scheduler receives `std::logic_error`.
Destroy the scheduler from its owning external thread, and do not race object
destruction against calls to its API. Blocking a worker on another task from the
same saturated pool can deadlock; dependency scheduling will address this in a
later phase. Shutdown waits for user tasks to return and cannot interrupt them.

## Roadmap

1. Basic runtime — complete.
2. **Worker-local queues, work stealing, and steal counters** — complete.
3. **Priorities and weighted fairness** — current milestone.
4. Dependency graph scheduling and task states.
5. Metrics and reproducible performance comparisons.
6. Stress testing, sanitizers, CI, documentation, and examples.
7. Optional advanced scheduling and optimization.

Queues remain mutex-protected. Performance results will be published only after
measurement; no performance claim is made yet.
