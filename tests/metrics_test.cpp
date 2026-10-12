#include "threadforge/scheduler.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using threadforge::MetricsSnapshot;
using threadforge::Priority;
using threadforge::WorkerMetricsSnapshot;

void check_worker_totals(const MetricsSnapshot& snapshot) {
    const auto sum = [&snapshot](std::uint64_t WorkerMetricsSnapshot::* field) {
        std::uint64_t total = 0;
        for (const auto& worker : snapshot.workers) {
            total += worker.*field;
        }
        return total;
    };
    EXPECT_EQ(snapshot.local_tasks, sum(&WorkerMetricsSnapshot::local_tasks));
    EXPECT_EQ(snapshot.global_tasks, sum(&WorkerMetricsSnapshot::global_tasks));
    EXPECT_EQ(snapshot.tasks_stolen, sum(&WorkerMetricsSnapshot::tasks_stolen));
    EXPECT_EQ(snapshot.failed_steal_attempts, sum(&WorkerMetricsSnapshot::failed_steal_attempts));
    EXPECT_EQ(snapshot.waits, sum(&WorkerMetricsSnapshot::waits));
    EXPECT_EQ(snapshot.tasks_started, sum(&WorkerMetricsSnapshot::tasks_started));
    EXPECT_EQ(snapshot.tasks_completed, sum(&WorkerMetricsSnapshot::tasks_completed));
    EXPECT_EQ(snapshot.tasks_failed, sum(&WorkerMetricsSnapshot::tasks_failed));
    EXPECT_EQ(snapshot.total_execution_ns, sum(&WorkerMetricsSnapshot::total_execution_ns));
    EXPECT_EQ(snapshot.total_queue_wait_ns, sum(&WorkerMetricsSnapshot::total_queue_wait_ns));
    EXPECT_EQ(snapshot.total_idle_wait_ns, sum(&WorkerMetricsSnapshot::total_idle_wait_ns));
    std::uint64_t max_execution = 0;
    std::uint64_t max_queue_wait = 0;
    std::array<std::uint64_t, 3> priorities{};
    for (const auto& worker : snapshot.workers) {
        max_execution = std::max(max_execution, worker.max_execution_ns);
        max_queue_wait = std::max(max_queue_wait, worker.max_queue_wait_ns);
        for (std::size_t lane = 0; lane < priorities.size(); ++lane) {
            priorities[lane] += worker.tasks_by_priority[lane];
        }
    }
    EXPECT_EQ(snapshot.max_execution_ns, max_execution);
    EXPECT_EQ(snapshot.max_queue_wait_ns, max_queue_wait);
    EXPECT_EQ(snapshot.tasks_by_priority, priorities);
    EXPECT_EQ(snapshot.tasks_started, snapshot.tasks_submitted);
    EXPECT_EQ(snapshot.tasks_completed, snapshot.tasks_submitted);
    EXPECT_EQ(snapshot.local_tasks + snapshot.global_tasks + snapshot.tasks_stolen, snapshot.tasks_submitted);
    EXPECT_EQ(snapshot.tasks_pending, 0U);
    EXPECT_EQ(snapshot.tasks_ready, 0U);
    EXPECT_EQ(snapshot.tasks_running, 0U);
    EXPECT_EQ(snapshot.tasks_waiting_dependencies, 0U);
}

void check_disabled_timing(const MetricsSnapshot& snapshot) {
    EXPECT_FALSE(snapshot.timing_enabled);
    EXPECT_EQ(snapshot.total_execution_ns, 0U);
    EXPECT_EQ(snapshot.max_execution_ns, 0U);
    EXPECT_EQ(snapshot.total_queue_wait_ns, 0U);
    EXPECT_EQ(snapshot.max_queue_wait_ns, 0U);
    EXPECT_EQ(snapshot.total_idle_wait_ns, 0U);
    EXPECT_EQ(snapshot.total_dependency_wait_ns, 0U);
    for (const auto& worker : snapshot.workers) {
        EXPECT_EQ(worker.total_execution_ns, 0U);
        EXPECT_EQ(worker.max_execution_ns, 0U);
        EXPECT_EQ(worker.total_queue_wait_ns, 0U);
        EXPECT_EQ(worker.max_queue_wait_ns, 0U);
        EXPECT_EQ(worker.total_idle_wait_ns, 0U);
    }
}

