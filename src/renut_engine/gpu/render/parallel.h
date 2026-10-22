/**
 * @file    gpu/render/parallel.h
 * @brief   Fork-join over the renderer's helper threads.
 *
 * The flushes before a submission (vertex conversions, fetch data copies,
 * texture fills) are independent reads of guest memory into upload memory,
 * most of which misses the cache: run serially they were a tenth of the
 * render thread (xperf, 2026-10-22). For splits them over a few helper
 * threads and the caller, and returns when every piece is done.
 */
#pragma once

#include <cstddef>
#include <functional>

namespace renut::gpu::parallel {

// Calls body(begin, end) over [0, count) in pieces of up to 'grain' items, on
// the calling thread and the helper threads; returns once all have run. The
// body must be safe to run on several threads at once. Small jobs run inline.
void For(size_t count, size_t grain, const std::function<void(size_t, size_t)> &body);

}  // namespace renut::gpu::parallel
