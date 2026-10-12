#include "threadforge/priority.hpp"
#include "threadforge/scheduler.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <future>
#include <latch>
#include <utility>
#include <vector>

namespace {

using threadforge::Priority;

TEST(PriorityApi, NestedDefaultSubmissionUsesNormalRegardlessOfParentPriority) {
    for (const auto parent_priority : {Priority::High, Priority::Low}) {
        SCOPED_TRACE(static_cast<int>(parent_priority));
        threadforge::Scheduler scheduler(1);
        std::vector<int> order;
        auto parent = scheduler.submit(parent_priority, [&] {
            // This insertion order distinguishes inherited High and inherited Low
            // from Normal: each local lane consumes its newest task first.
            scheduler.submit(Priority::High, [&] { order.push_back(1); });
            scheduler.submit([&] { order.push_back(2); });
            scheduler.submit(Priority::Low, [&] { order.push_back(3); });
        });
        parent.get();
        scheduler.shutdown();
        EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
        EXPECT_EQ(scheduler.metrics().local_tasks, 3U);
    }
}

TEST(PriorityApi, PriorityEnumCanBeAnOrdinaryCallableArgument) {
    threadforge::Scheduler scheduler(1);
    for (const auto argument : {Priority::High, Priority::Normal, Priority::Low}) {
        EXPECT_EQ(scheduler.submit([](Priority value) { return value; }, argument).get(), argument);
        EXPECT_EQ(scheduler.submit(Priority::Low, [](Priority value) { return value; }, argument).get(), argument);
    }
}

TEST(PriorityFairness, PendingLowTaskRunsWhileHighTasksKeepSubmittingSuccessors) {
    threadforge::Scheduler scheduler(1);
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto blocker = scheduler.submit(Priority::Low, [&, gate] {
        entered.count_down();
        gate.wait();
    });
    entered.wait();

    constexpr int chain_length = 32;
    int high_completed = 0;
    int normal_completed = 0;
    std::promise<void> chain_complete;
    auto chain_done = chain_complete.get_future();
    std::function<void()> high_chain;
    high_chain = [&] {
        ++high_completed;
        if (high_completed < chain_length) {
            scheduler.submit(Priority::High, high_chain);
        } else {
            chain_complete.set_value();
        }
    };
    scheduler.submit(Priority::High, high_chain);
    for (int i = 0; i < 4; ++i) {
        scheduler.submit(Priority::Normal, [&] { ++normal_completed; });
    }
    auto low = scheduler.submit(Priority::Low, [&] {
        return std::pair{high_completed, normal_completed};
    });
    release.set_value();
    blocker.get();
    const auto observed_at_low = low.get();
    // Keep admission open until the last successor has been submitted and run.
    chain_done.get();
    scheduler.shutdown();
    EXPECT_EQ(observed_at_low.first, 8);
    EXPECT_EQ(observed_at_low.second, 4);
    EXPECT_LT(observed_at_low.first, chain_length);
    EXPECT_EQ(high_completed, chain_length);
    EXPECT_EQ(scheduler.metrics().tasks_submitted, 38U);
    EXPECT_EQ(scheduler.metrics().local_tasks, 31U);
}

} // namespace
