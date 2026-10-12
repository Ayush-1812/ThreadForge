#include "threadforge/scheduler.hpp"
#include "threadforge/work_stealing_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <thread>
#include <vector>

namespace {

TEST(WorkStealingQueue, OwnerConsumesNewestAndThiefConsumesOldest) {
    threadforge::WorkStealingQueue queue;
    std::vector<int> order;
    for (int value = 1; value <= 3; ++value) {
        queue.push(threadforge::Task([&, value] { order.push_back(value); }));
    }
    auto newest = queue.try_pop();
    auto oldest = queue.try_steal();
    auto remaining = queue.try_pop();
    ASSERT_TRUE(newest);
    ASSERT_TRUE(oldest);
    ASSERT_TRUE(remaining);
    (*newest)();
    (*oldest)();
    (*remaining)();
    EXPECT_EQ(order, (std::vector<int>{3, 1, 2}));
    EXPECT_FALSE(queue.try_pop());
    EXPECT_FALSE(queue.try_steal());
}

TEST(WorkStealingQueue, RejectsInvalidTasksAndPreservesMoveOnlyOwnership) {
    threadforge::WorkStealingQueue queue;
    EXPECT_THROW(queue.push(threadforge::Task{}), std::invalid_argument);
    std::packaged_task<int()> inner([value = std::make_unique<int>(19)] { return *value; });
    auto result = inner.get_future();
    queue.push(threadforge::Task([task = std::move(inner)]() mutable { task(); }));
    auto stolen = queue.try_steal();
    ASSERT_TRUE(stolen);
    (*stolen)();
    EXPECT_EQ(result.get(), 19);
}

TEST(WorkStealingQueue, ConcurrentOwnerAndThievesExecuteEachTaskExactlyOnce) {
    constexpr int task_count = 10000;
    threadforge::WorkStealingQueue queue;
    std::vector<std::atomic<int>> executions(task_count);
    for (int id = 0; id < task_count; ++id) {
        queue.push(threadforge::Task([&, id] { executions[id].fetch_add(1); }));
    }
    std::latch start(4);
    std::vector<std::jthread> consumers;
    for (int worker = 0; worker < 4; ++worker) {
        consumers.emplace_back([&, worker] {
            start.arrive_and_wait();
            for (;;) {
                auto task = worker == 0 ? queue.try_pop() : queue.try_steal();
                if (!task) {
                    return;
                }
                (*task)();
            }
        });
    }
    consumers.clear();
    for (int id = 0; id < task_count; ++id) {
        ASSERT_EQ(executions[id].load(), 1) << "task " << id;
    }
}

TEST(WorkStealingQueue, LastTaskHasExactlyOneOwnerUnderContention) {
    for (int round = 0; round < 50; ++round) {
        threadforge::WorkStealingQueue queue;
        std::atomic<int> claimed{0};
        std::atomic<int> executed{0};
        queue.push(threadforge::Task([&] { executed.fetch_add(1); }));
        std::latch start(4);
        std::vector<std::jthread> consumers;
        for (int worker = 0; worker < 4; ++worker) {
            consumers.emplace_back([&, worker] {
                start.arrive_and_wait();
                auto task = worker == 0 ? queue.try_pop() : queue.try_steal();
                if (task) {
                    claimed.fetch_add(1);
                    (*task)();
                }
            });
        }
        consumers.clear();
        ASSERT_EQ(claimed.load(), 1) << "round " << round;
        ASSERT_EQ(executed.load(), 1) << "round " << round;
    }
}

TEST(WorkStealing, NestedSubmissionsUseOwnerQueueInLifoOrder) {
    threadforge::Scheduler scheduler(1);
    std::vector<int> order;
    scheduler.submit([&] {
        for (int value = 1; value <= 3; ++value) {
            scheduler.submit([&, value] { order.push_back(value); });
        }
    }).get();
    scheduler.shutdown();
    EXPECT_EQ(order, (std::vector<int>{3, 2, 1}));
    const auto metrics = scheduler.metrics();
    EXPECT_EQ(metrics.tasks_submitted, 4U);
    EXPECT_EQ(metrics.global_tasks, 1U);
    EXPECT_EQ(metrics.local_tasks, 3U);
    EXPECT_EQ(metrics.tasks_stolen, 0U);
    EXPECT_EQ(metrics.failed_steal_attempts, 0U);
    EXPECT_THROW(scheduler.submit([] {}), std::runtime_error);
    EXPECT_EQ(scheduler.metrics().tasks_submitted, 4U);
}

TEST(WorkStealing, LocalQueuePrecedesGlobalFallback) {
    threadforge::Scheduler scheduler(1);
    std::vector<int> order;
    std::latch children_submitted(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto parent = scheduler.submit([&, gate] {
        scheduler.submit([&] { order.push_back(1); });
        scheduler.submit([&] { order.push_back(2); });
        children_submitted.count_down();
        gate.wait();
    });
    children_submitted.wait();
    auto external = scheduler.submit([&] { order.push_back(3); });
    release.set_value();
    parent.get();
    external.get();
    scheduler.shutdown();
    EXPECT_EQ(order, (std::vector<int>{2, 1, 3}));
    EXPECT_EQ(scheduler.metrics().global_tasks, 2U);
    EXPECT_EQ(scheduler.metrics().local_tasks, 2U);
}

TEST(WorkStealing, IdleWorkerStealsWhileOwnerRemainsBusy) {
    threadforge::Scheduler scheduler(2);
    const auto idle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (scheduler.metrics().waits < 2 && std::chrono::steady_clock::now() < idle_deadline) {
        std::this_thread::yield();
    }
    EXPECT_GE(scheduler.metrics().waits, 2U);
    std::promise<std::future<std::thread::id>> child_result;
    auto child_available = child_result.get_future();
    std::promise<std::thread::id> owner_id;
    auto owner_identity = owner_id.get_future();
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto parent = scheduler.submit([&, gate] {
        owner_id.set_value(std::this_thread::get_id());
        child_result.set_value(scheduler.submit([] { return std::this_thread::get_id(); }));
        gate.wait();
    });
    auto child = child_available.get();
    const auto status = child.wait_for(std::chrono::seconds(5));
    // Always release the owner before assertions, even when stealing regresses.
    release.set_value();
    parent.get();
    scheduler.shutdown();
    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_NE(child.get(), owner_identity.get());
    const auto metrics = scheduler.metrics();
    EXPECT_EQ(metrics.tasks_submitted, 2U);
    EXPECT_EQ(metrics.global_tasks, 1U);
    EXPECT_EQ(metrics.local_tasks, 0U);
    EXPECT_EQ(metrics.tasks_stolen, 1U);
}

TEST(WorkStealing, ThiefSubmitsDescendantsToItsOwnLocalQueue) {
    threadforge::Scheduler scheduler(2);
    std::promise<std::future<std::future<int>>> child_result;
    auto child_available = child_result.get_future();
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto parent = scheduler.submit([&, gate] {
        child_result.set_value(scheduler.submit([&] {
            return scheduler.submit([] { return 47; });
        }));
        gate.wait();
    });
    auto child = child_available.get();
    const auto child_status = child.wait_for(std::chrono::seconds(5));
    std::future<int> grandchild;
    auto grandchild_status = std::future_status::timeout;
    if (child_status == std::future_status::ready) {
        grandchild = child.get();
        grandchild_status = grandchild.wait_for(std::chrono::seconds(5));
    }
    release.set_value();
    parent.get();
    scheduler.shutdown();
    ASSERT_EQ(child_status, std::future_status::ready);
    ASSERT_EQ(grandchild_status, std::future_status::ready);
    EXPECT_EQ(grandchild.get(), 47);
    const auto metrics = scheduler.metrics();
    EXPECT_EQ(metrics.tasks_submitted, 3U);
    EXPECT_EQ(metrics.global_tasks, 1U);
    EXPECT_EQ(metrics.tasks_stolen, 1U);
    EXPECT_EQ(metrics.local_tasks, 1U);
}

TEST(WorkStealing, StolenExceptionsPropagateAndThiefRemainsAvailable) {
    threadforge::Scheduler scheduler(2);
    std::promise<std::future<int>> child_result;
    auto child_available = child_result.get_future();
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto parent = scheduler.submit([&, gate] {
        child_result.set_value(scheduler.submit([]() -> int {
            throw std::runtime_error("stolen task failed");
        }));
        gate.wait();
    });
    auto child = child_available.get();
    const auto child_status = child.wait_for(std::chrono::seconds(5));
    auto followup = scheduler.submit([] { return 53; });
    const auto followup_status = followup.wait_for(std::chrono::seconds(5));
    release.set_value();
    parent.get();
    scheduler.shutdown();
    ASSERT_EQ(child_status, std::future_status::ready);
    ASSERT_EQ(followup_status, std::future_status::ready);
    EXPECT_THROW(child.get(), std::runtime_error);
    EXPECT_EQ(followup.get(), 53);
    EXPECT_EQ(scheduler.metrics().tasks_stolen, 1U);
    EXPECT_EQ(scheduler.metrics().global_tasks, 2U);
}

TEST(WorkStealing, SubmissionFromAnotherSchedulerUsesGlobalQueue) {
    threadforge::Scheduler first(1);
    threadforge::Scheduler second(1);
    auto parent = first.submit([&] { return second.submit([] { return 23; }); });
    auto child = parent.get();
    EXPECT_EQ(child.get(), 23);
    first.shutdown();
    second.shutdown();
    EXPECT_EQ(first.metrics().global_tasks, 1U);
    EXPECT_EQ(second.metrics().global_tasks, 1U);
    EXPECT_EQ(second.metrics().local_tasks, 0U);
}

TEST(WorkStealing, WorkersWaitAndWakeForExternalSubmissions) {
    threadforge::Scheduler scheduler(3);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (scheduler.metrics().waits < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_GE(scheduler.metrics().waits, 3U);
    EXPECT_EQ(scheduler.submit([] { return 31; }).get(), 31);
    scheduler.shutdown();
    const auto metrics = scheduler.metrics();
    EXPECT_EQ(metrics.global_tasks, 1U);
    EXPECT_GE(metrics.failed_steal_attempts, 6U);
    ASSERT_EQ(metrics.workers.size(), 3U);
    std::uint64_t global_tasks = 0;
    for (const auto& worker : metrics.workers) {
        global_tasks += worker.global_tasks;
    }
    EXPECT_EQ(global_tasks, metrics.global_tasks);
}

TEST(WorkStealing, RepeatedLocalPublicationsWakeParkedWorkers) {
    threadforge::Scheduler scheduler(2);
    for (int round = 0; round < 50; ++round) {
        const auto before = scheduler.metrics().waits;
        auto parent = scheduler.submit([&, round] {
            return scheduler.submit([round] { return round * 3; });
        });
        ASSERT_EQ(parent.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        auto child = parent.get();
        ASSERT_EQ(child.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        EXPECT_EQ(child.get(), round * 3);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (scheduler.metrics().waits <= before && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        ASSERT_GT(scheduler.metrics().waits, before);
    }
    scheduler.shutdown();
    const auto metrics = scheduler.metrics();
    EXPECT_EQ(metrics.tasks_submitted, 100U);
    EXPECT_EQ(metrics.global_tasks, 50U);
    EXPECT_EQ(metrics.local_tasks + metrics.tasks_stolen, 50U);
}

TEST(WorkStealing, ShutdownDrainsOwnerQueueAfterClosingAdmission) {
    threadforge::Scheduler scheduler(1);
    constexpr int child_count = 1000;
    std::vector<std::atomic<int>> executions(child_count);
    std::latch published(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto parent = scheduler.submit([&, gate] {
        for (int id = 0; id < child_count; ++id) {
            scheduler.submit([&, id] { executions[id].fetch_add(1); });
        }
        published.count_down();
        gate.wait();
    });
    published.wait();
    auto shutdown = std::async(std::launch::async, [&] { scheduler.shutdown(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!scheduler.is_shutdown() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_TRUE(scheduler.is_shutdown());
    release.set_value();
    shutdown.get();
    parent.get();
    for (const auto& count : executions) {
        EXPECT_EQ(count.load(), 1);
    }
    EXPECT_EQ(scheduler.metrics().local_tasks, child_count);
}

} // namespace
