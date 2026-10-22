/**
 * @file    gpu/render/textures.cpp
 */
#include "renut_engine/gpu/render/textures.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/graphics_options.h"
#include "renut_engine/gpu/render/parallel.h"
#include "renut_engine/gpu/backend/video.h"

namespace renut::gpu {

namespace {

// Fetch constant bits that describe the texture data (not sampling).
constexpr u32 kDescMask[6] = {
    0xFFC003FFu,  // type, signs, pitch, tiled (not clamp modes)
    0xFFFFFFFFu,  // format, endian, stacked, base address
    0xFFFFFFFFu,  // size
    0x0007FFFFu,  // number format, swizzle, exponent adjust (not filters)
    0x000003FCu,  // mip min / max level
    0xFFFFFE00u,  // dimension, packed mips, mip address (not border color)
};

constexpr uint32_t kMaxTextureSize = 8192;

std::unordered_map<u32, std::unique_ptr<HostTexture>> g_textures;

// Recent g_textures entries by VA: BindTextureLocked looks up every texture
// of every draw. The map's nodes stay put until erased (ReleaseHostTextureLocked
// drops the entry here first).
struct TextureMemo {
  u32 va = 0;
  std::unique_ptr<HostTexture> *slot = nullptr;
};
constexpr u32 kTextureMemoBits = 10;
TextureMemo g_texture_memo[1u << kTextureMemoBits];

TextureMemo &TextureMemoFor(u32 texture_va) {
  return g_texture_memo[(texture_va * 0x9E3779B1u) >> (32 - kTextureMemoBits)];
}

// g_textures[texture_va], through the memo.
std::unique_ptr<HostTexture> &TextureSlot(u32 texture_va) {
  TextureMemo &memo = TextureMemoFor(texture_va);
  if (memo.slot && memo.va == texture_va)
    return *memo.slot;
  auto &slot = g_textures[texture_va];
  memo = {texture_va, &slot};
  return slot;
}
// Depth resolve destinations with guest words, by base address (4 KB pages).
std::unordered_map<u32, HostTexture *> g_depth_words;
// The last resolve destination into each base address (4 KB pages): D3D
// textures are views of memory, and a game may resolve through one header and
// sample the same memory through another.
std::unordered_map<u32, HostTexture *> g_resolve_targets;
std::unordered_map<uint64_t, uint32_t> g_samplers;  // sampler key -> slot
std::vector<std::unique_ptr<plume::RenderSampler>> g_sampler_objects;
uint64_t g_frame = 1;

// BindTextureLocked's answers by texture, device fetch constant and the
// header's own, within a generation: a frame's draws bind the same few
// hundred textures thousands of times, and each bind read the header,
// converted it and looked the texture up in up to three maps (a tenth of the
// render thread, 2026-10-22). A generation ends with the frame and with
// whatever changes what a bind finds: a host texture retired (released,
// replaced, evicted) and any resolve. Two ways to a set, the newer first.
uint64_t g_bind_generation = 1;
struct BindMemo {
  uint64_t generation = 0;
  u32 va = 0;
  u32 device[6] = {};
  u32 header[6] = {};
  HostTexture *texture = nullptr;  // made SHADER_READ again at each hit
  TextureBinding binding;
};
constexpr u32 kBindMemoBits = 11;
BindMemo g_bind_memo[2u << kBindMemoBits];

BindMemo *BindMemoSet(u32 texture_va, const u32 device[6]) {
  uint32_t hash = texture_va * 0x9E3779B1u;
  for (u32 i = 0; i < 6; ++i)
    hash = (hash ^ device[i]) * 0x85EBCA6Bu;
  return &g_bind_memo[(hash >> (32 - kBindMemoBits)) * 2];
}

// Resources D3D::UnlockResource was called on since the last bind that
// looked (NoteTextureUnlocked; any thread, hence the lock of its own).
std::mutex g_unlocked_mutex;
std::vector<u32> g_unlocked;

// The bindless table filling up (past kEvictPressureSlots of its slots), or
// video memory nearing the budget the OS gives the process (past
// kEvictMemoryShare of it): host textures not bound or resolved into for
// kEvictIdleFrames go, checked every kEvictCheckFrames, and are made again
// from guest memory if bound later. Resolve destinations hold what only the
// GPU has and wait kEvictIdleGpuFrames. The section pools' textures normally
// go with their memory (ReleaseHostTexturesInMemoryLocked); this catches
// whatever else piles up: at 4K, resolve destinations of levels played
// earlier (2.2 GB of them after a few levels, 2026-10-15), while past the
// budget the OS moves allocations out to system memory and the frame rate
// sinks until it frees them.
constexpr uint64_t kEvictCheckFrames = 120;
constexpr uint64_t kEvictIdleFrames = 600;
constexpr uint64_t kEvictIdleGpuFrames = 3600;
constexpr uint32_t kEvictPressureSlots = kTextureDescriptorCount * 3 / 4;
constexpr double kEvictMemoryShare = 0.85;

// Linear textures (see BindTextureLocked): how often a lock-driven texture
// is checked for writes that went around an unlock, and a dynamic one for
// having stopped changing; how long after an unlock the content is watched;
// and how many checks in a row must see a change to make a texture dynamic.
constexpr uint64_t kLinearVerifyFrames = 30;
constexpr uint64_t kUnlockWatchFrames = 8;
constexpr uint32_t kDynamicChanges = 2;

uint32_t NullSlot(TextureDimension dimension) {
  switch (dimension) {
  case TextureDimension::Tex3D:
    return kNullTexture3DSlot;
  case TextureDimension::Cube:
    return kNullTextureCubeSlot;
  default:
    return kNullTextureSlot;
  }
}

// Host dimension the shaders sample it as: stacked 2D arrays go through the
// 3D table, like tfetch3D reads them.
TextureDimension HostDimension(const TextureFetch &fetch) {
  if (fetch.Dimension() == TextureDimension::Tex2D && fetch.Stacked())
    return TextureDimension::Tex3D;
  if (fetch.Dimension() == TextureDimension::Tex1D)
    return TextureDimension::Tex2D;
  return fetch.Dimension();
}

// Host layers of a stacked texture: a 2D array view takes two at least.
uint32_t StackedLayers(const HostTexture &t) {
  return std::max(t.depth, 2u);
}

bool AllSigned(const TextureFetch &fetch) {
  for (u32 c = 0; c < 4; ++c) {
    if (fetch.Sign(c) != TextureSign::Signed)
      return false;
  }
  return true;
}

plume::RenderSwizzle ViewSwizzle(uint32_t component) {
  switch (component) {
  case 0:
    return plume::RenderSwizzle::R;
  case 1:
    return plume::RenderSwizzle::G;
  case 2:
    return plume::RenderSwizzle::B;
  case 3:
    return plume::RenderSwizzle::A;
  case 4:
    return plume::RenderSwizzle::ZERO;
  default:
    return plume::RenderSwizzle::ONE;
  }
}

// Host formats with a single component.
bool SingleComponent(plume::RenderFormat format) {
  using F = plume::RenderFormat;
  switch (format) {
  case F::R8_UNORM:
  case F::R8_SNORM:
  case F::R16_UNORM:
  case F::R16_SNORM:
  case F::R16_FLOAT:
  case F::R32_FLOAT:
  case F::R32_UINT:
  case F::BC4_UNORM:
  case F::BC4_SNORM:
    return true;
  default:
    return false;
  }
}

// The view mapping of a fetch constant swizzle: which stored component each
// shader component reads (0-3 XYZW, 4 zero, 5 one), with X and Z exchanged
// for content stored so (swap_red_blue, see Create). A one-component format
// fetches its value in every component: the game's depth restore
// (ps_beb4ddc38d3b2df5) writes the W of a depth resolve fetched with the
// identity swizzle as its depth, where a host view has 1.
plume::RenderComponentMapping SwizzleMapping(u32 swizzle, bool swap_red_blue,
                                             plume::RenderFormat format) {
  const bool single = SingleComponent(format);
  auto component = [&](uint32_t c) {
    uint32_t source = (swizzle >> (c * 3)) & 7;
    if (single && source < 4)
      source = 0;
    else if (swap_red_blue && (source == 0 || source == 2))
      source ^= 2;
    return ViewSwizzle(source);
  };
  return plume::RenderComponentMapping(component(0), component(1), component(2), component(3));
}

// The bindless slot of 't' seen through 'swizzle': its own view's when that is
// the swizzle it was made with, else a view made for it (kept with 't').
uint32_t SlotForSwizzle(HostTexture &t, u32 swizzle) {
  if (swizzle == t.view_swizzle || !t.texture)
    return t.slot;
  for (const HostTexture::SwizzleView &v : t.swizzle_views) {
    if (v.swizzle == swizzle)
      return v.slot;
  }
  plume::RenderTextureViewDesc desc = t.view_desc;
  desc.componentMapping = SwizzleMapping(swizzle, t.view_swap_red_blue, t.host_format);
  HostTexture::SwizzleView v;
  v.swizzle = swizzle;
  v.view = t.texture->createTextureView(desc);
  if (!v.view)
    return t.slot;
  v.slot = Video::AllocateTextureSlotLocked();
  if (v.slot == kInvalidDescriptor) {
    Video::RetireLocked(std::move(v.view));
    return t.slot;
  }
  Video::SetTextureSlotLocked(v.slot, t.texture.get(), v.view.get());
  t.swizzle_views.push_back(std::move(v));
  return t.swizzle_views.back().slot;
}

// Where a texture's guest data is depends only on the fetch constant bits in
// t.desc, so it is worked out once per texture.
void KnowGuestExtent(const TextureFetch &fetch, HostTexture &t) {
  if (!t.sample_known) {
    TextureGuestExtent(fetch, t.sample_pages, t.sample_sizes);
    t.sample_known = true;
  }
}

// The ends SampleHash reads of a region, and the spans between them.
constexpr uint32_t kSampleEndBytes = 256;
constexpr uint32_t kSampleProbes = 8;
constexpr uint32_t kSampleProbeBytes = 64;

// Cheap content fingerprint of a tiled texture: the first and last 256 bytes
// of the base level and of the mip chain, and spans spread through each. The
// game streams textures into memory another one used, with the same header
// (format, size, address): masks that are white at the edges (the hill's
// blend mask after the clearing's ring stencil) have the same ends, so the
// ends alone kept the old texture. Every bound texture is checked each frame
// and its guest memory is mostly out of the cache, so the cost is the lines
// read: eight spans, gathered and hashed at once. (32 spans hashed one at a
// time were a tenth of the render thread.)
uint64_t SampleHash(const TextureFetch &fetch, HostTexture &t) {
  KnowGuestExtent(fetch, t);
  alignas(64) uint8_t gathered[2 * (2 * kSampleEndBytes + kSampleProbes * kSampleProbeBytes)];
  uint32_t at = 0;
  for (u32 i = 0; i < 2; ++i) {
    const uint32_t page = t.sample_pages[i], size = t.sample_sizes[i];
    if (!page || !size)
      continue;
    const auto *data = GuestPhysicalPtr<const uint8_t>(page << 12);
    const uint32_t n = std::min<uint32_t>(size, kSampleEndBytes);
    std::memcpy(gathered + at, data, n);
    std::memcpy(gathered + at + n, data + size - n, n);
    at += 2 * n;
    if (size > 2 * n + kSampleProbeBytes) {
      const uint32_t span = size - 2 * n - kSampleProbeBytes;
      for (uint32_t p = 1; p <= kSampleProbes; ++p) {
        const uint32_t offset = n + uint32_t(uint64_t(span) * p / (kSampleProbes + 1));
        std::memcpy(gathered + at, data + offset, kSampleProbeBytes);
        at += kSampleProbeBytes;
      }
    }
  }
  return at ? XXH3_64bits(gathered, at) : 0;
}

// Fingerprint of every guest byte of a texture, for the CPU-written linear
// ones, whose writes land anywhere: N&B's font cache (FontCache.cpp,
// sub_82B8F848) inflates each new glyph into its cell of a 1920x1920 texture
// between D3DLineTexture_LockRect and D3DArrayTexture_UnlockRect.
uint64_t ContentHash(const TextureFetch &fetch, HostTexture &t) {
  KnowGuestExtent(fetch, t);
  uint64_t hash = 0;
  for (u32 i = 0; i < 2; ++i) {
    const uint32_t page = t.sample_pages[i], size = t.sample_sizes[i];
    if (page && size)
      hash = XXH3_64bits_withSeed(GuestPhysicalPtr<const uint8_t>(page << 12), size, hash);
  }
  return hash;
}

struct LevelSource {
  const uint8_t *base = nullptr;  // start of the level's storage (or its packed tail)
  uint32_t pitch_blocks = 0;      // blocks per row of the storage
  uint32_t height_blocks = 0;     // rows of blocks per depth slice (3D)
  uint32_t x_blocks = 0, y_blocks = 0, z_offset = 0;  // offset inside a packed tail
};

// Untiles/copies one subresource (level, slice) into 'dst' rows of host blocks.
// Tiled rows are copied a run of blocks at a time where the tiling keeps
// them together, as the game's UntileSurface does.
void ReadLevel(const TextureFormatInfo &info, const TextureFetch &fetch, const LevelSource &src,
               uint32_t blocks_w, uint32_t blocks_h, uint32_t depth, uint8_t *dst,
               uint32_t dst_row_pitch, uint32_t dst_slice_pitch) {
  const uint32_t gbpb = info.guest_block_bytes;
  const uint32_t hbpb = info.host_block_bytes;
  const uint32_t bpb_log2 = uint32_t(std::countr_zero(gbpb));
  const u32 endian = fetch.Endian();
  const bool is_3d = fetch.Dimension() == TextureDimension::Tex3D;
  const uint32_t run = is_3d ? 1 : TiledRunBlocks(bpb_log2);

  std::vector<uint8_t> row(size_t(blocks_w) * gbpb);
  for (uint32_t z = 0; z < depth; ++z) {
    for (uint32_t y = 0; y < blocks_h; ++y) {
      const uint32_t sy = y + src.y_blocks;
      const uint32_t sz = z + src.z_offset;
      if (fetch.Tiled()) {
        for (uint32_t x = 0; x < blocks_w;) {
          const uint32_t sx = x + src.x_blocks;
          const uint32_t offset =
              is_3d ? TiledOffset3D(sx, sy, sz, src.pitch_blocks, src.height_blocks, bpb_log2)
                    : TiledOffset2D(sx, sy, src.pitch_blocks, bpb_log2);
          const uint32_t count = std::min(run - sx % run, blocks_w - x);
          std::memcpy(row.data() + size_t(x) * gbpb, src.base + offset, size_t(count) * gbpb);
          x += count;
        }
      } else {
        const size_t offset = (size_t(sz) * src.height_blocks + sy) * src.pitch_blocks * gbpb +
                              size_t(src.x_blocks) * gbpb;
        std::memcpy(row.data(), src.base + offset, row.size());
      }
      SwapEndian(endian, row.data(), row.data(), row.size());
      uint8_t *out = dst + size_t(z) * dst_slice_pitch + size_t(y) * dst_row_pitch;
      if (info.conversion == TexelConversion::Copy)
        std::memcpy(out, row.data(), size_t(blocks_w) * hbpb);
      else if (info.decoded_blocks)
        DecodeBlockRow(info.conversion, row.data(), out, blocks_w, dst_row_pitch / info.block_size);
      else
        ConvertTexels(info.conversion, row.data(), out, blocks_w);
    }
  }
}

// Staging data read from guest memory at submission rather than at the draw,
// the way the GPU reads a texture when it executes (see BindTextureLocked).
struct PendingFill {
  TextureFormatInfo info;
  TextureFetch fetch;
  LevelSource src;
  uint32_t blocks_w, blocks_h, depth;
  uint8_t *dst;
  uint32_t row_pitch, slice_pitch;
};
std::vector<PendingFill> g_pending_fills;

// Records the copies of every level into 't'. The guest data is read now, or
// when the command list is submitted if 'defer_read'.
bool Upload(HostTexture &t, const TextureFetch &fetch, bool defer_read) {
  u32 block_width, block_height;
  FormatBlockSize(fetch.Format(), block_width, block_height);

  auto &s = State();
  plume::RenderCommandList *cmd = s.command_list;
  static const uint32_t kProfileLabel = gpu_profile::Label("texture uploads");
  gpu_profile::Mark(s, kProfileLabel);
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(t.texture.get(), plume::RenderTextureLayout::COPY_DEST));
  t.layout = plume::RenderTextureLayout::COPY_DEST;

