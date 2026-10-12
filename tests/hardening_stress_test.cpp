#include "threadforge/scheduler.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::array priorities{threadforge::Priority::High, threadforge::Priority::Normal,
                                threadforge::Priority::Low};

std::uint64_t contribution(std::size_t id) {
    const auto value = static_cast<std::uint64_t>(id);
    return value * value + value * 31 + 7;
}

void expect_settled(const threadforge::MetricsSnapshot& metrics, std::uint64_t submitted,
                    std::uint64_t failed, const std::array<std::uint64_t, 3>& by_priority,
                    bool timing_enabled) {
    EXPECT_EQ(metrics.tasks_submitted, submitted);
    EXPECT_EQ(metrics.tasks_started, submitted);
    EXPECT_EQ(metrics.tasks_completed, submitted);
    EXPECT_EQ(metrics.tasks_failed, failed);
    EXPECT_EQ(metrics.local_tasks + metrics.global_tasks + metrics.tasks_stolen, submitted);
    EXPECT_EQ(metrics.tasks_ready, 0U);
    EXPECT_EQ(metrics.tasks_running, 0U);
    EXPECT_EQ(metrics.tasks_waiting_dependencies, 0U);
    EXPECT_EQ(metrics.tasks_pending, 0U);
    EXPECT_GE(metrics.peak_tasks_pending, 1U);
    EXPECT_LE(metrics.peak_tasks_pending, submitted);
    EXPECT_EQ(metrics.submitted_by_priority, by_priority);
    EXPECT_EQ(metrics.tasks_by_priority, by_priority);
    EXPECT_EQ(metrics.timing_enabled, timing_enabled);

    std::uint64_t started = 0;
    std::uint64_t completed = 0;
    std::uint64_t failures = 0;
    std::uint64_t local = 0;
    std::uint64_t global = 0;
    std::uint64_t stolen = 0;
    std::uint64_t probes = 0;
    std::uint64_t waits = 0;
    std::uint64_t execution_ns = 0;
    std::uint64_t queue_ns = 0;
    std::uint64_t idle_ns = 0;
    std::uint64_t max_execution_ns = 0;
    std::uint64_t max_queue_ns = 0;
    std::array<std::uint64_t, 3> worker_priorities{};
    for (const auto& worker : metrics.workers) {
        EXPECT_EQ(worker.tasks_started, worker.tasks_completed);
        EXPECT_EQ(worker.local_tasks + worker.global_tasks + worker.tasks_stolen,
                  worker.tasks_completed);
        EXPECT_LE(worker.tasks_failed, worker.tasks_completed);
        EXPECT_LE(worker.max_execution_ns, worker.total_execution_ns);
        EXPECT_LE(worker.max_queue_wait_ns, worker.total_queue_wait_ns);
        started += worker.tasks_started;
        completed += worker.tasks_completed;
        failures += worker.tasks_failed;
        local += worker.local_tasks;
        global += worker.global_tasks;
        stolen += worker.tasks_stolen;
        probes += worker.failed_steal_attempts;
        waits += worker.waits;
        execution_ns += worker.total_execution_ns;
        queue_ns += worker.total_queue_wait_ns;
        idle_ns += worker.total_idle_wait_ns;
        max_execution_ns = std::max(max_execution_ns, worker.max_execution_ns);
        max_queue_ns = std::max(max_queue_ns, worker.max_queue_wait_ns);
        for (std::size_t lane = 0; lane < priorities.size(); ++lane) {
            worker_priorities[lane] += worker.tasks_by_priority[lane];
        }
    }
    EXPECT_EQ(started, submitted);
    EXPECT_EQ(completed, submitted);
    EXPECT_EQ(failures, failed);
    EXPECT_EQ(local, metrics.local_tasks);
    EXPECT_EQ(global, metrics.global_tasks);
    EXPECT_EQ(stolen, metrics.tasks_stolen);
    EXPECT_EQ(probes, metrics.failed_steal_attempts);
    EXPECT_EQ(waits, metrics.waits);
    EXPECT_EQ(worker_priorities, by_priority);
    EXPECT_EQ(execution_ns, metrics.total_execution_ns);
    EXPECT_EQ(queue_ns, metrics.total_queue_wait_ns);
    EXPECT_EQ(idle_ns, metrics.total_idle_wait_ns);
    EXPECT_EQ(max_execution_ns, metrics.max_execution_ns);
    EXPECT_EQ(max_queue_ns, metrics.max_queue_wait_ns);
    if (!timing_enabled) {
        EXPECT_EQ(execution_ns, 0U);
        EXPECT_EQ(queue_ns, 0U);
        EXPECT_EQ(idle_ns, 0U);
        EXPECT_EQ(metrics.total_dependency_wait_ns, 0U);
    }
    if (metrics.workers.size() == 1) {
        EXPECT_EQ(stolen, 0U);
        EXPECT_EQ(probes, 0U);
    }
}

