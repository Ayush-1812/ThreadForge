#include "threadforge/scheduler.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <latch>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::array priorities{threadforge::Priority::High, threadforge::Priority::Normal,
                                threadforge::Priority::Low};

void check_graph_metrics(const threadforge::MetricsSnapshot& metrics, std::uint64_t submitted) {
    EXPECT_EQ(metrics.tasks_submitted, submitted);
    EXPECT_EQ(metrics.local_tasks + metrics.global_tasks + metrics.tasks_stolen, submitted);
    std::uint64_t claims = 0;
    for (const auto& worker : metrics.workers) {
        claims += worker.local_tasks + worker.global_tasks + worker.tasks_stolen;
    }
    EXPECT_EQ(claims, submitted);
}

class DependencyStress : public testing::TestWithParam<std::size_t> {};

TEST_P(DependencyStress, Layered10000NodeGraphReturnsExactValuesAndExecutesEachNodeOnce) {
    constexpr int width = 100;
    constexpr int layers = 100;
    constexpr int node_count = width * layers;
    std::vector<std::atomic<int>> executions(node_count);
    std::vector<std::uint64_t> expected(node_count);
    std::vector<threadforge::TaskHandle> handles;
    std::vector<std::shared_future<std::uint64_t>> values;
    handles.reserve(node_count);
    values.reserve(node_count);
    threadforge::Scheduler scheduler(GetParam());
    for (int id = 0; id < node_count; ++id) {
        const auto priority = priorities[static_cast<std::size_t>(id) % priorities.size()];
        if (id < width) {
            expected[id] = static_cast<std::uint64_t>(id + 1);
            auto root = scheduler.submit_task(priority, [&, id] {
                ++executions[id];
                return static_cast<std::uint64_t>(id + 1);
            });
            handles.push_back(root.handle);
            values.push_back(root.future.share());
        } else {
            const int previous_layer = (id / width - 1) * width;
            const int first = previous_layer + id % width;
            const int second = previous_layer + (id % width + 1) % width;
            // Unsigned wraparound keeps the deeply layered computation defined.
            expected[id] = expected[first] + expected[second] + static_cast<std::uint64_t>(id + 1);
            auto left = values[first];
            auto right = values[second];
            auto child = scheduler.submit_after({handles[first], handles[second]}, priority,
                [&, id, left, right] {
                    ++executions[id];
                    return left.get() + right.get() + static_cast<std::uint64_t>(id + 1);
                });
            handles.push_back(child.handle);
            values.push_back(child.future.share());
        }
    }
    scheduler.shutdown();
    for (int id = 0; id < node_count; ++id) {
        ASSERT_EQ(values[id].get(), expected[id]) << "node " << id;
        ASSERT_EQ(executions[id].load(), 1) << "node " << id;
    }
    check_graph_metrics(scheduler.metrics(), node_count);
}

TEST_P(DependencyStress, RegistrationRacingShutdownDrainsEveryAcceptedDependent) {
    constexpr int producer_count = 4;
    constexpr int attempts_per_producer = 1000;
    constexpr int attempt_count = producer_count * attempts_per_producer;
    std::vector<std::atomic<int>> executions(attempt_count);
    std::vector<unsigned char> accepted(attempt_count, 0);
    std::vector<std::vector<std::pair<int, std::future<int>>>> results(producer_count);
    threadforge::Scheduler scheduler(GetParam());
    std::latch root_entered(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto root = scheduler.submit_task([&, gate] {
        root_entered.count_down();
        gate.wait();
        return 7;
    });
    root_entered.wait();
    auto root_value = root.future.share();
    std::latch first_submissions(producer_count);
    std::vector<std::jthread> producers;
    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            results[producer].reserve(attempts_per_producer);
            for (int i = 0; i < attempts_per_producer; ++i) {
                const int id = producer * attempts_per_producer + i;
                try {
                    auto child = scheduler.submit_after({root.handle}, priorities[static_cast<std::size_t>(id) % 3],
                        [&, id, root_value] {
                            ++executions[id];
                            return root_value.get() + id;
                        });
                    accepted[id] = 1;
                    results[producer].emplace_back(id, std::move(child.future));
                } catch (const std::runtime_error&) {
                    // Closing admission rejects this child without registering it.
                }
                if (i == 0) {
                    first_submissions.count_down();
                }
            }
        });
    }
    first_submissions.wait();
    auto closing = std::async(std::launch::async, [&] { scheduler.shutdown(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!scheduler.is_shutdown() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool closed = scheduler.is_shutdown();
    release.set_value();
    closing.get();
    producers.clear();
    EXPECT_TRUE(closed);
    std::uint64_t submitted = 1; // The root, which was accepted before closure.
    for (int id = 0; id < attempt_count; ++id) {
        ASSERT_EQ(executions[id].load(), accepted[id]) << "node " << id;
        submitted += accepted[id];
    }
    for (int producer = 0; producer < producer_count; ++producer) {
        EXPECT_EQ(accepted[producer * attempts_per_producer], 1);
        for (auto& [id, value] : results[producer]) {
            EXPECT_EQ(value.get(), id + 7);
        }
    }
    check_graph_metrics(scheduler.metrics(), submitted);
}

INSTANTIATE_TEST_SUITE_P(WorkerCounts, DependencyStress, testing::Values(1U, 2U, 4U, 8U));

} // namespace
