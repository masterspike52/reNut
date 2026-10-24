/**
 * @file    gpu/d3d/hooks/state.cpp
 * @brief   N&B's D3D state calls, hooked with the native ports (d3d_native.h).
 *
 * The render and sampler state setters are the ones N&B's state tables name
 * (D3D::InitializeApiState puts them into every device's dispatch table,
 * which the game's inline SetRenderState / SetSamplerState call through).
 * The rest are D3DDevice_Set* and the CDevice internals that write the ring.
 */
#include <cstring>

#include <rex/hook.h>
#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/guest/guest_memory.h"

using namespace renut::gpu;

namespace {

template <u32 State>
void SetRenderStateHook(u32 device, u32 value) {
  native::SetRenderState(native::DeviceAt(device), State, value);
}

template <u32 State>
void SetSamplerStateHook(u32 device, u32 sampler, u32 value) {
  native::SetSamplerState(native::DeviceAt(device), sampler, State, value);
}

template <u32 State>
u32 GetRenderStateHook(u32 device) {
  return native::GetRenderState(native::DeviceAt(device), State);
}

template <u32 State>
u32 GetSamplerStateHook(u32 device, u32 sampler) {
  return native::GetSamplerState(native::DeviceAt(device), sampler, State);
}

// The device's AddRef of a resource it hands out (D3DResource_AddRef,
// resource.cpp).
u32 AddRefResource(u32 resource) {
  return native::AddRefResource(resource);
}

// D3DSurface *GetRenderTarget(D3DDevice*, DWORD Index) [0x8264D8D0]
u32 D3DDevice_GetRenderTarget_hook(u32 device, u32 index) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d && index < 4 ? d.W(native::off::RenderTargets + index * 4) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DSurface *GetDepthStencilSurface(D3DDevice*) [0x8264D910]
u32 D3DDevice_GetDepthStencilSurface_hook(u32 device) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d ? d.W(native::off::DepthStencil) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DVertexBuffer *GetStreamSource(D3DDevice*, UINT StreamNumber,
//     UINT *pOffsetInBytes, UINT *pStride) [0x8264D840]: the offset is the
// stream's fetch constant address less the buffer's.
u32 D3DDevice_GetStreamSource_hook(u32 device, u32 stream, mapped_u32 offset, mapped_u32 stride) {
  const native::Device d = native::DeviceAt(device);
  if (!d || stream >= 16)
    return 0;
  const u32 buffer = d.W(native::off::StreamSources + stream * 4);
  u32 bytes = 0;
  if (buffer) {
    const u32 fetch = d.W(native::off::TextureFetch + (95 - stream) * 8);
    bytes = fetch - native::GpuAddress(GuestPtr<const be_u32>(buffer)[6]);
    AddRefResource(buffer);
  }
  if (offset)
    *offset = bytes;
  if (stride)
    *stride = u32(d.B(native::off::StreamStrides + stream)) << 2;
  return buffer;
}

// void D3DDevice_GetViewport(D3DDevice*, D3DVIEWPORT9*) [0x82276AC0]
void D3DDevice_GetViewport_hook(u32 device, mapped_u32 viewport) {
  const native::Device d = native::DeviceAt(device);
  if (!d || !viewport)
    return;
  for (u32 i = 0; i < 4; ++i)  // fctidz, the low word
    viewport[i] = u32(native::TruncateToI64(d.F(0x3170 + i * 4)));
  viewport[4] = d.W(0x3180);
  viewport[5] = d.W(0x3184);
}

// void D3DDevice_SetClipPlane(D3DDevice*, DWORD Index, const float *pPlane) [0x8264DAE8]
void D3DDevice_SetClipPlane_hook(u32 device, u32 index, mapped_u32 plane) {
  native::Device d = native::DeviceAt(device);
  if (!d || !plane || index >= 6)
    return;
  for (u32 i = 0; i < 4; ++i)
    d.SetW(0x2820 + index * 16 + i * 4, plane[i]);
  d.Dirty(4, u64(1) << (54 - index));
}

// void D3DDevice_SetVertexFetchConstant(D3DDevice*, UINT VertexFetchRegister,
//     D3DVertexBuffer*, UINT Offset) [0x8264DB38]: a vertex fetch slot of the
// 3 in a fetch constant, made a vertex fetch constant first.
void D3DDevice_SetVertexFetchConstant_hook(u32 device, u32 reg, u32 vertex_buffer, u32 offset) {
  native::Device d = native::DeviceAt(device);
  const auto *vb = vertex_buffer ? GuestPtr<const be_u32>(vertex_buffer) : nullptr;
  if (!d || !vb || reg > 0x5F)
    return;
  const u32 slot = (0x5F - reg) / 3;
  const u32 part = (0x5F - reg) - slot * 3;
  const u32 constant = native::off::TextureFetch + slot * 24;
  const u32 type = d.W(constant) & 3;
  if (type == 0 || type == 2)
    std::memset(d.bytes + constant, 0, 24);
  d.SetW(constant, native::Insert(d.W(constant), 1, 0, 2));
  d.Dirty(3, u64(1) << (31 - slot));
  d.SetW(constant + part * 8, native::GpuAddress(u32(vb[6]) + offset));
  d.SetW(constant + part * 8 + 4, native::Insert(u32(vb[7]) - offset, 1, 28, 2));
}

// The boolean and integer shader constants (D3DDevice_Set{Vertex,Pixel}
// ShaderConstant{B,I}): bits from 0x2780 (vertex) / 0x2790 (pixel), a word
// of x, y, z bytes each from 0x27A0 / 0x27E0.
void SetShaderConstantsB(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i, ++start) {
    const u32 word = base + (start >> 5) * 4;
    const u32 bit = 1u << (start & 31);
    d.SetW(word, (d.W(word) & ~bit) | ((u32(data[i]) & 1) << (start & 31)));
  }
  d.Dirty(4, u64(1) << 56);
}

void SetShaderConstantsI(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i) {
    const u32 x = u32(data[i * 4]) & 0xFF, y = u32(data[i * 4 + 1]) & 0xFF;
    const u32 z = u32(data[i * 4 + 2]) & 0xFF;
    d.SetW(base + (start + i) * 4, (z << 16) | (y << 8) | x);
  }
  d.Dirty(4, u64(1) << 56);
}

