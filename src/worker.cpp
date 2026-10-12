#include "threadforge/worker.hpp"
#include "threadforge/task_queue.hpp"

namespace threadforge::detail {

thread_local const TaskQueue* Worker::current_queue_ = nullptr;

Worker::Worker(TaskQueue& queue)
    : thread_([&queue] {
        current_queue_ = &queue;
        while (auto task = queue.wait_pop()) {
            // packaged_task captures callable exceptions in its associated future.
            (*task)();
        }
        current_queue_ = nullptr;
    }) {}

void Worker::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool Worker::is_current_worker(const TaskQueue& queue) noexcept {
    return current_queue_ == &queue;
}

} // namespace threadforge::detail
