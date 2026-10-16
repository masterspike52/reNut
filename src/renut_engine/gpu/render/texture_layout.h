/**
 * @file    gpu/render/texture_layout.h
 * @brief   Texture fetch constants and where a texture's data lies in guest
 *          memory, as N&B's own D3D and XGraphics code lays them out.
 *
 * Every field and rule here comes from the game's statically linked
 * libraries (addresses in the N&B database):
 * - The fetch constant: D3D::SetTextureHeader (0x8228E990) writes the data
 *   fields, D3DDevice_SetTexture (0x8222F410) merges them with the sampler
 *   state the D3DDevice_SetSamplerState_* setters keep.
 * - Sizes: GetGpuTextureDimensions (0x82655540); per-format bits per pixel
 *   (table 0x820052F8) and block size (BlockSizeOfGpuFormat, 0x82207B90);
 *   alignment (AlignTextureDimensions, 0x82207C28); total sizes
 *   (FindTextureSize, 0x82287E60).
 * - Level placement: the subresource layout D3D and XGGetTextureLayout use
 *   (0x82207680) and the packed mip tail (GetMipTailLevelOffsetCoords,
 *   0x826555E0).
 * - Tiling: XGRAPHICS::UntileSurface (0x8280A050) and
 *   XGAddress3DTiledOffset (0x8280D318).
 */
#pragma once

#include <cstdint>

#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/render/format.h"

namespace renut::gpu {

// Fetch constant dword 5 bits 9-10.
enum class TextureDimension : u32 {
  Tex1D = 0,
  Tex2D = 1,  // also stacked 2D arrays (dword 1 bit 10)
  Tex3D = 2,
  Cube = 3,
};

// D3DTADDRESS, which D3DDevice_SetSamplerState_AddressU/V/W write as is.
enum class TextureAddress : u32 {
  Wrap = 0,
  Mirror = 1,
  Clamp = 2,
  MirrorOnce = 3,
  BorderHalf = 4,
  MirrorOnceBorderHalf = 5,
  Border = 6,
  MirrorOnceBorder = 7,
};

// Filter fields as the MagFilter / MinFilter / MipFilter setters write them
// (D3DTEXF_POINT 0, D3DTEXF_LINEAR 1; a mip filter of 2 samples the base
// level only).
enum class TextureFilter : u32 {
  Point = 0,
  Linear = 1,
  BaseMap = 2,
};

// Per-component sign (dword 0, two bits each from bit 2).
enum class TextureSign : u32 {
  Unsigned = 0,
  Signed = 1,
  Biased = 2,
  Gamma = 3,
};

// A texture fetch constant, native-endian. Bit layout as the game's D3D
// writes it (see the file comment):
//   0: type 0-1 (2 = texture), signs 2-9, clamp U/V/W 10-12/13-15/16-18,
//      pitch / 32 texels 22-30, tiled 31
//   1: format 0-5, endian 6-7, stacked 10, base address 12-31
//   2: size (see Size)
//   3: number format 0, swizzle 1-12, exponent adjust 13-18, mag / min / mip
//      filter 19-20 / 21-22 / 23-24, anisotropy 25-27, border 31
//   4: volume mag / min filter 0 / 1, mip levels min 2-5 / max 6-9, LOD
//      bias 12-21 (signed, 1/32 steps)
//   5: border color 0-1, dimension 9-10, packed mips 11, mip address 12-31
struct TextureFetch {
  u32 dword[6] = {};

  static TextureFetch FromGuest(const d3d::TextureFetchConstant &guest);
  // The constant D3DDevice_SetTexture builds from a texture header: its CPU
  // base and mip addresses become GPU addresses.
  static TextureFetch FromHeader(const d3d::TextureFetchConstant &header);

  u32 Type() const { return dword[0] & 3; }
  bool IsTexture() const { return Type() == 2; }
  TextureSign Sign(u32 component) const {
    return TextureSign((dword[0] >> (2 + 2 * component)) & 3);
  }
  TextureAddress Address(u32 axis) const {
    return TextureAddress((dword[0] >> (10 + 3 * axis)) & 7);
  }
  u32 PitchTexels() const { return ((dword[0] >> 22) & 0x1FF) << 5; }
  bool Tiled() const { return (dword[0] >> 31) != 0; }

