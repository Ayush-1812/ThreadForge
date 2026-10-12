#include "threadforge/worker.hpp"
#include "threadforge/scheduler.hpp"

namespace threadforge::detail {

thread_local Worker* Worker::current_worker_ = nullptr;

Worker::Worker(Scheduler& scheduler, std::size_t index)
    : scheduler_(scheduler), index_(index), next_victim_(index + 1) {}

void Worker::start() {
    thread_ = std::jthread([this] {
        current_worker_ = this;
        while (auto task = scheduler_.take_task(*this)) {
            // packaged_task captures callable exceptions in its associated future.
            (*task)();
        }
        current_worker_ = nullptr;
    });
}

void Worker::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

Worker* Worker::current() noexcept {
    return current_worker_;
}

} // namespace threadforge::detail