TEST(Metrics, IdleSchedulerHasZeroTaskCountersAndStableWorkerSlots) {
    threadforge::Scheduler scheduler(3);
    const auto idle = scheduler.metrics();
    EXPECT_EQ(idle.workers.size(), 3U);
    EXPECT_EQ(idle.tasks_submitted, 0U);
    EXPECT_EQ(idle.tasks_started, 0U);
    EXPECT_EQ(idle.tasks_completed, 0U);
    EXPECT_EQ(idle.tasks_failed, 0U);
    EXPECT_EQ(idle.tasks_pending, 0U);
    EXPECT_EQ(idle.tasks_ready, 0U);
    EXPECT_EQ(idle.tasks_running, 0U);
    EXPECT_EQ(idle.tasks_waiting_dependencies, 0U);
    EXPECT_EQ(idle.peak_tasks_pending, 0U);
    EXPECT_EQ(idle.submitted_by_priority, (std::array<std::uint64_t, 3>{0, 0, 0}));
    EXPECT_EQ(idle.tasks_by_priority, (std::array<std::uint64_t, 3>{0, 0, 0}));
    scheduler.shutdown();
    check_worker_totals(scheduler.metrics());
    check_disabled_timing(scheduler.metrics());
}

TEST(Metrics, GatedRunningAndQueuedTasksExposeLifecycleAndPeakGauges) {
    threadforge::Scheduler scheduler(1);
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto blocker = scheduler.submit(Priority::Low, [&, gate] {
        entered.count_down();
        gate.wait();
        return 11;
    });
    entered.wait();
    const auto running = scheduler.metrics();
    auto failed = scheduler.submit(Priority::High, []() -> int { throw std::runtime_error("counted failure"); });
    auto normal = scheduler.submit([] { return 17; });
    auto low = scheduler.submit(Priority::Low, [] {});
    const auto queued = scheduler.metrics();
    release.set_value();
    EXPECT_EQ(blocker.get(), 11);
    EXPECT_THROW(failed.get(), std::runtime_error);
    EXPECT_EQ(normal.get(), 17);
    low.get();
    scheduler.shutdown();
    EXPECT_EQ(running.tasks_submitted, 1U);
    EXPECT_EQ(running.tasks_started, 1U);
    EXPECT_EQ(running.tasks_completed, 0U);
    EXPECT_EQ(running.tasks_running, 1U);
    EXPECT_EQ(running.tasks_ready, 0U);
    EXPECT_EQ(running.tasks_pending, 1U);
    EXPECT_EQ(queued.tasks_submitted, 4U);
    EXPECT_EQ(queued.tasks_started, 1U);
    EXPECT_EQ(queued.tasks_completed, 0U);
    EXPECT_EQ(queued.tasks_failed, 0U);
    EXPECT_EQ(queued.tasks_running, 1U);
    EXPECT_EQ(queued.tasks_ready, 3U);
    EXPECT_EQ(queued.tasks_waiting_dependencies, 0U);
    EXPECT_EQ(queued.tasks_pending, 4U);
    EXPECT_EQ(queued.peak_tasks_pending, 4U);
    const auto finished = scheduler.metrics();
    EXPECT_EQ(finished.tasks_failed, 1U);
    EXPECT_EQ(finished.peak_tasks_pending, 4U);
    EXPECT_EQ(finished.submitted_by_priority, (std::array<std::uint64_t, 3>{1, 1, 2}));
    EXPECT_EQ(finished.tasks_by_priority, finished.submitted_by_priority);
    check_worker_totals(finished);
    check_disabled_timing(finished);
}