// void D3DDevice_SetVertexShaderConstantB(D3DDevice*, UINT StartRegister,
//     const BOOL*, UINT BoolCount) [0x82221930], and the others alike.
void D3DDevice_SetVertexShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2780, start, data, count);
}
void D3DDevice_SetPixelShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2790, start, data, count);
}
void D3DDevice_SetVertexShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27A0, start, data, count);
}
void D3DDevice_SetPixelShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27E0, start, data, count);
}

// void D3DDevice_EndIndexedVertices(D3DDevice*) [0x826561F8]: the inline end
// of BeginIndexedVertices, back to the ring pointer it saved.
void D3DDevice_EndIndexedVertices_hook(u32 device) {
  native::Device d = native::DeviceAt(device);
  if (d)
    d.SetW(native::off::Ring, d.W(0x3484));
}

// void D3DDevice_SetTexture(D3DDevice*, DWORD Sampler, D3DBaseTexture*,
//     UINT64 PendingMask3) [0x8222F410]; the mask is the sampler's fetch
// constant bit, which the port derives from Sampler.
void D3DDevice_SetTexture_hook(u32 device, u32 sampler, u32 texture) {
  native::SetTexture(native::DeviceAt(device), sampler, texture);
}

// void D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber, D3DVertexBuffer*,
//     UINT OffsetInBytes, UINT Stride, UINT64 PendingMask3) [0x8222A408]
void D3DDevice_SetStreamSource_hook(u32 device, u32 stream, u32 vertex_buffer, u32 offset,
                                    u32 stride) {
  native::SetStreamSource(native::DeviceAt(device), stream, vertex_buffer, offset, stride);
}

// void D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*) [0x8220EE38]
void D3DDevice_SetIndices_hook(u32 device, u32 index_buffer) {
  native::SetIndices(native::DeviceAt(device), index_buffer);
}

// void D3DDevice_SetVertexDeclaration(D3DDevice*, D3DVertexDeclaration*) [0x82205700]
void D3DDevice_SetVertexDeclaration_hook(u32 device, u32 declaration) {
  native::SetVertexDeclaration(native::DeviceAt(device), declaration);
}

// void D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*) [0x82222EA0]
void D3DDevice_SetViewport_hook(u32 device, u32 viewport) {
  native::SetViewport(native::DeviceAt(device), viewport);
}

// void D3D::SetViewport(D3DDevice*, float X, float Y, float Width, float Height,
//     float MinZ, float MaxZ) [0x82222F20]
void D3D_SetViewport_hook(u32 device, float x, float y, float width, float height, float min_z,
                          float max_z) {
  native::SetViewport(native::DeviceAt(device), x, y, width, height, min_z, max_z);
}

// void D3DDevice_SetScissorRect(D3DDevice*, const RECT*) [0x82223178]
void D3DDevice_SetScissorRect_hook(u32 device, mapped_u32 rect) {
  if (!rect)
    return;
  native::SetScissorRect(native::DeviceAt(device), i32(u32(rect[0])), i32(u32(rect[1])),
                         i32(u32(rect[2])), i32(u32(rect[3])));
}

// void D3D::SetSurfaceClip(CDevice*, int, int, int, int) [0x82223270]: the
// window scissor and per-tile clip packets, nothing else.
void D3D_SetSurfaceClip_hook(u32 /*device*/, u32, u32, u32, u32) {}

// void D3DDevice_SetRenderTarget(D3DDevice*, DWORD Index, D3DSurface*) [0x8227AB68]
void D3DDevice_SetRenderTarget_hook(u32 device, u32 index, u32 surface) {
  native::SetRenderTarget(native::DeviceAt(device), index, surface);
}

// void D3DDevice_SetDepthStencilSurface(D3DDevice*, D3DSurface*) [0x8227AF40]
void D3DDevice_SetDepthStencilSurface_hook(u32 device, u32 surface) {
  native::SetDepthStencilSurface(native::DeviceAt(device), surface);
}

// void D3D::SetSurfaceInfo(CDevice*, D3DSurface*) [0x8227AEB0]
void D3D_SetSurfaceInfo_hook(u32 device, u32 surface) {
  native::SetSurfaceInfo(native::DeviceAt(device), surface);
}

