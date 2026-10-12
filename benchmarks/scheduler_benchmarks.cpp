#include "centralized_pool.hpp"
#include "threadforge/scheduler.hpp"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <string>
#include <type_traits>
#include <vector>

namespace {

enum class Workload { Uniform, UnevenFanout };

std::uint64_t kernel(std::uint64_t task_id, std::size_t rounds) {
    std::uint64_t value = task_id + 0x9e3779b97f4a7c15ULL;
    benchmark::DoNotOptimize(value);
    for (std::size_t round = 0; round < rounds; ++round) {
        value ^= value >> 12;
        value ^= value << 25;
        value ^= value >> 27;
        value *= 0x2545f4914f6cdd1dULL;
    }
    return value;
}

std::size_t task_rounds(std::size_t task_id, std::size_t rounds, Workload workload) {
    return workload == Workload::UnevenFanout && task_id % 16 == 0 ? rounds * 16 : rounds;
}

std::uint64_t sequential_batch(std::size_t task_count, std::size_t rounds, Workload workload) {
    std::uint64_t checksum = 0;
    for (std::size_t id = 0; id < task_count; ++id) {
        checksum += kernel(id, task_rounds(id, rounds, workload));
    }
    return checksum;
}

template<class Pool>
std::uint64_t parallel_batch(Pool& pool, std::size_t task_count, std::size_t rounds,
                             Workload workload) {
    std::vector<std::future<std::uint64_t>> results;
    if (workload == Workload::Uniform) {
        results.reserve(task_count);
        for (std::size_t id = 0; id < task_count; ++id) {
            results.push_back(pool.submit([id, rounds] { return kernel(id, rounds); }));
        }
    } else {
        auto parent = pool.submit([&pool, task_count, rounds, workload] {
            std::vector<std::future<std::uint64_t>> children;
            children.reserve(task_count);
            for (std::size_t id = 0; id < task_count; ++id) {
                children.push_back(pool.submit([id, rounds, workload] {
                    return kernel(id, task_rounds(id, rounds, workload));
                }));
            }
            // The parent returns instead of occupying a worker waiting for children.
            return children;
        });
        results = parent.get();
    }
    std::uint64_t checksum = 0;
    for (auto& result : results) {
        checksum += result.get();
    }
    return checksum;
}

void report_workload(benchmark::State& state, std::size_t task_count, std::size_t rounds,
                     std::uint64_t checksum) {
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(task_count));
    state.counters["leaf_tasks_per_batch"] = static_cast<double>(task_count);
    state.counters["rounds"] = static_cast<double>(rounds);
    // Two exact 32-bit values avoid rounding a 64-bit checksum to a double.
    state.counters["checksum_hi"] = static_cast<double>(checksum >> 32);
    state.counters["checksum_lo"] = static_cast<double>(checksum & 0xffffffffULL);
}

void report_metrics(benchmark::State& state, const threadforge::MetricsSnapshot& metrics) {
    const auto iterations = static_cast<double>(state.iterations());
    if (iterations == 0) {
        return;
    }
    state.counters["submitted_per_batch"] = static_cast<double>(metrics.tasks_submitted) / iterations;
    state.counters["completed_per_batch"] = static_cast<double>(metrics.tasks_completed) / iterations;
    state.counters["local_per_batch"] = static_cast<double>(metrics.local_tasks) / iterations;
    state.counters["global_per_batch"] = static_cast<double>(metrics.global_tasks) / iterations;
    state.counters["stolen_per_batch"] = static_cast<double>(metrics.tasks_stolen) / iterations;
    state.counters["failed_steals_per_batch"] =
        static_cast<double>(metrics.failed_steal_attempts) / iterations;
    state.counters["waits_per_batch"] = static_cast<double>(metrics.waits) / iterations;
    for (std::size_t i = 0; i < metrics.workers.size(); ++i) {
        const auto share = metrics.tasks_completed == 0 ? 0.0 :
            static_cast<double>(metrics.workers[i].tasks_completed) /
                static_cast<double>(metrics.tasks_completed);
        state.counters["worker" + std::to_string(i) + "_completed_share"] = share;
    }
}

void sequential_benchmark(benchmark::State& state, Workload workload) {
    const auto task_count = static_cast<std::size_t>(state.range(0));
    const auto rounds = static_cast<std::size_t>(state.range(1));
    const auto expected = sequential_batch(task_count, rounds, workload);
    for (auto iteration : state) {
        static_cast<void>(iteration);
        const auto started = std::chrono::steady_clock::now();
        auto actual = sequential_batch(task_count, rounds, workload);
        const auto finished = std::chrono::steady_clock::now();
        state.SetIterationTime(std::chrono::duration<double>(finished - started).count());
        benchmark::DoNotOptimize(actual);
        if (actual != expected) {
            state.SkipWithError("sequential checksum mismatch");
            break;
        }
    }
    report_workload(state, task_count, rounds, expected);
}

