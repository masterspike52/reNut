/**
 * @file    gpu/d3d/d3d_native.h
 * @brief   N&B's D3D runtime as native code: the device-state work of its D3D
 *          calls, ported from the game's own code (default.xex), without a GPU.
 *
 * Every D3D call the renderer hooks is implemented natively from N&B's
 * disassembly, here or in the hooks (gpu/d3d/hooks). The ports keep the device's
 * shadow state exactly as N&B's D3D does: the constants, the register images
 * (m_*Packet), the bound objects, the dirty masks (m_Pending) and the fences.
 * The renderer reads that state at every draw. What they leave out is what
 * only the GPU needed: command packets, ring and segment bookkeeping, cache
 * flushes, the GPU-side copies of command buffer resource lists.
 *
 * The command ring is a stub, as in reblue: m_pRing points at a scratch block
 * that KickOff rewinds. N&B code that writes packets inline (D3D's header
 * helpers compiled into the game) writes there, and nothing reads it.
 *
 * Offsets are byte offsets into the device allocation, as N&B's code uses them
 * (d3d.h names the ones the renderer reads).
 */
#pragma once

#include <bit>
#include <cmath>
#include <limits>

#include <rex/types.h>

#include "renut_engine/gpu/guest/guest_memory.h"

namespace renut::gpu::native {

// A guest D3D device's bytes, big-endian guest memory.
struct Device {
  u32 va = 0;
  u8 *bytes = nullptr;

  explicit operator bool() const { return bytes != nullptr; }

  u32 W(u32 offset) const { return *reinterpret_cast<const be_u32 *>(bytes + offset); }
  void SetW(u32 offset, u32 value) { *reinterpret_cast<be_u32 *>(bytes + offset) = value; }
  u16 H(u32 offset) const { return *reinterpret_cast<const be_u16 *>(bytes + offset); }
  void SetH(u32 offset, u16 value) { *reinterpret_cast<be_u16 *>(bytes + offset) = value; }
  u8 B(u32 offset) const { return bytes[offset]; }
  void SetB(u32 offset, u8 value) { bytes[offset] = value; }
  float F(u32 offset) const { return std::bit_cast<float>(W(offset)); }
  void SetF(u32 offset, float value) { SetW(offset, std::bit_cast<u32>(value)); }
  u64 Q(u32 offset) const { return *reinterpret_cast<const be_u64 *>(bytes + offset); }
  void SetQ(u32 offset, u64 value) { *reinterpret_cast<be_u64 *>(bytes + offset) = value; }

