/**
 * @file    gpu/render/format.h
 * @brief   Xenos render target / texture / render state values to plume.
 */
#pragma once

#include <cstdint>

#include <plume_render_interface.h>

#include <rex/types.h>

namespace renut::gpu {

// Render state values as the Xbox D3D API takes them. The game's
// D3DDevice_SetRenderState_* setters write them into the control registers
// unchanged: SrcBlend / DestBlend (0x82230090 / 0x822307B8, RB_BLENDCONTROL
// bits 0-4 / 8-12), BlendOp (0x82226380, bits 5-7), ZFunc (0x821FB7B0,
// RB_DEPTHCONTROL bits 4-6), StencilFail (0x8264C268, bits 11-13).
enum class D3DBlend : u32 {
  Zero = 0,
  One = 1,
  SrcColor = 4,
  InvSrcColor = 5,
  SrcAlpha = 6,
  InvSrcAlpha = 7,
  DestColor = 8,
  InvDestColor = 9,
  DestAlpha = 10,
  InvDestAlpha = 11,
  BlendFactor = 12,
  InvBlendFactor = 13,
  ConstantAlpha = 14,
  InvConstantAlpha = 15,
  SrcAlphaSat = 16,
};

enum class D3DBlendOp : u32 {
  Add = 0,
  Subtract = 1,
  Min = 2,
  Max = 3,
  RevSubtract = 4,
};

enum class D3DCmpFunc : u32 {
  Never = 0,
  Less = 1,
  Equal = 2,
  LessEqual = 3,
  Greater = 4,
  NotEqual = 5,
  GreaterEqual = 6,
  Always = 7,
};

enum class D3DStencilOp : u32 {
  Keep = 0,
  Zero = 1,
  Replace = 2,
  IncrSat = 3,
  DecrSat = 4,
  Invert = 5,
  Incr = 6,
  Decr = 7,
};

// The GPU texture formats (the low 6 bits of a D3DFORMAT, fetch constant
// dword 1 bits 0-5), by their XDK names. The game's per-format table at
// 0x820052F8 (bits per pixel in its low byte) agrees with the sizes.
enum class GpuTextureFormat : u32 {
  Fmt8 = 2,
  Fmt1_5_5_5 = 3,
  Fmt5_6_5 = 4,
  Fmt6_5_5 = 5,
  Fmt8_8_8_8 = 6,
  Fmt2_10_10_10 = 7,
  Fmt8_A = 8,
  Fmt8_B = 9,
  Fmt8_8 = 10,
  Fmt16_16_Edram = 13,
  Fmt8_8_8_8_A = 14,
  Fmt4_4_4_4 = 15,
  Fmt10_11_11 = 16,
  Fmt11_11_10 = 17,
  FmtDXT1 = 18,
  FmtDXT2_3 = 19,
  FmtDXT4_5 = 20,
  Fmt16_16_16_16_Edram = 21,
  Fmt24_8 = 22,
  Fmt24_8_Float = 23,
  Fmt16 = 24,
  Fmt16_16 = 25,
  Fmt16_16_16_16 = 26,
  Fmt16_Expand = 27,
  Fmt16_16_Expand = 28,
  Fmt16_16_16_16_Expand = 29,
  Fmt16_Float = 30,
  Fmt16_16_Float = 31,
  Fmt16_16_16_16_Float = 32,
  Fmt32 = 33,
  Fmt32_32 = 34,
  Fmt32_32_32_32 = 35,
  Fmt32_Float = 36,
  Fmt32_32_Float = 37,
  Fmt32_32_32_32_Float = 38,
  FmtDXN = 49,
  Fmt8_8_8_8_As16_16_16_16 = 50,
  FmtDXT1_As16_16_16_16 = 51,
  FmtDXT2_3_As16_16_16_16 = 52,
  FmtDXT4_5_As16_16_16_16 = 53,
  Fmt2_10_10_10_As16_16_16_16 = 54,
  Fmt10_11_11_As16_16_16_16 = 55,
  Fmt11_11_10_As16_16_16_16 = 56,
  Fmt32_32_32_Float = 57,
  FmtDXT3A = 58,
  FmtDXT5A = 59,
  FmtCTX1 = 60,
};

// The render target format in a surface's color info (bits 16-19), as the
// game's SetSurfaceHeader (0x82655A50) picks it: the high nibble's low bits
// of its per-format table entry, 1 for an 8_8_8_8 format whose X, Y and Z
// are gamma.
enum class GpuColorFormat : u32 {
  Fmt8_8_8_8 = 0,
  Fmt8_8_8_8_Gamma = 1,
  Fmt2_10_10_10 = 2,
  Fmt2_10_10_10_Float = 3,
  Fmt16_16 = 4,
  Fmt16_16_16_16 = 5,
  Fmt16_16_Float = 6,
  Fmt16_16_16_16_Float = 7,
  Fmt32_Float = 14,
  Fmt32_32_Float = 15,
};

plume::RenderFormat ColorTargetFormat(GpuColorFormat format);
plume::RenderFormat DepthTargetFormat();

plume::RenderBlend ConvertBlendFactor(u32 factor);
plume::RenderBlendOperation ConvertBlendOp(u32 op);
plume::RenderComparisonFunction ConvertCompareFunction(u32 func);
plume::RenderStencilOp ConvertStencilOp(u32 op);

// How a Xenos texture format's blocks are stored on the host.
enum class TexelConversion : u8 {
  Unsupported,
  Copy,          // endian swap only; the host format reads the swapped block as is
  R5G6B5,        // 16-bit packed formats expanded to RGBA8
  R5G5B5A1,
  R4G4B4A4,
  R10G10B10A2,   // expanded to RGBA16 UNORM (plume has no 10:10:10:2 format)
  R11G11B10,     // Fmt11_11_10: R 11, G 11, B 10 bits, UNORM, to RGBA16 UNORM
  R10G11B11,     // Fmt10_11_11: R 10, G 11, B 11 bits, UNORM, to RGBA16 UNORM
  Depth24,       // Fmt24_8 / Fmt24_8_Float: depth in the high 24 bits, to R32_FLOAT
  CTX1,          // 4x4 blocks of two-component (normal map) data, decoded to R8G8
};

struct TextureFormatInfo {
  plume::RenderFormat host = plume::RenderFormat::UNKNOWN;
  TexelConversion conversion = TexelConversion::Unsupported;
  u32 block_size = 1;        // texels per block edge (4 for BC)
  u32 guest_block_bytes = 0;
  u32 host_block_bytes = 0;
  bool compressed = false;
  // Guest blocks (block_size^2 texels) decoded to texels of an uncompressed
  // host format: host_block_bytes covers the whole decoded block, and a row of
  // blocks is block_size rows of texels (see DecodeBlockRow).
  bool decoded_blocks = false;
};

// 'is_signed' picks the SNORM host format where one exists (all components
// signed in the fetch constant).
TextureFormatInfo GetTextureFormatInfo(GpuTextureFormat format, bool is_signed);

// Converts 'count' guest blocks (already byte-swapped per the fetch endian)
// into host blocks.
void ConvertTexels(TexelConversion conversion, const uint8_t *src, uint8_t *dst, uint32_t count);

// Decodes a row of 'count' guest blocks (already byte-swapped) of a
// decoded_blocks format into block_size rows of host texels, 'dst_row_pitch'
// bytes apart.
void DecodeBlockRow(TexelConversion conversion, const uint8_t *src, uint8_t *dst, uint32_t count,
                    uint32_t dst_row_pitch);

// Byte swap for Xenos endian modes (0 none, 1 8in16, 2 8in32, 3 16in32) over
// 'bytes' bytes (a multiple of 4 for the 32-bit modes).
void SwapEndian(u32 endian, const void *src, void *dst, size_t bytes);

} // namespace renut::gpu