// void D3DDevice_SetPredication(D3DDevice*, DWORD PredicationMask) [0x8222AA18]
void D3DDevice_SetPredication_hook(u32 device, u32 mask) {
  native::SetPredication(native::DeviceAt(device), mask);
}

// void D3DDevice_SetCommandBufferPredication(D3DDevice*, DWORD TilePredication,
//     DWORD RunPredication) [0x82658678]
void D3DDevice_SetCommandBufferPredication_hook(u32 device, u32 tile_predication,
                                                u32 run_predication) {
  native::SetCommandBufferPredication(native::DeviceAt(device), tile_predication,
                                      run_predication);
}

// void D3DDevice_SetShaderGPRAllocation(D3DDevice*, DWORD Flags,
//     DWORD VertexShaderCount, DWORD PixelShaderCount) [0x82220B20]
void D3DDevice_SetShaderGPRAllocation_hook(u32 device, u32 flags, u32 vertex_count,
                                           u32 pixel_count) {
  native::SetShaderGPRAllocation(native::DeviceAt(device), flags, vertex_count, pixel_count);
}

// void D3D::CleanupGprAllocationSetup(CDevice*) [0x8264E2C0]
void D3D_CleanupGprAllocationSetup_hook(u32 device) {
  native::CleanupGprAllocationSetup(native::DeviceAt(device));
}

// void D3DDevice_SetScreenExtentQueryMode(D3DDevice*, D3DSCREENEXTENTQUERYMODE)
// [0x82658988]
void D3DDevice_SetScreenExtentQueryMode_hook(u32 device, u32 mode) {
  native::SetScreenExtentQueryMode(native::DeviceAt(device), mode);
}

// void D3DDevice_UnsetAll(D3DDevice*) [0x8264E1A8]
void D3DDevice_UnsetAll_hook(u32 device) {
  native::UnsetAll(native::DeviceAt(device));
}

// void D3D::ResetAllState(CDevice*, BOOL) [0x82287D38]
void D3D_ResetAllState_hook(u32 device, u32 /*skip_packets*/) {
  native::ResetAllState(native::DeviceAt(device));
}

// void D3D::InvalidateConstants(D3DDevice*) [0x8264E128]
void D3D_InvalidateConstants_hook(u32 device) {
  native::InvalidateConstants(native::DeviceAt(device));
}

// void D3DDevice_BlockUntilIdle(D3DDevice*) [0x82657488] (IDA: the second
// D3D::InvalidateConstants): waits for the device's current fence (N&B calls
// it before releasing a device).
void D3DDevice_BlockUntilIdle_hook(u32 device) {
  native::InvalidateConstantsAndWait(native::DeviceAt(device));
}

} // namespace

