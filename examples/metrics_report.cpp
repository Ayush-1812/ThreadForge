#include "threadforge/scheduler.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    threadforge::Scheduler scheduler(threadforge::Config{4, true});
    const auto started = std::chrono::steady_clock::now();
    auto root = scheduler.submit_task([&scheduler] {
        std::vector<std::future<std::uint64_t>> leaves;
        for (std::uint64_t i = 1; i <= 1024; ++i) {
            leaves.push_back(scheduler.submit([i] { return i * i; }));
        }
        // Return futures to the caller so this worker remains free to run leaves.
        return leaves;
    });
    auto leaves = root.future.get();
    std::uint64_t checksum = 0;
    for (auto& leaf : leaves) {
        checksum += leaf.get();
    }
    scheduler.shutdown();
    const auto seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    const auto metrics = scheduler.metrics();
    std::cout << "Checksum: " << checksum << '\n';
    threadforge::write_metrics(std::cout, metrics);
    std::cout << "Batch wall time (seconds, including shutdown): " << seconds << '\n'
              << "Batch throughput (tasks/second): " << metrics.tasks_completed / seconds << '\n';
    return checksum == 358438400 && metrics.tasks_completed == 1025 ? 0 : 1;
}
