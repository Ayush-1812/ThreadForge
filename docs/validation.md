# Phase 1 validation

Validation is performed on Ubuntu 24.04 under WSL2, using GCC 13.3.0, CMake 3.28.3,
Ninja, and GoogleTest 1.14.0. The core library uses portable C++20; other compilers
and operating systems have not yet been validated.

The Debug build passes 34 GoogleTest cases: 25 focused runtime/queue tests and
9 stress cases. The example prints `Answer: 42` and `Sum: 30`.

The stress suite checks 100,000 tasks with four producers at each of 1, 2, 4, and
8 worker counts. It validates every returned value and every task's execution
count. It also races submissions against queue closure and checks that each
accepted task ran exactly once while rejected tasks never ran. A lifecycle case
constructs and destroys 50 pools with uneven CPU work, validating all outputs.

Shutdown regression tests use promises and latches to control running and queued
tasks. They check that shutdown blocks for active work, rejects new submissions,
and avoids circular waiting when a worker requests shutdown during an external
join. Tests contain no arbitrary sleep-based scheduling assumptions.

## Validated builds

| Configuration | Result |
| --- | --- |
| Debug | 34/34 tests passed |
| Release | 34/34 tests passed; basic example output verified |
| AddressSanitizer + UndefinedBehaviorSanitizer | 34/34 tests passed; no sanitizer findings reported |
| ThreadSanitizer | 34/34 tests passed with the WSL workaround below; no race findings reported |

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
