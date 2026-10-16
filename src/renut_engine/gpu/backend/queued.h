/**
 * @file    gpu/backend/queued.h
 * @brief   The game thread's side of draws, clears and resolves on a device
 *          that is not recording a command buffer.
 *
 * Each call captures what the render thread will need and the game may change
 * once the call returns (a snapshot of the device state, the data of Draw*UP
 * calls) and queues the work (render_thread.h). A Begin*Vertices draw waits for
 * its vertices, which the game writes after the call, until the device's next
 * D3D call (FlushDeferredVertices) before it is queued. These are called on
 * the game's threads without State().mutex.
 */
#pragma once

#include <rex/types.h>

#include "renut_engine/gpu/render/draw.h"

namespace renut::gpu {

// 'up_vertex_bytes' / 'up_index_bytes': the sizes of call.up_vertices /
// call.up_indices, copied now.
void QueueDraw(u32 device_va, const DrawCall &call, u32 up_vertex_bytes, u32 up_index_bytes);

// A Begin*Vertices draw whose 'bytes' of vertices the game writes to 'scratch'
// after the call.
void QueueDeferredDraw(u32 device_va, const DrawCall &call, const u8 *scratch, u32 bytes);

// Queues the device's Begin*Vertices draw with its vertices as they are now,
// and copies those of one it recorded into a command buffer. Every hook calls
// this first for its device.
void FlushDeferredVertices(u32 device_va);

void QueueClear(u32 device_va, u32 flags, const ClearRect *rect, const float color[4], float depth,
                u32 stencil);

void QueueResolve(u32 device_va, u32 flags, const ClearRect *source_rect, u32 dest_texture_va,
                  i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                  const float clear_color[4], float clear_depth, u32 clear_stencil);

} // namespace renut::gpu