TEST(Metrics, DependencyWaitersMoveThroughRunningLifecycleWithoutBeingReadyEarly) {
    threadforge::Scheduler scheduler(1);
    std::latch root_entered(1);
    std::promise<void> release_root;
    auto root_gate = release_root.get_future().share();
    auto root = scheduler.submit_task(Priority::Low, [&, root_gate] {
        root_entered.count_down();
        root_gate.wait();
        return 5;
    });
    root_entered.wait();
    auto root_value = root.future.share();
    std::latch child_entered(1);
    std::promise<void> release_child;
    auto child_gate = release_child.get_future().share();
    auto child = scheduler.submit_after({root.handle}, [&, root_value, child_gate] {
        child_entered.count_down();
        child_gate.wait();
        return root_value.get() + 3;
    });
    auto child_value = child.future.share();
    auto grandchild = scheduler.submit_after({child.handle}, Priority::High, [child_value] { return child_value.get() * 2; });
    const auto waiting = scheduler.metrics();
    release_root.set_value();
    child_entered.wait();
    const auto child_running = scheduler.metrics();
    release_child.set_value();
    EXPECT_EQ(grandchild.future.get(), 16);
    scheduler.shutdown();
    EXPECT_EQ(waiting.tasks_submitted, 3U);
    EXPECT_EQ(waiting.tasks_pending, 3U);
    EXPECT_EQ(waiting.tasks_running, 1U);
    EXPECT_EQ(waiting.tasks_ready, 0U);
    EXPECT_EQ(waiting.tasks_waiting_dependencies, 2U);
    EXPECT_EQ(waiting.tasks_started, 1U);
    EXPECT_EQ(child_running.tasks_pending, 2U);
    EXPECT_EQ(child_running.tasks_running, 1U);
    EXPECT_EQ(child_running.tasks_ready, 0U);
    EXPECT_EQ(child_running.tasks_waiting_dependencies, 1U);
    EXPECT_EQ(child_running.tasks_started, 2U);
    EXPECT_EQ(child_running.tasks_completed, 1U);
    const auto finished = scheduler.metrics();
    EXPECT_EQ(finished.peak_tasks_pending, 3U);
    EXPECT_EQ(finished.submitted_by_priority, (std::array<std::uint64_t, 3>{1, 1, 1}));
    EXPECT_EQ(finished.tasks_by_priority, finished.submitted_by_priority);
    check_worker_totals(finished);
    check_disabled_timing(finished);
}

TEST(Metrics, ValueVoidReferenceAndCaughtExceptionSucceedWhileEscapingExceptionsFail) {
    threadforge::Scheduler scheduler(3);
    EXPECT_EQ(scheduler.submit(Priority::High, [] { return 7; }).get(), 7);
    int target = 0;
    scheduler.submit([&] { target = 11; }).get();
    int& reference = scheduler.submit(Priority::Low, [&]() -> int& { return target; }).get();
    EXPECT_EQ(&reference, &target);
    EXPECT_EQ(reference, 11);
    auto failed_high = scheduler.submit(Priority::High, []() -> int { throw std::runtime_error("failure"); });
    auto failed_normal = scheduler.submit([] { throw 9; });
    EXPECT_THROW(failed_high.get(), std::runtime_error);
    EXPECT_THROW(failed_normal.get(), int);
    EXPECT_EQ(scheduler.submit(Priority::Low, [] {
        try {
            throw std::logic_error("caught inside task");
        } catch (const std::logic_error&) {
            return 13;
        }
    }).get(), 13);
    scheduler.shutdown();
    const auto finished = scheduler.metrics();
    EXPECT_EQ(finished.tasks_submitted, 6U);
    EXPECT_EQ(finished.tasks_failed, 2U);
    EXPECT_EQ(finished.submitted_by_priority, (std::array<std::uint64_t, 3>{2, 2, 2}));
    EXPECT_EQ(finished.tasks_by_priority, finished.submitted_by_priority);
    check_worker_totals(finished);
    check_disabled_timing(finished);
}

