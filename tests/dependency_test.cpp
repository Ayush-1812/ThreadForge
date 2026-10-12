#include "threadforge/scheduler.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

namespace {

using threadforge::Priority;
using threadforge::TaskHandle;

TEST(Dependencies, TaskSubmissionExposesFutureAndCopyableHandle) {
    threadforge::Scheduler scheduler(1);
    auto root = scheduler.submit_task([] { return 17; });
    EXPECT_TRUE(root.future.valid());
    EXPECT_TRUE(root.handle.valid());
    TaskHandle copied = root.handle;
    EXPECT_TRUE(copied.valid());
    EXPECT_FALSE(TaskHandle{}.valid());
    EXPECT_EQ(root.future.get(), 17);
    auto child = scheduler.submit_after({copied}, [] { return 23; });
    EXPECT_TRUE(child.handle.valid());
    EXPECT_EQ(child.future.get(), 23);
}

TEST(Dependencies, EmptyPrerequisitesRunImmediatelyWithFutureValues) {
    threadforge::Scheduler scheduler(1);
    auto normal = scheduler.submit_after({}, [](int value) { return value + 1; }, 40);
    auto high = scheduler.submit_after({}, Priority::High, [] { return 43; });
    std::vector<TaskHandle> empty;
    auto low = scheduler.submit_after(empty, Priority::Low, [] { return 47; });
    EXPECT_EQ(normal.future.get(), 41);
    EXPECT_EQ(high.future.get(), 43);
    EXPECT_EQ(low.future.get(), 47);
}

TEST(Dependencies, OneWorkerDiamondWaitsForBothBranchesWithoutBlockingAWorker) {
    threadforge::Scheduler scheduler(1);
    std::array<std::atomic<int>, 4> executions{};
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto root = scheduler.submit_task(Priority::Low, [&, gate] {
        entered.count_down();
        gate.wait();
        ++executions[0];
        return 5;
    });
    entered.wait();
    auto root_value = root.future.share();
    auto left = scheduler.submit_after({root.handle}, [&, root_value] {
        ++executions[1];
        return root_value.get() + 3;
    });
    auto right = scheduler.submit_after({root.handle}, [&, root_value] {
        ++executions[2];
        return root_value.get() * 2;
    });
    auto left_value = left.future.share();
    auto right_value = right.future.share();
    auto joined = scheduler.submit_after({left.handle, right.handle}, [&, left_value, right_value] {
        ++executions[3];
        return left_value.get() + right_value.get();
    });
    const auto pending = joined.future.wait_for(std::chrono::seconds(0));
    release.set_value();
    EXPECT_EQ(joined.future.get(), 18);
    scheduler.shutdown();
    EXPECT_EQ(pending, std::future_status::timeout);
    EXPECT_EQ(root_value.get(), 5);
    EXPECT_EQ(left_value.get(), 8);
    EXPECT_EQ(right_value.get(), 10);
    for (const auto& count : executions) {
        EXPECT_EQ(count.load(), 1);
    }
    EXPECT_EQ(scheduler.metrics().tasks_submitted, 4U);
}

TEST(Dependencies, OneWorkerFanOutAndVectorFanInExecuteExactlyOnce) {
    threadforge::Scheduler scheduler(1);
    constexpr int branches = 32;
    std::vector<std::atomic<int>> executions(branches);
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto root = scheduler.submit_task([&, gate] {
        entered.count_down();
        gate.wait();
        return 2;
    });
    entered.wait();
    auto root_value = root.future.share();
    std::vector<TaskHandle> prerequisites;
    std::vector<std::shared_future<int>> values;
    for (int i = 0; i < branches; ++i) {
        auto branch = scheduler.submit_after({root.handle}, [&, root_value, i] {
            ++executions[i];
            return root_value.get() + i;
        });
        prerequisites.push_back(branch.handle);
        values.push_back(branch.future.share());
    }
    auto joined = scheduler.submit_after(prerequisites, [values] {
        int total = 0;
        for (const auto& value : values) {
            total += value.get();
        }
        return total;
    });
    release.set_value();
    EXPECT_EQ(joined.future.get(), branches * 2 + branches * (branches - 1) / 2);
    scheduler.shutdown();
    for (const auto& count : executions) {
        EXPECT_EQ(count.load(), 1);
    }
    EXPECT_EQ(scheduler.metrics().tasks_submitted, branches + 2U);
}

TEST(Dependencies, FailedPrerequisiteStillReleasesDependentAndRetainsItsException) {
    threadforge::Scheduler scheduler(1);
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto failed = scheduler.submit_task([&, gate]() -> int {
        entered.count_down();
        gate.wait();
        throw std::logic_error("failed root");
    });
    entered.wait();
    auto failure = failed.future.share();
    auto child = scheduler.submit_after({failed.handle}, [failure] {
        try {
            static_cast<void>(failure.get());
        } catch (const std::logic_error&) {
            return 19;
        }
        return 0;
    });
    const auto pending = child.future.wait_for(std::chrono::seconds(0));
    release.set_value();
    EXPECT_EQ(child.future.get(), 19);
    EXPECT_EQ(pending, std::future_status::timeout);
    EXPECT_THROW(failure.get(), std::logic_error);
    EXPECT_EQ(scheduler.submit([] { return 29; }).get(), 29);
}

TEST(Dependencies, RegistrationAfterPrerequisiteCompletionDoesNotMissRelease) {
    threadforge::Scheduler scheduler(1);
    auto root = scheduler.submit_task([] { return 31; });
    EXPECT_EQ(root.future.get(), 31);
    // On one worker this sentinel runs after the root's completion bookkeeping.
    scheduler.submit([] {}).get();
    auto child = scheduler.submit_after({root.handle}, [] { return 37; });
    EXPECT_EQ(child.future.get(), 37);
}

TEST(Dependencies, RegistrationRacingPrerequisiteCompletionReleasesEachChild) {
    threadforge::Scheduler scheduler(4);
    for (int iteration = 0; iteration < 100; ++iteration) {
        std::latch entered(1);
        std::promise<void> release;
        auto gate = release.get_future().share();
        auto root = scheduler.submit_task([&, gate, iteration] {
            entered.count_down();
            gate.wait();
            return iteration;
        });
        entered.wait();
        auto root_value = root.future.share();
        std::latch race(2);
        std::future<int> result;
        std::jthread registrar([&] {
            race.arrive_and_wait();
            auto child = scheduler.submit_after({root.handle}, [root_value] { return root_value.get() + 1; });
            result = std::move(child.future);
        });
        race.arrive_and_wait();
        release.set_value();
        registrar.join();
        EXPECT_EQ(result.get(), iteration + 1);
    }
    scheduler.shutdown();
    EXPECT_EQ(scheduler.metrics().tasks_submitted, 200U);
}

TEST(Dependencies, DuplicatePrerequisitesAreDeduplicatedAndChildExecutesOnce) {
    threadforge::Scheduler scheduler(1);
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto root = scheduler.submit_task([&, gate] { entered.count_down(); gate.wait(); });
    entered.wait();
    std::atomic<int> executions{0};
    auto child = scheduler.submit_after({root.handle, root.handle, root.handle}, [&] {
        ++executions;
        return 41;
    });
    release.set_value();
    root.future.get();
    EXPECT_EQ(child.future.get(), 41);
    scheduler.shutdown();
    EXPECT_EQ(executions.load(), 1);
    EXPECT_EQ(scheduler.metrics().tasks_submitted, 2U);
}

TEST(Dependencies, DiscardedFuturesAndHandlesDoNotCancelAcceptedGraph) {
    threadforge::Scheduler scheduler(1);
    std::atomic<int> executions{0};
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    TaskHandle root_handle;
    {
        auto root = scheduler.submit_task([&, gate] {
            entered.count_down();
            gate.wait();
            ++executions;
        });
        root_handle = root.handle;
    }
    entered.wait();
    for (int i = 0; i < 32; ++i) {
        static_cast<void>(scheduler.submit_after({root_handle}, [&] { ++executions; }));
    }
    root_handle = TaskHandle{};
    release.set_value();
    scheduler.shutdown();
    EXPECT_EQ(executions.load(), 33);
    EXPECT_EQ(scheduler.metrics().tasks_submitted, 33U);
}

TEST(Dependencies, WaitingChildLeavesOtherWorkerAvailableForUnrelatedWork) {
    threadforge::Scheduler scheduler(2);
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto root = scheduler.submit_task(Priority::Low, [&, gate] {
        entered.count_down();
        gate.wait();
        return 43;
    });
    entered.wait();
    auto value = root.future.share();
    auto child = scheduler.submit_after({root.handle}, Priority::High, [value] { return value.get() + 1; });
    auto unrelated = scheduler.submit([] { return 47; });
    const auto unrelated_status = unrelated.wait_for(std::chrono::seconds(5));
    const auto child_status = child.future.wait_for(std::chrono::seconds(0));
    release.set_value();
    scheduler.shutdown();
    ASSERT_EQ(unrelated_status, std::future_status::ready);
    EXPECT_EQ(child_status, std::future_status::timeout);
    EXPECT_EQ(unrelated.get(), 47);
    EXPECT_EQ(child.future.get(), 44);
}

TEST(Dependencies, SupportsMoveOnlyFunctionsArgumentsAndReferenceResults) {
    threadforge::Scheduler scheduler(1);
    auto root = scheduler.submit_task(Priority::High, [value = std::make_unique<int>(53)] { return *value; });
    auto root_value = root.future.share();
    auto moved = scheduler.submit_after({root.handle}, Priority::Low,
        [root_value, owned = std::make_unique<int>(7)](std::unique_ptr<int> argument) {
            return root_value.get() + *owned + *argument;
        }, std::make_unique<int>(11));
    int target = 3;
    auto reference = scheduler.submit_after({moved.handle}, Priority::Normal,
        [](int& value) -> int& { ++value; return value; }, std::ref(target));
    EXPECT_EQ(moved.future.get(), 71);
    int& returned = reference.future.get();
    EXPECT_EQ(&returned, &target);
    EXPECT_EQ(target, 4);
}

TEST(Dependencies, PendingDescendantsUseExplicitPrioritiesAndDefaultNormalWithoutInheritance) {
    for (const auto root_priority : {Priority::High, Priority::Low}) {
        SCOPED_TRACE(static_cast<int>(root_priority));
        threadforge::Scheduler scheduler(1);
        std::latch entered(1);
        std::promise<void> release;
        auto gate = release.get_future().share();
        auto root = scheduler.submit_task(root_priority, [&, gate] { entered.count_down(); gate.wait(); });
        entered.wait();
        std::vector<int> order;
        // Promoted descendants from one completion preserve registration FIFO.
        // The registration order therefore distinguishes either inherited class.
        if (root_priority == Priority::High) {
            scheduler.submit_after({root.handle}, [&] { order.push_back(2); });
            scheduler.submit_after({root.handle}, Priority::High, [&] { order.push_back(1); });
            scheduler.submit_after({root.handle}, Priority::Low, [&] { order.push_back(3); });
        } else {
            scheduler.submit_after({root.handle}, Priority::Low, [&] { order.push_back(3); });
            scheduler.submit_after({root.handle}, [&] { order.push_back(2); });
            scheduler.submit_after({root.handle}, Priority::High, [&] { order.push_back(1); });
        }
        release.set_value();
        root.future.get();
        scheduler.shutdown();
        EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
    }
}

TEST(Dependencies, InvalidEmptyForeignAndExpiredHandlesRejectBeforeAdmission) {
    threadforge::Scheduler scheduler(1);
    threadforge::Scheduler other(1);
    auto valid = scheduler.submit_task([] {});
    auto foreign = other.submit_task([] {});
    valid.future.get();
    foreign.future.get();
    TaskHandle expired;
    {
        threadforge::Scheduler previous(1);
        auto old = previous.submit_task([] {});
        expired = old.handle;
        old.future.get();
    }
    EXPECT_FALSE(expired.valid());
    std::atomic<int> executed{0};
    EXPECT_THROW(scheduler.submit_after({TaskHandle{}}, [&] { ++executed; }), std::invalid_argument);
    EXPECT_THROW(scheduler.submit_after({foreign.handle}, [&] { ++executed; }), std::invalid_argument);
    EXPECT_THROW(scheduler.submit_after({expired}, [&] { ++executed; }), std::invalid_argument);
    EXPECT_THROW(scheduler.submit_after({valid.handle, TaskHandle{}}, [&] { ++executed; }), std::invalid_argument);
    EXPECT_THROW(scheduler.submit_after({valid.handle}, static_cast<Priority>(-1), [&] { ++executed; }),
                 std::invalid_argument);
    EXPECT_THROW(scheduler.submit_task(static_cast<Priority>(3), [&] { ++executed; }), std::invalid_argument);
    scheduler.shutdown();
    EXPECT_EQ(executed.load(), 0);
    EXPECT_EQ(scheduler.metrics().tasks_submitted, 1U);
}

TEST(Dependencies, ShutdownRejectsNewRootsAndChildrenWithoutChangingMetrics) {
    threadforge::Scheduler scheduler(1);
    auto root = scheduler.submit_task([] {});
    root.future.get();
    scheduler.shutdown();
    EXPECT_THROW(scheduler.submit_task([] {}), std::runtime_error);
    EXPECT_THROW(scheduler.submit_after({root.handle}, [] {}), std::runtime_error);
    EXPECT_THROW(scheduler.submit_after({}, Priority::High, [] {}), std::runtime_error);
    EXPECT_EQ(scheduler.metrics().tasks_submitted, 1U);
}

TEST(Dependencies, ShutdownWaitsForRunningRootAndDrainsAlreadyAcceptedWaitingGraph) {
    threadforge::Scheduler scheduler(1);
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto root = scheduler.submit_task([&, gate] {
        entered.count_down();
        gate.wait();
        return 5;
    });
    entered.wait();
    auto root_value = root.future.share();
    auto first = scheduler.submit_after({root.handle}, [root_value] { return root_value.get() * 2; });
    auto first_value = first.future.share();
    auto second = scheduler.submit_after({first.handle}, Priority::Low, [first_value] { return first_value.get() + 3; });
    auto second_value = second.future.share();
    auto last = scheduler.submit_after({root.handle, second.handle}, Priority::High,
        [root_value, second_value] { return root_value.get() + second_value.get(); });
    auto closing = std::async(std::launch::async, [&] { scheduler.shutdown(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!scheduler.is_shutdown() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool closed = scheduler.is_shutdown();
    const auto closing_status = closing.wait_for(std::chrono::seconds(0));
    const auto last_status = last.future.wait_for(std::chrono::seconds(0));
    release.set_value();
    closing.get();
    EXPECT_TRUE(closed);
    EXPECT_EQ(closing_status, std::future_status::timeout);
    EXPECT_EQ(last_status, std::future_status::timeout);
    EXPECT_EQ(last.future.get(), 18);
    EXPECT_EQ(first_value.get(), 10);
    EXPECT_EQ(second_value.get(), 13);
    const auto metrics = scheduler.metrics();
    EXPECT_EQ(metrics.tasks_submitted, 4U);
    EXPECT_EQ(metrics.local_tasks + metrics.global_tasks + metrics.tasks_stolen, 4U);
}

} // namespace
