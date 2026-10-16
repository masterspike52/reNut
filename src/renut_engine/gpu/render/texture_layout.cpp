/**
 * @file    gpu/render/texture_layout.cpp
 */
#include "renut_engine/gpu/render/texture_layout.h"

#include <algorithm>
#include <bit>

namespace renut::gpu {

namespace {

// Bits per pixel of each GPU texture format: the low byte of the game's
// per-format table at 0x820052F8.
constexpr u8 kBitsPerPixel[64] = {
    1,  1,  8,  16, 16, 16, 32, 32, 8,  8,  16, 16, 16, 32, 32, 16,  // 0-15
    32, 32, 4,  8,  8,  64, 32, 32, 16, 32, 64, 16, 32, 64, 16, 32,  // 16-31
    64, 32, 64, 128, 32, 64, 128, 8, 16, 16, 32, 8, 8, 16, 16, 16,  // 32-47
    32, 8,  32, 4,  8,  8,  32, 32, 32, 96, 4,  4,  4,  4,  32, 32,  // 48-63
};

// 32 - cntlzw(v - 1), as the game's layout code takes the log2 of a size
// rounded up to a power of two.
u32 CeilLog2(u32 v) {
  return 32 - u32(std::countl_zero(v - 1));
}

u32 Align4K(u32 bytes) {
  return (bytes + 4095) & ~4095u;
}

// (v + a - 1) & ~(a - 1), the game's alignment (a is a power of two for
// every format it lays out).
u32 AlignMask(u32 v, u32 a) {
  return (v + a - 1) & ~(a - 1);
}

// AlignTextureDimensions (0x82207C28): pads a level's size to what its
// storage takes and returns its bytes. Rows are 32 blocks at least (for a
// linear texture as many as fill 256 bytes, if more); rows of blocks come in
// 32s but for 1D textures, depth slices in 4s for 3D textures; a 2D slice is
// 4 KiB aligned, a 3D level as a whole.
u32 AlignTextureDimensions(u32 &width, u32 &height, u32 &depth, u32 bpp, GpuTextureFormat format,
                           TextureDimension dimension, bool tiled) {
  u32 block_width, block_height;
  FormatBlockSize(format, block_width, block_height);
  u32 row_blocks = 32;
  if (!tiled) {
    const u32 block_bytes = (block_width * block_height * bpp) >> 3;
    if (block_bytes && 256 / block_bytes >= 32)
      row_blocks = 256 / block_bytes;
  }
  const u32 column_blocks = dimension == TextureDimension::Tex1D ? 1 : 32;
  width = AlignMask(width, block_width * row_blocks);
  height = AlignMask(height, block_height * column_blocks);
  depth = AlignMask(depth, dimension == TextureDimension::Tex3D ? 4 : 1);
  const u32 plane = ((width * bpp) >> 3) * height;
  return dimension == TextureDimension::Tex3D ? Align4K(depth * plane) : Align4K(plane) * depth;
}

// GetMipTailLevelOffsetCoords (0x826555E0): where level 'level' of a packed
// tail starts in the tail level (w x h x d, powers of two). The first three
// levels line up along the tail's shorter side, 16 >> level texels in; the
// rest along the other side, at its size >> (level - 2), and once that is
// under 4, in depth slices 4 apart.
void MipTailCoords(u32 level, u32 width, u32 height, u32 depth, GpuTextureFormat format,
                   u32 &x_blocks, u32 &y_blocks, u32 &z) {
  const u32 log2_width = CeilLog2(width);
  const u32 log2_height = CeilLog2(height);
  const u32 log2_depth = CeilLog2(depth);
  const u32 size[2] = {1u << log2_width, 1u << log2_height};
  u32 texel[2] = {0, 0};
  z = 0;
  const u32 shorter = log2_height < log2_width ? 1 : 0;
  if (level < 3) {
    texel[shorter] = 16 >> level;
  } else {
    const u32 other = shorter ^ 1;
    texel[other] = size[other] >> (level - 2);
    if (texel[other] < 4)
      z = 4 * u32(std::max(int32_t(log2_depth) - int32_t(level), 1));
  }
  u32 block_width, block_height;
  FormatBlockSize(format, block_width, block_height);
  x_blocks = texel[0] / block_width;
  y_blocks = texel[1] / block_height;
}

// Array slices / cube faces each mip level stores (the level stride in the
// subresource layout): cube maps their 6, stacked arrays their slices in 4s.
u32 StoredSlices(const TextureFetch &fetch) {
  if (fetch.Dimension() != TextureDimension::Cube && !fetch.Stacked())
    return 1;
  const u32 slices = (fetch.dword[2] >> 26) + 1;
  return fetch.Stacked() ? AlignMask(slices, 4) : slices;
}

u32 LevelBytes(const TextureLevelLayout &level, TextureDimension dimension, u32 bpp) {
  const u32 row_bytes = (level.pitch_texels * bpp) >> 3;
  return dimension == TextureDimension::Tex3D ? Align4K(row_bytes * level.height * level.depth)
                                              : Align4K(row_bytes * level.height);
}

} // namespace

TextureFetch TextureFetch::FromGuest(const d3d::TextureFetchConstant &guest) {
  TextureFetch fetch;
  for (u32 i = 0; i < 6; ++i)
    fetch.dword[i] = guest.dword[i];
  return fetch;
}

TextureFetch TextureFetch::FromHeader(const d3d::TextureFetchConstant &header) {
  TextureFetch fetch = FromGuest(header);
  if (fetch.BasePage())
    fetch.dword[1] = (fetch.dword[1] & 0xFFFu) | (d3d::CpuToGpuAddress(fetch.dword[1] & ~0xFFFu) & ~0xFFFu);
  if (fetch.MipPage())
    fetch.dword[5] = (fetch.dword[5] & 0xFFFu) | (d3d::CpuToGpuAddress(fetch.dword[5] & ~0xFFFu) & ~0xFFFu);
  return fetch;
}

void TextureFetch::Size(u32 &width, u32 &height, u32 &depth) const {
  const u32 add = (Border() ? 2u : 0u) + 1;
  const u32 size = dword[2];
  switch (Dimension()) {
  case TextureDimension::Tex1D:
    width = (size & 0xFFFFFF) + add;
    height = 1;
    depth = 1;
    break;
  case TextureDimension::Tex3D:
    width = (size & 0x7FF) + add;
    height = ((size >> 11) & 0x7FF) + add;
    depth = (size >> 22) + add;
    break;
  default:
    width = (size & 0x1FFF) + add;
    height = ((size >> 13) & 0x1FFF) + add;
    depth = (size >> 26) + 1;
    break;
  }
}

u32 FormatBitsPerPixel(GpuTextureFormat format) {
  return kBitsPerPixel[u32(format) & 0x3F];
}

// BlockSizeOfGpuFormat (0x82207B90).
void FormatBlockSize(GpuTextureFormat format, u32 &block_width, u32 &block_height) {
  const u32 f = u32(format) & 0x3F;
  block_width = block_height = 1;
  if (f == 11 || f == 12 || f == 40) {
    block_width = 2;
  } else if (f == 39 || f == 44) {
    block_width = 4;
  } else if ((f >= 18 && f <= 20) || f == 49 || (f >= 51 && f <= 53) || (f >= 58 && f <= 61)) {
    block_width = block_height = 4;
  }
}

// The subresource layout (0x82207680) for slice 0 of 'level'. The base level
// has the header's size and pitch at the base address. Mip levels follow one
// another from the mip address, each padded from the base size rounded up
// to powers of two, every slice of one before the next. With packed mips,
// the first level 16 texels or less on a side holds itself and every
// smaller level (MipTailCoords); when that is the base level, the whole
// texture is at the base address.
TextureLevelLayout GetTextureLevelLayout(const TextureFetch &fetch, u32 level) {
  const GpuTextureFormat format = fetch.Format();
  const u32 bpp = FormatBitsPerPixel(format);
  const TextureDimension dimension = fetch.Dimension();
  const bool tiled = fetch.Tiled();
  const u32 border = fetch.Border() ? 1 : 0;
  u32 width, height, depth;
  fetch.Size(width, height, depth);
  const u32 pow2_width = 1u << (border + CeilLog2(width - 2 * border));
  const u32 pow2_height = 1u << (border + CeilLog2(height - 2 * border));
  const u32 pow2_depth =
      dimension == TextureDimension::Tex3D ? 1u << (border + CeilLog2(depth - 2 * border)) : 1u;
  const bool packed_base = fetch.PackedMips() && (pow2_width <= 16 || pow2_height <= 16);

  TextureLevelLayout out;
  out.page = level == 0 || packed_base ? fetch.BasePage() : fetch.MipPage();
  if (level == 0 && !packed_base) {
    u32 w = width, h = height, d = dimension == TextureDimension::Tex3D ? depth : 1;
    AlignTextureDimensions(w, h, d, bpp, format, dimension, tiled);
    // A linear 2D texture of a single level is padded to whole blocks only.
    if (!tiled && !fetch.Stacked() && !fetch.PackedMips() && dimension == TextureDimension::Tex2D &&
        fetch.MaxMipLevel() == 0 && !border) {
      u32 block_width, block_height;
      FormatBlockSize(format, block_width, block_height);
      h = AlignMask(height, block_height);
    }
    out.pitch_texels = dimension == TextureDimension::Tex1D ? w : fetch.PitchTexels();
    out.height = h;
    out.depth = d;
    out.slice_bytes = Align4K(((out.pitch_texels * bpp) >> 3) * h);
    return out;
  }

  const u32 slices = StoredSlices(fetch);
  u32 log2_width = u32(std::countr_zero(pow2_width));
  u32 log2_height = u32(std::countr_zero(pow2_height));
  u32 log2_depth = u32(std::countr_zero(pow2_depth));
  int32_t remaining = int32_t(level);
  if (packed_base) {
    ++log2_width, ++log2_height, ++log2_depth;
  } else {
    --remaining;
  }
  u32 offset = 0;
  while (remaining >= 0) {
    log2_width -= log2_width ? 1 : 0;
    log2_height -= log2_height ? 1 : 0;
    log2_depth -= log2_depth ? 1 : 0;
    u32 w = 1u << log2_width, h = 1u << log2_height, d = 1u << log2_depth;
    const u32 bytes = AlignTextureDimensions(w, h, d, bpp, format, dimension, tiled);
    out.pitch_texels = w;
    out.height = h;
    out.depth = dimension == TextureDimension::Tex3D ? d : 1;
    if (fetch.PackedMips() && ((1u << log2_width) <= 16 || (1u << log2_height) <= 16)) {
      out.in_tail = true;
      MipTailCoords(u32(remaining), 1u << log2_width, 1u << log2_height, 1u << log2_depth, format,
                    out.tail_x_blocks, out.tail_y_blocks, out.tail_z);
      break;
    }
    if (remaining > 0)
      offset += bytes * slices;
    --remaining;
  }
  out.offset = offset;
  out.slice_bytes = Align4K(((out.pitch_texels * bpp) >> 3) * out.height);
  return out;
}

// FindTextureSize (0x82287E60): the base level with all its slices, and the
// mip levels up to (and with) the packed tail.
void GetTextureDataSizes(const TextureFetch &fetch, u32 &base_bytes, u32 &mip_bytes) {
  const u32 bpp = FormatBitsPerPixel(fetch.Format());
  const TextureDimension dimension = fetch.Dimension();
  const u32 slices = StoredSlices(fetch);
  const TextureLevelLayout base = GetTextureLevelLayout(fetch, 0);
  mip_bytes = 0;
  if (base.in_tail) {
    base_bytes = LevelBytes(base, dimension, bpp) * slices;
    return;
  }
  u32 width, height, depth;
  fetch.Size(width, height, depth);
  const bool layered = dimension == TextureDimension::Cube || fetch.Stacked();
  base_bytes = LevelBytes(base, dimension, bpp) * (layered ? depth : 1);
  for (u32 level = 1; level <= fetch.MaxMipLevel(); ++level) {
    const TextureLevelLayout mip = GetTextureLevelLayout(fetch, level);
    mip_bytes = mip.offset + LevelBytes(mip, dimension, bpp) * slices;
    if (mip.in_tail)
      break;
  }
}

// XGRAPHICS::UntileSurface (0x8280A050): tiles of 32 x 32 blocks, 'pitch'
// rounded up to whole tiles, the blocks inside a tile interleaved.
u32 TiledOffset2D(u32 x, u32 y, u32 pitch_blocks, u32 log2_block_bytes) {
  const u32 tiles_per_row = AlignMask(pitch_blocks, 32) >> 5;
  const u32 shift = log2_block_bytes + 6;
  const u32 tile = (((x >> 5) + (y >> 5) * tiles_per_row) << shift) & 0x1FFFFFFFu;
  const u32 micro = (((x & 7) + ((y << 2) & 0x18)) << shift) >> 6;
  const u32 a = 2 * (tile + (micro & ~0xFu)) + (micro & 0xF) + (((y >> 3) & 1) << shift) +
                ((y & 1) << 4);
  const u32 b = ((2 * ((x >> 3) + ((y >> 2) & 2))) & 6) + ((y >> 4) & 1);
  return ((b & 1) << 11) + (((a >> 6) & 7) << 8) + ((b & ~1u) << 5) + ((a & ~0x1FFu) << 3) +
         (a & 0x3F);
}

// XGAddress3DTiledOffset (0x8280D318), in bytes rather than elements.
u32 TiledOffset3D(u32 x, u32 y, u32 z, u32 width_blocks, u32 height_blocks,
                  u32 log2_block_bytes) {
  const uint64_t shift = log2_block_bytes;
  const uint64_t tiles_high = ((height_blocks + 31) >> 4) & ~1u;
  const uint64_t tiles_wide = (width_blocks + 31) >> 5;
  const uint64_t tile = ((z >> 2) * tiles_high + (y >> 4)) * tiles_wide + (x >> 5);
  const uint64_t micro = uint64_t(((y << 2) & 0x18) + (x & 7)) << shift;
  const uint64_t t = (tile << (shift + 8)) + 2 * (((y & 1) << 3) + (micro & ~uint64_t(0xF))) +
                     (uint64_t(z & 3) << (shift + 6)) + (micro & 0xF);
  const uint64_t bank = ((y >> 3) + (z >> 2)) & 1;
  const uint64_t lane = ((uint64_t(x >> 3) << 6) - (bank << 7)) & 0xC0;
  return u32(4 * (2 * ((t & ~uint64_t(0x1FF)) + (bank << 8)) + (t & 0x1C0)) + lane + (t & 0x3F));
}

} // namespace renut::gpu