// Render states, by their index in N&B's table (off_82E506F0).
REX_HOOK(D3DDevice_SetRenderState_ZEnable, SetRenderStateHook<10>);
REX_HOOK(D3DDevice_SetRenderState_ZFunc, SetRenderStateHook<11>);
REX_HOOK(D3DDevice_SetRenderState_ZWriteEnable, SetRenderStateHook<12>);
REX_HOOK(D3DDevice_SetRenderState_FillMode, SetRenderStateHook<13>);
REX_HOOK(D3DDevice_SetRenderState_CullMode, SetRenderStateHook<14>);
REX_HOOK(D3DDevice_SetRenderState_AlphaBlendEnable, SetRenderStateHook<15>);
REX_HOOK(D3DDevice_SetRenderState_SeparateAlphaBlendEnable, SetRenderStateHook<16>);
REX_HOOK(D3DDevice_SetRenderState_BlendFactor, SetRenderStateHook<17>);
REX_HOOK(D3DDevice_SetRenderState_SrcBlend, SetRenderStateHook<18>);
REX_HOOK(D3DDevice_SetRenderState_DestBlend, SetRenderStateHook<19>);
REX_HOOK(D3DDevice_SetRenderState_BlendOp, SetRenderStateHook<20>);
REX_HOOK(D3DDevice_SetRenderState_SrcBlendAlpha, SetRenderStateHook<21>);
REX_HOOK(D3DDevice_SetRenderState_DestBlendAlpha, SetRenderStateHook<22>);
REX_HOOK(D3DDevice_SetRenderState_BlendOpAlpha, SetRenderStateHook<23>);
REX_HOOK(D3DDevice_SetRenderState_AlphaTestEnable, SetRenderStateHook<24>);
REX_HOOK(D3DDevice_SetRenderState_AlphaRef, SetRenderStateHook<25>);
REX_HOOK(D3DDevice_SetRenderState_AlphaFunc, SetRenderStateHook<26>);
REX_HOOK(D3DDevice_SetRenderState_StencilEnable, SetRenderStateHook<27>);
REX_HOOK(D3DDevice_SetRenderState_TwoSidedStencilMode, SetRenderStateHook<28>);
REX_HOOK(D3DDevice_SetRenderState_StencilFail, SetRenderStateHook<29>);
REX_HOOK(D3DDevice_SetRenderState_StencilZFail, SetRenderStateHook<30>);
REX_HOOK(D3DDevice_SetRenderState_StencilPass, SetRenderStateHook<31>);
REX_HOOK(D3DDevice_SetRenderState_StencilFunc, SetRenderStateHook<32>);
REX_HOOK(D3DDevice_SetRenderState_StencilRef, SetRenderStateHook<33>);
REX_HOOK(D3DDevice_SetRenderState_StencilMask, SetRenderStateHook<34>);
REX_HOOK(D3DDevice_SetRenderState_StencilWriteMask, SetRenderStateHook<35>);
REX_HOOK(D3DDevice_SetRenderState_CCWStencilFail, SetRenderStateHook<36>);
REX_HOOK(D3DDevice_SetRenderState_CCWStencilZFail, SetRenderStateHook<37>);
REX_HOOK(D3DDevice_SetRenderState_CCWStencilPass, SetRenderStateHook<38>);
REX_HOOK(D3DDevice_SetRenderState_CCWStencilFunc, SetRenderStateHook<39>);
REX_HOOK(D3DDevice_SetRenderState_CCWStencilRef, SetRenderStateHook<40>);
REX_HOOK(D3DDevice_SetRenderState_CCWStencilMask, SetRenderStateHook<41>);
REX_HOOK(D3DDevice_SetRenderState_CCWStencilWriteMask, SetRenderStateHook<42>);
REX_HOOK(D3DDevice_SetRenderState_ClipPlaneEnable, SetRenderStateHook<43>);
REX_HOOK(D3DDevice_SetRenderState_PointSize, SetRenderStateHook<44>);
REX_HOOK(D3DDevice_SetRenderState_PointSizeMin, SetRenderStateHook<45>);
REX_HOOK(D3DDevice_SetRenderState_PointSpriteEnable, SetRenderStateHook<46>);
REX_HOOK(D3DDevice_SetRenderState_PointSizeMax, SetRenderStateHook<47>);
REX_HOOK(D3DDevice_SetRenderState_MultiSampleAntiAlias, SetRenderStateHook<48>);
REX_HOOK(D3DDevice_SetRenderState_MultiSampleMask, SetRenderStateHook<49>);
REX_HOOK(D3DDevice_SetRenderState_ScissorTestEnable, SetRenderStateHook<50>);
REX_HOOK(D3DDevice_SetRenderState_SlopeScaleDepthBias, SetRenderStateHook<51>);
REX_HOOK(D3DDevice_SetRenderState_DepthBias, SetRenderStateHook<52>);
REX_HOOK(D3DDevice_SetRenderState_ColorWriteEnable, SetRenderStateHook<53>);
REX_HOOK(D3DDevice_SetRenderState_ColorWriteEnable1, SetRenderStateHook<54>);
REX_HOOK(D3DDevice_SetRenderState_ColorWriteEnable2, SetRenderStateHook<55>);
REX_HOOK(D3DDevice_SetRenderState_ColorWriteEnable3, SetRenderStateHook<56>);
REX_HOOK(D3DDevice_SetRenderState_TessellationMode, SetRenderStateHook<57>);
REX_HOOK(D3DDevice_SetRenderState_MinTessellationLevel, SetRenderStateHook<58>);
REX_HOOK(D3DDevice_SetRenderState_MaxTessellationLevel, SetRenderStateHook<59>);
REX_HOOK(D3DDevice_SetRenderState_Wrap0, SetRenderStateHook<60>);
REX_HOOK(D3DDevice_SetRenderState_Wrap1, SetRenderStateHook<61>);
REX_HOOK(D3DDevice_SetRenderState_Wrap2, SetRenderStateHook<62>);
REX_HOOK(D3DDevice_SetRenderState_Wrap3, SetRenderStateHook<63>);
REX_HOOK(D3DDevice_SetRenderState_Wrap4, SetRenderStateHook<64>);
REX_HOOK(D3DDevice_SetRenderState_Wrap5, SetRenderStateHook<65>);
REX_HOOK(D3DDevice_SetRenderState_Wrap6, SetRenderStateHook<66>);
REX_HOOK(D3DDevice_SetRenderState_Wrap7, SetRenderStateHook<67>);
REX_HOOK(D3DDevice_SetRenderState_Wrap8, SetRenderStateHook<68>);
REX_HOOK(D3DDevice_SetRenderState_Wrap9, SetRenderStateHook<69>);
REX_HOOK(D3DDevice_SetRenderState_Wrap10, SetRenderStateHook<70>);
REX_HOOK(D3DDevice_SetRenderState_Wrap11, SetRenderStateHook<71>);
REX_HOOK(D3DDevice_SetRenderState_Wrap12, SetRenderStateHook<72>);
REX_HOOK(D3DDevice_SetRenderState_Wrap13, SetRenderStateHook<73>);
REX_HOOK(D3DDevice_SetRenderState_Wrap14, SetRenderStateHook<74>);
REX_HOOK(D3DDevice_SetRenderState_Wrap15, SetRenderStateHook<75>);
REX_HOOK(D3DDevice_SetRenderState_ViewportEnable, SetRenderStateHook<76>);
REX_HOOK(D3DDevice_SetRenderState_HighPrecisionBlendEnable, SetRenderStateHook<77>);
REX_HOOK(D3DDevice_SetRenderState_HighPrecisionBlendEnable1, SetRenderStateHook<78>);
REX_HOOK(D3DDevice_SetRenderState_HighPrecisionBlendEnable2, SetRenderStateHook<79>);
REX_HOOK(D3DDevice_SetRenderState_HighPrecisionBlendEnable3, SetRenderStateHook<80>);
REX_HOOK(D3DDevice_SetRenderState_HalfPixelOffset, SetRenderStateHook<81>);
REX_HOOK(D3DDevice_SetRenderState_PrimitiveResetEnable, SetRenderStateHook<82>);
REX_HOOK(D3DDevice_SetRenderState_PrimitiveResetIndex, SetRenderStateHook<83>);
REX_HOOK(D3DDevice_SetRenderState_AlphaToMaskEnable, SetRenderStateHook<84>);
REX_HOOK(D3DDevice_SetRenderState_AlphaToMaskOffsets, SetRenderStateHook<85>);
REX_HOOK(D3DDevice_SetRenderState_GuardBand_X, SetRenderStateHook<86>);
REX_HOOK(D3DDevice_SetRenderState_GuardBand_Y, SetRenderStateHook<87>);
REX_HOOK(D3DDevice_SetRenderState_DiscardBand_X, SetRenderStateHook<88>);
REX_HOOK(D3DDevice_SetRenderState_DiscardBand_Y, SetRenderStateHook<89>);
REX_HOOK(D3DDevice_SetRenderState_HiStencilEnable, SetRenderStateHook<90>);
REX_HOOK(D3DDevice_SetRenderState_HiStencilWriteEnable, SetRenderStateHook<91>);
REX_HOOK(D3DDevice_SetRenderState_HiStencilFunc, SetRenderStateHook<92>);
REX_HOOK(D3DDevice_SetRenderState_HiStencilRef, SetRenderStateHook<93>);
REX_HOOK(D3DDevice_SetRenderState_PresentInterval, SetRenderStateHook<94>);
REX_HOOK(D3DDevice_SetRenderState_PresentImmediateThreshold, SetRenderStateHook<95>);
REX_HOOK(D3DDevice_SetRenderState_HiZEnable, SetRenderStateHook<96>);
REX_HOOK(D3DDevice_SetRenderState_HiZWriteEnable, SetRenderStateHook<97>);
REX_HOOK(D3DDevice_SetRenderState_LastPixel, SetRenderStateHook<98>);
REX_HOOK(D3DDevice_SetRenderState_LineWidth, SetRenderStateHook<99>);
REX_HOOK(D3DDevice_SetRenderState_Buffer2Frames, SetRenderStateHook<100>);

