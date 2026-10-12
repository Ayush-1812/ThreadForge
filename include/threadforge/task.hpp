#pragma once

#include <future>

namespace threadforge {

// Move-only type erasure keeps ownership unique, including for move-only callables.
using Task = std::packaged_task<void()>;

} // namespace threadforge