  const bool is_3d_data = fetch.Dimension() == TextureDimension::Tex3D;
  // Host array layers (cube faces, stacked layers); a stacked texture's extra
  // layer (StackedLayers) repeats its last one.
  const uint32_t slices = is_3d_data ? 1 : t.stacked ? StackedLayers(t) : t.depth;
  const uint32_t bs = t.info.block_size;
  for (uint32_t level = 0; level < t.mip_levels; ++level) {
    const uint32_t w = std::max(1u, t.width >> level);
    const uint32_t h = std::max(1u, t.height >> level);
    const uint32_t d = is_3d_data ? std::max(1u, t.depth >> level) : 1;
    const uint32_t blocks_w = (w + bs - 1) / bs;
    const uint32_t blocks_h = (h + bs - 1) / bs;

    // Where the level lives: base or mip memory, possibly inside the packed tail.
    const TextureLevelLayout layout = GetTextureLevelLayout(fetch, level);
    const uint32_t page = layout.page;
    if (!page)
      continue;
    LevelSource src;
    src.pitch_blocks = layout.pitch_texels / block_width;
    src.height_blocks = layout.height / block_height;
    src.x_blocks = layout.tail_x_blocks;
    src.y_blocks = layout.tail_y_blocks;
    src.z_offset = layout.tail_z;
    const uint32_t level_offset = layout.offset;

    const uint32_t row_bytes = blocks_w * t.info.host_block_bytes;
    // Footprint rows are 256-byte aligned: rows of blocks, or for decoded
    // blocks each of the block_size texel rows a row of blocks holds.
    const uint32_t row_align = t.info.decoded_blocks ? 256u * bs : 256u;
    const uint32_t row_pitch = (row_bytes + row_align - 1) / row_align * row_align;
    const uint32_t slice_pitch = row_pitch * blocks_h;
    for (uint32_t slice = 0; slice < slices; ++slice) {
      const uint32_t guest_slice = std::min(slice, t.depth - 1);
      src.base = GuestPhysicalPtr<const uint8_t>((page << 12) + level_offset +
                                                 guest_slice * layout.slice_bytes);
      UploadAllocation staging = s.uploads[s.frame].Allocate(uint64_t(slice_pitch) * d, 512);
      if (!staging)
        return false;
      auto *dst = static_cast<uint8_t *>(staging.cpu);
      if (defer_read)
        g_pending_fills.push_back({t.info, fetch, src, blocks_w, blocks_h, d, dst, row_pitch, slice_pitch});
      else
        ReadLevel(t.info, fetch, src, blocks_w, blocks_h, d, dst, row_pitch, slice_pitch);
      // Footprints are in texels, block-aligned for compressed formats. Decoded
      // blocks are plain texels, so a level smaller than a block (a small mip)
      // takes only its own part of the decoded block.
      const uint32_t copy_w = std::min(blocks_w * bs, std::max(1u, t.alloc_width >> level));
      const uint32_t copy_h = std::min(blocks_h * bs, std::max(1u, t.alloc_height >> level));
      const plume::RenderBox level_box(0, 0, int32_t(copy_w), int32_t(copy_h), 0, int32_t(d));
      // plume's Vulkan copy takes its extent from the footprint and ignores the
      // box, and Vulkan allows no extent past the level (a compressed one ends
      // at the level's edge instead of a block's): the footprint is the level's
      // part there. D3D12 wants block-aligned footprints, cut by the box.
      const bool vulkan = IsVulkan();
      const uint32_t row_texels = row_pitch / t.info.host_block_bytes * bs;
      if (vulkan && d > 1 && copy_h != blocks_h * bs) {
        // The footprint's height is also its rows per depth slice there, which
        // the staging has blocks_h * bs of: one depth slice at a time.
        for (uint32_t z = 0; z < d; ++z) {
          cmd->copyTextureRegion(
              plume::RenderTextureCopyLocation::Subresource(t.texture.get(), level, slice),
              plume::RenderTextureCopyLocation::PlacedFootprint(
                  staging.ref.ref, t.info.host, copy_w, copy_h, 1, row_texels,
                  staging.ref.offset + uint64_t(z) * slice_pitch),
              0, 0, z, nullptr);
        }
      } else {
        cmd->copyTextureRegion(
            plume::RenderTextureCopyLocation::Subresource(t.texture.get(), level, slice),
            plume::RenderTextureCopyLocation::PlacedFootprint(
                staging.ref.ref, t.info.host, vulkan ? copy_w : blocks_w * bs,
                vulkan ? copy_h : blocks_h * bs, d, row_texels, staging.ref.offset),
            0, 0, 0, !vulkan && t.info.decoded_blocks ? &level_box : nullptr);
      }
    }
  }
  return true;
}

// The host levels a scaled resolve destination gets (HostTexture::extra_levels):
// down to a texel of at least the guest's.
uint32_t ExtraLevels(RenderScale scale) {
  const float factor = scale.Factor();
  return factor > 4.0f ? 3 : factor > 2.0f ? 2 : factor > 1.0f ? 1 : 0;
}

// swap_red_blue: the content was stored with X and Z exchanged (a guest
// resolve does that when the destination's X swizzle is not X). 'scale': see
// HostTexture::scale (content the GPU writes only). host_levels: a color
// resolve destination, which gets HostTexture::extra_levels when it is a
// scaled 2D one.
bool Create(HostTexture &t, const TextureFetch &fetch, bool render_target,
            plume::RenderFormat rt_format, bool swap_red_blue = false, RenderScale scale = {},
            bool host_levels = false) {
  u32 width, height, depth;
  fetch.Size(width, height, depth);
  t.dimension = HostDimension(fetch);
  t.guest_format = fetch.Format();
  t.info = GetTextureFormatInfo(fetch.Format(), AllSigned(fetch));
  t.width = width;
  t.height = height;
  t.depth = depth;
  t.scale = scale;
  // No more levels than the size has (GetMaxMipLevels, 0x826554B8).
  const u32 largest = std::max({width, height, fetch.Dimension() == TextureDimension::Tex3D ? depth : 1u});
  const u32 size_levels = 32 - u32(std::countl_zero(largest));
  t.mip_levels = render_target ? 1 : std::min(fetch.MaxMipLevel() + 1, size_levels);
  t.render_target = render_target;
  plume::RenderFormat format = render_target ? rt_format : t.info.host;
  if (format == plume::RenderFormat::UNKNOWN)
    return false;

  plume::RenderDevice *device = Video::Device();
  plume::RenderTextureDesc desc;
  plume::RenderTextureViewDesc view_desc;
  const uint32_t bs = render_target ? 1 : t.info.block_size;
  const uint32_t w = scale.Size((t.width + bs - 1) / bs * bs);
  const uint32_t h = scale.Size((t.height + bs - 1) / bs * bs);
  t.extra_levels = 0;
  if (render_target && host_levels && t.dimension == TextureDimension::Tex2D &&
      State().texture_mip_ps) {
    // No more than the host size has.
    const uint32_t host_levels_max = 32 - uint32_t(std::countl_zero(std::max(w, h)));
    t.extra_levels = std::min(ExtraLevels(scale), host_levels_max - 1);
    t.mip_levels += t.extra_levels;
  }
  t.stacked = fetch.Dimension() == TextureDimension::Tex2D && fetch.Stacked();
  switch (t.dimension) {
  case TextureDimension::Tex3D:
    if (t.stacked) {
      // Viewed as a 2D array, which takes two layers at least (StackedLayers).
      desc = plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, w, h, 1,
                                               t.mip_levels, StackedLayers(t), format);
      view_desc = plume::RenderTextureViewDesc::Texture2D(format);
    } else {
      desc = plume::RenderTextureDesc::Texture3D(w, h, t.depth, t.mip_levels, format);
      view_desc = plume::RenderTextureViewDesc::Texture3D(format);
    }
    break;
  case TextureDimension::Cube:
    desc = plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, w, h, 1,
                                             t.mip_levels, 6, format,
                                             plume::RenderTextureFlag::CUBE);
    view_desc = plume::RenderTextureViewDesc::TextureCube(format);
    t.depth = 6;
    break;
  default:
    desc = plume::RenderTextureDesc::Texture2D(w, h, t.mip_levels, format);
    view_desc = plume::RenderTextureViewDesc::Texture2D(format);
    t.depth = 1;
    break;
  }
  if (render_target)
    desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  t.host_format = format;
  t.alloc_width = w;
  t.alloc_height = h;
  // Beyond what the backends create (plume returns an object without a
  // resource then); Xenos textures are at most 8192 wide/high, and the
  // backends' limit is 16384.
  const uint32_t max_size = scale.Factor() > 1.0f ? kMaxTextureSize * 2 : kMaxTextureSize;
  if (w > max_size || h > max_size || t.depth > kMaxTextureSize)
    return false;
  t.texture = device->createTexture(desc);
  if (!TextureCreated(t.texture.get())) {
    t.texture.reset();
    ReportDeviceLost(device);
    return false;
  }
  t.texture->setName("renut texture");

  view_desc.mipLevels = t.mip_levels;
  view_desc.componentMapping = SwizzleMapping(fetch.Swizzle(), swap_red_blue, format);
  t.view_desc = view_desc;
  t.view_swizzle = fetch.Swizzle();
  t.view_swap_red_blue = swap_red_blue;
  t.view = t.texture->createTextureView(view_desc);
  if (!t.view)
    return false;
  t.layout = plume::RenderTextureLayout::UNKNOWN;
  t.slot = Video::AllocateTextureSlotLocked();
  if (t.slot == kInvalidDescriptor)
    return false;
  Video::SetTextureSlotLocked(t.slot, t.texture.get(), t.view.get());
  return true;
}

