# Testing and hardening: Phase 6

ThreadForge's hardening checks target Linux. Runtime tests cover task ownership,
submission, work stealing, priorities, dependency registration, shutdown races,
metrics, and allocation failures. Stress workloads validate returned values and
exactly-once execution at 1, 2, 4, and 8 workers. Deterministic scheduling tests use
promises and latches to establish ready or blocked work before checking behavior.

The GitHub Actions workflow is
[`.github/workflows/ci.yml`](../.github/workflows/ci.yml). Verified local and hosted
results and environment limitations are recorded in [validation.md](validation.md).

## Build warnings and test selection

Use the installed compiler, CMake, Ninja, and GoogleTest rather than downloading
test dependencies during configuration:

```sh
sudo apt-get install build-essential cmake ninja-build libgtest-dev python3
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DTHREADFORGE_WARNINGS_AS_ERRORS=ON
cmake --build build-debug --parallel 2
ctest --test-dir build-debug --output-on-failure --parallel 1
```

`THREADFORGE_WARNINGS_AS_ERRORS` defaults to `OFF` and is enabled in every CI
lane. Warning options apply privately to ThreadForge's own library, examples,
tests, and benchmark target. Linking `threadforge::threadforge` does not force
warnings-as-errors on a consuming application. System GoogleTest and Google
Benchmark packages are prebuilt dependencies rather than project warning targets.

The default build includes focused tests and the heavier stress suite. To select
only one group:

```sh
ctest --test-dir build-debug -LE stress --output-on-failure
ctest --test-dir build-debug -L stress --output-on-failure
```

Disable the stress executable with `-DTHREADFORGE_BUILD_STRESS_TESTS=OFF`, or all
tests with `-DTHREADFORGE_BUILD_TESTS=OFF`. These options help development builds;
the CI matrix enables both suites. CTest timeouts bound a failing test rather than
prove a runtime latency guarantee.

Allocation-failure regressions run in an isolated executable with thread-local
allocation injection. They exercise setup, worker-local admission and deque
fallback, and registration rollback across multiple prerequisites. Production
runtime code does not expose fault-injection hooks. Accepted work must still
drain, while rejected work must leave admission and dependency accounting intact.

When a Python 3 interpreter is available, CTest also runs
[`tests/test_benchmark_summary.py`](../tests/test_benchmark_summary.py). Its
standard-library fixtures check median and unit selection, matching sequential
baselines, invalid timing and benchmark-error rejection, portable JSON
normalization, preservation of finite values and metadata, and refusal to
overwrite a raw report. The helper can be run independently:

```sh
python3 tests/test_benchmark_summary.py
```

## Continuous integration matrix

Every lane runs on `ubuntu-24.04`, builds with warnings-as-errors, and executes the
focused and stress tests without a performance threshold:

| Lane | Configuration |
| --- | --- |
| GCC 13 Debug | Debug library, tests, and examples |
| GCC 13 Release | Optimized library, tests, examples, and benchmark executable |
| Clang 18 Release | Optimized second-compiler coverage |
| GCC 13 ASan + UBSan | Address and undefined-behavior instrumentation |
| GCC 13 TSan | ThreadSanitizer instrumentation |

Examples are compiled in all lanes and run in the non-TSan lanes. The Release GCC
lane also compiles the centralized FIFO benchmark baseline, checks runner shell
syntax, and runs the one-round uniform and uneven checksum cases. Its JSON is
validated and normalized by the summary helper, whose failure causes CI to fail.
Google Benchmark can return a successful process status after a benchmark marks
an error, so inspecting the report is part of the smoke check. No benchmark
duration, speedup, steal count, or worker distribution is a pass/fail threshold.

The workflow triggers on pushes, pull requests, and manual dispatch. It uses
`contents: read`, disables persisted checkout credentials, and cancels superseded
runs for the same workflow/ref. Dependencies come from Ubuntu system packages.
The checkout action is pinned to the full commit for
[official `actions/checkout` v7.0.1](https://github.com/actions/checkout/commit/3d3c42e5aac5ba805825da76410c181273ba90b1),
verified against the [official release page](https://github.com/actions/checkout/releases/tag/v7.0.1).
Recheck the release and commit before updating that pin.

## Sanitizers and address-layout limits

Use separate build directories for AddressSanitizer/UndefinedBehaviorSanitizer
and ThreadSanitizer:

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DTHREADFORGE_WARNINGS_AS_ERRORS=ON \
  -DTHREADFORGE_ENABLE_ASAN=ON -DTHREADFORGE_ENABLE_UBSAN=ON
cmake --build build-asan --parallel 2
ctest --test-dir build-asan --output-on-failure --parallel 1

cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DTHREADFORGE_WARNINGS_AS_ERRORS=ON -DTHREADFORGE_ENABLE_TSAN=ON
cmake --build build-tsan --parallel 2
ctest --test-dir build-tsan --output-on-failure --parallel 1
```

Sanitizer support is checked during configuration. ASan and TSan cannot be enabled
together. The chosen instrumentation propagates to linked project executables,
while the system test/benchmark libraries remain prebuilt. A supported compiler
and instrumented build do not guarantee that a sanitizer can start under every
host address layout.

The TSan CI lane first starts the instrumented focused-test executable with
`--gtest_list_tests`, preserving that startup preflight's diagnostics in the job
log. Successful startup selects ordinary CTest. Only a failed preflight containing
the exact `FATAL: ThreadSanitizer: unexpected memory mapping` diagnostic and no
other nonblank output selects per-process `setarch -R` for CTest. Any other
preflight error fails the lane. The full suite then runs once in the selected
layout: race findings, assertions, crashes, timeouts, and subsequent startup
errors fail without retry. A runner that forbids `setarch` also fails that check.
The workflow does not change global kernel address-randomization settings.

The recorded WSL environment needs the following per-process workaround:

```sh
setarch "$(uname -m)" -R ctest --test-dir build-tsan --output-on-failure --parallel 1
```

Try the ordinary command first elsewhere. A startup mapping failure happens
before the affected test executes; it is distinct from a sanitizer report about
runtime code. See [validation.md](validation.md) for the environment and actual
results, including the workaround used.

## Scope and remaining limits

The successful hosted matrix covers GCC and Clang on Ubuntu, alongside the local
GCC/WSL checks recorded in [validation.md](validation.md). Windows and macOS builds
have not been validated. The library uses portable C++20, and its current
automated matrix runs on Linux.

Passing tests and sanitizers provides evidence for exercised paths rather than
proof that every concurrency defect is absent. Queues remain mutex-protected,
admission is unbounded, and graceful shutdown requires user tasks to terminate.
Blocking all workers on undeclared task futures or application locks can still
deadlock. There is no cancellation, forced interruption, deadline scheduler,
cooperative future waiting, or dependency-mutation API. Priority service is a
conditional class policy rather than an individual-task or wall-time guarantee.

Live metrics remain observational snapshots. Synthetic benchmark workloads and
the recorded host do not establish general application speedups. Keep timing
boundaries and known limitations with results, as documented in
[metrics.md](metrics.md) and [benchmarks.md](benchmarks.md).
