# Phase 3 validation

Validation is performed on Ubuntu 24.04 under WSL2, using GCC 13.3.0, CMake 3.28.3,
Ninja, and GoogleTest 1.14.0. The core library uses portable C++20; other compilers
and operating systems have not yet been validated.

The suite contains 84 GoogleTest cases: 59 focused runtime, queue, work-stealing,
and priority tests, plus 25 stress cases. The basic example prints `Answer: 42`
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

## Validated builds

| Configuration | Result |
| --- | --- |
| Debug | 84/84 tests passed |
| Release | 84/84 tests passed; all three example outputs verified |
| AddressSanitizer + UndefinedBehaviorSanitizer | 84/84 tests passed; no sanitizer findings reported |
| ThreadSanitizer | 84/84 tests passed with the WSL workaround below; no race findings reported |

The work-stealing example reports a checksum of `357389824`, `1025` submitted
tasks, and one global claim. The remaining `1024` claims are split between local
execution and stealing according to the actual scheduling of that run; the split
is not a fixed expected value or a performance benchmark.

The gated priority example prints `High`, `Normal`, then `Low`.

The host disk filled during the sanitizer build. Generated build folders were
removed and ASan/UBSan was rebuilt with one compiler job and
`-DCMAKE_CXX_FLAGS_DEBUG=-g1`. Once the focused executable was linked, its
disposable object files were removed to make room for the stress executable.
Both executables then completed the full 84-test suite. Sanitizer instrumentation
remained enabled for library and test code throughout.

The sanitizer flags apply to library, example, and test translation units. The
system GoogleTest package is prebuilt and is not rebuilt with instrumentation.
Passing these workloads is evidence for the tested paths, not proof of the absence
of all concurrency defects. No benchmark result is inferred from test execution
time.

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
