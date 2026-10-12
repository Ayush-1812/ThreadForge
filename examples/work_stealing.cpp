#include "threadforge/scheduler.hpp"

#include <cstdint>
#include <future>
#include <iostream>
#include <vector>

int main() {
    threadforge::Scheduler scheduler(4);
    auto parent = scheduler.submit([&scheduler] {
        std::vector<std::future<std::uint64_t>> children;
        for (std::uint64_t i = 0; i < 1024; ++i) {
            children.push_back(scheduler.submit([i] { return i * i; }));
        }
        // Return futures so the parent does not occupy a worker awaiting children.
        return children;
    });
    auto children = parent.get();
    std::uint64_t checksum = 0;
    for (auto& child : children) {
        checksum += child.get();
    }
    scheduler.shutdown();
    const auto metrics = scheduler.metrics();
    std::cout << "Checksum: " << checksum << '\n'
              << "Submitted: " << metrics.tasks_submitted << '\n'
              << "Global: " << metrics.global_tasks << '\n'
              << "Local: " << metrics.local_tasks << '\n'
              << "Stolen: " << metrics.tasks_stolen << '\n';
}
