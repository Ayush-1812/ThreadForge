# Benchmark plan

No scheduler benchmarks have been measured in Phase 1. Test durations are not
performance benchmark results, and no throughput or speedup is claimed.

Phase 5 will compare sequential execution, the centralized queue baseline, and
ThreadForge's work-stealing runtime using Google Benchmark where practical.

Planned workloads include tiny scheduling-heavy tasks, medium and long CPU-bound
tasks, skewed task distribution, and mixed task durations. Worker counts will
include 1, 2, 4, and 8 where the machine supports them. Every implementation must
produce the same validated outputs, and task creation/submission will be included
or excluded consistently and explicitly.

Reports will record compiler, flags, CPU, core count, operating system, workload
parameters, warmup, repetitions, and timing boundaries. Wall time, throughput,
speedup relative to sequential work, steals, and worker distribution will be
reported only after collecting them. Shared-host load, WSL scheduling, allocation
overhead, and background processes will be identified as measurement limitations.
