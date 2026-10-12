#include "threadforge/priority.hpp"
#include "threadforge/scheduler.hpp"
#include "threadforge/task_queue.hpp"
#include "threadforge/work_stealing_queue.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {

using threadforge::Priority;
constexpr std::array priorities{Priority::High, Priority::Normal, Priority::Low};

void execute(std::optional<threadforge::Task> task) {
    ASSERT_TRUE(task);
    (*task)();
}

TEST(PriorityQueue, GlobalChoosesHighestAvailableAndPreservesFifoWithinLanes) {
    threadforge::TaskQueue queue;
    std::vector<int> order;
    for (int value = 1; value <= 2; ++value) {
        for (const auto priority : {Priority::Low, Priority::Normal, Priority::High}) {
            const auto label = static_cast<int>(priority) * 10 + value;
            EXPECT_TRUE(queue.push(threadforge::Task([&, label] { order.push_back(label); }), priority));
        }
    }
    for (int i = 0; i < 6; ++i) {
        execute(queue.try_pop());
    }
    const std::vector<int> expected{
        static_cast<int>(Priority::High) * 10 + 1, static_cast<int>(Priority::High) * 10 + 2,
        static_cast<int>(Priority::Normal) * 10 + 1, static_cast<int>(Priority::Normal) * 10 + 2,
        static_cast<int>(Priority::Low) * 10 + 1, static_cast<int>(Priority::Low) * 10 + 2};
    EXPECT_EQ(order, expected);
    EXPECT_FALSE(queue.try_pop());
}

TEST(PriorityQueue, GlobalCanClaimOnlySelectedLaneAndDrainClosedQueue) {
    threadforge::TaskQueue queue;
    std::vector<int> order;
    EXPECT_TRUE(queue.push(threadforge::Task([&] { order.push_back(1); }), Priority::High));
    EXPECT_TRUE(queue.push(threadforge::Task([&] { order.push_back(2); }), Priority::Low));
    EXPECT_FALSE(queue.try_pop(Priority::Normal));
    execute(queue.try_pop(Priority::Low));
    queue.close();
    execute(queue.wait_pop());
    EXPECT_FALSE(queue.wait_pop());
    EXPECT_EQ(order, (std::vector<int>{2, 1}));
    EXPECT_FALSE(queue.push(threadforge::Task([] {}), Priority::High));
}

TEST(PriorityQueue, LocalOwnerUsesLifoAndThiefUsesOldestWithinSelectedPriority) {
    threadforge::WorkStealingQueue queue;
    std::vector<int> order;
    for (const auto priority : priorities) {
        for (int value = 1; value <= 3; ++value) {
            const auto label = static_cast<int>(priority) * 10 + value;
            queue.push(threadforge::Task([&, label] { order.push_back(label); }), priority);
        }
    }
    for (const auto priority : priorities) {
        execute(queue.try_pop(priority));
        execute(queue.try_steal(priority));
        execute(queue.try_pop(priority));
        EXPECT_FALSE(queue.try_pop(priority));
        EXPECT_FALSE(queue.try_steal(priority));
    }
    std::vector<int> expected;
    for (const auto priority : priorities) {
        for (int value : {3, 1, 2}) {
            expected.push_back(static_cast<int>(priority) * 10 + value);
        }
    }
    EXPECT_EQ(order, expected);
}

TEST(PriorityQueue, LocalDefaultClaimsChooseHighestAvailableLane) {
    threadforge::WorkStealingQueue queue;
    std::vector<Priority> order;
    for (const auto priority : {Priority::Low, Priority::Normal, Priority::High}) {
        queue.push(threadforge::Task([&, priority] { order.push_back(priority); }), priority);
    }
    execute(queue.try_steal());
    execute(queue.try_pop());
    execute(queue.try_steal());
    EXPECT_EQ(order, (std::vector<Priority>{Priority::High, Priority::Normal, Priority::Low}));
    EXPECT_FALSE(queue.try_steal());
}

TEST(PriorityQueue, DefaultQueueSubmissionUsesNormalLane) {
    threadforge::TaskQueue global;
    threadforge::WorkStealingQueue local;
    int executions = 0;
    EXPECT_TRUE(global.push(threadforge::Task([&] { ++executions; })));
    local.push(threadforge::Task([&] { ++executions; }));
    EXPECT_FALSE(global.try_pop(Priority::High));
    EXPECT_FALSE(local.try_steal(Priority::Low));
    execute(global.try_pop(Priority::Normal));
    execute(local.try_pop(Priority::Normal));
    EXPECT_EQ(executions, 2);
}