// Sampler states, by their index in N&B's table (off_82E50BD0).
REX_HOOK(D3DDevice_SetSamplerState_AddressU, SetSamplerStateHook<0>);
REX_HOOK(D3DDevice_SetSamplerState_AddressV, SetSamplerStateHook<1>);
REX_HOOK(D3DDevice_SetSamplerState_AddressW, SetSamplerStateHook<2>);
REX_HOOK(D3DDevice_SetSamplerState_BorderColor, SetSamplerStateHook<3>);
REX_HOOK(D3DDevice_SetSamplerState_MagFilter, SetSamplerStateHook<4>);
REX_HOOK(D3DDevice_SetSamplerState_MinFilter, SetSamplerStateHook<5>);
REX_HOOK(D3DDevice_SetSamplerState_MipFilter, SetSamplerStateHook<6>);
REX_HOOK(D3DDevice_SetSamplerState_MipMapLodBias, SetSamplerStateHook<7>);
REX_HOOK(D3DDevice_SetSamplerState_MaxMipLevel, SetSamplerStateHook<8>);
REX_HOOK(D3DDevice_SetSamplerState_MaxAnisotropy, SetSamplerStateHook<9>);
REX_HOOK(D3DDevice_SetSamplerState_MagFilterZ, SetSamplerStateHook<10>);
REX_HOOK(D3DDevice_SetSamplerState_MinFilterZ, SetSamplerStateHook<11>);
REX_HOOK(D3DDevice_SetSamplerState_SeparateZFilterEnable, SetSamplerStateHook<12>);
REX_HOOK(D3DDevice_SetSamplerState_MinMipLevel, SetSamplerStateHook<13>);
REX_HOOK(D3DDevice_SetSamplerState_TrilinearThreshold, SetSamplerStateHook<14>);
REX_HOOK(D3DDevice_SetSamplerState_AnisotropyBias, SetSamplerStateHook<15>);
REX_HOOK(D3DDevice_SetSamplerState_HGradientExpBias, SetSamplerStateHook<16>);
REX_HOOK(D3DDevice_SetSamplerState_VGradientExpBias, SetSamplerStateHook<17>);
REX_HOOK(D3DDevice_SetSamplerState_WhiteBorderColorW, SetSamplerStateHook<18>);
REX_HOOK(D3DDevice_SetSamplerState_PointBorderEnable, SetSamplerStateHook<19>);

