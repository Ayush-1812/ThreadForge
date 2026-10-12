#include "threadforge/scheduler.hpp"

#include <future>
#include <iostream>
#include <string>
#include <vector>

int main() {
    std::vector<std::string> order;
    order.reserve(3);
    threadforge::Scheduler scheduler(1);
    std::promise<void> started;
    std::promise<void> release;
    auto released = release.get_future().share();

    // A High gate leaves the service cursor inside the High block.
    auto gate = scheduler.submit(threadforge::Priority::High, [&started, released] {
        started.set_value();
        released.wait();
    });
    started.get_future().get();

    auto low = scheduler.submit(threadforge::Priority::Low, [&order] {
        order.emplace_back("Low");
    });
    auto normal = scheduler.submit(threadforge::Priority::Normal, [&order] {
        order.emplace_back("Normal");
    });
    auto high = scheduler.submit(threadforge::Priority::High, [&order] {
        order.emplace_back("High");
    });

    // Release the worker before waiting for queued work or draining the scheduler.
    release.set_value();
    gate.get();
    low.get();
    normal.get();
    high.get();
    scheduler.shutdown();

    for (const auto& priority : order) {
        std::cout << priority << '\n';
    }
}