TEST(PriorityQueue, InvalidPrioritiesRejectWithoutRemovingExistingWork) {
    threadforge::TaskQueue global;
    threadforge::WorkStealingQueue local;
    int executions = 0;
    EXPECT_TRUE(global.push(threadforge::Task([&] { ++executions; }), Priority::High));
    local.push(threadforge::Task([&] { ++executions; }), Priority::Low);
    for (const auto invalid : {static_cast<Priority>(-1), static_cast<Priority>(3)}) {
        EXPECT_THROW(global.push(threadforge::Task([] {}), invalid), std::invalid_argument);
        EXPECT_THROW(global.try_pop(invalid), std::invalid_argument);
        EXPECT_THROW(local.push(threadforge::Task([] {}), invalid), std::invalid_argument);
        EXPECT_THROW(local.try_pop(invalid), std::invalid_argument);
        EXPECT_THROW(local.try_steal(invalid), std::invalid_argument);
    }
    execute(global.try_pop(Priority::High));
    execute(local.try_steal(Priority::Low));
    EXPECT_EQ(executions, 2);
    EXPECT_FALSE(global.try_pop());
    EXPECT_FALSE(local.try_pop());
}

TEST(PriorityQueue, BlockingGlobalPopChoosesHighestAvailableWhenClosedAndDrainsEveryLane) {
    threadforge::TaskQueue queue;
    std::vector<Priority> order;
    for (const auto priority : {Priority::Low, Priority::Normal, Priority::High}) {
        EXPECT_TRUE(queue.push(threadforge::Task([&, priority] { order.push_back(priority); }), priority));
    }
    queue.close();
    for (int i = 0; i < 3; ++i) {
        execute(queue.wait_pop());
    }
    EXPECT_EQ(order, (std::vector<Priority>{Priority::High, Priority::Normal, Priority::Low}));
    EXPECT_FALSE(queue.wait_pop());
}

TEST(PriorityScheduling, QueuedGlobalTasksRunHighThenNormalThenLow) {
    threadforge::Scheduler scheduler(1);
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    // A Low blocker leaves the next successful-claim cursor at High.
    auto blocker = scheduler.submit(Priority::Low, [&, gate] {
        entered.count_down();
        gate.wait();
    });
    entered.wait();
    std::vector<int> order;
    for (int id : {5, 3, 1, 6, 4, 2}) {
        const auto priority = id <= 2 ? Priority::High : (id <= 4 ? Priority::Normal : Priority::Low);
        scheduler.submit(priority, [&, id] { order.push_back(id); });
    }
    release.set_value();
    blocker.get();
    scheduler.shutdown();
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 4, 5, 6}));
    EXPECT_EQ(scheduler.metrics().global_tasks, 7U);
}

TEST(PriorityScheduling, DefaultSubmissionHasNormalPriorityAndFifoOrder) {
    threadforge::Scheduler scheduler(1);
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto blocker = scheduler.submit(Priority::Low, [&, gate] { entered.count_down(); gate.wait(); });
    entered.wait();
    std::vector<int> order;
    scheduler.submit(Priority::Low, [&] { order.push_back(4); });
    scheduler.submit([&] { order.push_back(2); });
    scheduler.submit(Priority::Normal, [&] { order.push_back(3); });
    scheduler.submit(Priority::High, [&] { order.push_back(1); });
    release.set_value();
    blocker.get();
    scheduler.shutdown();
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 4}));
}

TEST(PriorityScheduling, BackloggedLanesReceiveEightFourOneFairness) {
    threadforge::Scheduler scheduler(1);
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto blocker = scheduler.submit(Priority::Low, [&, gate] { entered.count_down(); gate.wait(); });
    entered.wait();
    std::vector<Priority> order;
    for (const auto priority : priorities) {
        const int count = priority == Priority::High ? 24 : (priority == Priority::Normal ? 12 : 3);
        for (int i = 0; i < count; ++i) {
            scheduler.submit(priority, [&, priority] { order.push_back(priority); });
        }
    }
    release.set_value();
    blocker.get();
    scheduler.shutdown();
    std::vector<Priority> expected;
    for (int cycle = 0; cycle < 3; ++cycle) {
        expected.insert(expected.end(), 8, Priority::High);
        expected.insert(expected.end(), 4, Priority::Normal);
        expected.push_back(Priority::Low);
    }
    EXPECT_EQ(order, expected);
    EXPECT_EQ(scheduler.metrics().tasks_submitted, 40U);
}