REX_HOOK(D3DDevice_SetTexture, D3DDevice_SetTexture_hook);
REX_HOOK(D3DDevice_SetTexture_Inline, D3DDevice_SetTexture_hook);
REX_HOOK(D3DDevice_SetStreamSource, D3DDevice_SetStreamSource_hook);
REX_HOOK(D3DDevice_SetIndices, D3DDevice_SetIndices_hook);
REX_HOOK(D3DDevice_SetVertexDeclaration, D3DDevice_SetVertexDeclaration_hook);
REX_HOOK(D3DDevice_SetViewport, D3DDevice_SetViewport_hook);
REX_HOOK(D3D_SetViewport, D3D_SetViewport_hook);
REX_HOOK(D3DDevice_SetScissorRect, D3DDevice_SetScissorRect_hook);
REX_HOOK(D3D_SetSurfaceClip, D3D_SetSurfaceClip_hook);
REX_HOOK(D3DDevice_SetRenderTarget, D3DDevice_SetRenderTarget_hook);
REX_HOOK(D3DDevice_SetDepthStencilSurface, D3DDevice_SetDepthStencilSurface_hook);
REX_HOOK(D3D_SetSurfaceInfo, D3D_SetSurfaceInfo_hook);
REX_HOOK(D3DDevice_SetPredication, D3DDevice_SetPredication_hook);
REX_HOOK(D3DDevice_SetCommandBufferPredication, D3DDevice_SetCommandBufferPredication_hook);
REX_HOOK(D3DDevice_SetShaderGPRAllocation, D3DDevice_SetShaderGPRAllocation_hook);
REX_HOOK(D3D_CleanupGprAllocationSetup, D3D_CleanupGprAllocationSetup_hook);
REX_HOOK(D3DDevice_SetScreenExtentQueryMode, D3DDevice_SetScreenExtentQueryMode_hook);
REX_HOOK(D3DDevice_UnsetAll, D3DDevice_UnsetAll_hook);
REX_HOOK(D3D_ResetAllState, D3D_ResetAllState_hook);
REX_HOOK(D3D_InvalidateConstants, D3D_InvalidateConstants_hook);
REX_HOOK(D3DDevice_BlockUntilIdle, D3DDevice_BlockUntilIdle_hook);
// Through a thunk (j_D3DDevice_SetRenderTarget 0x8264D8C8).
REX_HOOK(D3DDevice_SetRenderTarget_Thunk, D3DDevice_SetRenderTarget_hook);
REX_HOOK(D3DDevice_GetRenderTarget, D3DDevice_GetRenderTarget_hook);
REX_HOOK(D3DDevice_GetDepthStencilSurface, D3DDevice_GetDepthStencilSurface_hook);
REX_HOOK(D3DDevice_GetStreamSource, D3DDevice_GetStreamSource_hook);
REX_HOOK(D3DDevice_GetViewport, D3DDevice_GetViewport_hook);
REX_HOOK(D3DDevice_SetClipPlane, D3DDevice_SetClipPlane_hook);
REX_HOOK(D3DDevice_SetVertexFetchConstant, D3DDevice_SetVertexFetchConstant_hook);
REX_HOOK(D3DDevice_SetVertexShaderConstantB, D3DDevice_SetVertexShaderConstantB_hook);
REX_HOOK(D3DDevice_SetPixelShaderConstantB, D3DDevice_SetPixelShaderConstantB_hook);
REX_HOOK(D3DDevice_SetVertexShaderConstantI, D3DDevice_SetVertexShaderConstantI_hook);
REX_HOOK(D3DDevice_SetPixelShaderConstantI, D3DDevice_SetPixelShaderConstantI_hook);
REX_HOOK(D3DDevice_EndIndexedVertices, D3DDevice_EndIndexedVertices_hook);