void MaskedDesc(const TextureFetch &fetch, u32 out[6]) {
  for (int i = 0; i < 6; ++i)
    out[i] = fetch.dword[i] & kDescMask[i];
}

void Retire(HostTexture &t) {
  ++g_bind_generation;
  if (t.slot != UINT32_MAX && t.slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(t.slot);
  t.slot = UINT32_MAX;
  for (HostTexture::SwizzleView &v : t.swizzle_views) {
    Video::FreeTextureSlotLocked(v.slot);
    Video::RetireLocked(std::move(v.view));
  }
  t.swizzle_views.clear();
  if (t.level0_slot != UINT32_MAX && t.level0_slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(t.level0_slot);
  t.level0_slot = UINT32_MAX;
  Video::RetireLocked(std::move(t.level0_view));
  Video::RetireLocked(std::move(t.view));
  Video::RetireLocked(std::move(t.texture));
  if (t.words) {
    Video::RetireLocked(std::move(t.depth_framebuffer));
    Retire(*t.words);
    t.words.reset();
    std::erase_if(g_depth_words, [&t](const auto &entry) { return entry.second == &t; });
  }
  if (t.render_target)
    std::erase_if(g_resolve_targets, [&t](const auto &entry) { return entry.second == &t; });
}

// Two descriptions of the same data: the same memory, format, endianness, size
// and dimension (fetch dwords 1, 2 and the dimension in 5). Fetch constants of
// the same memory may still differ in pitch, swizzle or mip fields.
bool SameData(const u32 a[6], const u32 b[6]) {
  constexpr u32 kDimensionMask = 0x600u;  // fetch dword 5 bits 9-10
  return a[1] == b[1] && a[2] == b[2] && (a[5] & kDimensionMask) == (b[5] & kDimensionMask);
}

// The resolve destination another header wrote this texture's memory through,
// when it holds the same data (SameData). Only the GPU has what the resolve
// wrote.
HostTexture *ResolvedAlias(u32 texture_va, const TextureFetch &fetch, const u32 desc[6]) {
  auto it = g_resolve_targets.find(fetch.BasePage());
  if (it == g_resolve_targets.end())
    return nullptr;
  HostTexture *resolved = it->second;
  if (resolved->guest_va == texture_va || !resolved->texture)
    return nullptr;
  return SameData(resolved->desc, desc) ? resolved : nullptr;
}

// The depth resolve whose words a texture described by 'fetch' reads: a
// 32-bit 8_8_8_8 texture over a depth resolve destination's memory, of its
// size.
HostTexture *DepthWordsSource(const TextureFetch &fetch) {
  if (fetch.Format() != GpuTextureFormat::Fmt8_8_8_8 || g_depth_words.empty())
    return nullptr;
  auto it = g_depth_words.find(fetch.BasePage());
  if (it == g_depth_words.end())
    return nullptr;
  HostTexture *source = it->second;
  u32 width, height, depth;
  fetch.Size(width, height, depth);
  if (!source->words || source->width != width || source->height != height)
    return nullptr;
  return source;
}

// Copies the depth resolve's words into 't' (of the source's scale) when it
// has resolved again.
void CopyDepthWords(HostTexture &t, HostTexture &source) {
  if (t.copied_resolve == source.resolves)
    return;
  plume::RenderCommandList *cmd = State().command_list;
  TransitionTextureLocked(source.words.get(), plume::RenderTextureLayout::COPY_SOURCE);
  TransitionTextureLocked(&t, plume::RenderTextureLayout::COPY_DEST);
  const plume::RenderBox box(0, 0, int32_t(t.scale.Size(t.width)), int32_t(t.scale.Size(t.height)));
  cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(t.texture.get(), 0, 0),
                         plume::RenderTextureCopyLocation::Subresource(source.words->texture.get()),
                         0, 0, 0, &box);
  t.copied_resolve = source.resolves;
  t.uploaded = true;
}

// GetResolveTextureLocked; 'fetch' receives the header's fetch constant.
HostTexture *ResolveTarget(u32 texture_va, plume::RenderFormat format, RenderScale scale,
                           const d3d::TextureFetchConstant *header_format, bool depth,
                           TextureFetch &fetch) {
  // What binds of this memory (or depth words over it) find changes.
  ++g_bind_generation;
  if (!texture_va)
    return nullptr;
  if (!header_format) {
    const auto *header = GuestPtr<const d3d::BaseTexture>(texture_va);
    if (!header)
      return nullptr;
    header_format = &header->Format;
  }
  fetch = TextureFetch::FromHeader(*header_format);
  if (!fetch.IsTexture())
    return nullptr;
  u32 desc[6];
  MaskedDesc(fetch, desc);

  auto &slot = TextureSlot(texture_va);
  if (slot && (!slot->render_target || std::memcmp(slot->desc, desc, sizeof(desc)) != 0 ||
               !slot->texture || slot->host_format != format || slot->scale != scale)) {
    // What an earlier resolve wrote is lost (only the GPU had it).
    static uint32_t logged = 0;
    if (slot->gpu_written && slot->texture && logged++ < 32) {
      RGPU_WARN("Resolve destination {:08X} rebuilt ({}), frame {}: what it held is lost",
                texture_va,
                !slot->render_target                                 ? "was a sampled texture"
                : std::memcmp(slot->desc, desc, sizeof(desc)) != 0 ? "header changed"
                : slot->host_format != format                        ? "format changed"
                                                                     : "scale changed",
                g_frame);
    }
    Retire(*slot);
    slot.reset();
  }
  if (!slot) {
    slot = std::make_unique<HostTexture>();
    slot->guest_va = texture_va;
    std::memcpy(slot->desc, desc, sizeof(desc));
    // A depth destination has one component, which the swizzle places.
    if (!Create(*slot, fetch, true, format, !depth && (fetch.Swizzle() & 7) != 0, scale, !depth)) {
      RGPU_ERROR("Resolve destination {:08X}: {}x{} target creation failed", texture_va,
                 slot->width, slot->height);
      Retire(*slot);
      return nullptr;
    }
    // A new render target has no defined content, and D3D12 wants it
    // discarded (or cleared) before a resolve or copy writes part of it.
    if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
      TransitionTextureLocked(slot.get(), plume::RenderTextureLayout::COLOR_WRITE);
      cmd->discardTexture(slot->texture.get());
    }
  }
  slot->gpu_written = true;
  slot->uploaded = true;
  slot->used_frame = g_frame;
  g_resolve_targets[fetch.BasePage()] = slot.get();
  return slot.get();
}

} // namespace

