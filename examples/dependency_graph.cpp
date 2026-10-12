#include "threadforge/scheduler.hpp"

#include <iostream>

int main() {
    threadforge::Scheduler scheduler(1);
    auto root = scheduler.submit_task([] { return 6; });
    auto root_result = root.future.share();

    // Each handle edge ensures the captured result is ready before get().
    auto left = scheduler.submit_after({root.handle}, [root_result] {
        return root_result.get() * 2;
    });
    auto right = scheduler.submit_after({root.handle}, [root_result] {
        return root_result.get() + 3;
    });
    auto left_result = left.future.share();
    auto right_result = right.future.share();

    auto combined = scheduler.submit_after(
        {left.handle, right.handle}, threadforge::Priority::High,
        [left_result, right_result] {
            return left_result.get() + right_result.get();
        });

    const auto combined_result = combined.future.get();
    scheduler.shutdown();
    std::cout << "Root: " << root_result.get() << '\n'
              << "Left: " << left_result.get() << '\n'
              << "Right: " << right_result.get() << '\n'
              << "Combined: " << combined_result << '\n';
}