  // m_Pending[word] (D3DTAGCOLLECTION).
  u64 Pending(u32 word) const { return Q(word * 8); }
  void SetPending(u32 word, u64 value) { SetQ(word * 8, value); }
  void Dirty(u32 word, u64 bits) { SetPending(word, Pending(word) | bits); }
};

inline Device DeviceAt(u32 va) {
  return Device{va, GuestPtr<u8>(va)};
}

// The PowerPC field operations N&B's code uses, with bits counted from the
// least significant: insrwi (Insert) and extrwi (Extract).
constexpr u32 FieldMask(u32 shift, u32 width) {
  return (width >= 32 ? ~0u : ((1u << width) - 1)) << shift;
}
constexpr u32 Insert(u32 word, u32 value, u32 shift, u32 width) {
  const u32 mask = FieldMask(shift, width);
  return (word & ~mask) | ((value << shift) & mask);
}
constexpr u32 Extract(u32 word, u32 shift, u32 width) {
  return (word >> shift) & FieldMask(0, width);
}

// D3D's GPU address for a CPU address (the expression its code inlines).
constexpr u32 GpuAddress(u32 cpu_address) {
  return (cpu_address & 0x1FFFFFFFu) + (((cpu_address >> 20) + 0x200u) & 0x1000u);
}

// fctiwz / fctidz: truncate, saturating, NaN to the most negative value.
inline i32 TruncateToI32(double value) {
  if (std::isnan(value))
    return std::numeric_limits<i32>::min();
  if (value >= 2147483647.0)
    return std::numeric_limits<i32>::max();
  if (value <= -2147483648.0)
    return std::numeric_limits<i32>::min();
  return static_cast<i32>(value);
}
inline i64 TruncateToI64(double value) {
  if (std::isnan(value))
    return std::numeric_limits<i64>::min();
  if (value >= 9223372036854775807.0)
    return std::numeric_limits<i64>::max();
  if (value <= -9223372036854775808.0)
    return std::numeric_limits<i64>::min();
  return static_cast<i64>(value);
}

// Device fields used by more than one part of the runtime.
namespace off {
constexpr u32 Ring = 0x30;                 // m_pRing
constexpr u32 RingLimit = 0x34;            // m_pRingLimit
constexpr u32 RingGuarantee = 0x38;        // m_pRingGuarantee
constexpr u32 PredicatedPending2 = 0x28;   // m_Predicated_PendingMask2
constexpr u32 SetRenderStateCall = 0x40;   // [101]
constexpr u32 SetSamplerStateCall = 0x1D4; // [20]
constexpr u32 GetRenderStateCall = 0x224;  // [101]
constexpr u32 GetSamplerStateCall = 0x3B8; // [20]
constexpr u32 TextureFetch = 0x480;        // m_Constants.TextureFetch[26], 24 bytes each
constexpr u32 VertexFetch = 0x6F0;         // m_Constants.VertexFetch[18], 8 bytes each
constexpr u32 SurfaceInfo = 0x2880;        // m_DestinationPacket
constexpr u32 Color0Info = 0x2884;
constexpr u32 DepthInfo = 0x2888;
constexpr u32 WindowOffset = 0x28C0;       // m_WindowPacket
constexpr u32 WindowScissorTL = 0x28C4;
constexpr u32 WindowScissorBR = 0x28C8;
constexpr u32 ColorMask = 0x28DC;          // m_ValuesPacket
constexpr u32 DepthControl = 0x2934;       // m_ControlPacket
constexpr u32 BlendControl0 = 0x2938;
constexpr u32 ColorControl = 0x293C;
constexpr u32 HiControl = 0x2940;
constexpr u32 ClipControl = 0x2944;
constexpr u32 ModeControl = 0x2948;
constexpr u32 BlendControl1 = 0x2958;
constexpr u32 BlendControl2 = 0x295C;
constexpr u32 BlendControl3 = 0x2960;
constexpr u32 FenceCompletion = 0x2A90;    // -> the completed-fence word
constexpr u32 FenceTag = 0x2A9C;           // the current fence
constexpr u32 CommandBufferTagMask = 0x2AA0;  // Common bits a recorded buffer tracks
constexpr u32 TilePredication = 0x2AB4;    // SetPredication / SetCommandBufferPredication args
constexpr u32 RunPredication = 0x2AB8;
constexpr u32 Flags = 0x2ABC;              // bytes 0x2ABC-0x2ABF, see below
constexpr u32 Flags1 = 0x2ABD;
constexpr u32 Flags2 = 0x2ABE;
constexpr u32 Flags3 = 0x2ABF;
constexpr u32 ScissorRect = 0x3184;        // SetScissorRect's RECT
constexpr u32 IndexBuffer = 0x3094;
constexpr u32 RenderTargets = 0x3098;      // [4]
constexpr u32 DepthStencil = 0x30A8;
constexpr u32 StreamSources = 0x30AC;      // [16]
constexpr u32 StreamStrides = 0x30F0;      // u8[16], stride / 4
constexpr u32 Textures = 0x3100;           // [26]
constexpr u32 Predication = 0x31A4;        // tile predication of the next draws, ~0 none
constexpr u32 RunPredicationState = 0x31A8;
constexpr u32 PredicationSet = 0x31AC;     // 1 while SetPredication's mask is non-zero
constexpr u32 TilingSurfaces = 0x31B8;     // the 4 targets and depth BeginTiling bound
constexpr u32 TileCount = 0x31CC;
constexpr u32 TileRects = 0x31D0;          // D3DRECT[TileCount]
constexpr u32 TileOrigins = 0x32C0;        // per tile, x & ~31, y & ~31
constexpr u32 TilingExtent = 0x337C;       // widest and tallest tile edge
constexpr u32 CommandBuffer = 0x34CC;      // the D3DCommandBuffer being recorded
constexpr u32 SideBuffer = 0x4158;
constexpr u32 AutoDepthStencil = 0x39E8;   // InitializePresentationParameters' surfaces
constexpr u32 AutoFrontBuffer = 0x39EC;
constexpr u32 AutoBackBuffer = 0x39F0;
}  // namespace off

// Flags (0x2ABC) bits: 0x01 the draws that follow run in each tile's pass,
// 0x08/0x04 command buffer recording modes, 0x10/0x20 tiling, 0x80 no GPU
// hardware. Flags1 (0x2ABD): 0x02 fences complete as they are written, 0x20
// the device's command memory ran out.
constexpr u8 kFlagsNoHardware = 0x80;
constexpr u8 kFlags1ImmediateFences = 0x02;

// Ring stub and fences (hooks/device.cpp).
void RewindRing(Device device);
// CDevice::KickOff: ends the device's batch of work (and its fence).
u32 KickOff(u32 device_va);
// CDevice::BlockOnFence(Fence, Type, NoKick=FALSE).
void BlockOnFence(u32 device_va, u32 fence);
// The title device D3D internals reach through VdGlobalDevice.
u32 GlobalDevice();
// D3DDevice_Swap (D3DDevice_Present ends with it).
void Swap(u32 device_va, u32 front_buffer, u32 parameters);
// CDevice::SetFence on its own (EndTiling's): the current fence covers the
// work so far, and the device moves on to the next.
void SetFence(u32 device_va);

// Resources (hooks/resource.cpp).
// D3DDevice_CreateSurface(Width, Height, D3DFORMAT, D3DMULTISAMPLE_TYPE,
//     const D3DSURFACE_PARAMETERS*): the new surface, or 0.
u32 CreateSurface(u32 width, u32 height, u32 format, u32 multisample, u32 parameters_va);
// D3DResource_AddRef (a texture level's first reference is one on its texture).
u32 AddRefResource(u32 resource);

// Device state (d3d_native_state.cpp).
void SetRenderState(Device device, u32 state, u32 value);
void SetSamplerState(Device device, u32 sampler, u32 state, u32 value);
// The getters of N&B's state tables (field 0): the value as D3D reports it.
u32 GetRenderState(Device device, u32 state);
u32 GetSamplerState(Device device, u32 sampler, u32 state);
// InitializeApiState's walk of the default render and sampler state tables.
void InitializeStateTables(Device device);
void SetTexture(Device device, u32 sampler, u32 texture);
void SetStreamSource(Device device, u32 stream, u32 vertex_buffer, u32 offset, u32 stride);
void SetIndices(Device device, u32 index_buffer);
void SetVertexDeclaration(Device device, u32 declaration);
void SetVertexShader(Device device, u32 shader);
void SetPixelShader(Device device, u32 shader);
void SetViewport(Device device, float x, float y, float width, float height, float min_z,
                 float max_z);
void SetViewport(Device device, u32 viewport_va);  // D3DVIEWPORT9*
void SetScissorRect(Device device, i32 left, i32 top, i32 right, i32 bottom);
void SetRenderTarget(Device device, u32 index, u32 surface);
void SetDepthStencilSurface(Device device, u32 surface);
void SetSurfaceInfo(Device device, u32 surface);
void SetPredication(Device device, u32 mask);
void SetCommandBufferPredication(Device device, u32 tile_predication, u32 run_predication);
void SetShaderGPRAllocation(Device device, u32 flags, u32 vertex_count, u32 pixel_count);
void CleanupGprAllocationSetup(Device device);
void SetScreenExtentQueryMode(Device device, u32 mode);
void UnsetAll(Device device);
// ResetAllState: every piece of state dirty.
void ResetAllState(Device device);
// Recomputes Flags bit 0: the draws that follow run in each tile's pass.
void UpdateTilePredicated(Device device);
// True when the bound targets are the ones BeginTiling bound (or none).
bool TilingBoundSurfaces(Device device);
// What D3D does with a resource the device stops using: it is busy until the
// current fence (a recorded buffer's resource list is the GPU's; left out).
void TagResourceFence(Device device, u32 resource, bool read_fence);
// D3D's InvalidateConstants (0x8264E128) and its waiting variant (0x82657488,
// D3DDevice_BlockUntilIdle).
void InvalidateConstants(Device device);
void InvalidateConstantsAndWait(Device device);

// Clear and Resolve (d3d_native_state.cpp): the device work of the calls; the
// clear and the copy themselves are the renderer's (queued.h).
struct Rect {
  i32 x1, y1, x2, y2;
};
// D3DDevice_ClearF (0x822811A0, D3D::ClearF 0x82223560); no rect: the bound
// target's (or the tiling extent's).
void Clear(Device device, u32 flags, const Rect *rect);
// D3DDevice_Resolve (0x8222D6C8).
void Resolve(Device device, u32 flags, u32 dest_texture, float clear_z, u32 clear_stencil,
             u32 parameters);
// D3DDevice_BeginExport (0x82269618) / D3DDevice_EndExport (0x822693B0): a
// memexport stream's destination, Index, is Resource.
void BeginExport(Device device, u32 index, u32 resource, u32 flags);
void EndExport(Device device, u32 index, u32 resource, u32 flags);

}  // namespace renut::gpu::native
