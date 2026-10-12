# ThreadForge validation

Validation is performed on Ubuntu 24.04 under WSL2, using GCC 13.3.0, CMake 3.28.3,
Ninja, and GoogleTest 1.14.0. The core library uses portable C++20; other compilers
and operating systems have not yet been validated.

The suite contains 107 GoogleTest cases: 74 focused runtime, queue, work-stealing,
priority, and dependency tests, plus 33 stress cases. The basic example prints `Answer: 42`
and `Sum: 30`.

The stress suite checks 100,000 tasks with four producers at each of 1, 2, 4, and
8 worker counts. It validates every returned value and every task's execution
count. A second 100,000-task workload fans out from a single worker at each worker
count, exercising local queue publication, concurrent steals, and owner draining.
Both external and local submission workloads race shutdown and verify that every
accepted task ran exactly once while rejected tasks never ran. Final metrics must
classify every accepted task as exactly one local, global, or stolen claim.
A lifecycle case constructs and destroys 50 pools with uneven CPU work, validating
all outputs.

Mixed-priority stress cases run another 100,000 tasks from four concurrent
external producers and another 100,000 tasks from one worker-local fan-out at
each of 1, 2, 4, and 8 worker counts. They validate every task's result,
exactly-once execution, and final source metrics without assuming a steal count
or completion order.

Shutdown regression tests use promises and latches to control running and queued
tasks. They check that shutdown blocks for active work, rejects new submissions,
and avoids circular waiting when a worker requests shutdown during an external
join. Tests contain no arbitrary sleep-based scheduling assumptions.

Dedicated work-stealing coverage checks LIFO owner order, oldest-first steals,
the last-item owner/thief race, move-only task ownership, global fallback,
cross-scheduler routing, stolen exceptions, and a thief publishing descendants
to its own local queue. Promise-gated tests keep the owner busy until a thief has
finished a child, so successful stealing is established without timing assumptions.
Waiting metrics establish that both workers have entered a wait before the local
wakeup test begins. Another case exercises 50 repeated publication/wait cycles.

Priority tests use gated workers to establish the backlog before checking claim
order. They cover per-lane FIFO/LIFO behavior, class selection across local,
global, and victim queues, the 8:4:1 service cycle, skipping empty classes,
non-preemption, invalid enum values, and draining every class at shutdown. API
cases check that nested submissions default to Normal and that Priority remains
usable as an ordinary callable argument. A High task chain repeatedly publishes
its successor; a pending Low task observes eight High and four Normal claims
before the chain finishes. These tests check service opportunities rather than
elapsed-time fairness, as described in [priorities.md](priorities.md).

Dependency tests cover one-worker diamond graphs, fan-in and fan-out, ready
promotion across priorities, discarded handles/futures, move-only callables,
invalid and stale handles, duplicate prerequisites, and shutdown of a graph
with waiting descendants. Gates force an exceptional prerequisite to finish
after its child is registered. A one-worker sentinel establishes completed
registration, and repeated registration/completion races check for missed
releases. Another case verifies unrelated work can run while a child is waiting.

Graph stress tests validate every result and execution count in 100-layer,
10,000-node graphs with two prerequisites per non-root node. Concurrent
registration also races shutdown, checking both accepted and rejected tasks.
Both workloads run with 1, 2, 4, and 8 workers.

## Phase 4 validated builds

| Configuration | Result |
| --- | --- |
| Debug | 107/107 tests passed; one-worker diamond output verified |
| Release | 107/107 tests passed; all four example outputs verified |
| AddressSanitizer + UndefinedBehaviorSanitizer | 107/107 tests passed; no sanitizer findings reported |
| ThreadSanitizer | 107/107 tests passed with the WSL workaround below; no race findings reported |

The work-stealing example reports a checksum of `357389824`, `1025` submitted
tasks, and one global claim. The remaining `1024` claims are split between local
execution and stealing according to the actual scheduling of that run; the split
is not a fixed expected value or a performance benchmark.

The gated priority example prints `High`, `Normal`, then `Low`.
The dependency example prints `Root: 6`, `Left: 12`, `Right: 9`, and `Combined: 21`.

Phase 3 completed all 84 tests in Debug, Release, ASan/UBSan, and TSan before
dependency implementation began. The host disk filled during its sanitizer build. Generated build folders were
removed and ASan/UBSan was rebuilt with one compiler job and
`-DCMAKE_CXX_FLAGS_DEBUG=-g1`. Once the focused executable was linked, its
disposable object files were removed to make room for the stress executable.
Both executables then completed the full 84-test suite. Sanitizer instrumentation
remained enabled for library and test code throughout.

Phase 4 Debug and TSan builds use `-DCMAKE_CXX_FLAGS_DEBUG=-g0` and one compiler
job to reduce disk use. Debug symbols are omitted; the runtime and tests remain
unoptimized, and TSan instrumentation is enabled for its build.

After the final fan-out storage-growth improvement, the full Release suite was
repeated and all 23 dependency cases, including graph stress, were rerun under
TSan. Both checks passed. The earlier Debug and complete TSan suite results
preceded this storage-growth change.

The final ASan/UBSan run also covered the complete 107-test suite after that
change. It used `-DCMAKE_CXX_FLAGS_DEBUG=-O1` to omit debug symbols and apply
modest optimization while retaining sanitizer instrumentation. The focused-test
objects were removed after linking that executable to make room for the stress
link. Only generated workspace build artifacts were deleted; completed build
directories were removed between configurations as required by the full host
disk.

The sanitizer flags apply to library, example, and test translation units. The
system GoogleTest package is prebuilt and is not rebuilt with instrumentation.
Passing these workloads is evidence for the tested paths, not proof of the absence
of all concurrency defects. No benchmark result is inferred from test execution
time.

## Phase 5 validation

Phase 5 adds eight metrics cases and nine centralized-baseline pool cases, for
124 tests: 91 focused cases and 33 stress cases. Metrics tests use gates to verify
running, ready, dependency-blocked, and completed transitions, priority totals,
callable failures, and optional timings. Four concurrent producers submit 8,000
tasks while snapshots check the locked gauge partition, then verify settled
totals and every result. Baseline tests cover FIFO selection, forwarding,
exceptions, fan-out, concurrent producers, admission closure, and thread joins.

The full Release build (`-O3 -DNDEBUG`) passed 124/124 tests. The metrics report
example returned checksum `358438400`, 1,025 started/completed tasks, zero failures,
and zero final pending/ready/running/dependency gauges. All 54 Google Benchmark
cases completed with three repetitions and validated their checksums. The short
sample is described in [benchmarks.md](benchmarks.md); it is separate from test
execution times.

## WSL ThreadSanitizer startup

The default TSan run failed before executing tests with
`FATAL: ThreadSanitizer: unexpected memory mapping`. On this environment the
following per-process address-layout setting allowed the full suite to run:

```sh
setarch x86_64 -R ctest --test-dir build-tsan --output-on-failure
```

This disables address randomization only for that command and its children.
No global kernel setting was changed. On other Linux environments, try the
ordinary CTest command first; containers may disallow changing the process
personality. The workaround is environment-specific rather than a library fix.
