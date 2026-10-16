/**
 * @file    gpu/guest/command_buffers.h
 * @brief   D3D command buffers: draws recorded on one device, run on another.
 *
 * N&B records parts of its frame on worker threads, each with its own device
 * between D3DDevice_BeginCommandBuffer and EndCommandBuffer, and runs the
 * buffers on the title device with D3DDevice_RunCommandBuffer. The hooked
 * draw, clear and resolve calls of a recording device are kept with a copy of
 * the device state they read, and replayed through the same draw path when
 * the buffer runs. The guest buffer still gets the originals' packets for the
 * calls that are not replaced; nothing reads them.
 *
 * Every buffer N&B records inherits the vertex and pixel shader float
 * constants: those it does not set itself come from the running device.
 *
 * The recorded buffers have a lock of their own, which these functions take;
 * none of them needs State().mutex. A run is queued for the render thread
 * with the buffers as they are at the call (a buffer recorded again later is
 * a new one), so the game may record the next frame's while it replays.
 */
#pragma once

#include <rex/types.h>

#include "renut_engine/gpu/render/draw.h"

namespace renut::gpu {

// True while the device records a command buffer.
bool IsRecordingCommandBuffer(u32 device_va);

// After the original BeginCommandBuffer: drops what the buffer held before.
void BeginRecording(u32 device_va, u32 command_buffer_va);

// Records a draw on a recording device. 'up_vertex_bytes' / 'up_index_bytes'
// are the sizes of call.up_vertices / call.up_indices, copied now, as the GPU
// copies them into the buffer.
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
// buffer's replay on it, with the device's state now (the constants the
// buffer inherits).
void QueueRunCommandBuffer(u32 device_va, u32 command_buffer_va);

void ReleaseCommandBuffer(u32 command_buffer_va);

// For the memory report: the capacity of the recordings the game's command
// buffers hold now, and of the spare recordings kept for reuse; 'live'
// counts every recording, including older ones queued runs still hold.
struct CommandBufferMemory {
  size_t recorded_bytes = 0;
  size_t recorded = 0;
  size_t spare_bytes = 0;
  size_t spare = 0;
  size_t live = 0;
};
CommandBufferMemory CommandBufferMemoryUsage();

} // namespace renut::gpu
