#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/render/render_scale.h"
#include "renut_engine/gpu/render/texture_layout.h"

namespace renut::gpu {

struct HostTexture {
  u32 guest_va = 0;
  u32 desc[6] = {};  // the header fetch constant bits that define the data

  TextureDimension dimension = TextureDimension::Tex2D;
  GpuTextureFormat guest_format = GpuTextureFormat::Fmt8_8_8_8;
  TextureFormatInfo info;
  uint32_t width = 0;       // guest size
  uint32_t height = 0;
  uint32_t depth = 1;       // 3D depth, or array slices (6 for cubes)
  // Resolve destinations: the host texture is 'scale' times the guest width
  // and height, the render scale of the targets resolved into it.
  RenderScale scale;
  uint32_t mip_levels = 1;
  // Host levels past the guest's one for a scaled 2D color resolve destination
  // (UpdateResolveMipsLocked), sampled up to LOD log2(scale).
  uint32_t extra_levels = 0;
  // Level 0 without the fetch swizzle, which the levels are made from.
  std::unique_ptr<plume::RenderTextureView> level0_view;
  uint32_t level0_slot = UINT32_MAX;
  // A stacked 2D texture: a host 2D array (every layer in every mip), though
  // the shaders fetch it with 3D fetches (dimension k3D).
  bool stacked = false;

  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  plume::RenderFormat host_format = plume::RenderFormat::UNKNOWN;
  uint32_t alloc_width = 0;   // host texture size (block-aligned for BC, scaled)
  uint32_t alloc_height = 0;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  uint32_t slot = UINT32_MAX;  // bindless texture slot
  bool render_target = false;  // created as a color target (resolve destination)
  // What 'view' was made with; fetch constants with another swizzle over the
  // same data get views of their own (SlotForSwizzle).
  plume::RenderTextureViewDesc view_desc;
  u32 view_swizzle = 0;
  bool view_swap_red_blue = false;
  struct SwizzleView {
    u32 swizzle = 0;
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t slot = UINT32_MAX;
  };
  std::vector<SwizzleView> swizzle_views;
  // A stacked texture's layer 0 as a plain 2D view per swizzle, for 2D fetches.
  std::vector<SwizzleView> layer_views;

  bool gpu_written = false;     // content comes from a Resolve, not guest memory
  // Resolved from a shadow map (HostSurface::shadow_map): its descriptor index
  // takes the shadow filter (kShadowFilterShift).
  bool shadow_map = false;
  bool uploaded = false;
  uint64_t content_hash = 0;
  uint64_t checked_frame = 0;
  // The last frame it was bound or resolved into (EvictIdleTexturesLocked).
  uint64_t used_frame = 0;
  // Linear (CPU-written) textures, see BindTextureLocked:
  bool unlocked = false;       // D3D::UnlockResource since the last check
  uint64_t unlock_frame = 0;   // frame of the last unlock
  bool lock_driven = false;    // written between Lock and Unlock (the font cache)
  bool dynamic = false;        // changes all the time (movie frames)
  uint32_t changes = 0;        // checks in a row that saw the content change
  uint64_t verified_frame = 0; // last periodic full check
  // MenuColourGeneration at the last upload: a menu panel candidate
  // (MenuPanelCandidate) is uploaded again when the panels' colour changes.
  uint32_t menu_colour = 0;
  // What SampleHash / ContentHash read: the base level's and the mip chain's
  // first page and extent in bytes, fixed by desc, worked out at the first check.
  bool sample_known = false;
  uint32_t sample_pages[2] = {};
  uint32_t sample_sizes[2] = {};

