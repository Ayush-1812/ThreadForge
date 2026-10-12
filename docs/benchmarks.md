# Scheduler benchmarks: Phase 5

The optional Google Benchmark executable compares identical validated CPU work
through sequential execution, a centralized FIFO worker pool, and ThreadForge.
Benchmark infrastructure is separate from runtime tests. No speedup or throughput
claim follows from test durations or from adding these benchmarks.

## Build and run

Google Benchmark is resolved from the system with `find_package(benchmark
REQUIRED)` when `THREADFORGE_BUILD_BENCHMARKS=ON`. The option defaults to `OFF`;
normal library, example, and test builds do not require this package. Builds do
not download benchmark dependencies.

On Ubuntu, build an optimized executable:

```sh
sudo apt-get install libbenchmark-dev
cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DTHREADFORGE_BUILD_BENCHMARKS=ON \
  -DTHREADFORGE_BUILD_TESTS=OFF -DTHREADFORGE_BUILD_EXAMPLES=OFF
cmake --build build-bench --target threadforge_benchmarks
./build-bench/benchmarks/threadforge_benchmarks \
  --benchmark_min_time=0.2 --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true \
  --benchmark_out=benchmark-results.raw.json --benchmark_out_format=json
python3 scripts/summarize_benchmarks.py benchmark-results.raw.json \
  --normalized-json benchmark-results.json > benchmark-summary.md
```

List registered cases with `--benchmark_list_tests=true`. Filter the cases when
examining a specific workload:

```sh
./build-bench/benchmarks/threadforge_benchmarks \
  --benchmark_filter='^BM_ThreadForgeUniform' --benchmark_min_time=0.2
./build-bench/benchmarks/threadforge_benchmarks \
  --benchmark_filter='UnevenFanout' --benchmark_min_time=0.2
```

Use the same repetition count and minimum time for each implementation being
compared. For sustained measurements, increase the minimum time and retain the
JSON output alongside the machine and compiler details.

[`scripts/run_benchmarks.sh`](../scripts/run_benchmarks.sh) automates an optimized
benchmark-only build and records the environment, raw JSON, and summary:

```sh
bash scripts/run_benchmarks.sh
bash scripts/run_benchmarks.sh benchmark-results --benchmark_repetitions=3
```

The runner defaults to a 0.1-second warmup, 0.2-second minimum measurement time,
and five repetitions. An optional first argument selects the output directory;
remaining arguments are passed to Google Benchmark. The default
`benchmark-results/` directory is ignored by Git. Set
`THREADFORGE_BENCHMARK_BUILD_DIR` to choose the build directory and
`THREADFORGE_BUILD_JOBS` to change the default one build job. The output contains
`environment.txt`, `results.raw.json`, portable `results.json`, and `summary.md`;
Python 3 is required for the summary step. The raw file is retained unchanged.
Normalization replaces undefined nonfinite constants with `null`; recognized
batch timings must be positive and finite before a portable report is written.
The runner disables CMake sanitizer options and records effective compiler flags
and supplied benchmark arguments in the environment file.

The summary script prints batch wall times in microseconds, preferring the
median when repetition aggregates are present, and speedups against the matching
sequential case. It rejects reports containing benchmark errors. A filtered
report without a matching sequential case can show its batch time without a
speedup.

## Implementations and workloads

| Cases | Execution model |
| --- | --- |
| `BM_SequentialUniform`, `BM_SequentialUnevenFanout` | The calling thread executes the CPU kernel directly |
| `BM_CentralizedUniform`, `BM_CentralizedUnevenFanout` | A fixed worker pool consumes one mutex-protected FIFO queue |
| `BM_ThreadForgeUniform`, `BM_ThreadForgeUnevenFanout` | ThreadForge uses worker-local queues, global injection, and stealing |

Parallel cases run with 1, 2, 4, and 8 workers. Every case processes 1,024 child
tasks with kernel sizes of 1, 256, or 16,384 rounds. Sequential cases have task and
round parameters; parallel cases also have a worker-count parameter. The tiny
case emphasizes scheduling overhead, while larger cases increase the work per
task. These names describe configured work, not measured task durations.

The executable registers 54 cases. Named arguments appear in the result keys,
for example
`BM_ThreadForgeUniform/workers:4/tasks:1024/rounds:256/manual_time`.

Uniform cases submit all independent tasks externally. Uneven fan-out cases
submit one parent that creates all child tasks: they enter that worker's local
queue in ThreadForge and the shared FIFO queue in the centralized baseline. The
parent returns child futures to the external caller, so it never blocks a worker
waiting for its children. Every sixteenth child performs sixteen times the base
kernel rounds; the other children perform the base amount. The sequential
equivalent computes the same uneven child workload directly.

The centralized baseline is a deliberately small FIFO pool, rather than a copy
of ThreadForge's priority and dependency features. Benchmark tasks use default
Normal priority and do not declare dependencies. The comparison measures each
implementation's actual scheduling and submission costs for these workloads; it
does not isolate a single algorithm while holding every internal cost constant.

Each iteration validates the exact unsigned integer checksum against the same
sequential kernel. Validation protects against discarded work or mismatched
payloads. A failed result marks the benchmark as an error rather than producing a
performance result.

## Timing boundaries

Cases use manual elapsed wall-time measurements from a monotonic clock. Pool
construction occurs before iteration timing, and final shutdown and destruction
occur after timing. The reference checksum is also computed outside the interval.

