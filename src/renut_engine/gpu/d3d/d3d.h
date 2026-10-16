/**
 * @file    gpu/d3d/d3d.h
 * @brief   Guest D3D (Xbox 360 XDK) layouts as used by Banjo-Kazooie: Nuts & Bolts.
 *
 * Offsets come from N&B's own default.xex IDB (typed D3DDevice / D3DConstants /
 * GPU_*PACKET and resource structs); every one is static_asserted. Only fields
 * the renderer reads or maintains are named.
 */
#pragma once

#include <cstddef>

#include <rex/types.h>

namespace renut::gpu::d3d {

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

// D3DRESOURCETYPE as N&B's XDK defines it (differs from other XDK versions).
// Common's low 4 bits hold the base type; the 16+ values are what
// D3DResource_GetType reports for texture variants.
enum class ResourceType : u32 {
  VertexBuffer = 1,
  IndexBuffer = 2,
  Texture = 3,
  Surface = 4,
  VertexDeclaration = 5,
  VertexShader = 6,
  PixelShader = 7,
  ConstantBuffer = 8,
  CommandBuffer = 9,
  AsyncCommandBufferCall = 10,
  PerfCounterBatch = 11,
  OcclusionQueryBatch = 12,
  Volume = 16,
  VolumeTexture = 17,
  CubeTexture = 18,
  ArrayTexture = 19,
  LineTexture = 20,
};
constexpr u32 kCommonTypeMask = 0xF;

struct Resource {
  be_u32 Common;          // +0x00
  be_u32 ReferenceCount;  // +0x04
  be_u32 Fence;           // +0x08
  be_u32 ReadFence;       // +0x0C
  be_u32 Identifier;      // +0x10
  be_u32 BaseFlush;       // +0x14
};
static_assert(sizeof(Resource) == 24);

struct TextureFetchConstant {
  be_u32 dword[6];
};
static_assert(sizeof(TextureFetchConstant) == 24);

// A texture header holds the CPU addresses of its base and mip data; the
// GPU address is what D3DDevice_SetTexture (0x8222F410) writes into the
// device's fetch constant: the low 29 bits, plus 4 KB for the 0xE0000000
// (4 KB page) physical view, which starts at physical 0x1000.
constexpr u32 CpuToGpuAddress(u32 cpu_address) {
  return (cpu_address & 0x1FFFFFFFu) + (((cpu_address >> 20) + 0x200u) & 0x1000u);
}

struct VertexFetchConstant {
  be_u32 dword[2];
};
static_assert(sizeof(VertexFetchConstant) == 8);

// D3DBaseTexture / D3DTexture / D3DCubeTexture / D3DVolumeTexture / D3DLineTexture.
struct BaseTexture {
  Resource resource;            // +0x00
  be_u32 MipFlush;              // +0x18
  TextureFetchConstant Format;  // +0x1C
};
static_assert(sizeof(BaseTexture) == 52);
static_assert(offsetof(BaseTexture, Format) == 0x1C);

struct Surface {
  Resource resource;   // +0x00
  be_u32 word[6];      // +0x18 union (surface/depth info, size, format)
};
static_assert(sizeof(Surface) == 48);

struct VertexBuffer {
  Resource resource;           // +0x00
  VertexFetchConstant Format;  // +0x18
};
static_assert(sizeof(VertexBuffer) == 32);

struct IndexBuffer {
  Resource resource;  // +0x00
  be_u32 Address;     // +0x18
  be_u32 Size;        // +0x1C
};
static_assert(sizeof(IndexBuffer) == 32);

// D3DVertexShader / D3DPixelShader / D3DVertexDeclaration headers.
struct Shader {
  Resource resource;
};
static_assert(sizeof(Shader) == 24);

// The microcode container handed to CreateVertexShader / CreatePixelShader
// (same layout XenosRecomp's shader.h reads).
struct ShaderContainer {
  be_u32 flags;
  be_u32 virtualSize;
  be_u32 physicalSize;
  be_u32 fieldC;
  be_u32 constantTableOffset;
  be_u32 definitionTableOffset;
  be_u32 shaderOffset;
  be_u32 field1C;
  be_u32 field20;
};
static_assert(sizeof(ShaderContainer) == 0x24);

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

constexpr u32 kTextureFetchSlots = 26;
constexpr u32 kVertexFetchConstants = 18;
constexpr u32 kFirstVertexFetchIndex = 78;  // Xenos vfetch index of VertexFetch[0]

// _D3DConstants, the register image D3D keeps for the GPU.
struct Constants {
  TextureFetchConstant TextureFetch[kTextureFetchSlots];  // +0x000
  VertexFetchConstant VertexFetch[kVertexFetchConstants];  // +0x270
  be_f32 VertexShaderF[256][4];                           // +0x300
  be_f32 PixelShaderF[256][4];                            // +0x1300
  be_u32 VertexShaderB[4];                                // +0x2300
  be_u32 PixelShaderB[4];                                 // +0x2310
  be_u32 VertexShaderI[16];                               // +0x2320
  be_u32 PixelShaderI[16];                                // +0x2360
};
static_assert(sizeof(Constants) == 9120);
static_assert(offsetof(Constants, VertexFetch) == 0x270);
static_assert(offsetof(Constants, VertexShaderF) == 0x300);
static_assert(offsetof(Constants, PixelShaderF) == 0x1300);
static_assert(offsetof(Constants, VertexShaderB) == 0x2300);
static_assert(offsetof(Constants, VertexShaderI) == 0x2320);

struct DestinationPacket {
  be_u32 SurfaceInfo;      // +0x00
  be_u32 Color0Info;       // +0x04
  be_u32 DepthInfo;        // +0x08
  be_u32 Color1Info;       // +0x0C
  be_u32 Color2Info;       // +0x10
  be_u32 Color3Info;       // +0x14
  be_u32 CoherDestBase[8]; // +0x18
  be_u32 ScreenScissorTL;  // +0x38
  be_u32 ScreenScissorBR;  // +0x3C
};
static_assert(sizeof(DestinationPacket) == 64);

struct WindowPacket {
  be_u32 WindowOffset;
  be_u32 WindowScissorTL;
  be_u32 WindowScissorBR;
};
static_assert(sizeof(WindowPacket) == 12);

struct ValuesPacket {
  be_u32 MaxVtxIndx;            // +0x00
  be_u32 MinVtxIndx;            // +0x04
  be_u32 IndxOffset;            // +0x08
  be_u32 MultiPrimIbResetIndx;  // +0x0C
  be_u32 ColorMask;             // +0x10
  be_f32 BlendRed;              // +0x14
  be_f32 BlendGreen;            // +0x18
  be_f32 BlendBlue;             // +0x1C
  be_f32 BlendAlpha;            // +0x20
  be_u32 Unused[3];             // +0x24
  be_u32 StencilRefMaskBF;      // +0x30
  be_u32 StencilRefMask;        // +0x34
  be_f32 AlphaRef;              // +0x38
  be_f32 VportXScale;           // +0x3C
  be_f32 VportXOffset;          // +0x40
  be_f32 VportYScale;           // +0x44
  be_f32 VportYOffset;          // +0x48
  be_f32 VportZScale;           // +0x4C
  be_f32 VportZOffset;          // +0x50
};
static_assert(sizeof(ValuesPacket) == 84);

struct ProgramPacket {
  be_u32 ProgramControl;
  be_u32 ContextMisc;
  be_u32 InterpolatorControl;
  be_u32 Wrapping0;
  be_u32 Wrapping1;
};
static_assert(sizeof(ProgramPacket) == 20);

struct ControlPacket {
  be_u32 DepthControl;      // +0x00
  be_u32 BlendControl0;     // +0x04
  be_u32 ColorControl;      // +0x08
  be_u32 HiControl;         // +0x0C
  be_u32 ClipControl;       // +0x10
  be_u32 ModeControl;       // +0x14
  be_u32 VteControl;        // +0x18
  be_u32 Unused;            // +0x1C
  be_u32 EdramModeControl;  // +0x20
  be_u32 BlendControl1;     // +0x24
  be_u32 BlendControl2;     // +0x28
  be_u32 BlendControl3;     // +0x2C
};
static_assert(sizeof(ControlPacket) == 48);

// Public D3DDevice (0x2B00 bytes in N&B's XDK) followed by the CDevice-private
// tail of the 0x5E00-byte allocation Direct3D_CreateDevice makes.
constexpr u32 kDeviceAllocSize = 0x5E00;
constexpr u32 kDeviceAlignment = 0x80;

struct Device {
  be_u64 m_Pending[5];                // +0x0000  D3DTAGCOLLECTION dirty masks
  be_u64 m_Predicated_PendingMask2;   // +0x0028
  be_u32 m_pRing;                     // +0x0030
  be_u32 m_pRingLimit;                // +0x0034
  be_u32 m_pRingGuarantee;            // +0x0038
  be_u32 m_ReferenceCount;            // +0x003C
  be_u32 m_SetRenderStateCall[101];   // +0x0040
  be_u32 m_SetSamplerStateCall[20];   // +0x01D4
  be_u32 m_GetRenderStateCall[101];   // +0x0224
  be_u32 m_GetSamplerStateCall[20];   // +0x03B8
  u8 pad_0408[0x480 - 0x408];         // +0x0408
  Constants m_Constants;              // +0x0480
  be_f32 m_ClipPlanes[6][4];          // +0x2820
  DestinationPacket m_DestinationPacket;  // +0x2880
  WindowPacket m_WindowPacket;        // +0x28C0
  ValuesPacket m_ValuesPacket;        // +0x28CC
  ProgramPacket m_ProgramPacket;      // +0x2920
  ControlPacket m_ControlPacket;      // +0x2934
  u8 m_TessellatorPacket[84];         // +0x2964
  u8 m_MiscPacket[152];               // +0x29B8
  u8 m_PointPacket[32];               // +0x2A50
  u8 m_MaxAnisotropy[26];             // +0x2A70
  u8 m_ZFilter[26];                   // +0x2A8A
  u8 pad_2AA4[0x2B00 - 0x2AA4];       // +0x2AA4
  u8 private_[kDeviceAllocSize - 0x2B00];  // +0x2B00  CDevice private state
};
static_assert(sizeof(Device) == kDeviceAllocSize);
static_assert(offsetof(Device, m_pRing) == 0x30);
static_assert(offsetof(Device, m_ReferenceCount) == 0x3C);
static_assert(offsetof(Device, m_SetRenderStateCall) == 0x40);
static_assert(offsetof(Device, m_SetSamplerStateCall) == 0x1D4);
static_assert(offsetof(Device, m_GetRenderStateCall) == 0x224);
static_assert(offsetof(Device, m_GetSamplerStateCall) == 0x3B8);
static_assert(offsetof(Device, m_Constants) == 0x480);
static_assert(offsetof(Device, m_ClipPlanes) == 0x2820);
static_assert(offsetof(Device, m_DestinationPacket) == 0x2880);
static_assert(offsetof(Device, m_WindowPacket) == 0x28C0);
static_assert(offsetof(Device, m_ValuesPacket) == 0x28CC);
static_assert(offsetof(Device, m_ProgramPacket) == 0x2920);
static_assert(offsetof(Device, m_ControlPacket) == 0x2934);
static_assert(offsetof(Device, m_TessellatorPacket) == 0x2964);
static_assert(offsetof(Device, m_MiscPacket) == 0x29B8);
static_assert(offsetof(Device, m_PointPacket) == 0x2A50);
static_assert(offsetof(Device, m_MaxAnisotropy) == 0x2A70);
static_assert(offsetof(Device, m_ZFilter) == 0x2A8A);

// CDevice-private fields Direct3D_CreateDevice initialises (byte offsets into
// the allocation, from its decompilation at 0x8264DF28).
constexpr u32 kDeviceOffsetCreationThreadId = 10888;  // GetCurrentThreadId()
constexpr u32 kDeviceOffsetIsTitleProcess = 10892;    // KeGetCurrentProcessType() == 2
constexpr u32 kDeviceOffsetBehaviorFlags = 23980;     // Direct3D_CreateDevice arg 4

// Bound objects the Set* calls store in the CDevice-private area (guest VAs).
constexpr u32 kDeviceOffsetVertexDeclaration = 0x2E2C;  // SetVertexDeclaration
constexpr u32 kDeviceOffsetIndexBuffer = 0x3094;        // SetIndices
constexpr u32 kDeviceOffsetRenderTargets = 0x3098;      // SetRenderTarget, [4]
constexpr u32 kDeviceOffsetDepthStencil = 0x30A8;       // SetDepthStencilSurface
constexpr u32 kDeviceOffsetStreamSources = 0x30AC;      // SetStreamSource, [16]
constexpr u32 kDeviceOffsetStreamStrides = 0x30F0;      // SetStreamSource, u8[16], stride / 4
constexpr u32 kDeviceOffsetTextures = 0x3100;           // SetTexture, [kTextureFetchSlots]
constexpr u32 kDeviceOffsetPixelShader = 0x3194;        // SetPixelShader
constexpr u32 kDeviceOffsetVertexShader = 0x3198;       // SetVertexShader
// In m_TessellatorPacket, which holds registers 0x2280 on: VGT_HOS_CNTL
// (tessellation mode in bits 0-1: 0 discrete, 1 continuous, 2 adaptive) and
// the float VGT_HOS_MAX_TESS_LEVEL / VGT_HOS_MIN_TESS_LEVEL.
constexpr u32 kDeviceOffsetTessellationControl = 0x2978;
constexpr u32 kDeviceOffsetMaxTessellationLevel = 0x297C;
constexpr u32 kDeviceOffsetMinTessellationLevel = 0x2980;
// Tile predication mask of the draws that follow, ~0 for none
// (D3DDevice_SetPredication 0x8222AA18 and SetCommandBufferPredication
// 0x82658678). Bits 2i and 2i+1 run the commands in tile i's pass of
// predicated tiling (Z and render).
constexpr u32 kDeviceOffsetPredication = 0x31A4;
// D3DCommandBuffer being recorded (BeginCommandBuffer sets, End clears).
constexpr u32 kDeviceOffsetCommandBuffer = 13516;
// The leading part of the device the draw, clear and resolve paths read (up
// to and including kDeviceOffsetPredication); a copy of it stands in for the
// device when a recorded command buffer is replayed.
constexpr u32 kDeviceSnapshotSize = 0x31A8;
static_assert(kDeviceOffsetVertexShader + 4 <= kDeviceSnapshotSize);
static_assert(kDeviceOffsetPredication + 4 <= kDeviceSnapshotSize);
// m_pRing the inline end of BeginVertices / BeginIndexedVertices restores.
constexpr u32 kDeviceOffsetVerticesRingEnd = 0x3484;
// Swap bookkeeping (D3DDevice_Swap 0x82287590).
constexpr u32 kDeviceOffsetSwapCount = 0x40B0;
constexpr u32 kDeviceOffsetFenceTag = 0x2A9C;         // current fence tag
constexpr u32 kDeviceOffsetSwapFenceTag = 0x2AB0;     // fence tag at the last Swap
constexpr u32 kDeviceOffsetPresentFenceTag = 0x3A5C;  // fence tag of the last presented frame
constexpr u32 kDeviceOffsetSwapFlags = 0x2ABD;
constexpr u8 kSwapFlagPending = 0x10;

constexpr u32 kMaxStreams = 16;
constexpr u32 kMaxRenderTargets = 4;

// Stream n's vertex fetch constant is VertexFetch[17 - n] (Xenos index 95 - n).
constexpr u32 StreamFetchIndex(u32 stream) {
  return kVertexFetchConstants - 1 - stream;
}

// Common bits of an index buffer: bit 31 32-bit indices, 29-30 endian mode.
constexpr u32 kIndexBufferCommon32Bit = 0x80000000u;
constexpr u32 IndexBufferEndian(u32 common) {
  return (common >> 29) & 3;
}

// D3DSURFACE after its Resource header (SetSurfaceHeader 0x82655A40).
struct SurfaceDesc {
  be_u32 SurfaceInfo;  // +0x18 RB_SURFACE_INFO: pitch 0-13, msaa 16-17
  be_u32 ColorInfo;    // +0x1C RB_COLOR_INFO / RB_DEPTH_INFO: EDRAM base, format at 16
  be_u32 HiControl;    // +0x20
  be_u32 Size;         // +0x24 (height - 1) << 3 | (width - 1) << 18
  be_u32 Format;       // +0x28 D3DFORMAT
  be_u32 EdramSize;    // +0x2C bytes (5120 per tile)
};
static_assert(sizeof(SurfaceDesc) == 24);

inline u32 SurfaceWidth(u32 size) {
  return ((size >> 18) & 0x3FFF) + 1;
}
inline u32 SurfaceHeight(u32 size) {
  return ((size >> 3) & 0x7FFF) + 1;
}
// D3DFORMAT low 6 bits of the two depth formats (D24S8, D24FS8).
inline bool IsDepthFormat(u32 d3d_format) {
  const u32 f = d3d_format & 0x3F;
  return f == 22 || f == 23;
}

} // namespace renut::gpu::d3d