TEST(PriorityScheduling, SkipsEmptyNormalLaneWithoutStallingOrLosingLowService) {
    threadforge::Scheduler scheduler(1);
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto blocker = scheduler.submit(Priority::Low, [&, gate] { entered.count_down(); gate.wait(); });
    entered.wait();
    std::vector<Priority> order;
    for (int i = 0; i < 9; ++i) {
        scheduler.submit(Priority::High, [&] { order.push_back(Priority::High); });
    }
    for (int i = 0; i < 2; ++i) {
        scheduler.submit(Priority::Low, [&] { order.push_back(Priority::Low); });
    }
    release.set_value();
    blocker.get();
    scheduler.shutdown();
    std::vector<Priority> expected(8, Priority::High);
    expected.insert(expected.end(), {Priority::Low, Priority::High, Priority::Low});
    EXPECT_EQ(order, expected);
}

TEST(PriorityScheduling, HighGlobalWorkPrecedesLowOwnerLocalWork) {
    threadforge::Scheduler scheduler(1);
    std::latch published(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::vector<int> order;
    auto parent = scheduler.submit(Priority::Low, [&, gate] {
        scheduler.submit(Priority::Low, [&] { order.push_back(2); });
        published.count_down();
        gate.wait();
    });
    published.wait();
    auto high = scheduler.submit(Priority::High, [&] { order.push_back(1); });
    release.set_value();
    parent.get();
    high.get();
    scheduler.shutdown();
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
    EXPECT_EQ(scheduler.metrics().global_tasks, 2U);
    EXPECT_EQ(scheduler.metrics().local_tasks, 1U);
}

TEST(PriorityScheduling, EqualPriorityLocalTasksUseLifoBeforeGlobalFallback) {
    threadforge::Scheduler scheduler(1);
    std::latch published(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::vector<int> order;
    auto parent = scheduler.submit(Priority::Low, [&, gate] {
        scheduler.submit(Priority::Normal, [&] { order.push_back(1); });
        scheduler.submit(Priority::Normal, [&] { order.push_back(2); });
        published.count_down();
        gate.wait();
    });
    published.wait();
    auto external = scheduler.submit(Priority::Normal, [&] { order.push_back(3); });
    release.set_value();
    parent.get();
    external.get();
    scheduler.shutdown();
    EXPECT_EQ(order, (std::vector<int>{2, 1, 3}));
}

TEST(PriorityScheduling, HighVictimWorkPrecedesLowGlobalWork) {
    threadforge::Scheduler scheduler(2);
    std::latch thief_entered(1);
    std::promise<void> release_thief;
    auto thief_gate = release_thief.get_future().share();
    auto thief_blocker = scheduler.submit(Priority::Low, [&, thief_gate] {
        thief_entered.count_down();
        thief_gate.wait();
    });
    thief_entered.wait();

    std::promise<void> release_owner;
    auto owner_gate = release_owner.get_future().share();
    std::promise<std::future<std::pair<int, std::thread::id>>> published;
    auto publication = published.get_future();
    std::promise<std::thread::id> owner_identity;
    auto owner_id = owner_identity.get_future();
    std::atomic<int> sequence{0};
    auto owner = scheduler.submit(Priority::Low, [&, owner_gate] {
        owner_identity.set_value(std::this_thread::get_id());
        published.set_value(scheduler.submit(Priority::High, [&] {
            return std::pair{sequence.fetch_add(1), std::this_thread::get_id()};
        }));
        owner_gate.wait();
    });
    auto high = publication.get();
    auto low = scheduler.submit(Priority::Low, [&] { return sequence.fetch_add(1); });
    release_thief.set_value();
    const auto high_status = high.wait_for(std::chrono::seconds(5));
    const auto low_status = low.wait_for(std::chrono::seconds(5));
    // Release every gate before assertions, including when a stealing regression occurs.
    release_owner.set_value();
    thief_blocker.get();
    owner.get();
    scheduler.shutdown();
    ASSERT_EQ(high_status, std::future_status::ready);
    ASSERT_EQ(low_status, std::future_status::ready);
    const auto high_result = high.get();
    EXPECT_EQ(high_result.first, 0);
    EXPECT_EQ(low.get(), 1);
    EXPECT_NE(high_result.second, owner_id.get());
    EXPECT_EQ(scheduler.metrics().tasks_stolen, 1U);
    EXPECT_EQ(scheduler.metrics().global_tasks, 3U);
}

TEST(PriorityScheduling, HighSubmissionDoesNotPreemptRunningLowTask) {
    threadforge::Scheduler scheduler(1);
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto low = scheduler.submit(Priority::Low, [&, gate] { entered.count_down(); gate.wait(); });
    entered.wait();
    auto high = scheduler.submit(Priority::High, [] { return 7; });
    const auto status = high.wait_for(std::chrono::seconds(0));
    release.set_value();
    low.get();
    EXPECT_EQ(status, std::future_status::timeout);
    EXPECT_EQ(high.get(), 7);
}

TEST(PriorityScheduling, ExplicitPrioritySupportsArgumentsMovesReferencesAndExceptions) {
    threadforge::Scheduler scheduler(2);
    for (const auto priority : priorities) {
        EXPECT_EQ(scheduler.submit(priority, [](int a, int b) { return a + b; }, 19, 23).get(), 42);
        EXPECT_EQ(scheduler.submit(priority, [value = std::make_unique<int>(11)] { return *value; }).get(), 11);
        EXPECT_EQ(scheduler.submit(priority, [](std::unique_ptr<int> value) { return *value; },
                                   std::make_unique<int>(17)).get(), 17);
        int value = 5;
        scheduler.submit(priority, [](int& argument) { ++argument; }, std::ref(value)).get();
        EXPECT_EQ(value, 6);
        auto failed = scheduler.submit(priority, []() -> int { throw std::logic_error("priority failure"); });
        EXPECT_THROW(failed.get(), std::logic_error);
        EXPECT_EQ(scheduler.submit(priority, [] { return 13; }).get(), 13);
    }
}

TEST(PriorityScheduling, InvalidPrioritiesRejectBeforeAdmissionAndDoNotCountAsSubmissions) {
    threadforge::Scheduler scheduler(1);
    std::atomic<int> executed{0};
    EXPECT_THROW(scheduler.submit(static_cast<Priority>(-1), [&] { ++executed; }), std::invalid_argument);
    EXPECT_THROW(scheduler.submit(static_cast<Priority>(3), [&] { ++executed; }), std::invalid_argument);
    EXPECT_EQ(scheduler.submit(Priority::High, [] { return 29; }).get(), 29);
    scheduler.shutdown();
    EXPECT_EQ(executed.load(), 0);
    EXPECT_EQ(scheduler.metrics().tasks_submitted, 1U);
}

TEST(PriorityScheduling, ShutdownDrainsAllPrioritiesFromBothLocalAndGlobalQueues) {
    threadforge::Scheduler scheduler(1);
    std::array<std::atomic<int>, 3> completed{};
    std::latch published(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto parent = scheduler.submit(Priority::Low, [&, gate] {
        for (std::size_t lane = 0; lane < priorities.size(); ++lane) {
            for (int i = 0; i < 7; ++i) {
                scheduler.submit(priorities[lane], [&, lane] { ++completed[lane]; });
            }
        }
        published.count_down();
        gate.wait();
    });
    published.wait();
    for (std::size_t lane = 0; lane < priorities.size(); ++lane) {
        for (int i = 0; i < 7; ++i) {
            scheduler.submit(priorities[lane], [&, lane] { ++completed[lane]; });
        }
    }
    auto closing = std::async(std::launch::async, [&] { scheduler.shutdown(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!scheduler.is_shutdown() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool closed = scheduler.is_shutdown();
    release.set_value();
    closing.get();
    parent.get();
    EXPECT_TRUE(closed);
    for (const auto priority : priorities) {
        EXPECT_THROW(scheduler.submit(priority, [] {}), std::runtime_error);
    }
    for (const auto& count : completed) {
        EXPECT_EQ(count.load(), 14);
    }
    const auto metrics = scheduler.metrics();
    EXPECT_EQ(metrics.tasks_submitted, 43U);
    EXPECT_EQ(metrics.local_tasks, 21U);
    EXPECT_EQ(metrics.global_tasks, 22U);
    EXPECT_EQ(metrics.tasks_stolen, 0U);
}

} // namespace