TEST(Metrics, RejectedSubmissionsDoNotAlterAdmissionOrPriorityCounters) {
    threadforge::Scheduler scheduler(1);
    std::latch entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto root = scheduler.submit_task(Priority::Low, [&, gate] { entered.count_down(); gate.wait(); });
    entered.wait();
    EXPECT_THROW(scheduler.submit(static_cast<Priority>(3), [] {}), std::invalid_argument);
    EXPECT_THROW(scheduler.submit_after({threadforge::TaskHandle{}}, [] {}), std::invalid_argument);
    EXPECT_THROW(scheduler.submit_after({root.handle}, static_cast<Priority>(-1), [] {}), std::invalid_argument);
    const auto before_release = scheduler.metrics();
    release.set_value();
    root.future.get();
    scheduler.shutdown();
    EXPECT_THROW(scheduler.submit(Priority::High, [] {}), std::runtime_error);
    EXPECT_THROW(scheduler.submit_after({root.handle}, [] {}), std::runtime_error);
    EXPECT_EQ(before_release.tasks_submitted, 1U);
    EXPECT_EQ(before_release.tasks_pending, 1U);
    EXPECT_EQ(before_release.peak_tasks_pending, 1U);
    EXPECT_EQ(before_release.submitted_by_priority, (std::array<std::uint64_t, 3>{0, 0, 1}));
    const auto finished = scheduler.metrics();
    EXPECT_EQ(finished.tasks_submitted, 1U);
    EXPECT_EQ(finished.tasks_failed, 0U);
    EXPECT_EQ(finished.submitted_by_priority, before_release.submitted_by_priority);
    check_worker_totals(finished);
}