namespace {

// Marks the host textures of the resources unlocked since the last call.
// Unlocks of anything else (vertex buffers, textures never bound) are dropped.
void ApplyUnlocksLocked() {
  std::vector<u32> unlocked;
  {
    std::lock_guard lock(g_unlocked_mutex);
    if (g_unlocked.empty())
      return;
    unlocked.swap(g_unlocked);
  }
  for (u32 va : unlocked) {
    auto it = g_textures.find(va);
    if (it != g_textures.end() && it->second) {
      it->second->unlocked = true;
      it->second->unlock_frame = g_frame;
      it->second->lock_driven = true;
    }
  }
}

// Linear textures are the CPU-written ones. The GPU reads them when it
// executes, after the CPU is done with the frame, so an upload reads memory
// as it is at submission; reading at the draw can catch writes still in
// progress. Only bytes that changed are worth uploading: converting the font
// cache every frame cost more than anything else in Present, and hashing its
// 7 MB every frame still cost more than half a millisecond. How a texture
// changes decides how it is checked, once per frame it is bound:
// - Lock-driven: the game writes it between a Lock and D3D::UnlockResource
//   (the font cache, sub_82B8F848). An unlock uploads; the content is watched
//   for kUnlockWatchFrames after one, and checked every kLinearVerifyFrames
//   for a write that went around the unlock (which ends lock-driven).
// - Dynamic: changed at kDynamicChanges checks in a row (the XMV logo movies,
//   whose decoder writes without a Lock and may still be writing at the
//   draw). Uploaded every frame; checked every kLinearVerifyFrames for having
//   stopped changing.
// - Otherwise the content is hashed at every check.
// A write after the check shows the next frame.
void RefreshLinearLocked(HostTexture &t, const TextureFetch &fetch) {
  ApplyUnlocksLocked();
  auto upload = [&](uint64_t hash) {
    t.content_hash = hash;
    t.uploaded = Upload(t, fetch, true);
  };
  const bool verify_due = g_frame - t.verified_frame >= kLinearVerifyFrames;
  if (!t.uploaded) {
    upload(ContentHash(fetch, t));
    t.verified_frame = g_frame;
  } else if (t.lock_driven) {
    const bool watched = g_frame - t.unlock_frame < kUnlockWatchFrames;
    if (t.unlocked) {
      upload(ContentHash(fetch, t));
    } else if (watched || verify_due) {
      const uint64_t hash = ContentHash(fetch, t);
      if (hash != t.content_hash) {
        upload(hash);
        t.lock_driven = watched;
      }
      if (verify_due)
        t.verified_frame = g_frame;
    }
  } else if (t.dynamic) {
    if (verify_due) {
      const uint64_t hash = ContentHash(fetch, t);
      t.dynamic = hash != t.content_hash;
      t.changes = 0;
      t.content_hash = hash;
      t.verified_frame = g_frame;
    }
    t.uploaded = Upload(t, fetch, true);
  } else {
    const uint64_t hash = ContentHash(fetch, t);
    if (hash != t.content_hash) {
      upload(hash);
      t.dynamic = ++t.changes >= kDynamicChanges;
      t.verified_frame = g_frame;
    } else {
      t.changes = 0;
    }
  }
  t.unlocked = false;
}

// The backstop (kEvictPressureSlots, kEvictMemoryShare): with the bindless
// table filling up or video memory near its budget, the host textures idle
// for a while go.
void EvictIdleTexturesLocked() {
  const bool slots = Video::TextureSlotsInUseLocked() >= kEvictPressureSlots;
  GpuMemoryUsage memory;
  if (!slots) {
    memory = QueryGpuMemory(State().device.get());
    if (!memory.valid || !memory.local_budget ||
        double(memory.local_used) < double(memory.local_budget) * kEvictMemoryShare) {
      return;
    }
  }
  std::vector<u32> idle;
  for (const auto &[va, t] : g_textures) {
    const uint64_t limit = t && t->gpu_written ? kEvictIdleGpuFrames : kEvictIdleFrames;
    if (!t || g_frame - t->used_frame >= limit)
      idle.push_back(va);
  }
  for (u32 va : idle)
    ReleaseHostTextureLocked(va);
  if (slots) {
    RGPU_WARN("Textures: bindless table nearly full; {} idle host textures released ({} of {} "
              "slots in use)",
              idle.size(), Video::TextureSlotsInUseLocked(), kTextureDescriptorCount);
  } else if (!idle.empty()) {
    RGPU_WARN("Textures: video memory near its budget ({} of {} MB); {} idle host textures "
              "released",
              memory.local_used >> 20, memory.local_budget >> 20, idle.size());
  }
}

} // namespace

