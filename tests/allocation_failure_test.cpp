#include "threadforge/scheduler.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <latch>
#include <new>
#include <optional>
#include <utility>

// This translation unit belongs to a separate test executable. Only ordinary
// C++ new/new[] allocations on the injecting thread are intercepted. Over-aligned
// allocation, direct malloc, and operating-system thread allocations are outside
// the sweep; scheduler nodes, dependency edges, deque storage, and C++ thread
// startup storage use the ordinary allocation paths exercised here.
namespace allocation_injection {

thread_local bool enabled = false;
thread_local std::size_t countdown = 0;
thread_local bool tripped = false;

class FailOnce {
public:
    explicit FailOnce(std::size_t index) noexcept {
        countdown = index;
        tripped = false;
        enabled = true;
    }
    ~FailOnce() { enabled = false; }
    FailOnce(const FailOnce&) = delete;
    FailOnce& operator=(const FailOnce&) = delete;
};

void* allocate(std::size_t size) {
    if (enabled) {
        if (countdown == 0) {
            // Cleanup and recovery can allocate after the one injected failure.
            enabled = false;
            tripped = true;
            throw std::bad_alloc{};
        }
        --countdown;
    }
    if (auto* storage = std::malloc(size == 0 ? 1 : size)) {
        return storage;
    }
    throw std::bad_alloc{};
}

} // namespace allocation_injection

// Prevent compilers from inlining replacement delete into ordinary new callers
// and incorrectly diagnosing the intentional malloc/free backing as a mismatch.
#if defined(_MSC_VER)
#define THREADFORGE_TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define THREADFORGE_TEST_NOINLINE __attribute__((noinline))
#else
#define THREADFORGE_TEST_NOINLINE
#endif

THREADFORGE_TEST_NOINLINE void* operator new(std::size_t size) {
    return allocation_injection::allocate(size);
}

THREADFORGE_TEST_NOINLINE void* operator new[](std::size_t size) {
    return allocation_injection::allocate(size);
}

// Prebuilt GoogleTest also uses nothrow allocation. Keep it on the same
// malloc/free path so sanitizer allocation/deallocation families remain paired.
THREADFORGE_TEST_NOINLINE void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocation_injection::allocate(size);
    } catch (...) {
        return nullptr;
    }
}

THREADFORGE_TEST_NOINLINE void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocation_injection::allocate(size);
    } catch (...) {
        return nullptr;
    }
}

THREADFORGE_TEST_NOINLINE void operator delete(void* storage) noexcept {
    std::free(storage);
}

THREADFORGE_TEST_NOINLINE void operator delete[](void* storage) noexcept {
    std::free(storage);
}

THREADFORGE_TEST_NOINLINE void operator delete(void* storage, std::size_t) noexcept {
    std::free(storage);
}

THREADFORGE_TEST_NOINLINE void operator delete[](void* storage, std::size_t) noexcept {
    std::free(storage);
}

THREADFORGE_TEST_NOINLINE void operator delete(void* storage, const std::nothrow_t&) noexcept {
    std::free(storage);
}

THREADFORGE_TEST_NOINLINE void operator delete[](void* storage, const std::nothrow_t&) noexcept {
    std::free(storage);
}

#undef THREADFORGE_TEST_NOINLINE