TEST(Metrics, EnabledTimingSeparatesDependencyWaitFromReadyQueueWaitAndExecution) {
    threadforge::Config config{1};
    config.enable_timing = true;
    threadforge::Scheduler scheduler(config);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (scheduler.metrics().waits < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool parked = scheduler.metrics().waits >= 1;
    std::latch root_entered(1);
    std::promise<void> release_root;
    auto root_gate = release_root.get_future().share();
    auto root = scheduler.submit_task(Priority::Low, [&, root_gate] {
        root_entered.count_down();
        root_gate.wait();
        return 17;
    });
    root_entered.wait();
    auto value = root.future.share();
    auto child = scheduler.submit_after({root.handle}, [value] { return value.get() + 2; });
    std::latch blocker_entered(1);
    std::promise<void> release_blocker;
    auto blocker_gate = release_blocker.get_future().share();
    auto blocker = scheduler.submit(Priority::High, [&, blocker_gate] {
        blocker_entered.count_down();
        blocker_gate.wait();
    });
    const auto dependency_waiting = scheduler.metrics();
    release_root.set_value();
    blocker_entered.wait();
    const auto dependency_ready = scheduler.metrics();
    release_blocker.set_value();
    blocker.get();
    EXPECT_EQ(child.future.get(), 19);
    scheduler.shutdown();
    EXPECT_TRUE(parked);
    EXPECT_EQ(dependency_waiting.tasks_waiting_dependencies, 1U);
    EXPECT_EQ(dependency_ready.tasks_waiting_dependencies, 0U);
    EXPECT_EQ(dependency_ready.tasks_ready, 1U);
    EXPECT_EQ(dependency_ready.tasks_running, 1U);
    EXPECT_EQ(dependency_ready.tasks_pending, 2U);
    EXPECT_GT(dependency_ready.total_dependency_wait_ns, 0U);
    const auto finished = scheduler.metrics();
    EXPECT_TRUE(finished.timing_enabled);
    EXPECT_GT(finished.total_execution_ns, 0U);
    EXPECT_GT(finished.max_execution_ns, 0U);
    EXPECT_GE(finished.total_execution_ns, finished.max_execution_ns);
    EXPECT_GT(finished.total_queue_wait_ns, 0U);
    EXPECT_GT(finished.max_queue_wait_ns, 0U);
    EXPECT_GE(finished.total_queue_wait_ns, finished.max_queue_wait_ns);
    EXPECT_GT(finished.total_idle_wait_ns, 0U);
    EXPECT_EQ(finished.total_dependency_wait_ns, dependency_ready.total_dependency_wait_ns);
    check_worker_totals(finished);
}

TEST(Metrics, EnabledTimingWithoutDependenciesLeavesDependencyTimeAtZero) {
    threadforge::Config config{2};
    config.enable_timing = true;
    threadforge::Scheduler scheduler(config);
    std::vector<std::future<std::uint64_t>> values;
    for (std::uint64_t id = 0; id < 200; ++id) {
        values.push_back(scheduler.submit([id] { return id * id; }));
    }
    scheduler.shutdown();
    for (std::uint64_t id = 0; id < values.size(); ++id) {
        EXPECT_EQ(values[id].get(), id * id);
    }
    const auto finished = scheduler.metrics();
    EXPECT_TRUE(finished.timing_enabled);
    EXPECT_EQ(finished.total_dependency_wait_ns, 0U);
    EXPECT_GE(finished.total_execution_ns, finished.max_execution_ns);
    EXPECT_GE(finished.total_queue_wait_ns, finished.max_queue_wait_ns);
    check_worker_totals(finished);
}

TEST(Metrics, ConcurrentSnapshotsKeepLifecycleGaugesConsistentDuringSubmissionAndShutdown) {
    constexpr int producer_count = 4;
    constexpr int tasks_per_producer = 2000;
    constexpr int task_count = producer_count * tasks_per_producer;
    constexpr std::array priorities{Priority::High, Priority::Normal, Priority::Low};
    threadforge::Scheduler scheduler(4);
    std::atomic<bool> sampling_done{false};
    std::atomic<bool> invalid_gauges{false};
    std::atomic<int> snapshots{0};
    std::latch first_snapshot(1);
    std::jthread sampler([&] {
        bool first = true;
        while (!sampling_done.load(std::memory_order_relaxed)) {
            const auto snapshot = scheduler.metrics();
            if (snapshot.tasks_pending != snapshot.tasks_ready + snapshot.tasks_running + snapshot.tasks_waiting_dependencies ||
                snapshot.tasks_running > scheduler.worker_count() ||
                snapshot.peak_tasks_pending < snapshot.tasks_pending) {
                invalid_gauges.store(true, std::memory_order_relaxed);
            }
            snapshots.fetch_add(1, std::memory_order_relaxed);
            if (first) {
                first = false;
                first_snapshot.count_down();
            }
            std::this_thread::yield();
        }
    });
    first_snapshot.wait();
    std::latch start(producer_count);
    std::vector<std::vector<std::future<std::uint64_t>>> results(producer_count);
    std::vector<std::jthread> producers;
    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            results[producer].reserve(tasks_per_producer);
            start.arrive_and_wait();
            for (int i = 0; i < tasks_per_producer; ++i) {
                const auto id = static_cast<std::uint64_t>(producer * tasks_per_producer + i);
                results[producer].push_back(scheduler.submit(priorities[id % priorities.size()], [id] {
                    if (id % 11 == 0) {
                        throw std::runtime_error("sampled failure");
                    }
                    return id * id;
                }));
            }
        });
    }
    producers.clear();
    scheduler.shutdown();
    sampling_done.store(true, std::memory_order_relaxed);
    sampler.join();
    EXPECT_FALSE(invalid_gauges.load(std::memory_order_relaxed));
    EXPECT_GT(snapshots.load(std::memory_order_relaxed), 0);
    for (int producer = 0; producer < producer_count; ++producer) {
        for (int i = 0; i < tasks_per_producer; ++i) {
            const auto id = static_cast<std::uint64_t>(producer * tasks_per_producer + i);
            if (id % 11 == 0) {
                EXPECT_THROW(results[producer][i].get(), std::runtime_error);
            } else {
                EXPECT_EQ(results[producer][i].get(), id * id);
            }
        }
    }
    const auto finished = scheduler.metrics();
    EXPECT_EQ(finished.tasks_submitted, task_count);
    EXPECT_EQ(finished.tasks_failed, (task_count - 1) / 11 + 1);
    EXPECT_EQ(finished.submitted_by_priority, (std::array<std::uint64_t, 3>{2667, 2667, 2666}));
    EXPECT_EQ(finished.tasks_by_priority, finished.submitted_by_priority);
    check_worker_totals(finished);
    check_disabled_timing(finished);
}

} // namespace