Timed work includes task containers and reserves, callable submission, the CPU
kernel, waiting for results, and destruction of the iteration's result containers.
The completion boundary is availability of every result through `future.get()`.
ThreadForge's final completion bookkeeping may briefly trail future readiness and
overlap the next iteration; the final drain is excluded. This measures time until
application results are available, rather than time to construct and destroy a
fresh pool for every batch.

The same benchmark process reuses a pool across its iterations. ThreadForge's
optional duration collection remains disabled for these comparisons, so its
reported runtime counters do not require task-level clock reads. Stable
queue-source and worker-distribution counts are collected after final shutdown,
outside iteration timing. They describe that run's scheduling and are not fixed
expected steal counts.

Google Benchmark's `items_per_second` counts the 1,024 child tasks per batch.
ThreadForge also reports `submitted_per_batch`, `completed_per_batch`,
`local_per_batch`, `global_per_batch`, `stolen_per_batch`,
`failed_steals_per_batch`, `waits_per_batch`, and each
`workerN_completed_share`. Fan-out submission/completion totals include the
control parent, so they are 1,025 rather than the 1,024 throughput items. Worker
shares range from zero to one and describe completed task counts across the run.
The exact checksum is reported as `checksum_hi` and `checksum_lo`, its two 32-bit
halves, so floating-point counter storage does not round the full 64-bit value.

`failed_steals_per_batch` and `waits_per_batch` divide the pool's lifetime counters
by the iteration count. They also include untimed startup and final-shutdown
scans and waits, so they are not exclusive event counts inside the measured
intervals.

## Reporting and interpretation

Record compiler version, optimization flags, CPU model, available logical and
physical cores, operating system, benchmark library version, workload parameters,
warmup, minimum time, repetition count, and timing boundaries. Retain per-run results and
Google Benchmark's repetition aggregates. Report measured wall time and task
throughput, with units and the corresponding worker and workload parameters.

Calculate speedup only between matching workload and task-count configurations:
`sequential wall time / parallel wall time`. A one-worker ThreadForge result
includes submission and synchronization costs that direct sequential execution
does not incur. Oversubscribing available cores, shared-host activity, WSL
scheduling, CPU frequency changes, memory allocation, and background processes
can affect the result. A short correctness smoke run establishes that cases run
and validate their outputs; it does not establish a reliable speedup.

Counter reports complement wall time: uneven per-worker task counts can reflect
different task costs, and more steals do not alone establish better performance.
Dependency latency, priority fairness, and task-level timing collection are not
benchmark workloads in this first suite. The fixed 1,024-task batch and synthetic
integer kernel also limit conclusions about larger graphs or application-specific
memory access patterns.

## Measured WSL sample: 2026-10-12

All 54 cases completed with valid checksums. This is a short sample for verifying
the benchmark suite and illustrating the reports, rather than a stable ranking.
The [complete median table](benchmark-results/phase5-wsl.md) and
[measurement data](benchmark-results/phase5-wsl.json) retain the results.

| Environment | Value |
| --- | --- |
| CPU | Intel Core i5-1155G7, 4 physical cores / 8 logical CPUs visible to WSL |
| OS | Ubuntu 24.04 under WSL2 on Windows |
| Compiler / build | GCC 13.3.0; CMake 3.28.3; Release `-O3 -DNDEBUG`, C++20 |
| Benchmark package | Ubuntu Google Benchmark 1.8.3 |
| Runtime timing | Disabled; counters enabled |
| Sampling | 0.01-second warmup, 0.05-second minimum time, 3 repetitions per case |
| Placement | No explicit CPU affinity; shared host |

The packaged benchmark library reports `library_build_type: debug` and prints
`Library was built as DEBUG. Timings may be affected.` The ThreadForge executable
and library were built in Release. No CPU-work compilation ran concurrently with
the sample. The library warning, short intervals, and host variability limit
performance conclusions.

The following batch medians cover 1,024 leaves at 16,384 base kernel rounds.
Uneven work uses sixteen times that amount for every sixteenth leaf. Times are
milliseconds; sequential execution computes the same payload directly.

| Workload | Workers | Centralized pool (ms) | ThreadForge (ms) |
| --- | ---: | ---: | ---: |
| Uniform; sequential 35.941 ms | 1 | 46.195 | 50.226 |
| Uniform | 2 | 20.558 | 24.189 |
| Uniform | 4 | 10.955 | 10.715 |
| Uniform | 8 | 6.311 | 9.139 |
| Uneven; sequential 70.276 ms | 1 | 109.742 | 86.751 |
| Uneven | 2 | 37.211 | 51.886 |
| Uneven | 4 | 20.002 | 19.645 |
| Uneven | 8 | 14.824 | 28.692 |

Larger payloads benefited from multiple workers in this sample. Tiny and medium
payloads were dominated by submission, synchronization, and scheduling costs;
some cases varied substantially across repetitions. Eight workers did not
consistently improve ThreadForge's time over four workers. These observations
do not establish a general advantage for either pool or predict application
performance. Longer runs on a controlled host are needed before optimization
decisions or performance claims.

The sample command was:

```sh
./build-release/benchmarks/threadforge_benchmarks \
  --benchmark_min_warmup_time=0.01 --benchmark_min_time=0.05s \
  --benchmark_repetitions=3 --benchmark_report_aggregates_only=true \
  --benchmark_out=phase5-wsl.raw.json --benchmark_out_format=json
```

The published JSON omits the host identifier and represents undefined
coefficient-of-variation values for zero counters as `null`; measured finite
values are unchanged. Google Benchmark 1.8.3 emits nonstandard `NaN` tokens for
those undefined values. The summary ignores coefficient-of-variation entries
and uses each case's measured median batch time.
