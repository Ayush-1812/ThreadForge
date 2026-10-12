# ThreadForge

ThreadForge is a C++20 multicore task scheduling runtime. The project is built in
tested increments, starting with a centralized queue before adding worker-local
queues and work stealing.

## Current milestone: Phase 1

- Configurable, fixed worker pool owned through `std::jthread`.
- Mutex-protected FIFO queue and condition-variable waiting.
- Callable submission with arguments, move-only captures, and `std::future` results.
- Task exceptions delivered through futures without terminating workers.
- Graceful shutdown that drains accepted tasks and joins every worker.
- GoogleTest coverage and a runnable example.

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
```

GoogleTest is resolved using `find_package(GTest REQUIRED)`; builds do not download
dependencies. Use `-DTHREADFORGE_BUILD_TESTS=OFF` to build only the library and
examples. Link consumers against `threadforge::threadforge` after `add_subdirectory`.

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

1. **Basic runtime** — current milestone.
2. Worker-local queues, work stealing, and steal counters.
3. Priorities, fairness, and queue statistics.
4. Dependency graph scheduling and task states.
5. Metrics and reproducible performance comparisons.
6. Stress testing, sanitizers, CI, documentation, and examples.
7. Optional advanced scheduling and optimization.

The centralized queue is a deliberate correctness baseline. Performance results
will be published only after measurement; no performance claim is made yet.
