/**
 * @file    gpu/render/draw.h
 * @brief   Guest draws, clears and resolves, recorded from the D3D device's
 *          state at the time of the call.
 *
 * The hooked D3D entry points hand over the call's arguments; everything else
 * (bound shaders, declaration, streams, textures, render targets, render state
 * and constants) is read from a snapshot of the guest D3DDevice, which the
 * game's Set* calls and inline state writes keep current: taken at the call
 * (queued.h) or when a command buffer recorded it. These run on the render
 * thread (render_thread.h) and require State().mutex.
 */
#pragma once

#include <cstdint>
#include <vector>

#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/backend/video.h"

namespace renut::gpu {

struct DrawCall {
  u32 primitive = 0;  // D3DPRIMITIVETYPE (Xenos primitive type)
  // Non-indexed: 'count' vertices from start_vertex.
  // Indexed: 'count' indices from start_index; vertex = base_vertex + index.
  bool indexed = false;
  u32 start_vertex = 0;
  u32 count = 0;
  i32 base_vertex = 0;
  u32 start_index = 0;

  // DrawIndexedTessellatedVertices: 'primitive' is a patch type (triangle or
  // quad patches), base_vertex the first patch and 'count' the patch count;
  // in adaptive mode the indices from start_index on are the patches' edge
  // tessellation factors. Drawn as one instance per patch
  // (renutVertexRegisters in shader_common.h).
  bool tessellation = false;

  // Draw*UP / Begin*Vertices: stream 0 comes from here instead of its fetch
  // constant, and indices (when indexed) from up_indices: copies the renderer
  // owns, taken when the game's data was complete (queued.h).
  const u8 *up_vertices = nullptr;
  u32 up_stride = 0;
  u32 up_vertex_count = 0;
  const u8 *up_indices = nullptr;
  bool up_indices_32 = false;
};

// Reads the guest vertex buffers (and vertex fetch buffers) the draws recorded
// since the last call use into their host copies. Draws leave the reads for
// the device's KickOff, which calls this, as the GPU reads them only once the
// ring is kicked; the game may write them after the draw call until then.
// Also before the command list is submitted.
void FlushVertexFillsLocked();

// The float constants of a replayed draw: the groups of four registers its
// command buffer had set by the draw (m_Pending bit 63 - group) are in the
// draw's snapshot, the rest are the running device's.
struct ReplayConstants {
  const u8 *running = nullptr;
  uint64_t vs_set = 0, ps_set = 0;
  // Groups written since the last draw of this replay that uploaded its
  // constants (same bit order), which only those need comparing: Replay
  // gathers each op's writes, the upload consumes them. 'replay' tells
  // replays apart (0: unknown, compare everything); within one the running
  // device is the same.
  uint64_t *vs_dirty = nullptr, *ps_dirty = nullptr;
  uint64_t replay = 0;
};

// A draw from a device state snapshot (kDeviceSnapshotSize bytes laid out as
// the guest D3DDevice). Without 'replay', every float constant is the
// snapshot's.
bool DrawStateLocked(const u8 *device_state, const DrawCall &call,
                     const ReplayConstants *replay = nullptr);

struct ClearRect {
  i32 x1, y1, x2, y2;
};

// Predicated tiling (D3DDevice_BeginTiling .. EndTiling): the tile rects, in
// screen space; count 0 ends tiling. A draw predicated to one tile
// (d3d::kDeviceOffsetPredication) is set up for that tile's EDRAM, which the
// GPU resolves to the tile's rect: the host cuts it to the rect, offsetting
// it there unless its viewport spans the screen (see ComputeViewport).
void SetTilesLocked(const ClearRect *rects, u32 count);

// The predication mask of the RunCommandBuffer being replayed, which the
// replayed draws' own masks narrow; ~0 outside a replay. Returns the previous.
u32 SetReplayPredicationLocked(u32 mask);

// D3DDevice_ClearF. flags: 0x1-0x8 render targets 0-3, 0x10 depth, 0x20 stencil.
void ClearStateLocked(const u8 *device_state, u32 flags, const ClearRect *rect,
                      const float color[4], float depth, u32 stencil);

// D3DDevice_Resolve. flags & 7: source (0-3 render target, 4 depth);
// 0x100 clear the color target after, 0x200 clear depth/stencil after.
// 'dest_format': the destination header's fetch constant when the resolve was
// issued, or nullptr to read the header now.
void ResolveStateLocked(const u8 *device_state, u32 flags, const ClearRect *source_rect,
                        u32 dest_texture_va, const d3d::TextureFetchConstant *dest_format,
                        i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                        const float clear_color[4], float clear_depth, u32 clear_stencil);

// Reads render target 'source' (0-3) of the device state back for the CPU:
// what a color resolve of its rect (0, 0, width, height) into an X8R8G8B8
// texture would write, untiled, as width x height guest words (bytes
// FF RR GG BB each), row after row; the host texels a guest pixel covers at
// the target's render scale are averaged. 'width' and 'height' come back
// clamped to the target. Submits the open command list and waits for the GPU.
bool ReadRenderTargetLocked(const u8 *device_state, u32 source, u32 &width, u32 &height,
                            std::vector<u32> &words);

// What Swap scans out for the guest front buffer texture, or an empty image.
PresentImage FrontBufferImageLocked(u32 texture_va);

// A vertex declaration record at 'declaration_va' is being destroyed.
void ForgetVertexDeclarationLocked(u32 declaration_va);

// Once per guest Swap: ages the converted vertex stream cache and drops the
// streams no draw has read for a while.
void AdvanceStreamCacheFrameLocked();

// For the memory report: the converted vertex stream and index caches (the
// pool's host memory and what the copies in it take).
struct StreamCacheMemory {
  uint64_t pool_bytes = 0;
  uint64_t cached_bytes = 0;
  size_t streams = 0;
  size_t index_ranges = 0;
};
StreamCacheMemory StreamCacheMemoryLocked();

} // namespace renut::gpu
