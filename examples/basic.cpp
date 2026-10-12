#include "threadforge/scheduler.hpp"

#include <iostream>

int main() {
    threadforge::Scheduler scheduler(4);
    auto answer = scheduler.submit([] { return 42; });
    auto sum = scheduler.submit([](int a, int b) { return a + b; }, 10, 20);

    std::cout << "Answer: " << answer.get() << '\n';
    std::cout << "Sum: " << sum.get() << '\n';
    scheduler.shutdown();
}
