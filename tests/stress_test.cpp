#include "threadforge/scheduler.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <latch>
#include <thread>
#include <vector>

namespace {

class Stress : public testing::TestWithParam<std::size_t> {};

TEST_P(Stress, ConcurrentProducersExecute100000TasksExactlyOnce) {
    constexpr int producer_count = 4;
    constexpr int tasks_per_producer = 25000;
    constexpr int task_count = producer_count * tasks_per_producer;
    std::vector<std::atomic<int>> executions(task_count);
    threadforge::Scheduler scheduler(GetParam());
    std::latch start(producer_count);
    std::vector<std::vector<std::future<std::uint64_t>>> results(producer_count);
    std::vector<std::jthread> producers;
    for (int p = 0; p < producer_count; ++p) {
        producers.emplace_back([&, p] {
            results[p].reserve(tasks_per_producer);
            start.arrive_and_wait();
            for (int i = 0; i < tasks_per_producer; ++i) {
                const auto id = static_cast<std::uint64_t>(p * tasks_per_producer + i);
                results[p].push_back(scheduler.submit([&, id] {
                    executions[id].fetch_add(1, std::memory_order_relaxed);
                    return id * id;
                }));
            }
        });
    }
    producers.clear();
    scheduler.shutdown();
    for (int p = 0; p < producer_count; ++p) {
        for (int i = 0; i < tasks_per_producer; ++i) {
            const auto id = static_cast<std::uint64_t>(p * tasks_per_producer + i);
            ASSERT_EQ(results[p][i].get(), id * id) << "task " << id;
            ASSERT_EQ(executions[id].load(), 1) << "task " << id;
        }
    }
}

TEST_P(Stress, SubmissionRacingShutdownDrainsEveryAcceptedTask) {
    constexpr int producer_count = 4;
    constexpr int attempts_per_producer = 2000;
    constexpr int total_attempts = producer_count * attempts_per_producer;
    std::vector<std::atomic<int>> executions(total_attempts);
    std::vector<unsigned char> accepted(total_attempts, 0);
    threadforge::Scheduler scheduler(GetParam());
    std::latch first_submissions(producer_count);
    std::vector<std::jthread> producers;
    for (int p = 0; p < producer_count; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < attempts_per_producer; ++i) {
                const auto id = p * attempts_per_producer + i;
                try {
                    scheduler.submit([&, id] {
                        executions[id].fetch_add(1, std::memory_order_relaxed);
                    });
                    accepted[id] = 1;
                } catch (const std::runtime_error&) {
                    // Closure rejects this attempt before it can enter the queue.
                }
                if (i == 0) {
                    first_submissions.count_down();
                }
            }
        });
    }
    first_submissions.wait();
    scheduler.shutdown();
    producers.clear();
    for (int id = 0; id < total_attempts; ++id) {
        ASSERT_EQ(executions[id].load(), accepted[id]) << "task " << id;
    }
    for (int p = 0; p < producer_count; ++p) {
        EXPECT_EQ(accepted[p * attempts_per_producer], 1);
    }
}

TEST_P(Stress, LocalFanOutExecutes100000TasksExactlyOnce) {
    constexpr int task_count = 100000;
    std::vector<std::atomic<int>> executions(task_count);
    threadforge::Scheduler scheduler(GetParam());
    auto parent = scheduler.submit([&] {
        std::vector<std::future<std::uint64_t>> results;
        results.reserve(task_count);
        for (int i = 0; i < task_count; ++i) {
            const auto id = static_cast<std::uint64_t>(i);
            results.push_back(scheduler.submit([&, id] {
                executions[id].fetch_add(1, std::memory_order_relaxed);
                return id * id;
            }));
        }
        return results;
    });
    // The parent returns child futures rather than waiting for them on a worker.
    auto results = parent.get();
    scheduler.shutdown();
    for (int i = 0; i < task_count; ++i) {
        const auto id = static_cast<std::uint64_t>(i);
        ASSERT_EQ(results[i].get(), id * id) << "task " << id;
        ASSERT_EQ(executions[i].load(), 1) << "task " << id;
    }
    const auto metrics = scheduler.metrics();
    EXPECT_EQ(metrics.tasks_submitted, task_count + 1U);
    EXPECT_EQ(metrics.global_tasks, 1U);
    EXPECT_EQ(metrics.local_tasks + metrics.tasks_stolen, task_count);
    std::uint64_t worker_claims = 0;
    for (const auto& worker : metrics.workers) {
        worker_claims += worker.local_tasks + worker.global_tasks + worker.tasks_stolen;
    }
    EXPECT_EQ(worker_claims, metrics.tasks_submitted);
}

TEST_P(Stress, LocalSubmissionsRacingShutdownMatchAcceptedWork) {
    constexpr int attempts = 10000;
    std::vector<std::atomic<int>> executions(attempts);
    std::vector<unsigned char> accepted(attempts, 0);
    std::latch first_submission(1);
    threadforge::Scheduler scheduler(GetParam());
    auto parent = scheduler.submit([&] {
        for (int id = 0; id < attempts; ++id) {
            try {
                scheduler.submit([&, id] {
                    executions[id].fetch_add(1, std::memory_order_relaxed);
                });
                accepted[id] = 1;
            } catch (const std::runtime_error&) {
                // Closing admission rejects local submissions through the same gate.
            }
            if (id == 0) {
                first_submission.count_down();
            }
        }
    });
    first_submission.wait();
    scheduler.shutdown();
    parent.get();
    std::uint64_t accepted_count = 1; // The externally submitted parent.
    for (int id = 0; id < attempts; ++id) {
        ASSERT_EQ(executions[id].load(), accepted[id]) << "task " << id;
        accepted_count += accepted[id];
    }
    EXPECT_EQ(accepted[0], 1);
    const auto metrics = scheduler.metrics();
    EXPECT_EQ(metrics.tasks_submitted, accepted_count);
    EXPECT_EQ(metrics.global_tasks, 1U);
    EXPECT_EQ(metrics.local_tasks + metrics.tasks_stolen + metrics.global_tasks, accepted_count);
}

TEST(StressLifecycle, RepeatedConstructionDestructionDrainsUnevenTasks) {
    for (int round = 0; round < 50; ++round) {
        std::vector<std::future<std::uint64_t>> results;
        {
            threadforge::Scheduler scheduler(static_cast<std::size_t>(round % 4 + 1));
            for (int i = 0; i < 100; ++i) {
                results.push_back(scheduler.submit([i] {
                    std::uint64_t sum = 0;
                    for (int n = 0; n <= i * 10; ++n) {
                        sum += static_cast<std::uint64_t>(n);
                    }
                    return sum;
                }));
            }
        }
        for (int i = 0; i < 100; ++i) {
            const auto limit = static_cast<std::uint64_t>(i * 10);
            ASSERT_EQ(results[i].get(), limit * (limit + 1) / 2);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(WorkerCounts, Stress, testing::Values(1U, 2U, 4U, 8U));

} // namespace