// Render state getters, by their index in N&B's table (off_82E506F0, field 0).
REX_HOOK(D3DDevice_GetRenderState_ZEnable, GetRenderStateHook<10>);
REX_HOOK(D3DDevice_GetRenderState_ZFunc, GetRenderStateHook<11>);
REX_HOOK(D3DDevice_GetRenderState_ZWriteEnable, GetRenderStateHook<12>);
REX_HOOK(D3DDevice_GetRenderState_FillMode, GetRenderStateHook<13>);
REX_HOOK(D3DDevice_GetRenderState_CullMode, GetRenderStateHook<14>);
REX_HOOK(D3DDevice_GetRenderState_AlphaBlendEnable, GetRenderStateHook<15>);
REX_HOOK(D3DDevice_GetRenderState_SeparateAlphaBlendEnable, GetRenderStateHook<16>);
REX_HOOK(D3DDevice_GetRenderState_BlendFactor, GetRenderStateHook<17>);
REX_HOOK(D3DDevice_GetRenderState_SrcBlend, GetRenderStateHook<18>);
REX_HOOK(D3DDevice_GetRenderState_DestBlend, GetRenderStateHook<19>);
REX_HOOK(D3DDevice_GetRenderState_BlendOp, GetRenderStateHook<20>);
REX_HOOK(D3DDevice_GetRenderState_SrcBlendAlpha, GetRenderStateHook<21>);
REX_HOOK(D3DDevice_GetRenderState_DestBlendAlpha, GetRenderStateHook<22>);
REX_HOOK(D3DDevice_GetRenderState_BlendOpAlpha, GetRenderStateHook<23>);
REX_HOOK(D3DDevice_GetRenderState_AlphaTestEnable, GetRenderStateHook<24>);
REX_HOOK(D3DDevice_GetRenderState_AlphaRef, GetRenderStateHook<25>);
REX_HOOK(D3DDevice_GetRenderState_AlphaFunc, GetRenderStateHook<26>);
REX_HOOK(D3DDevice_GetRenderState_StencilEnable, GetRenderStateHook<27>);
REX_HOOK(D3DDevice_GetRenderState_TwoSidedStencilMode, GetRenderStateHook<28>);
REX_HOOK(D3DDevice_GetRenderState_StencilFail, GetRenderStateHook<29>);
REX_HOOK(D3DDevice_GetRenderState_StencilZFail, GetRenderStateHook<30>);
REX_HOOK(D3DDevice_GetRenderState_StencilPass, GetRenderStateHook<31>);
REX_HOOK(D3DDevice_GetRenderState_StencilFunc, GetRenderStateHook<32>);
REX_HOOK(D3DDevice_GetRenderState_StencilRef, GetRenderStateHook<33>);
REX_HOOK(D3DDevice_GetRenderState_StencilMask, GetRenderStateHook<34>);
REX_HOOK(D3DDevice_GetRenderState_StencilWriteMask, GetRenderStateHook<35>);
REX_HOOK(D3DDevice_GetRenderState_CCWStencilFail, GetRenderStateHook<36>);
REX_HOOK(D3DDevice_GetRenderState_CCWStencilZFail, GetRenderStateHook<37>);
REX_HOOK(D3DDevice_GetRenderState_CCWStencilPass, GetRenderStateHook<38>);
REX_HOOK(D3DDevice_GetRenderState_CCWStencilFunc, GetRenderStateHook<39>);
REX_HOOK(D3DDevice_GetRenderState_CCWStencilRef, GetRenderStateHook<40>);
REX_HOOK(D3DDevice_GetRenderState_CCWStencilMask, GetRenderStateHook<41>);
REX_HOOK(D3DDevice_GetRenderState_CCWStencilWriteMask, GetRenderStateHook<42>);
REX_HOOK(D3DDevice_GetRenderState_ClipPlaneEnable, GetRenderStateHook<43>);
REX_HOOK(D3DDevice_GetRenderState_PointSize, GetRenderStateHook<44>);
REX_HOOK(D3DDevice_GetRenderState_PointSizeMin, GetRenderStateHook<45>);
REX_HOOK(D3DDevice_GetRenderState_PointSpriteEnable, GetRenderStateHook<46>);
REX_HOOK(D3DDevice_GetRenderState_PointSizeMax, GetRenderStateHook<47>);
REX_HOOK(D3DDevice_GetRenderState_MultiSampleAntiAlias, GetRenderStateHook<48>);
REX_HOOK(D3DDevice_GetRenderState_MultiSampleMask, GetRenderStateHook<49>);
REX_HOOK(D3DDevice_GetRenderState_ScissorTestEnable, GetRenderStateHook<50>);
REX_HOOK(D3DDevice_GetRenderState_SlopeScaleDepthBias, GetRenderStateHook<51>);
REX_HOOK(D3DDevice_GetRenderState_DepthBias, GetRenderStateHook<52>);
REX_HOOK(D3DDevice_GetRenderState_ColorWriteEnable, GetRenderStateHook<53>);
REX_HOOK(D3DDevice_GetRenderState_ColorWriteEnable1, GetRenderStateHook<54>);
REX_HOOK(D3DDevice_GetRenderState_ColorWriteEnable2, GetRenderStateHook<55>);
REX_HOOK(D3DDevice_GetRenderState_ColorWriteEnable3, GetRenderStateHook<56>);
REX_HOOK(D3DDevice_GetRenderState_TessellationMode, GetRenderStateHook<57>);
REX_HOOK(D3DDevice_GetRenderState_MinTessellationLevel, GetRenderStateHook<58>);
REX_HOOK(D3DDevice_GetRenderState_MaxTessellationLevel, GetRenderStateHook<59>);
REX_HOOK(D3DDevice_GetRenderState_Wrap0, GetRenderStateHook<60>);
REX_HOOK(D3DDevice_GetRenderState_Wrap1, GetRenderStateHook<61>);
REX_HOOK(D3DDevice_GetRenderState_Wrap2, GetRenderStateHook<62>);
REX_HOOK(D3DDevice_GetRenderState_Wrap3, GetRenderStateHook<63>);
REX_HOOK(D3DDevice_GetRenderState_Wrap4, GetRenderStateHook<64>);
REX_HOOK(D3DDevice_GetRenderState_Wrap5, GetRenderStateHook<65>);
REX_HOOK(D3DDevice_GetRenderState_Wrap6, GetRenderStateHook<66>);
REX_HOOK(D3DDevice_GetRenderState_Wrap7, GetRenderStateHook<67>);
REX_HOOK(D3DDevice_GetRenderState_Wrap8, GetRenderStateHook<68>);
REX_HOOK(D3DDevice_GetRenderState_Wrap9, GetRenderStateHook<69>);
REX_HOOK(D3DDevice_GetRenderState_Wrap10, GetRenderStateHook<70>);
REX_HOOK(D3DDevice_GetRenderState_Wrap11, GetRenderStateHook<71>);
REX_HOOK(D3DDevice_GetRenderState_Wrap12, GetRenderStateHook<72>);
REX_HOOK(D3DDevice_GetRenderState_Wrap13, GetRenderStateHook<73>);
REX_HOOK(D3DDevice_GetRenderState_Wrap14, GetRenderStateHook<74>);
REX_HOOK(D3DDevice_GetRenderState_Wrap15, GetRenderStateHook<75>);
REX_HOOK(D3DDevice_GetRenderState_ViewportEnable, GetRenderStateHook<76>);
REX_HOOK(D3DDevice_GetRenderState_HighPrecisionBlendEnable, GetRenderStateHook<77>);
REX_HOOK(D3DDevice_GetRenderState_HighPrecisionBlendEnable1, GetRenderStateHook<78>);
REX_HOOK(D3DDevice_GetRenderState_HighPrecisionBlendEnable2, GetRenderStateHook<79>);
REX_HOOK(D3DDevice_GetRenderState_HighPrecisionBlendEnable3, GetRenderStateHook<80>);
REX_HOOK(D3DDevice_GetRenderState_HalfPixelOffset, GetRenderStateHook<81>);
REX_HOOK(D3DDevice_GetRenderState_PrimitiveResetEnable, GetRenderStateHook<82>);
REX_HOOK(D3DDevice_GetRenderState_PrimitiveResetIndex, GetRenderStateHook<83>);
REX_HOOK(D3DDevice_GetRenderState_AlphaToMaskEnable, GetRenderStateHook<84>);
REX_HOOK(D3DDevice_GetRenderState_AlphaToMaskOffsets, GetRenderStateHook<85>);
REX_HOOK(D3DDevice_GetRenderState_GuardBand_X, GetRenderStateHook<86>);
REX_HOOK(D3DDevice_GetRenderState_GuardBand_Y, GetRenderStateHook<87>);
REX_HOOK(D3DDevice_GetRenderState_DiscardBand_X, GetRenderStateHook<88>);
REX_HOOK(D3DDevice_GetRenderState_DiscardBand_Y, GetRenderStateHook<89>);
REX_HOOK(D3DDevice_GetRenderState_HiStencilEnable, GetRenderStateHook<90>);
REX_HOOK(D3DDevice_GetRenderState_HiStencilWriteEnable, GetRenderStateHook<91>);
REX_HOOK(D3DDevice_GetRenderState_HiStencilFunc, GetRenderStateHook<92>);
REX_HOOK(D3DDevice_GetRenderState_HiStencilRef, GetRenderStateHook<93>);
REX_HOOK(D3DDevice_GetRenderState_PresentInterval, GetRenderStateHook<94>);
REX_HOOK(D3DDevice_GetRenderState_PresentImmediateThreshold, GetRenderStateHook<95>);
REX_HOOK(D3DDevice_GetRenderState_HiZEnable, GetRenderStateHook<96>);
REX_HOOK(D3DDevice_GetRenderState_HiZWriteEnable, GetRenderStateHook<97>);
REX_HOOK(D3DDevice_GetRenderState_LastPixel, GetRenderStateHook<98>);
REX_HOOK(D3DDevice_GetRenderState_LineWidth, GetRenderStateHook<99>);
REX_HOOK(D3DDevice_GetRenderState_Buffer2Frames, GetRenderStateHook<100>);

