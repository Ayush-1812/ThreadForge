#include "threadforge/scheduler.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <latch>
#include <memory>
#include <set>
#include <thread>

namespace {

TEST(Scheduler, ConfiguresWorkersAndRejectsZero) {
    threadforge::Scheduler scheduler(threadforge::Config{3});
    EXPECT_EQ(scheduler.worker_count(), 3U);
    EXPECT_FALSE(scheduler.is_shutdown());
    EXPECT_THROW(threadforge::Scheduler(0), std::invalid_argument);
}

TEST(Scheduler, DefaultConfigurationHasAtLeastOneWorker) {
    const threadforge::Config config;
    EXPECT_GE(config.worker_count, 1U);
}

TEST(Scheduler, StartsAllConfiguredWorkers) {
    threadforge::Scheduler scheduler(4);
    std::latch entered(4);
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::vector<std::future<std::thread::id>> results;
    for (int i = 0; i < 4; ++i) {
        results.push_back(scheduler.submit([&entered, gate] {
            entered.count_down();
            gate.wait();
            return std::this_thread::get_id();
        }));
    }
    entered.wait();
    release.set_value();
    std::set<std::thread::id> ids;
    for (auto& result : results) {
        ids.insert(result.get());
    }
    EXPECT_EQ(ids.size(), 4U);
}

TEST(Scheduler, ExecutesVoidTasks) {
    threadforge::Scheduler scheduler(2);
    std::atomic<int> count{0};
    scheduler.submit([&count] { count.fetch_add(1); }).get();
    EXPECT_EQ(count.load(), 1);
}

TEST(Scheduler, ReturnsValuesAndForwardsArguments) {
    threadforge::Scheduler scheduler(2);
    EXPECT_EQ(scheduler.submit([] { return 42; }).get(), 42);
    EXPECT_EQ(scheduler.submit([](int a, int b) { return a + b; }, 10, 20).get(), 30);
}

TEST(Scheduler, SupportsMoveOnlyCallablesAndArguments) {
    threadforge::Scheduler scheduler(2);
    auto captured = scheduler.submit([value = std::make_unique<int>(7)] { return *value; });
    auto argument = scheduler.submit([](std::unique_ptr<int> value) { return *value; },
                                     std::make_unique<int>(9));
    EXPECT_EQ(captured.get(), 7);
    EXPECT_EQ(argument.get(), 9);
}

TEST(Scheduler, SupportsMemberFunctionsAndExplicitReferences) {
    struct Counter {
        int value = 0;
        int add(int amount) { return value += amount; }
    };
    Counter counter;
    threadforge::Scheduler scheduler(1);
    EXPECT_EQ(scheduler.submit(&Counter::add, &counter, 5).get(), 5);
    scheduler.submit([](int& value) { ++value; }, std::ref(counter.value)).get();
    EXPECT_EQ(counter.value, 6);
    int& reference = scheduler.submit([&counter]() -> int& { return counter.value; }).get();
    EXPECT_EQ(&reference, &counter.value);
}

TEST(Scheduler, PropagatesExceptionsAndKeepsWorkerAlive) {
    threadforge::Scheduler scheduler(1);
    auto failed = scheduler.submit([]() -> int { throw std::runtime_error("task failure"); });
    EXPECT_THROW(failed.get(), std::runtime_error);
    EXPECT_EQ(scheduler.submit([] { return 11; }).get(), 11);
}

TEST(Scheduler, ExecutesThousandsOfTasksExactlyOnce) {
    threadforge::Scheduler scheduler(4);
    constexpr int task_count = 10000;
    std::vector<std::atomic<int>> executions(task_count);
    std::vector<std::future<int>> results;
    for (int i = 0; i < task_count; ++i) {
        results.push_back(scheduler.submit([&, i] {
            executions[i].fetch_add(1, std::memory_order_relaxed);
            return i * 2;
        }));
    }
    for (int i = 0; i < task_count; ++i) {
        EXPECT_EQ(results[i].get(), i * 2);
        EXPECT_EQ(executions[i].load(), 1);
    }
}

TEST(Scheduler, AcceptsConcurrentProducers) {
    threadforge::Scheduler scheduler(4);
    std::atomic<int> completed{0};
    std::latch start(4);
    std::vector<std::jthread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&] {
            start.arrive_and_wait();
            for (int i = 0; i < 1000; ++i) {
                scheduler.submit([&] { completed.fetch_add(1, std::memory_order_relaxed); });
            }
        });
    }
    producers.clear();
    scheduler.shutdown();
    EXPECT_EQ(completed.load(), 4000);
}

TEST(Scheduler, DestructorDrainsTasksAndKeepsFuturesValid) {
    std::future<int> result;
    std::atomic<int> completed{0};
    {
        threadforge::Scheduler scheduler(2);
        for (int i = 0; i < 100; ++i) {
            scheduler.submit([&] { completed.fetch_add(1); });
        }
        result = scheduler.submit([] { return 123; });
    }
    EXPECT_EQ(completed.load(), 100);
    EXPECT_EQ(result.get(), 123);
}

TEST(Scheduler, ShutdownIsIdempotentAndRejectsNewTasks) {
    threadforge::Scheduler scheduler(2);
    scheduler.shutdown();
    scheduler.shutdown();
    EXPECT_TRUE(scheduler.is_shutdown());
    EXPECT_THROW(scheduler.submit([] {}), std::runtime_error);
}

TEST(Scheduler, SupportsConcurrentShutdownCallers) {
    threadforge::Scheduler scheduler(4);
    std::atomic<int> count{0};
    for (int i = 0; i < 1000; ++i) {
        scheduler.submit([&] { count.fetch_add(1); });
    }
    std::latch start(3);
    std::vector<std::jthread> callers;
    for (int i = 0; i < 3; ++i) {
        callers.emplace_back([&] {
            start.arrive_and_wait();
            scheduler.shutdown();
        });
    }
    callers.clear();
    EXPECT_EQ(count.load(), 1000);
}

TEST(Scheduler, RejectsShutdownFromOwnWorkerWithoutDeadlock) {
    threadforge::Scheduler scheduler(1);
    auto result = scheduler.submit([&] { scheduler.shutdown(); });
    EXPECT_THROW(result.get(), std::logic_error);
    EXPECT_EQ(scheduler.submit([] { return 5; }).get(), 5);
}

TEST(Scheduler, AllowsNestedSubmissionWithoutBlockingWorker) {
    threadforge::Scheduler scheduler(1);
    auto parent = scheduler.submit([&] { return scheduler.submit([] { return 17; }); });
    auto child = parent.get();
    EXPECT_EQ(child.get(), 17);
}

} // namespace
