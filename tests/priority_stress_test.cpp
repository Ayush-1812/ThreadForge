#include "threadforge/priority.hpp"
#include "threadforge/scheduler.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <latch>
#include <thread>
#include <vector>

namespace {

constexpr std::array priorities{threadforge::Priority::High, threadforge::Priority::Normal,
                                threadforge::Priority::Low};

void check_claim_metrics(const threadforge::MetricsSnapshot& metrics, std::uint64_t submitted) {
    EXPECT_EQ(metrics.tasks_submitted, submitted);
    EXPECT_EQ(metrics.local_tasks + metrics.global_tasks + metrics.tasks_stolen, submitted);
    std::uint64_t claims = 0;
    for (const auto& worker : metrics.workers) {
        claims += worker.local_tasks + worker.global_tasks + worker.tasks_stolen;
    }
    EXPECT_EQ(claims, submitted);
}

class PriorityStress : public testing::TestWithParam<std::size_t> {};

TEST_P(PriorityStress, ConcurrentMixedPriorityProducersExecute100000TasksExactlyOnce) {
    constexpr int producer_count = 4;
    constexpr int tasks_per_producer = 25000;
    constexpr int task_count = producer_count * tasks_per_producer;
    std::vector<std::atomic<int>> executions(task_count);
    threadforge::Scheduler scheduler(GetParam());
    std::latch start(producer_count);
    std::vector<std::vector<std::future<std::uint64_t>>> results(producer_count);
    std::vector<std::jthread> producers;
    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            results[producer].reserve(tasks_per_producer);
            start.arrive_and_wait();
            for (int i = 0; i < tasks_per_producer; ++i) {
                const auto id = static_cast<std::uint64_t>(producer * tasks_per_producer + i);
                results[producer].push_back(scheduler.submit(priorities[id % priorities.size()], [&, id] {
                    executions[id].fetch_add(1, std::memory_order_relaxed);
                    return id * id + 7;
                }));
            }
        });
    }
    producers.clear();
    scheduler.shutdown();
    for (int producer = 0; producer < producer_count; ++producer) {
        for (int i = 0; i < tasks_per_producer; ++i) {
            const auto id = static_cast<std::uint64_t>(producer * tasks_per_producer + i);
            ASSERT_EQ(results[producer][i].get(), id * id + 7) << "task " << id;
            ASSERT_EQ(executions[id].load(), 1) << "task " << id;
        }
    }
    check_claim_metrics(scheduler.metrics(), task_count);
    EXPECT_EQ(scheduler.metrics().global_tasks, task_count);
}

TEST_P(PriorityStress, MixedPriorityLocalFanOutExecutes100000TasksExactlyOnce) {
    constexpr int task_count = 100000;
    std::vector<std::atomic<int>> executions(task_count);
    threadforge::Scheduler scheduler(GetParam());
    auto parent = scheduler.submit(threadforge::Priority::Low, [&] {
        std::vector<std::future<std::uint64_t>> results;
        results.reserve(task_count);
        for (int i = 0; i < task_count; ++i) {
            const auto id = static_cast<std::uint64_t>(i);
            results.push_back(scheduler.submit(priorities[id % priorities.size()], [&, id] {
                executions[id].fetch_add(1, std::memory_order_relaxed);
                return id * id + 11;
            }));
        }
        return results;
    });
    // Waiting for children on this submitting worker would deadlock at one worker.
    auto results = parent.get();
    scheduler.shutdown();
    for (int i = 0; i < task_count; ++i) {
        const auto id = static_cast<std::uint64_t>(i);
        ASSERT_EQ(results[i].get(), id * id + 11) << "task " << id;
        ASSERT_EQ(executions[id].load(), 1) << "task " << id;
    }
    const auto metrics = scheduler.metrics();
    check_claim_metrics(metrics, task_count + 1U);
    EXPECT_EQ(metrics.global_tasks, 1U);
    EXPECT_EQ(metrics.local_tasks + metrics.tasks_stolen, task_count);
}

INSTANTIATE_TEST_SUITE_P(WorkerCounts, PriorityStress, testing::Values(1U, 2U, 4U, 8U));

} // namespace
