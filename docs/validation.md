# Phase 2 validation

Validation is performed on Ubuntu 24.04 under WSL2, using GCC 13.3.0, CMake 3.28.3,
Ninja, and GoogleTest 1.14.0. The core library uses portable C++20; other compilers
and operating systems have not yet been validated.

The Debug build passes 55 GoogleTest cases: 38 focused runtime/queue/work-stealing
tests and 17 stress cases. The basic example prints `Answer: 42` and `Sum: 30`.

The stress suite checks 100,000 tasks with four producers at each of 1, 2, 4, and
8 worker counts. It validates every returned value and every task's execution
count. A second 100,000-task workload fans out from a single worker at each worker
count, exercising local queue publication, concurrent steals, and owner draining.
Both external and local submission workloads race shutdown and verify that every
accepted task ran exactly once while rejected tasks never ran. Final metrics must
classify every accepted task as exactly one local, global, or stolen claim.
A lifecycle case constructs and destroys 50 pools with uneven CPU work, validating
all outputs.

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

## Validated builds

| Configuration | Result |
| --- | --- |
| Debug | 55/55 tests passed |
| Release | 55/55 tests passed; both example outputs verified |
| AddressSanitizer + UndefinedBehaviorSanitizer | 55/55 tests passed; no sanitizer findings reported |
| ThreadSanitizer | 55/55 tests passed with the WSL workaround below; no race findings reported |

The work-stealing example reports a checksum of `357389824`, `1025` submitted
tasks, and one global claim. The remaining `1024` claims are split between local
execution and stealing according to the actual scheduling of that run; the split
is not a fixed expected value or a performance benchmark.

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