void AdvanceTextureFrameLocked() {
  ++g_frame;
  ++g_bind_generation;
  ApplyUnlocksLocked();
  if (g_frame % kEvictCheckFrames == 0)
    EvictIdleTexturesLocked();
}

void ReleaseHostTexturesInMemoryLocked(u32 physical, u32 size) {
  if (!size)
    return;
  const u32 first_page = (physical & 0x1FFFFFFFu) >> 12;
  const u32 end_page = first_page + ((size + 0xFFFu) >> 12);
  auto inside = [&](u32 page) { return page >= first_page && page < end_page; };
  std::vector<u32> gone;
  for (const auto &[va, t] : g_textures) {
    if (!t)
      continue;
    const u32 base_page = t->desc[1] >> 12;
    const u32 mip_page = t->desc[5] >> 12;
    if (inside(base_page) || (mip_page && inside(mip_page)))
      gone.push_back(va);
  }
  for (u32 va : gone)
    ReleaseHostTextureLocked(va);
}

void NoteTextureUnlocked(u32 resource_va) {
  std::lock_guard lock(g_unlocked_mutex);
  g_unlocked.push_back(resource_va);
}

void FlushTextureFillsLocked() {
  // Each fill writes its own staging memory (parallel.h).
  parallel::For(g_pending_fills.size(), 1, [](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      const PendingFill &f = g_pending_fills[i];
      ReadLevel(f.info, f.fetch, f.src, f.blocks_w, f.blocks_h, f.depth, f.dst, f.row_pitch,
                f.slice_pitch);
    }
  });
  g_pending_fills.clear();
}

HostTexture *FindHostTextureLocked(u32 texture_va) {
  auto it = g_textures.find(texture_va);
  return it != g_textures.end() ? it->second.get() : nullptr;
}

void ReleaseHostTextureLocked(u32 texture_va) {
  auto it = g_textures.find(texture_va);
  if (it == g_textures.end())
    return;
  Retire(*it->second);
  if (TextureMemo &memo = TextureMemoFor(texture_va); memo.va == texture_va)
    memo = {};
  g_textures.erase(it);
}

void TransitionTextureLocked(HostTexture *texture, plume::RenderTextureLayout layout) {
  if (!texture || !texture->texture || texture->layout == layout)
    return;
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!cmd)
    return;
  const bool copy = layout == plume::RenderTextureLayout::COPY_DEST ||
                    layout == plume::RenderTextureLayout::COPY_SOURCE ||
                    layout == plume::RenderTextureLayout::RESOLVE_DEST;
  cmd->barriers(copy ? plume::RenderBarrierStage::COPY : plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(texture->texture.get(), layout));
  texture->layout = layout;
}