class HardeningStress : public testing::TestWithParam<std::size_t> {};

TEST_P(HardeningStress, Layered100000NodeGraphSurvivesFailuresAndDiscardedResults) {
    constexpr std::size_t width = 200;
    constexpr std::size_t layers = 500;
    constexpr std::size_t node_count = width * layers;
    using Payload = std::array<std::uint64_t, width>;
    struct Layer {
        std::shared_ptr<Payload> payload;
        std::shared_ptr<std::latch> finished;
        Payload expected{};
        std::vector<threadforge::TaskHandle> handles;
    };
    struct RetainedResult {
        std::size_t id;
        std::uint64_t expected;
        std::future<std::uint64_t> future;
    };

    std::vector<std::atomic<unsigned>> executions(node_count);
    std::atomic<std::uint64_t> actual_checksum{0};
    std::uint64_t expected_checksum = 0;
    std::array<std::uint64_t, 3> expected_priorities{};
    threadforge::Scheduler scheduler(GetParam());
    Layer previous;
    std::vector<RetainedResult> retained;
    retained.reserve(width);

    const auto build_layer = [&](std::size_t index, const Layer* parent,
                                 std::shared_future<void> gate,
                                 std::shared_ptr<std::latch> entered) {
        Layer layer;
        layer.payload = std::make_shared<Payload>();
        layer.finished = std::make_shared<std::latch>(static_cast<std::ptrdiff_t>(width));
        layer.handles.reserve(width);
        const auto previous_payload = parent ? parent->payload : std::shared_ptr<Payload>{};
        for (std::size_t slot = 0; slot < width; ++slot) {
            const auto id = index * width + slot;
            const auto next = (slot + 1) % width;
            auto value = static_cast<std::uint64_t>(id + 1);
            if (parent) {
                value += parent->expected[0] + parent->expected[slot] + parent->expected[next];
            }
            layer.expected[slot] = value;
            expected_checksum += value;
            ++expected_priorities[id % priorities.size()];
            auto task = [&, id, slot, next, previous_payload, payload = layer.payload,
                         finished = layer.finished, entered,
                         wait_on = slot == 0 ? gate : std::shared_future<void>{}] {
                if (wait_on.valid()) {
                    entered->count_down();
                    wait_on.wait();
                }
                auto result = static_cast<std::uint64_t>(id + 1);
                if (previous_payload) {
                    result += (*previous_payload)[0] + (*previous_payload)[slot] +
                              (*previous_payload)[next];
                }
                // Declared handle edges synchronize these ordinary payload cells.
                (*payload)[slot] = result;
                executions[id].fetch_add(1, std::memory_order_relaxed);
                actual_checksum.fetch_add(result, std::memory_order_relaxed);
                finished->count_down();
                if (id % 31 == 0) {
                    throw std::logic_error("layered graph failure");
                }
                return result;
            };
            auto submission = parent
                ? scheduler.submit_after({parent->handles[0], parent->handles[slot],
                                          parent->handles[next]},
                                         priorities[id % priorities.size()], std::move(task))
                : scheduler.submit_task(priorities[id % priorities.size()], std::move(task));
            layer.handles.push_back(std::move(submission.handle));
            if (id % 17 == 0) {
                retained.push_back({id, value, std::move(submission.future)});
            }
        }
        return layer;
    };

    for (std::size_t index = 0; index < layers; index += 2) {
        std::promise<void> release;
        auto gate = release.get_future().share();
        auto entered = std::make_shared<std::latch>(1);
        auto first = build_layer(index, index == 0 ? nullptr : &previous, gate, entered);
        entered->wait();
        auto second = build_layer(index + 1, &first, {}, {});

        // Every second-layer task depends on the held anchor; registration is real.
        EXPECT_GE(scheduler.metrics().tasks_waiting_dependencies, width);
        first.handles.clear();
        previous.handles.clear();
        if (index + 2 == layers) {
            second.handles.clear();
        }
        release.set_value();
        second.finished->wait();
        for (auto& result : retained) {
            if (result.id % 31 == 0) {
                EXPECT_THROW(result.future.get(), std::logic_error);
            } else {
                EXPECT_EQ(result.future.get(), result.expected) << "node " << result.id;
            }
        }
        retained.clear();
        previous = std::move(second);
    }
    scheduler.shutdown();
    EXPECT_EQ(actual_checksum.load(), expected_checksum);
    for (std::size_t id = 0; id < node_count; ++id) {
        ASSERT_EQ(executions[id].load(), 1U) << "node " << id;
    }
    const auto metrics = scheduler.metrics();
    expect_settled(metrics, node_count, (node_count - 1) / 31 + 1, expected_priorities, false);
    EXPECT_EQ(metrics.global_tasks, node_count);
    EXPECT_LE(metrics.peak_tasks_pending, width * 3);
}

