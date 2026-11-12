#pragma once

#include <rex/types.h>

#include "renut_engine/gpu/render/draw.h"

namespace renut::gpu {

// True while the device records a command buffer.
bool IsRecordingCommandBuffer(u32 device_va);

// After the original BeginCommandBuffer: drops what the buffer held before.
void BeginRecording(u32 device_va, u32 command_buffer_va);

// Records a draw on a recording device. 'up_vertex_bytes' / 'up_index_bytes'
// are the sizes of call.up_vertices / call.up_indices, copied now.
void RecordDraw(u32 device_va, const DrawCall &call, u32 up_vertex_bytes, u32 up_index_bytes);

// Records a Begin*Vertices draw whose 'bytes' of vertex data the game writes
// to 'scratch' after the call; copied at the device's next hooked call.
void RecordDeferredDraw(u32 device_va, const DrawCall &call, const u8 *scratch, u32 bytes);

void RecordClear(u32 device_va, u32 flags, const ClearRect *rect, const float color[4],
                 float depth, u32 stencil);

void RecordResolve(u32 device_va, u32 flags, const ClearRect *source_rect, u32 dest_texture_va,
                   i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                   const float clear_color[4], float clear_depth, u32 clear_stencil);

// D3DDevice_RunCommandBuffer on a recording device: records the run.
void RecordRun(u32 device_va, u32 command_buffer_va);

// Copies the data of the device's recorded Begin*Vertices draw, if any. Any
// thread, with or without State().mutex.
void FlushRecordedVertices(u32 device_va);

// D3DDevice_RunCommandBuffer on a device that is not recording: queues the
// buffer's replay with the device's current state (its inherited constants).
void QueueRunCommandBuffer(u32 device_va, u32 command_buffer_va);

void ReleaseCommandBuffer(u32 command_buffer_va);

} // namespace renut::gpu