namespace {

// BindTextureLocked without the memo; 'bound' receives the host texture bound,
// if any.
TextureBinding BindUncached(u32 texture_va, const TextureFetch &device_fetch,
                            const d3d::BaseTexture *header, HostTexture *&bound) {
  TextureBinding binding;
  binding.dimension = HostDimension(device_fetch);
  binding.slot = NullSlot(binding.dimension);
  TextureFetch fetch = TextureFetch::FromHeader(header->Format);
  // SetTexture copied the header into the device's fetch constant. A header
  // that no longer describes the same data (SameData: memory, format, size,
  // dimension) was a temporary the game has overwritten since (a command
  // buffer runs after its recording thread's stack moved on); the device's
  // copy is what the GPU reads. Its memory alone does not tell: N&B's main
  // menu builds its bloom buffers' headers on the stack (160x90 and 80x45
  // 8_8_8_8 at 701CFB48 / 701CFB7C), whose size and dimension words are
  // overwritten while the base address stays; read as another texture, the
  // resolve those buffers hold was dropped, a black frame now and then.
  u32 desc[6];
  MaskedDesc(fetch, desc);
  u32 device_desc[6];
  MaskedDesc(device_fetch, device_desc);
  if (!fetch.IsTexture() || !SameData(desc, device_desc)) {
    fetch = device_fetch;
    std::memcpy(desc, device_desc, sizeof(desc));
  }
  if (!fetch.IsTexture())
    return binding;
  if (HostTexture *resolved = ResolvedAlias(texture_va, fetch, desc)) {
    bound = resolved;
    resolved->used_frame = g_frame;
    TransitionTextureLocked(resolved, plume::RenderTextureLayout::SHADER_READ);
    binding.dimension = resolved->dimension;
    binding.slot = SlotForSwizzle(*resolved, fetch.Swizzle());
    binding.stacked = resolved->stacked;
    binding.scale = resolved->scale;
    binding.lod_scale = resolved->extra_levels ? resolved->scale : RenderScale{};
    return binding;
  }
  auto &slot = TextureSlot(texture_va);
  // A resolve destination sampled through a fetch constant that differs from
  // its header only in fields that leave the data alone (the mip range, say)
  // keeps what the resolve wrote.
  if (slot && std::memcmp(slot->desc, desc, sizeof(desc)) != 0 &&
      !(slot->gpu_written && SameData(slot->desc, desc))) {
    static uint32_t logged = 0;
    if (slot->gpu_written && slot->texture && logged++ < 32) {
      RGPU_WARN("Texture {:08X}: sampled with another header, frame {}: what a resolve wrote is "
                "dropped for guest memory",
                texture_va, g_frame);
    }
    Retire(*slot);
    slot.reset();
  }
  // What a depth resolve wrote to the memory (only the GPU has it), which is
  // at the resolve's scale; guest data is at the guest size.
  HostTexture *depth_source = !(slot && slot->gpu_written) ? DepthWordsSource(fetch) : nullptr;
  const RenderScale scale = depth_source ? depth_source->scale : RenderScale{};
  if (slot && !slot->gpu_written && slot->scale != scale) {
    Retire(*slot);
    slot.reset();
  }
  if (!slot) {
    slot = std::make_unique<HostTexture>();
    slot->guest_va = texture_va;
    std::memcpy(slot->desc, desc, sizeof(desc));
    if (!Create(*slot, fetch, false, plume::RenderFormat::UNKNOWN, false, scale)) {
      static uint32_t logged = 0;
      if (logged++ < 16) {
        RGPU_WARN("Texture {:08X}: format {} {}x{}x{} not supported, frame {}", texture_va,
                  u32(fetch.Format()), slot->width, slot->height, slot->depth, g_frame);
      }
      Retire(*slot);
      return binding;  // keep the entry so it is not retried every draw
    }
  }
  HostTexture &t = *slot;
  // One that could not be made is not marked used: it goes once idle, and the
  // next bind tries again (the bindless table may have room by then).
  if (!t.texture)
    return binding;
  t.used_frame = g_frame;

  if (depth_source && !t.gpu_written) {
    CopyDepthWords(t, *depth_source);
  } else if (!t.gpu_written && t.checked_frame != g_frame) {
    t.checked_frame = g_frame;
    if (!fetch.Tiled()) {
      RefreshLinearLocked(t, fetch);
    } else {
      const uint64_t hash = SampleHash(fetch, t);
      if (!t.uploaded || hash != t.content_hash) {
        t.content_hash = hash;
        t.uploaded = Upload(t, fetch, false);
      }
    }
  }
  TransitionTextureLocked(&t, plume::RenderTextureLayout::SHADER_READ);
  bound = &t;
  binding.dimension = t.dimension;
  binding.slot = SlotForSwizzle(t, fetch.Swizzle());
  binding.stacked = t.stacked;
  binding.scale = t.scale;
  binding.lod_scale = t.extra_levels ? t.scale : RenderScale{};
  return binding;
}

} // namespace

TextureBinding BindTextureLocked(u32 texture_va, const TextureFetch &device_fetch) {
  const auto *header = texture_va ? GuestPtr<const d3d::BaseTexture>(texture_va) : nullptr;
  if (!header) {
    TextureBinding binding;
    binding.dimension = HostDimension(device_fetch);
    binding.slot = NullSlot(binding.dimension);
    return binding;
  }
  static_assert(sizeof(header->Format) == sizeof(BindMemo::header));
  static_assert(sizeof(device_fetch.dword) == sizeof(BindMemo::device));
  u32 header_words[6];
  std::memcpy(header_words, &header->Format, sizeof(header_words));
  BindMemo *set = BindMemoSet(texture_va, device_fetch.dword);
  for (u32 way = 0; way < 2; ++way) {
    BindMemo &m = set[way];
    if (m.generation == g_bind_generation && m.va == texture_va &&
        std::memcmp(m.device, device_fetch.dword, sizeof(m.device)) == 0 &&
        std::memcmp(m.header, header_words, sizeof(m.header)) == 0) {
      if (m.texture) {
        m.texture->used_frame = g_frame;
        TransitionTextureLocked(m.texture, plume::RenderTextureLayout::SHADER_READ);
      }
      return m.binding;
    }
  }
  HostTexture *bound = nullptr;
  const TextureBinding binding = BindUncached(texture_va, device_fetch, header, bound);
  // At the generation the bind ended in (it may have retired a texture).
  set[1] = set[0];
  BindMemo &m = set[0];
  m.generation = g_bind_generation;
  m.va = texture_va;
  std::memcpy(m.device, device_fetch.dword, sizeof(m.device));
  std::memcpy(m.header, header_words, sizeof(m.header));
  m.texture = bound;
  m.binding = binding;
  return binding;
}

void TextureGuestExtent(const TextureFetch &fetch, uint32_t pages[2], uint32_t sizes[2]) {
  // Memo over the fetch constant: while memexport results are pending, every
  // draw asks for its textures' extents (DrawNeedsExportFlush), and the layout
  // math cost more than the rest of the check.
  struct Entry {
    uint32_t key[6];
    uint32_t pages[2];
    uint32_t sizes[2];
    bool valid;
  };
  static_assert(sizeof(fetch.dword) == sizeof(Entry::key));
  thread_local Entry memo[1024] = {};
  uint32_t key[6];
  std::memcpy(key, fetch.dword, sizeof(key));
  uint32_t hash = 0;
  for (uint32_t word : key)
    hash = (hash ^ word) * 0x9E3779B1u;
  Entry &entry = memo[hash >> 22];
  if (entry.valid && std::memcmp(entry.key, key, sizeof(key)) == 0) {
    std::memcpy(pages, entry.pages, sizeof(entry.pages));
    std::memcpy(sizes, entry.sizes, sizeof(entry.sizes));
    return;
  }

  uint32_t base_bytes, mip_bytes;
  GetTextureDataSizes(fetch, base_bytes, mip_bytes);
  pages[0] = fetch.BasePage();
  sizes[0] = base_bytes;
  pages[1] = mip_bytes ? fetch.MipPage() : 0;
  sizes[1] = mip_bytes;
  std::memcpy(entry.key, key, sizeof(key));
  std::memcpy(entry.pages, pages, sizeof(entry.pages));
  std::memcpy(entry.sizes, sizes, sizeof(entry.sizes));
  entry.valid = true;
}

HostTexture *GetResolveTextureLocked(u32 texture_va, plume::RenderFormat format, RenderScale scale,
                                     const d3d::TextureFetchConstant *header_format) {
  TextureFetch fetch;
  return ResolveTarget(texture_va, format, scale, header_format, false, fetch);
}

HostTexture *GetDepthResolveTextureLocked(u32 texture_va, RenderScale scale,
                                          const d3d::TextureFetchConstant *header_format) {
  TextureFetch fetch;
  HostTexture *t =
      ResolveTarget(texture_va, kDepthResolveFormat, scale, header_format, true, fetch);
  if (!t)
    return nullptr;
  if (!t->words) {
    auto words = std::make_unique<HostTexture>();
    words->width = t->width;
    words->height = t->height;
    words->scale = t->scale;
    words->alloc_width = t->alloc_width;
    words->alloc_height = t->alloc_height;
    words->host_format = kDepthResolveWordsFormat;
    plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
        t->alloc_width, t->alloc_height, 1, kDepthResolveWordsFormat);
    desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
    plume::RenderDevice *device = Video::Device();
    words->texture = device->createTexture(desc);
    if (!TextureCreated(words->texture.get())) {
      ReportDeviceLost(device);
      return nullptr;
    }
    words->texture->setName("renut depth resolve words");
    const plume::RenderTexture *targets[] = {t->texture.get(), words->texture.get()};
    t->depth_framebuffer = device->createFramebuffer(plume::RenderFramebufferDesc(targets, 2));
    if (!t->depth_framebuffer) {
      Video::RetireLocked(std::move(words->texture));
      return nullptr;
    }
    if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
      TransitionTextureLocked(words.get(), plume::RenderTextureLayout::COLOR_WRITE);
      cmd->discardTexture(words->texture.get());
    }
    t->words = std::move(words);
  }
  g_depth_words[fetch.BasePage()] = t;
  ++t->resolves;
  return t;
}