TEST_P(HardeningStress, IrregularNestedFanoutReturnsFuturesAndPreservesExactAccounting) {
    constexpr std::size_t root_count = 96;
    struct BranchPlan {
        std::size_t id;
        std::size_t first_leaf;
        std::size_t leaves;
    };
    using LeafResults = std::vector<std::future<std::uint64_t>>;
    using BranchResults = std::vector<std::future<LeafResults>>;
    std::vector<std::vector<BranchPlan>> plans(root_count);
    std::size_t branch_count = 0;
    for (std::size_t root = 0; root < root_count; ++root) {
        branch_count += (root * 7) % 17 + 1;
    }
    std::size_t next_branch = root_count;
    std::size_t next_leaf = root_count + branch_count;
    std::uint64_t failed = 0;
    for (std::size_t root = 0; root < root_count; ++root) {
        for (std::size_t branch = 0; branch < (root * 7) % 17 + 1; ++branch) {
            const auto leaves = (root * 11 + branch * 13) % 31 + 1;
            plans[root].push_back({next_branch++, next_leaf, leaves});
            for (std::size_t leaf = 0; leaf < leaves; ++leaf) {
                failed += (next_leaf + leaf) % 41 == 0 ? 1U : 0U;
            }
            next_leaf += leaves;
        }
    }
    const auto node_count = next_leaf;
    std::vector<std::atomic<unsigned>> executions(node_count);
    std::atomic<std::uint64_t> actual_checksum{0};
    std::uint64_t expected_checksum = 0;
    std::array<std::uint64_t, 3> expected_priorities{};
    for (std::size_t id = 0; id < node_count; ++id) {
        expected_checksum += contribution(id);
        ++expected_priorities[id % priorities.size()];
    }
    threadforge::Scheduler scheduler(GetParam());
    std::vector<std::future<BranchResults>> roots;
    roots.reserve(root_count);
    for (std::size_t root = 0; root < root_count; ++root) {
        roots.push_back(scheduler.submit(priorities[root % priorities.size()],
            [&, root, branches = plans[root]]() -> BranchResults {
                executions[root].fetch_add(1, std::memory_order_relaxed);
                actual_checksum.fetch_add(contribution(root), std::memory_order_relaxed);
                BranchResults results;
                results.reserve(branches.size());
                for (const auto plan : branches) {
                    results.push_back(scheduler.submit(priorities[plan.id % priorities.size()],
                        [&, plan]() -> LeafResults {
                            executions[plan.id].fetch_add(1, std::memory_order_relaxed);
                            actual_checksum.fetch_add(contribution(plan.id), std::memory_order_relaxed);
                            LeafResults children;
                            children.reserve(plan.leaves);
                            for (std::size_t leaf = 0; leaf < plan.leaves; ++leaf) {
                                const auto id = plan.first_leaf + leaf;
                                children.push_back(scheduler.submit(priorities[id % priorities.size()], [&, id] {
                                    executions[id].fetch_add(1, std::memory_order_relaxed);
                                    actual_checksum.fetch_add(contribution(id), std::memory_order_relaxed);
                                    if (id % 41 == 0) {
                                        throw std::logic_error("nested leaf failure");
                                    }
                                    return contribution(id);
                                }));
                            }
                            return children;
                        }));
                }
                return results;
            }));
    }
    // All waits stay on the external caller, including with just one worker.
    for (std::size_t root = 0; root < root_count; ++root) {
        auto branches = roots[root].get();
        ASSERT_EQ(branches.size(), plans[root].size());
        for (std::size_t branch = 0; branch < branches.size(); ++branch) {
            auto children = branches[branch].get();
            const auto plan = plans[root][branch];
            ASSERT_EQ(children.size(), plan.leaves);
            for (std::size_t leaf = 0; leaf < children.size(); ++leaf) {
                const auto id = plan.first_leaf + leaf;
                if (id % 41 == 0) {
                    EXPECT_THROW(children[leaf].get(), std::logic_error);
                } else {
                    EXPECT_EQ(children[leaf].get(), contribution(id)) << "node " << id;
                }
            }
        }
    }
    scheduler.shutdown();
    EXPECT_EQ(actual_checksum.load(), expected_checksum);
    for (std::size_t id = 0; id < node_count; ++id) {
        ASSERT_EQ(executions[id].load(), 1U) << "node " << id;
    }
    const auto metrics = scheduler.metrics();
    expect_settled(metrics, node_count, failed, expected_priorities, false);
    EXPECT_EQ(metrics.global_tasks, root_count);
    EXPECT_EQ(metrics.local_tasks + metrics.tasks_stolen, node_count - root_count);
}