  // Depth resolve destinations: the guest words the resolve wrote, the
  // framebuffer over both targets and how many resolves went into it.
  std::unique_ptr<HostTexture> words;  // a bare target: texture and layout
  std::unique_ptr<plume::RenderFramebuffer> depth_framebuffer;
  uint64_t resolves = 0;
  // A texture over a depth resolve's memory: the resolve its words were
  // copied from last.
  uint64_t copied_resolve = 0;
};

struct TextureBinding {
  uint32_t slot = 0;
  TextureDimension dimension = TextureDimension::Tex2D;
  bool stacked = false;  // a 2D array fetched as 3D (dimension Tex3D)
  uint32_t layer_slot = 0;  // stacked: layer 0 as a 2D texture (HostTexture::layer_views)
  RenderScale scale;     // HostTexture::scale
  // The scale its host levels reach (HostTexture::extra_levels), 1x without
  // them; the sampler takes it (SamplerSlotLocked).
  RenderScale lod_scale;
  bool shadow_map = false;  // HostTexture::shadow_map
};

// Flags on a stacked texture's 3D descriptor index (RENUT_STACKED_* in
// shader_common.h); the layer wraps when the fetch constant's Z address does.
constexpr uint32_t kStackedTextureBit = 0x80000000u;
constexpr uint32_t kStackedLayerLinearBit = 0x40000000u;
constexpr uint32_t kStackedLayerWrapBit = 0x20000000u;
// Any descriptor index: bits 26-28 hold HostTexture::scale's code, which the
// shaders divide the texture's size by (RENUT_RESOLUTION_SCALE_SHIFT).
constexpr uint32_t kResolutionScaleShift = 26;
// The most host levels a resolve destination gets (ExtraLevels).
constexpr uint32_t kMaxExtraLevels = 3;

// Starts a new frame for content checks.
void AdvanceTextureFrameLocked();

// D3D::UnlockResource: the CPU has written the resource at 'resource_va'. Any
// thread; does not need State().mutex.
void NoteTextureUnlocked(u32 resource_va);

// Reads the guest data of uploads recorded with a deferred read (linear
// textures) into their staging memory. Before the command list is submitted.
void FlushTextureFillsLocked();

// Bindless slot for the texture bound as 'texture_va'. Falls back to the null
// texture of the fetch constant's dimension when there is nothing to bind.
TextureBinding BindTextureLocked(u32 texture_va, const TextureFetch &device_fetch);

// The guest memory a texture's data spans in 4 KiB physical pages: [0] its base
// level, [1] its mip chain (a zero page or size is absent).
void TextureGuestExtent(const TextureFetch &fetch, uint32_t pages[2], uint32_t sizes[2]);

// The GPU-written color target of 'format' and 'scale' a resolve into the guest
// texture writes, made as needed; 'header_format': the header when recorded.
HostTexture *GetResolveTextureLocked(u32 texture_va, plume::RenderFormat format, RenderScale scale,
                                     const d3d::TextureFetchConstant *header_format = nullptr);

// The same for a depth resolve: a kDepthResolveFormat target plus the guest
// words (kDepthResolveWordsFormat) 8_8_8_8 views read; bumps 'resolves'.
HostTexture *GetDepthResolveTextureLocked(u32 texture_va, RenderScale scale,
                                          const d3d::TextureFetchConstant *header_format = nullptr);

// Existing host texture for a guest texture VA, or nullptr.
HostTexture *FindHostTextureLocked(u32 texture_va);

void ReleaseHostTextureLocked(u32 texture_va);

// The game freed 'size' bytes of guest physical memory at 'physical': the host
// textures whose data was there (its level section pools') go.
void ReleaseHostTexturesInMemoryLocked(u32 physical, u32 size);

void TransitionTextureLocked(HostTexture *texture, plume::RenderTextureLayout layout);

// Rebuilds a resolve destination's host levels over 'rect' (host texels) of
// level 0, leaving it SHADER_READ and the command list's pipeline state changed.
void UpdateResolveMipsLocked(HostTexture *texture, const plume::RenderRect &rect);

// A single-level color target of a resolve destination's format and host size,
// for multisampled resolves into one with host levels; kept for reuse.
HostTexture *ResolveStagingLocked(const HostTexture *dest);

// Bindless sampler slot matching the device fetch constant's sampler state,
// sampling host levels up to LOD log2(lod_scale).
uint32_t SamplerSlotLocked(const TextureFetch &device_fetch, RenderScale lod_scale = {});

} // namespace renut::gpu