namespace {

// Single-level color targets the host levels are drawn into (plume's barriers
// cover a whole texture, so a level cannot be drawn while level 0 of the same
// texture is sampled) and multisampled resolves staged in: a bare target
// (texture, layout, size) and its framebuffer, by format and size. Kept for
// reuse.
struct ScratchTarget {
  HostTexture target;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  // Its bindless slot, for the next level down to read (ScratchSlot).
  std::unique_ptr<plume::RenderTextureView> view;
  uint32_t slot = UINT32_MAX;
};

uint32_t ScratchSlot(ScratchTarget &scratch) {
  if (scratch.slot != UINT32_MAX)
    return scratch.slot;
  scratch.view = scratch.target.texture->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(scratch.target.host_format));
  if (!scratch.view)
    return kInvalidDescriptor;
  scratch.slot = Video::AllocateTextureSlotLocked();
  if (scratch.slot != kInvalidDescriptor)
    Video::SetTextureSlotLocked(scratch.slot, scratch.target.texture.get(), scratch.view.get());
  return scratch.slot;
}
std::unordered_map<uint64_t, std::unique_ptr<ScratchTarget>> g_scratch_targets;
// The level pipelines, by render target format.
std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> g_mip_pipelines;

ScratchTarget *GetScratchTarget(plume::RenderFormat format, uint32_t width, uint32_t height) {
  const uint64_t key = uint64_t(format) << 40 | uint64_t(width) << 20 | height;
  auto &entry = g_scratch_targets[key];
  if (entry)
    return entry.get();
  auto scratch = std::make_unique<ScratchTarget>();
  HostTexture &t = scratch->target;
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(width, height, 1, format);
  desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  plume::RenderDevice *device = Video::Device();
  t.texture = device->createTexture(desc);
  if (!TextureCreated(t.texture.get())) {
    t.texture.reset();
    ReportDeviceLost(device);
    return nullptr;
  }
  t.texture->setName("renut scratch target");
  t.host_format = format;
  t.width = t.alloc_width = width;
  t.height = t.alloc_height = height;
  const plume::RenderTexture *targets[] = {t.texture.get()};
  scratch->framebuffer = device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1));
  if (!scratch->framebuffer) {
    Video::RetireLocked(std::move(t.texture));
    return nullptr;
  }
  // Like a resolve destination, defined before anything writes part of it.
  if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
    TransitionTextureLocked(&t, plume::RenderTextureLayout::COLOR_WRITE);
    cmd->discardTexture(t.texture.get());
  }
  entry = std::move(scratch);
  return entry.get();
}

plume::RenderPipeline *MipPipeline(plume::RenderFormat format) {
  auto &pipeline = g_mip_pipelines[uint32_t(format)];
  if (pipeline)
    return pipeline.get();
  auto &s = State();
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.utility_layout.get();
  desc.vertexShader = s.present_vs.get();
  desc.pixelShader = s.texture_mip_ps.get();
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = format;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  pipeline = s.device->createGraphicsPipeline(desc);
  if (!pipeline) {
    RGPU_ERROR("Texture mip pipeline creation failed (format {})", uint32_t(format));
    return nullptr;
  }
  pipeline->setName("renut texture mip");
  return pipeline.get();
}

// Level 0 of 't' without the fetch swizzle, in the texture table.
uint32_t Level0Slot(HostTexture &t) {
  if (t.level0_slot != UINT32_MAX)
    return t.level0_slot;
  plume::RenderTextureViewDesc desc = t.view_desc;
  desc.mipSlice = 0;
  desc.mipLevels = 1;
  desc.componentMapping = plume::RenderComponentMapping();
  t.level0_view = t.texture->createTextureView(desc);
  if (!t.level0_view)
    return kInvalidDescriptor;
  t.level0_slot = Video::AllocateTextureSlotLocked();
  if (t.level0_slot != kInvalidDescriptor)
    Video::SetTextureSlotLocked(t.level0_slot, t.texture.get(), t.level0_view.get());
  return t.level0_slot;
}

} // namespace

HostTexture *ResolveStagingLocked(const HostTexture *dest) {
  ScratchTarget *scratch = GetScratchTarget(dest->host_format, dest->alloc_width, dest->alloc_height);
  return scratch ? &scratch->target : nullptr;
}

void UpdateResolveMipsLocked(HostTexture *texture, const plume::RenderRect &rect) {
  auto &s = State();
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!texture || !texture->texture || !texture->extra_levels || !cmd || !s.texture_mip_ps)
    return;
  HostTexture &t = *texture;
  plume::RenderPipeline *pipeline = MipPipeline(t.host_format);
  const uint32_t source_slot = Level0Slot(t);
  if (!pipeline || source_slot == kInvalidDescriptor)
    return;
  static const uint32_t kProfileLabel = gpu_profile::Label("resolve host mip levels");
  gpu_profile::Mark(s, kProfileLabel);

  // Each level is the 2x2 box of the one above: level 1 of level 0, the rest
  // of the previous level's scratch target. The rect is first widened to
  // whole texels of the smallest level, so each level's region is exactly
  // what the next one reads (the scratch targets are shared, and hold other
  // textures' texels outside what was drawn now).
  struct Level {
    ScratchTarget *scratch;
    plume::RenderRect region;
  };
  Level levels[kMaxExtraLevels] = {};
  const uint32_t count = std::min(t.extra_levels, uint32_t(std::size(levels)));
  const int32_t align = 1 << count;
  const plume::RenderRect aligned(rect.left & ~(align - 1), rect.top & ~(align - 1),
                                  (rect.right + align - 1) & ~(align - 1),
                                  (rect.bottom + align - 1) & ~(align - 1));
  TransitionTextureLocked(&t, plume::RenderTextureLayout::SHADER_READ);
  uint32_t source = source_slot;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t level = i + 1;
    const int32_t span = 1 << level;
    const uint32_t width = std::max(1u, t.alloc_width >> level);
    const uint32_t height = std::max(1u, t.alloc_height >> level);
    Level &l = levels[i];
    l.region = plume::RenderRect(std::min(aligned.left >> level, int32_t(width)),
                                 std::min(aligned.top >> level, int32_t(height)),
                                 std::min((aligned.right + span - 1) >> level, int32_t(width)),
                                 std::min((aligned.bottom + span - 1) >> level, int32_t(height)));
    if (l.region.right <= l.region.left || l.region.bottom <= l.region.top)
      break;
    l.scratch = GetScratchTarget(t.host_format, width, height);
    if (!l.scratch || source == kInvalidDescriptor) {
      l.scratch = nullptr;
      break;
    }
    if (i > 0) {
      // The level above, drawn just now, is read.
      TransitionTextureLocked(&levels[i - 1].scratch->target, plume::RenderTextureLayout::SHADER_READ);
    }
    TransitionTextureLocked(&l.scratch->target, plume::RenderTextureLayout::COLOR_WRITE);
    cmd->setFramebuffer(l.scratch->framebuffer.get());
    cmd->setGraphicsPipelineLayout(s.utility_layout.get());
    cmd->setPipeline(pipeline);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
    cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
    // Pixel coordinates are texels of the level.
    cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
    cmd->setScissors(l.region);
    UtilityPush push{};
    push.scale[0] = push.scale[1] = 1.0f;
    push.texture_index = source;
    push.sampler_index = kPointClampSamplerSlot;
    push.param0 = 2.0f;
    cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    cmd->drawInstanced(3, 1, 0, 0);
    source = ScratchSlot(*l.scratch);
  }

  TransitionTextureLocked(&t, plume::RenderTextureLayout::COPY_DEST);
  for (uint32_t i = 0; i < count; ++i) {
    const Level &l = levels[i];
    if (!l.scratch)
      continue;
    TransitionTextureLocked(&l.scratch->target, plume::RenderTextureLayout::COPY_SOURCE);
    const plume::RenderBox box(l.region.left, l.region.top, l.region.right, l.region.bottom);
    cmd->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(t.texture.get(), i + 1, 0),
        plume::RenderTextureCopyLocation::Subresource(l.scratch->target.texture.get()),
        uint32_t(l.region.left), uint32_t(l.region.top), 0, &box);
  }
  TransitionTextureLocked(&t, plume::RenderTextureLayout::SHADER_READ);
}