namespace {

constexpr std::size_t sweep_limit = 128;

TEST(AllocationFailure, ConstructorSweepCleansUpEveryPartialWorkerStartupAndRecovers) {
    bool covered_all_allocations = false;
    std::size_t rejected_allocations = 0;
    for (std::size_t index = 0; index < sweep_limit; ++index) {
        std::optional<threadforge::Scheduler> constructed;
        bool rejected = false;
        bool tripped = false;
        {
            allocation_injection::FailOnce injection(index);
            try {
                constructed.emplace(4);
            } catch (const std::bad_alloc&) {
                rejected = true;
            }
            tripped = allocation_injection::tripped;
        }
        // No assertions, future waits, or recovery work run with injection active.
        EXPECT_EQ(rejected, tripped) << "allocation " << index;
        if (rejected) {
            ++rejected_allocations;
            EXPECT_FALSE(constructed);
            threadforge::Scheduler recovery(1);
            EXPECT_EQ(recovery.submit([] { return 17; }).get(), 17);
            recovery.shutdown();
            EXPECT_EQ(recovery.metrics().tasks_completed, 1U);
        } else {
            ASSERT_TRUE(constructed);
            EXPECT_EQ(constructed->submit([] { return 19; }).get(), 19);
            constructed->shutdown();
            EXPECT_EQ(constructed->metrics().tasks_completed, 1U);
            if (!tripped) {
                covered_all_allocations = true;
                break;
            }
        }
    }
    EXPECT_GT(rejected_allocations, 0U);
    EXPECT_TRUE(covered_all_allocations) << "constructor used more than " << sweep_limit << " allocations";
}

struct LocalSubmissionOutcome {
    std::future<int> result;
    bool accepted = false;
    bool tripped = false;
};

TEST(AllocationFailure, FreshOwnerQueueSweepAcceptsDequeFailureThroughIntrusiveFallback) {
    bool covered_all_allocations = false;
    std::size_t rejected_allocations = 0;
    std::size_t fallback_acceptances = 0;
    for (std::size_t index = 0; index < sweep_limit; ++index) {
        std::atomic<int> executions{0};
        threadforge::Scheduler scheduler(1);
        // No task has used this worker's local lanes before this parent runs.
        auto parent = scheduler.submit([&, index] {
            LocalSubmissionOutcome outcome;
            {
                allocation_injection::FailOnce injection(index);
                try {
                    outcome.result = scheduler.submit([&] { ++executions; return 23; });
                    outcome.accepted = true;
                } catch (const std::bad_alloc&) {
                    outcome.accepted = false;
                }
                outcome.tripped = allocation_injection::tripped;
            }
            // The parent returns child futures, allowing the sole worker to run them.
            return outcome;
        });
        auto outcome = parent.get();
        if (outcome.accepted) {
            EXPECT_EQ(outcome.result.get(), 23);
        } else {
            ++rejected_allocations;
            EXPECT_TRUE(outcome.tripped) << "allocation " << index;
            EXPECT_FALSE(outcome.result.valid());
        }
        EXPECT_EQ(scheduler.submit([] { return 29; }).get(), 29);
        scheduler.shutdown();
        const auto finished = scheduler.metrics();
        EXPECT_EQ(executions.load(), outcome.accepted ? 1 : 0);
        EXPECT_EQ(finished.tasks_submitted, outcome.accepted ? 3U : 2U);
        EXPECT_EQ(finished.tasks_completed, finished.tasks_submitted);
        EXPECT_EQ(finished.tasks_failed, 0U);
        EXPECT_EQ(finished.tasks_pending, 0U);
        EXPECT_EQ(finished.submitted_by_priority[1], finished.tasks_submitted);
        EXPECT_EQ(finished.tasks_by_priority[1], finished.tasks_submitted);
        if (outcome.accepted && outcome.tripped) {
            ++fallback_acceptances;
            // An ordinary successful child stays local. The allocation-free
            // fallback uses the shared intrusive ready lane, counted as global.
            EXPECT_EQ(finished.local_tasks, 0U);
            EXPECT_EQ(finished.global_tasks, 3U);
        }
        if (outcome.accepted && !outcome.tripped) {
            EXPECT_EQ(finished.local_tasks, 1U);
            EXPECT_EQ(finished.global_tasks, 2U);
            covered_all_allocations = true;
            break;
        }
    }
    EXPECT_GT(rejected_allocations, 0U);
    EXPECT_GT(fallback_acceptances, 0U) << "fresh local deque allocation fallback was not exercised";
    EXPECT_TRUE(covered_all_allocations) << "local submission used more than " << sweep_limit << " allocations";
}

TEST(AllocationFailure, MultiParentRegistrationSweepRollsBackRejectedChildrenAndPreservesHealthyEdges) {
    bool covered_all_allocations = false;
    std::size_t rejected_allocations = 0;
    for (std::size_t index = 0; index < sweep_limit; ++index) {
        std::atomic<int> candidate_executions{0};
        std::atomic<int> healthy_executions{0};
        threadforge::Scheduler scheduler(2);
        std::latch entered(2);
        std::promise<void> release;
        auto gate = release.get_future().share();
        auto first = scheduler.submit_task([&, gate] { entered.count_down(); gate.wait(); return 5; });
        auto second = scheduler.submit_task([&, gate] { entered.count_down(); gate.wait(); return 7; });
        entered.wait();
        auto first_value = first.future.share();
        auto second_value = second.future.share();
        std::future<int> candidate;
        bool accepted = false;
        bool tripped = false;
        {
            allocation_injection::FailOnce injection(index);
            try {
                auto child = scheduler.submit_after({first.handle, second.handle}, [&, first_value, second_value] {
                    ++candidate_executions;
                    return first_value.get() + second_value.get();
                });
                candidate = std::move(child.future);
                accepted = true;
            } catch (const std::bad_alloc&) {
                accepted = false;
            }
            tripped = allocation_injection::tripped;
        }
        auto healthy = scheduler.submit_after({first.handle, second.handle}, [&, first_value, second_value] {
            ++healthy_executions;
            return first_value.get() + second_value.get() + 1;
        });
        const auto waiting = scheduler.metrics();
        // Release both parents before inspecting results or making assertions.
        release.set_value();
        EXPECT_EQ(first_value.get(), 5);
        EXPECT_EQ(second_value.get(), 7);
        EXPECT_EQ(healthy.future.get(), 13);
        if (accepted) {
            EXPECT_EQ(candidate.get(), 12);
        } else {
            ++rejected_allocations;
            EXPECT_FALSE(candidate.valid());
        }
        scheduler.shutdown();
        EXPECT_EQ(accepted, !tripped) << "allocation " << index;
        EXPECT_EQ(candidate_executions.load(), accepted ? 1 : 0);
        EXPECT_EQ(healthy_executions.load(), 1);
        EXPECT_EQ(waiting.tasks_running, 2U);
        EXPECT_EQ(waiting.tasks_ready, 0U);
        EXPECT_EQ(waiting.tasks_waiting_dependencies, accepted ? 2U : 1U);
        EXPECT_EQ(waiting.tasks_submitted, accepted ? 4U : 3U);
        EXPECT_EQ(waiting.tasks_pending, waiting.tasks_submitted);
        const auto finished = scheduler.metrics();
        EXPECT_EQ(finished.tasks_submitted, waiting.tasks_submitted);
        EXPECT_EQ(finished.tasks_completed, finished.tasks_submitted);
        EXPECT_EQ(finished.tasks_failed, 0U);
        EXPECT_EQ(finished.tasks_pending, 0U);
        EXPECT_EQ(finished.tasks_waiting_dependencies, 0U);
        EXPECT_EQ(finished.tasks_started, finished.tasks_submitted);
        EXPECT_EQ(finished.submitted_by_priority[1], finished.tasks_submitted);
        EXPECT_EQ(finished.tasks_by_priority[1], finished.tasks_submitted);
        if (accepted && !tripped) {
            covered_all_allocations = true;
            break;
        }
    }
    EXPECT_GT(rejected_allocations, 0U);
    EXPECT_TRUE(covered_all_allocations) << "dependency submission used more than " << sweep_limit << " allocations";
}

} // namespace