template<class Pool>
void parallel_benchmark(benchmark::State& state, Workload workload) {
    const auto worker_count = static_cast<std::size_t>(state.range(0));
    const auto task_count = static_cast<std::size_t>(state.range(1));
    const auto rounds = static_cast<std::size_t>(state.range(2));
    const auto expected = sequential_batch(task_count, rounds, workload);
    // Setup, final shutdown, and stable metric collection are outside manual timing.
    Pool pool(worker_count);
    for (auto iteration : state) {
        static_cast<void>(iteration);
        try {
            const auto started = std::chrono::steady_clock::now();
            auto actual = parallel_batch(pool, task_count, rounds, workload);
            const auto finished = std::chrono::steady_clock::now();
            state.SetIterationTime(std::chrono::duration<double>(finished - started).count());
            benchmark::DoNotOptimize(actual);
            if (actual != expected) {
                state.SkipWithError("parallel checksum mismatch");
                break;
            }
        } catch (const std::exception& error) {
            state.SkipWithError(error.what());
            break;
        }
    }
    pool.shutdown();
    report_workload(state, task_count, rounds, expected);
    state.counters["workers"] = static_cast<double>(worker_count);
    if constexpr (std::is_same_v<Pool, threadforge::Scheduler>) {
        report_metrics(state, pool.metrics());
    }
}

void BM_SequentialUniform(benchmark::State& state) {
    sequential_benchmark(state, Workload::Uniform);
}

void BM_SequentialUnevenFanout(benchmark::State& state) {
    sequential_benchmark(state, Workload::UnevenFanout);
}

void BM_CentralizedUniform(benchmark::State& state) {
    parallel_benchmark<threadforge::benchmarks::CentralizedPool>(state, Workload::Uniform);
}

void BM_CentralizedUnevenFanout(benchmark::State& state) {
    parallel_benchmark<threadforge::benchmarks::CentralizedPool>(state, Workload::UnevenFanout);
}

void BM_ThreadForgeUniform(benchmark::State& state) {
    parallel_benchmark<threadforge::Scheduler>(state, Workload::Uniform);
}

void BM_ThreadForgeUnevenFanout(benchmark::State& state) {
    parallel_benchmark<threadforge::Scheduler>(state, Workload::UnevenFanout);
}

void sequential_arguments(benchmark::internal::Benchmark* benchmark) {
    for (const auto rounds : {1, 256, 16384}) {
        benchmark->Args({1024, rounds});
    }
}

void parallel_arguments(benchmark::internal::Benchmark* benchmark) {
    for (const auto workers : {1, 2, 4, 8}) {
        for (const auto rounds : {1, 256, 16384}) {
            benchmark->Args({workers, 1024, rounds});
        }
    }
}

BENCHMARK(BM_SequentialUniform)->Apply(sequential_arguments)
    ->ArgNames({"tasks", "rounds"})->UseManualTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_SequentialUnevenFanout)->Apply(sequential_arguments)
    ->ArgNames({"tasks", "rounds"})->UseManualTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_CentralizedUniform)->Apply(parallel_arguments)
    ->ArgNames({"workers", "tasks", "rounds"})->UseManualTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_CentralizedUnevenFanout)->Apply(parallel_arguments)
    ->ArgNames({"workers", "tasks", "rounds"})->UseManualTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ThreadForgeUniform)->Apply(parallel_arguments)
    ->ArgNames({"workers", "tasks", "rounds"})->UseManualTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ThreadForgeUnevenFanout)->Apply(parallel_arguments)
    ->ArgNames({"workers", "tasks", "rounds"})->UseManualTime()->Unit(benchmark::kMicrosecond);

} // namespace

int main(int argc, char** argv) {
    benchmark::AddCustomContext("threadforge_version", THREADFORGE_BENCHMARK_VERSION);
    benchmark::AddCustomContext("build_type", THREADFORGE_BENCHMARK_BUILD_TYPE);
    benchmark::AddCustomContext("runtime_timing", "disabled");
    benchmark::AddCustomContext("timed_interval", "submission through result destruction; setup/shutdown excluded");
#ifdef __VERSION__
    benchmark::AddCustomContext("compiler", __VERSION__);
#elif defined(_MSC_VER)
    benchmark::AddCustomContext("compiler", "MSVC " + std::to_string(_MSC_VER));
#endif
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
}
