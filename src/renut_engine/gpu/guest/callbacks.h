/**
 * @file    gpu/guest/callbacks.h
 * @brief   D3DDevice_InsertCallback: guest functions the GPU calls when it
 *          reaches their point in the ring.
 *
 * Here the render thread reaches the point (render_thread.h), and a guest
 * thread runs the callback at its next D3D call (RunReadyCallbacks): the
 * callbacks are guest code, which the render thread cannot run. A fence issued
 * after a callback completes only once the callback has run, as on the
 * console, where the game may free what a callback uses once the fence has
 * passed (N&B's XMV player frees itself after its last frame's
 * IXMediaXmvPlayer_Resume). Called on the game's threads, without
 * State().mutex.
 */
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