// Sampler state getters (off_82E50BD0, field 0).
REX_HOOK(D3DDevice_GetSamplerState_AddressU, GetSamplerStateHook<0>);
REX_HOOK(D3DDevice_GetSamplerState_AddressV, GetSamplerStateHook<1>);
REX_HOOK(D3DDevice_GetSamplerState_AddressW, GetSamplerStateHook<2>);
REX_HOOK(D3DDevice_GetSamplerState_BorderColor, GetSamplerStateHook<3>);
REX_HOOK(D3DDevice_GetSamplerState_MagFilter, GetSamplerStateHook<4>);
REX_HOOK(D3DDevice_GetSamplerState_MinFilter, GetSamplerStateHook<5>);
REX_HOOK(D3DDevice_GetSamplerState_MipFilter, GetSamplerStateHook<6>);
REX_HOOK(D3DDevice_GetSamplerState_MipMapLodBias, GetSamplerStateHook<7>);
REX_HOOK(D3DDevice_GetSamplerState_MaxMipLevel, GetSamplerStateHook<8>);
REX_HOOK(D3DDevice_GetSamplerState_MaxAnisotropy, GetSamplerStateHook<9>);
REX_HOOK(D3DDevice_GetSamplerState_MagFilterZ, GetSamplerStateHook<10>);
REX_HOOK(D3DDevice_GetSamplerState_MinFilterZ, GetSamplerStateHook<11>);
REX_HOOK(D3DDevice_GetSamplerState_SeparateZFilterEnable, GetSamplerStateHook<12>);
REX_HOOK(D3DDevice_GetSamplerState_MinMipLevel, GetSamplerStateHook<13>);
REX_HOOK(D3DDevice_GetSamplerState_TrilinearThreshold, GetSamplerStateHook<14>);
REX_HOOK(D3DDevice_GetSamplerState_AnisotropyBias, GetSamplerStateHook<15>);
REX_HOOK(D3DDevice_GetSamplerState_HGradientExpBias, GetSamplerStateHook<16>);
REX_HOOK(D3DDevice_GetSamplerState_VGradientExpBias, GetSamplerStateHook<17>);
REX_HOOK(D3DDevice_GetSamplerState_WhiteBorderColorW, GetSamplerStateHook<18>);
REX_HOOK(D3DDevice_GetSamplerState_PointBorderEnable, GetSamplerStateHook<19>);
