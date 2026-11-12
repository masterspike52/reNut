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

  // DrawIndexedTessellatedVertices: 'primitive' is a patch type, base_vertex
  // the first patch and 'count' the patch count, drawn one instance per patch.
  bool tessellation = false;

  // Draw*UP / Begin*Vertices: stream 0 (and indices, when indexed) come from
  // these renderer-owned copies instead of the fetch constant.
  const u8 *up_vertices = nullptr;
  u32 up_stride = 0;
  u32 up_vertex_count = 0;
  const u8 *up_indices = nullptr;
  bool up_indices_32 = false;
};

// Reads the guest vertex buffers used by draws since the last call into their
// host copies; called at the device's KickOff and before command list submit.
void FlushVertexFillsLocked();

// The float constants of a replayed draw: groups its command buffer set
// (m_Pending bit 63 - group) come from the snapshot, the rest from the device.
struct ReplayConstants {
  const u8 *running = nullptr;
  uint64_t vs_set = 0, ps_set = 0;
  // Groups written since this replay's last constant upload (only those need
  // comparing); 'replay' identifies the replay (0: unknown, compare all).
  uint64_t *vs_dirty = nullptr, *ps_dirty = nullptr;
  uint64_t replay = 0;
};

// A draw from a device state snapshot (kDeviceSnapshotSize bytes laid out as
// the guest D3DDevice); without 'replay', all float constants are its own.
bool DrawStateLocked(const u8 *device_state, const DrawCall &call,
                     const ReplayConstants *replay = nullptr);

struct ClearRect {
  i32 x1, y1, x2, y2;
};

// Predicated tiling (D3DDevice_BeginTiling .. EndTiling): sets the screen-space
// tile rects that predicated draws are cut to; count 0 ends tiling.
void SetTilesLocked(const ClearRect *rects, u32 count);

// The predication mask of the RunCommandBuffer being replayed, which the
// replayed draws' own masks narrow; ~0 outside a replay. Returns the previous.
u32 SetReplayPredicationLocked(u32 mask);

// D3DDevice_ClearF. flags: 0x1-0x8 render targets 0-3, 0x10 depth, 0x20 stencil.
void ClearStateLocked(const u8 *device_state, u32 flags, const ClearRect *rect,
                      const float color[4], float depth, u32 stencil);

// D3DDevice_Resolve. flags & 7: source (0-3 render target, 4 depth); 0x100 /
// 0x200 clear color / depth-stencil after. 'dest_format' nullptr: read it now.
void ResolveStateLocked(const u8 *device_state, u32 flags, const ClearRect *source_rect,
                        u32 dest_texture_va, const d3d::TextureFetchConstant *dest_format,
                        i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                        const float clear_color[4], float clear_depth, u32 clear_stencil);

// Reads render target 'source' (0-3) back as untiled X8R8G8B8 guest words,
// clamping 'width'/'height' to the target; submits and waits for the GPU.
bool ReadRenderTargetLocked(const u8 *device_state, u32 source, u32 &width, u32 &height,
                            std::vector<u32> &words);

// What Swap scans out for the guest front buffer texture, or an empty image.
PresentImage FrontBufferImageLocked(u32 texture_va);

// A vertex declaration record at 'declaration_va' is being destroyed.
void ForgetVertexDeclarationLocked(u32 declaration_va);

// Once per guest Swap: ages the vertex stream cache, dropping unused streams.
void AdvanceStreamCacheFrameLocked();

} // namespace renut::gpu