  GpuTextureFormat Format() const { return GpuTextureFormat(dword[1] & 0x3F); }
  u32 Endian() const { return (dword[1] >> 6) & 3; }
  bool Stacked() const { return (dword[1] >> 10) & 1; }
  u32 BasePage() const { return dword[1] >> 12; }

  u32 Swizzle() const { return (dword[3] >> 1) & 0xFFF; }
  TextureFilter MagFilter() const { return TextureFilter((dword[3] >> 19) & 3); }
  TextureFilter MinFilter() const { return TextureFilter((dword[3] >> 21) & 3); }
  TextureFilter MipFilter() const { return TextureFilter((dword[3] >> 23) & 3); }
  u32 Anisotropy() const { return (dword[3] >> 25) & 7; }  // ratio 1 << (n - 1) from 2
  bool Border() const { return (dword[3] >> 31) != 0; }

  // MagFilterZ / MinFilterZ: the filter between depth slices (layers).
  TextureFilter VolumeMagFilter() const { return TextureFilter(dword[4] & 1); }
  TextureFilter VolumeMinFilter() const { return TextureFilter((dword[4] >> 1) & 1); }
  u32 MinMipLevel() const { return (dword[4] >> 2) & 0xF; }
  u32 MaxMipLevel() const { return (dword[4] >> 6) & 0xF; }
  int32_t LodBias() const { return int32_t(dword[4] << 10) >> 22; }

  u32 BorderColor() const { return dword[5] & 3; }  // 0 transparent black, 1 white
  TextureDimension Dimension() const { return TextureDimension((dword[5] >> 9) & 3); }
  bool PackedMips() const { return (dword[5] >> 11) & 1; }
  u32 MipPage() const { return dword[5] >> 12; }

  // Width, height and depth including the border (GetGpuTextureDimensions);
  // depth is the slice count of 2D arrays and cube maps.
  void Size(u32 &width, u32 &height, u32 &depth) const;
};

// Bits per pixel and block size of a GPU texture format.
u32 FormatBitsPerPixel(GpuTextureFormat format);
void FormatBlockSize(GpuTextureFormat format, u32 &block_width, u32 &block_height);

// Where one mip level of a texture lies (its first array slice or cube face).
struct TextureLevelLayout {
  u32 page = 0;          // 4 KiB page of the base or mip data it is in
  u32 offset = 0;        // bytes from there to the level (or its packed tail)
  // The storage's aligned size in texels: rows are pitch_texels wide (the
  // tiling pitch too), height rows per 2D slice, depth slices for 3D.
  u32 pitch_texels = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 slice_bytes = 0;   // stride between array slices / cube faces
  // In a packed tail: the storage above is the tail level's, and the level
  // starts at these blocks of it.
  bool in_tail = false;
  u32 tail_x_blocks = 0, tail_y_blocks = 0, tail_z = 0;
};

TextureLevelLayout GetTextureLevelLayout(const TextureFetch &fetch, u32 level);

// Bytes of guest memory the base level (all slices) and the mip levels take.
void GetTextureDataSizes(const TextureFetch &fetch, u32 &base_bytes, u32 &mip_bytes);

// Byte offset of block (x, y) in a tiled 2D surface 'pitch_blocks' wide, and
// of block (x, y, z) in a tiled volume of 'width_blocks' x 'height_blocks'.
u32 TiledOffset2D(u32 x, u32 y, u32 pitch_blocks, u32 log2_block_bytes);
u32 TiledOffset3D(u32 x, u32 y, u32 z, u32 width_blocks, u32 height_blocks,
                  u32 log2_block_bytes);

// Blocks of a tiled row that lie together from a multiple of this (8, or 16
// bytes' worth): UntileSurface copies such runs at once.
constexpr u32 TiledRunBlocks(u32 log2_block_bytes) {
  return log2_block_bytes <= 1 ? 8u : 16u >> log2_block_bytes;
}

} // namespace renut::gpu
