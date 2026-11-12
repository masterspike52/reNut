#pragma once

#include <cstdint>

#include <rex/types.h>

namespace renut::gpu {

// Queues pCallback(Context) behind the work queued so far.
void InsertCallback(u32 function, u32 context);

// The last callback inserted: the fence of a KickOff issued now completes
// after it (render_thread::QueueFenceCompletion).
uint64_t LastCallback();

// Runs the callbacks the render thread has reached, in order, on this thread.
void RunReadyCallbacks();

} // namespace renut::gpu
