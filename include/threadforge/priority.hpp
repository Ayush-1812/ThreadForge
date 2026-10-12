#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace threadforge {

enum class Priority : std::uint8_t { High, Normal, Low };

namespace detail {

inline constexpr std::size_t priority_count = 3;

[[nodiscard]] constexpr std::size_t priority_index(Priority priority) {
    switch (priority) {
    case Priority::High: return 0;
    case Priority::Normal: return 1;
    case Priority::Low: return 2;
    }
    throw std::invalid_argument("invalid ThreadForge task priority");
}

// Each worker has its own cursor; absent classes are skipped without idling.
inline constexpr std::array priority_cycle{
    Priority::High, Priority::High, Priority::High, Priority::High,
    Priority::High, Priority::High, Priority::High, Priority::High,
    Priority::Normal, Priority::Normal, Priority::Normal, Priority::Normal,
    Priority::Low
};

} // namespace detail
} // namespace threadforge
