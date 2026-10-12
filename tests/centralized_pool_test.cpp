#include "../benchmarks/centralized_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

namespace {

using threadforge::benchmarks::CentralizedPool;

TEST(CentralizedPool, RejectsZeroWorkers) {
    EXPECT_THROW(CentralizedPool(0), std::invalid_argument);
}

TEST(CentralizedPool, ReturnsResultsAndSupportsMoveOnlyStorage) {
    CentralizedPool pool(2);
    EXPECT_EQ(pool.submit([](int a, int b) { return a + b; }, 3, 4).get(), 7);
    EXPECT_EQ(pool.submit([value = std::make_unique<int>(11)] { return *value; }).get(), 11);
    EXPECT_EQ(pool.submit([](std::unique_ptr<int> value) { return *value; },
                          std::make_unique<int>(13)).get(), 13);
    int value = 2;
    pool.submit([](int& target) { ++target; }, std::ref(value)).get();
    EXPECT_EQ(value, 3);
    int& reference = pool.submit([&value]() -> int& { return value; }).get();
    EXPECT_EQ(&reference, &value);
}

TEST(CentralizedPool, PropagatesExceptionsWithoutLosingWorker) {
    CentralizedPool pool(1);
    auto failed = pool.submit([]() -> int { throw std::runtime_error("baseline failure"); });
    EXPECT_THROW(failed.get(), std::runtime_error);
    EXPECT_EQ(pool.submit([] { return 17; }).get(), 17);
}

TEST(CentralizedPool, SingleWorkerClaimsQueuedTasksInFifoOrder) {
    std::vector<int> order;
    CentralizedPool pool(1);
    std::promise<void> entered;
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto blocker = pool.submit([&entered, gate] {
        entered.set_value();
        gate.wait();
    });
    entered.get_future().get();
    std::vector<std::future<void>> tasks;
    for (int i = 0; i < 20; ++i) {
        tasks.push_back(pool.submit([&order, i] { order.push_back(i); }));
    }
    release.set_value();
    blocker.get();
    for (auto& task : tasks) {
        task.get();
    }
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(order[static_cast<std::size_t>(i)], i);
    }
}

TEST(CentralizedPool, ConcurrentProducersExecuteEveryTaskExactlyOnce) {
    constexpr std::size_t task_count = 2048;
    std::vector<std::atomic<unsigned>> executions(task_count);
    CentralizedPool pool(4);
    std::latch start(2);
    std::vector<std::jthread> producers;
    for (std::size_t producer = 0; producer < 2; ++producer) {
        producers.emplace_back([&, producer] {
            start.arrive_and_wait();
            for (std::size_t id = producer; id < task_count; id += 2) {
                pool.submit([&executions, id] {
                    executions[id].fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }
    producers.clear();
    pool.shutdown();
    for (const auto& count : executions) {
        EXPECT_EQ(count.load(), 1U);
    }
}

TEST(CentralizedPool, OneWorkerFanoutReturnsFuturesWithoutWaitingForChildren) {
    CentralizedPool pool(1);
    auto parent = pool.submit([&pool] {
        std::vector<std::future<int>> children;
        for (int i = 0; i < 10; ++i) {
            children.push_back(pool.submit([i] { return i * i; }));
        }
        return children;
    });
    auto children = parent.get();
    int sum = 0;
    for (auto& child : children) {
        sum += child.get();
    }
    EXPECT_EQ(sum, 285);
}

TEST(CentralizedPool, ShutdownClosesAdmissionAndDrainsRunningAndQueuedWork) {
    CentralizedPool pool(1);
    std::promise<void> entered;
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto running = pool.submit([&entered, gate] {
        entered.set_value();
        gate.wait();
        return 19;
    });
    entered.get_future().get();
    auto queued = pool.submit([] { return 23; });
    auto stopped = std::async(std::launch::async, [&pool] { pool.shutdown(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!pool.is_shutdown() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_TRUE(pool.is_shutdown());
    EXPECT_EQ(stopped.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
    EXPECT_EQ(queued.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
    EXPECT_THROW(pool.submit([] {}), std::runtime_error);
    release.set_value();
    stopped.get();
    EXPECT_EQ(running.get(), 19);
    EXPECT_EQ(queued.get(), 23);
    pool.shutdown();
}

TEST(CentralizedPool, DestructorDrainsWorkAndFuturesOutlivePool) {
    std::atomic<unsigned> count{0};
    std::future<int> retained;
    {
        CentralizedPool pool(2);
        for (int i = 0; i < 100; ++i) {
            pool.submit([&count] { count.fetch_add(1, std::memory_order_relaxed); });
        }
        retained = pool.submit([] { return 29; });
    }
    EXPECT_EQ(count.load(), 100U);
    EXPECT_EQ(retained.get(), 29);
}

TEST(CentralizedPool, RejectsSelfShutdownAndSupportsConcurrentExternalJoiners) {
    CentralizedPool pool(2);
    auto self_shutdown = pool.submit([&pool] { pool.shutdown(); });
    EXPECT_THROW(self_shutdown.get(), std::logic_error);
    EXPECT_EQ(pool.submit([] { return 31; }).get(), 31);
    auto first = std::async(std::launch::async, [&pool] { pool.shutdown(); });
    auto second = std::async(std::launch::async, [&pool] { pool.shutdown(); });
    first.get();
    second.get();
    EXPECT_TRUE(pool.is_shutdown());
}

} // namespace