namespace {

// The host texture's levels in bytes, from its host format.
uint64_t HostTextureBytes(const HostTexture &t) {
  if (!t.texture)
    return 0;
  const uint32_t block = std::max(plume::RenderFormatBlockWidth(t.host_format), 1u);
  const uint64_t block_bytes = plume::RenderFormatSize(t.host_format);
  const uint32_t layers = t.stacked ? StackedLayers(t) : t.depth;
  uint64_t bytes = 0;
  for (uint32_t level = 0; level < t.mip_levels; ++level) {
    const uint64_t w = std::max(1u, t.alloc_width >> level);
    const uint64_t h = std::max(1u, t.alloc_height >> level);
    const uint64_t d = t.dimension == TextureDimension::Tex3D && !t.stacked
                           ? std::max(1u, t.depth >> level)
                           : layers;
    bytes += (w + block - 1) / block * ((h + block - 1) / block) * block_bytes * d;
  }
  if (t.words)
    bytes += HostTextureBytes(*t.words);
  return bytes;
}

} // namespace

TextureMemory TextureMemoryLocked() {
  TextureMemory usage;
  for (const auto &[va, t] : g_textures) {
    if (!t || !t->texture)
      continue;
    const uint64_t bytes = HostTextureBytes(*t);
    ++usage.textures;
    usage.bytes += bytes;
    if (t->render_target) {
      ++usage.resolve_targets;
      usage.resolve_bytes += bytes;
    }
  }
  return usage;
}

namespace {
uint32_t SamplerSlotUncached(const TextureFetch &fetch, RenderScale lod_scale);
} // namespace

uint32_t SamplerSlotLocked(const TextureFetch &fetch, RenderScale lod_scale) {
  // Every draw asks for each of its textures: remember the answer by the
  // fetch constant bits the sampler state comes from (clamps in 0, filters
  // and anisotropy in 3, mip range and LOD bias in 4, border color in 5).
  // Only those bits: the words also hold the pitch and the mip address, and
  // keyed by them every texture had entries of its own, which pushed each
  // other out.
  struct Memo {
    bool valid = false;
    u32 words[4] = {};
    RenderScale lod_scale;
    uint32_t min_anisotropy = 1;
    uint32_t slot = 0;
  };
  static Memo memo[256];
  const uint32_t min_anisotropy = MinAnisotropy();
  const u32 words[4] = {fetch.dword[0] & 0x0007FC00u, fetch.dword[3] & 0x0FF80000u,
                        fetch.dword[4] & 0x003FF3FCu, fetch.dword[5] & 0x00000003u};
  Memo &entry = memo[(words[0] * 0x9E3779B1u ^ words[1] * 0x85EBCA6Bu ^ words[2] * 0xC2B2AE35u ^
                      words[3] * 0x165667B1u ^ lod_scale.code * 0x27D4EB2Fu) >> 24];
  if (entry.valid && entry.lod_scale == lod_scale && entry.min_anisotropy == min_anisotropy &&
      std::memcmp(entry.words, words, sizeof(words)) == 0)
    return entry.slot;
  const uint32_t slot = SamplerSlotUncached(fetch, lod_scale);
  entry.valid = true;
  std::memcpy(entry.words, words, sizeof(words));
  entry.lod_scale = lod_scale;
  entry.min_anisotropy = min_anisotropy;
  entry.slot = slot;
  return slot;
}

namespace {

// The sampler slot for the fetch constant's sampler state, created the first
// time it is asked for.
uint32_t SamplerSlotUncached(const TextureFetch &fetch, RenderScale lod_scale) {
  auto address = [](TextureAddress mode) {
    switch (mode) {
    case TextureAddress::Wrap:
      return plume::RenderTextureAddressMode::WRAP;
    case TextureAddress::Mirror:
      return plume::RenderTextureAddressMode::MIRROR;
    case TextureAddress::Border:
    case TextureAddress::MirrorOnceBorder:
      return plume::RenderTextureAddressMode::BORDER;
    case TextureAddress::MirrorOnce:
    case TextureAddress::MirrorOnceBorderHalf:
      return plume::RenderTextureAddressMode::MIRROR_ONCE;
    default:
      return plume::RenderTextureAddressMode::CLAMP;
    }
  };
  auto filter = [](TextureFilter f) {
    return f == TextureFilter::Point ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  };

  plume::RenderSamplerDesc desc;
  desc.addressU = address(fetch.Address(0));
  desc.addressV = address(fetch.Address(1));
  desc.addressW = address(fetch.Address(2));
  desc.magFilter = filter(fetch.MagFilter());
  desc.minFilter = filter(fetch.MinFilter());
  desc.mipmapMode = fetch.MipFilter() == TextureFilter::Point ? plume::RenderMipmapMode::NEAREST
                                                              : plume::RenderMipmapMode::LINEAR;
  // MaxAnisotropy stores 2..5 for ratios 2:1..16:1 (its table at 0x82003E38).
  const uint32_t aniso = fetch.Anisotropy();
  desc.anisotropyEnabled = aniso > 1 && aniso <= 5;
  desc.maxAnisotropy = desc.anisotropyEnabled ? (1u << (aniso - 1)) : 1;
  // renut_anisotropic_filtering: the least a mipmapped, linearly filtered
  // texture gets (render targets' host levels keep their own filtering).
  const uint32_t min_anisotropy = MinAnisotropy();
  if (min_anisotropy > desc.maxAnisotropy && lod_scale.code == 0 &&
      desc.minFilter == plume::RenderFilter::LINEAR &&
      desc.magFilter == plume::RenderFilter::LINEAR &&
      fetch.MipFilter() != TextureFilter::BaseMap && fetch.MaxMipLevel() > fetch.MinMipLevel()) {
    desc.anisotropyEnabled = true;
    desc.maxAnisotropy = min_anisotropy;
  }
  desc.minLOD = float(fetch.MinMipLevel());
  desc.maxLOD = fetch.MipFilter() == TextureFilter::BaseMap ? float(fetch.MinMipLevel())
                                                            : float(fetch.MaxMipLevel());
  desc.mipLODBias = float(fetch.LodBias()) / 32.0f;  // MipMapLodBias stores bias * 32
  // A resolve destination with host levels (HostTexture::extra_levels) has
  // the guest's one level: a filtered fetch reaches the host level whose
  // texel is the guest's, blending between them; a point fetch stays at the
  // guest texel it hits.
  if (lod_scale.Factor() > 1.0f) {
    desc.minLOD = 0.0f;
    if (desc.minFilter == plume::RenderFilter::LINEAR) {
      desc.maxLOD = std::log2(lod_scale.Factor());
      desc.mipmapMode = plume::RenderMipmapMode::LINEAR;
    } else {
      desc.maxLOD = 0.0f;
    }
  }
  // BorderColor stores 0 for a zero colour and 1 (white) for any other.
  switch (fetch.BorderColor()) {
  case 1:
    desc.borderColor = plume::RenderBorderColor::OPAQUE_WHITE;
    break;
  case 0:
    desc.borderColor = plume::RenderBorderColor::TRANSPARENT_BLACK;
    break;
  default:
    desc.borderColor = plume::RenderBorderColor::OPAQUE_BLACK;
    break;
  }

  uint64_t key = uint64_t(desc.addressU) | uint64_t(desc.addressV) << 4 |
                 uint64_t(desc.addressW) << 8 | uint64_t(desc.magFilter) << 12 |
                 uint64_t(desc.minFilter) << 14 | uint64_t(desc.mipmapMode) << 16 |
                 uint64_t(desc.maxAnisotropy) << 18 | uint64_t(fetch.MinMipLevel()) << 24 |
                 uint64_t(fetch.MaxMipLevel()) << 28 | uint64_t(fetch.MipFilter()) << 32 |
                 uint64_t(uint32_t(fetch.LodBias()) & 0x3FF) << 34 | uint64_t(desc.borderColor) << 44 |
                 uint64_t(lod_scale.code) << 48;
  auto it = g_samplers.find(key);
  if (it != g_samplers.end())
    return it->second;

  auto &s = State();
  uint32_t slot = UINT32_MAX;
  for (uint32_t i = kFirstDynamicSamplerSlot; i < kSamplerDescriptorCount; ++i) {
    if (!s.sampler_slot_used[i]) {
      slot = i;
      break;
    }
  }
  if (slot == UINT32_MAX) {
    static bool logged = false;
    if (!logged) {
      RGPU_WARN("Sampler table full; falling back to linear clamp");
      logged = true;
    }
    return kLinearClampSamplerSlot;
  }
  auto sampler = s.device->createSampler(desc);
  if (!sampler)
    return kLinearClampSamplerSlot;
  s.sampler_slot_used[slot] = true;
  s.sampler_set->setSampler(slot, sampler.get());
  g_sampler_objects.push_back(std::move(sampler));
  g_samplers.emplace(key, slot);
  return slot;
}

} // namespace

} // namespace renut::gpu
