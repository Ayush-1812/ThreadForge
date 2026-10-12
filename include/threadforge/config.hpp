#pragma once

#include <cstddef>
#include <thread>

namespace threadforge {

struct Config {
    // hardware_concurrency() may return zero when the platform cannot determine it.
    std::size_t worker_count = [] {
        const auto count = std::thread::hardware_concurrency();
        return count == 0 ? std::size_t{1} : static_cast<std::size_t>(count);
    }();
    // Collect steady-clock durations in addition to always-on counters.
    bool enable_timing = false;
};

} // namespace threadforge
