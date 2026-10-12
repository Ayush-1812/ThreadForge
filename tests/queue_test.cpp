#include "threadforge/task_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <latch>
#include <thread>
#include <vector>

namespace {

TEST(TaskQueue, PreservesFifoOrderAndMovesTasks) {
    threadforge::TaskQueue queue;
    std::vector<int> order;
    EXPECT_TRUE(queue.push(threadforge::Task([&] { order.push_back(1); })));
    EXPECT_TRUE(queue.push(threadforge::Task([&] { order.push_back(2); })));
    queue.close();
    while (auto task = queue.wait_pop()) {
        (*task)();
    }
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST(TaskQueue, CloseDrainsAcceptedWorkAndRejectsNewWork) {
    threadforge::TaskQueue queue;
    EXPECT_TRUE(queue.push(threadforge::Task([] {})));
    queue.close();
    queue.close();
    EXPECT_TRUE(queue.is_closed());
    EXPECT_FALSE(queue.push(threadforge::Task([] {})));
    EXPECT_TRUE(queue.wait_pop().has_value());
    EXPECT_FALSE(queue.wait_pop().has_value());
}

TEST(TaskQueue, RejectsInvalidTasks) {
    threadforge::TaskQueue queue;
    EXPECT_THROW(static_cast<void>(queue.push(threadforge::Task{})), std::invalid_argument);
}

TEST(TaskQueue, SubmissionWakesWaitingConsumer) {
    threadforge::TaskQueue queue;
    std::promise<int> result;
    auto future = result.get_future();
    std::latch entered(1);
    std::jthread consumer([&] {
        entered.count_down();
        if (auto task = queue.wait_pop()) {
            (*task)();
        }
    });
    entered.wait();
    EXPECT_TRUE(queue.push(threadforge::Task([&] { result.set_value(42); })));
    EXPECT_EQ(future.get(), 42);
    queue.close();
}

TEST(TaskQueue, CloseWakesAllConsumers) {
    threadforge::TaskQueue queue;
    std::atomic<int> exited{0};
    std::latch entered(4);
    std::vector<std::jthread> consumers;
    for (int i = 0; i < 4; ++i) {
        consumers.emplace_back([&] {
            entered.count_down();
            if (!queue.wait_pop()) {
                exited.fetch_add(1);
            }
        });
    }
    entered.wait();
    queue.close();
    consumers.clear();
    EXPECT_EQ(exited.load(), 4);
}

} // namespace
