#pragma once

#include <cstddef>
#include <functional>

namespace renut::gpu::parallel {

// Calls the thread-safe body(begin, end) over [0, count) in pieces of up to
// 'grain' items on the caller and helper threads; returns once all have run.
void For(size_t count, size_t grain, const std::function<void(size_t, size_t)> &body);

}  // namespace renut::gpu::parallel