TEST_P(HardeningStress, RepeatedTimedDrainRacesChainedProducersJoinersAndSnapshots) {
    constexpr std::size_t rounds = 8;
    constexpr std::size_t producer_count = 4;
    constexpr std::size_t attempts_per_producer = 512;
    constexpr std::size_t warmup_per_producer = 64;
    constexpr std::size_t node_count = 1 + producer_count * attempts_per_producer;
    for (std::size_t round = 0; round < rounds; ++round) {
        SCOPED_TRACE(round);
        std::vector<std::atomic<unsigned>> executions(node_count);
        std::vector<unsigned char> accepted(node_count, 0);
        std::atomic<std::uint64_t> actual_checksum{0};
        threadforge::Scheduler scheduler(threadforge::Config{GetParam(), true});
        std::latch entered(1);
        std::promise<void> release;
        auto gate = release.get_future().share();
        const bool root_fails = round % 2 != 0;
        auto root = scheduler.submit_task(threadforge::Priority::High, [&, gate] {
            entered.count_down();
            gate.wait();
            executions[0].fetch_add(1, std::memory_order_relaxed);
            const auto value = contribution(0) + static_cast<std::uint64_t>(round * 17);
            actual_checksum.fetch_add(value, std::memory_order_relaxed);
            if (root_fails) {
                throw std::logic_error("drain root failure");
            }
            return value;
        });
        accepted[0] = 1;
        entered.wait();

        std::atomic<bool> invalid_snapshot{false};
        std::atomic<std::uint64_t> samples{0};
        std::latch sampled(1);
        std::jthread sampler([&](std::stop_token stop) {
            bool first = true;
            while (!stop.stop_requested()) {
                const auto snapshot = scheduler.metrics();
                if (snapshot.tasks_pending != snapshot.tasks_ready + snapshot.tasks_running +
                                              snapshot.tasks_waiting_dependencies ||
                    snapshot.tasks_running > GetParam() ||
                    snapshot.peak_tasks_pending < snapshot.tasks_pending ||
                    snapshot.tasks_submitted < snapshot.tasks_pending || !snapshot.timing_enabled ||
                    snapshot.workers.size() != GetParam()) {
                    invalid_snapshot.store(true, std::memory_order_relaxed);
                }
                samples.fetch_add(1, std::memory_order_relaxed);
                if (first) {
                    first = false;
                    sampled.count_down();
                }
                std::this_thread::yield();
            }
        });
        sampled.wait();

        std::latch warmed(static_cast<std::ptrdiff_t>(producer_count));
        std::latch race_start(1);
        std::vector<std::jthread> producers;
        for (std::size_t producer = 0; producer < producer_count; ++producer) {
            producers.emplace_back([&, producer] {
                auto parent = root.handle;
                for (std::size_t attempt = 0; attempt < attempts_per_producer; ++attempt) {
                    const auto id = 1 + producer * attempts_per_producer + attempt;
                    try {
                        auto child = scheduler.submit_after({parent}, priorities[id % priorities.size()], [&, id] {
                            executions[id].fetch_add(1, std::memory_order_relaxed);
                            const auto value = contribution(id) + static_cast<std::uint64_t>(round * 17);
                            actual_checksum.fetch_add(value, std::memory_order_relaxed);
                            if (id % 13 == 0) {
                                throw std::logic_error("drain child failure");
                            }
                            return value;
                        });
                        accepted[id] = 1;
                        parent = std::move(child.handle);
                    } catch (const std::runtime_error&) {
                        // Rejected registrations cannot leave a dependent edge behind.
                    }
                    if (attempt + 1 == warmup_per_producer) {
                        warmed.count_down();
                        race_start.wait();
                    }
                }
            });
        }
        warmed.wait();
        EXPECT_EQ(scheduler.metrics().tasks_waiting_dependencies,
                  producer_count * warmup_per_producer);
        std::vector<std::jthread> joiners;
        for (std::size_t joiner = 0; joiner < 3; ++joiner) {
            joiners.emplace_back([&] {
                race_start.wait();
                scheduler.shutdown();
            });
        }
        race_start.count_down();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!scheduler.is_shutdown() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        const bool closed = scheduler.is_shutdown();
        release.set_value();
        producers.clear();
        joiners.clear();
        sampler.request_stop();
        sampler.join();
        EXPECT_TRUE(closed);
        EXPECT_FALSE(invalid_snapshot.load());
        EXPECT_GT(samples.load(), 0U);
        EXPECT_THROW(scheduler.submit([] {}), std::runtime_error);
        if (root_fails) {
            EXPECT_THROW(root.future.get(), std::logic_error);
        } else {
            EXPECT_EQ(root.future.get(), contribution(0) + static_cast<std::uint64_t>(round * 17));
        }

        std::uint64_t expected_checksum = 0;
        std::uint64_t submitted = 0;
        std::uint64_t failed = root_fails ? 1U : 0U;
        std::array<std::uint64_t, 3> expected_priorities{};
        for (std::size_t id = 0; id < node_count; ++id) {
            ASSERT_EQ(executions[id].load(), accepted[id]) << "node " << id;
            if (accepted[id] != 0) {
                ++submitted;
                ++expected_priorities[id % priorities.size()];
                expected_checksum += contribution(id) + static_cast<std::uint64_t>(round * 17);
                failed += id != 0 && id % 13 == 0 ? 1U : 0U;
            }
        }
        EXPECT_EQ(actual_checksum.load(), expected_checksum);
        const auto metrics = scheduler.metrics();
        expect_settled(metrics, submitted, failed, expected_priorities, true);
        EXPECT_EQ(metrics.global_tasks, submitted);
    }
}

INSTANTIATE_TEST_SUITE_P(WorkerCounts, HardeningStress, testing::Values(1U, 2U, 4U, 8U));

} // namespace
