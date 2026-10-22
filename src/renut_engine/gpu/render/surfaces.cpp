/**
 * @file    gpu/render/surfaces.cpp
 */
#include "renut_engine/gpu/render/surfaces.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/render/graphics_options.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/guest/widescreen.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/video.h"

REXCVAR_DEFINE_STRING(renut_resolution, "720p", "Nuts&Bolts/Graphics",
                      "Internal render resolution, by its height: 480p, 720p (the game's own), "
                      "1080p, 1440p or 4K (2160 lines). Applies from the next frame.")
    .allowed({"480p", "720p", "1080p", "1440p", "4K"});

namespace renut::gpu {

namespace {

// ResolutionScale, once latched.
RenderScale g_resolution_scale;
bool g_resolution_latched = false;

struct FramebufferKey {
  const HostSurface *colors[d3d::kMaxRenderTargets] = {};
  uint64_t color_generations[d3d::kMaxRenderTargets] = {};
  const HostSurface *depth = nullptr;
  uint64_t depth_generation = 0;

  bool operator==(const FramebufferKey &o) const {
    for (uint32_t i = 0; i < d3d::kMaxRenderTargets; ++i) {
      if (colors[i] != o.colors[i] || color_generations[i] != o.color_generations[i])
        return false;
    }
    return depth == o.depth && depth_generation == o.depth_generation;
  }
};

struct CachedFramebuffer {
  FramebufferKey key;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
};

// A D3DSurface is a view of EDRAM: its base tile (RB_COLOR_INFO /
// RB_DEPTH_INFO bits 0-11), pitch (RB_SURFACE_INFO bits 0-13) and sample
// count. Surface objects over the same EDRAM with the same layout see the same
// pixels on the GPU, and N&B relies on it: the depth surface its after-effects
// pass binds is a second object over the scene's depth buffer (the character
// holograms test against it), and each hologram renders through surfaces of
// its own at the same EDRAM bases. So host targets belong to the EDRAM view,
// shared by every surface object over it, as long as they also agree on the
// host format, render scale and host sample count: graphics_options renders
// some surfaces at another scale (the post-processing buffers at 1x), and a
// view used by surfaces of both kinds would otherwise be rebuilt at every
// switch; a new renut_msaa gives new targets from the next frame.
uint64_t TargetKey(const d3d::SurfaceDesc &desc, bool depth, plume::RenderFormat format,
                   RenderScale scale, plume::RenderSampleCounts samples) {
  const u32 info = desc.SurfaceInfo;
  return uint64_t(u32(desc.ColorInfo) & 0xFFF) | uint64_t(info & 0x3FFF) << 12 |
         uint64_t((info >> 16) & 3) << 26 | uint64_t(depth) << 28 |
         uint64_t(scale.code & 7) << 29 | uint64_t(u32(format) & 0xFFFFFF) << 32 |
         uint64_t(samples & 0x1F) << 56;
}

std::unordered_map<uint64_t, std::unique_ptr<HostSurface>> g_targets;
// Surface VA -> its target's key, and how many surfaces use each target.
std::unordered_map<u32, uint64_t> g_surface_keys;
std::unordered_map<uint64_t, uint32_t> g_target_users;
std::vector<CachedFramebuffer> g_framebuffers;
uint64_t g_next_generation = 1;
// SetTilingExtentLocked; 0 x 0 when not tiling.
uint32_t g_tiling_width = 0;
uint32_t g_tiling_height = 0;

// GetHostSurfaceLocked's targets by surface, its header and the tiling, within
// a generation: every draw looks up each of its targets, two maps each. A
// generation ends with the frame (the options latch) and whenever a target is
// made or retired or a surface's target changes.
uint64_t g_surface_generation = 1;
struct SurfaceMemo {
  uint64_t generation = 0;
  u32 va = 0;
  u32 tiling[2] = {};
  u32 desc[6] = {};
  HostSurface *surface = nullptr;
};
SurfaceMemo g_surface_memo[256];

// Drops cached framebuffers that reference 'surface'.
void ForgetFramebuffers(const HostSurface *surface) {
  for (size_t i = 0; i < g_framebuffers.size();) {
    const FramebufferKey &key = g_framebuffers[i].key;
    bool uses = key.depth == surface;
    for (const HostSurface *color : key.colors)
      uses = uses || color == surface;
    if (uses) {
      Video::RetireLocked(std::move(g_framebuffers[i].framebuffer));
      g_framebuffers[i] = std::move(g_framebuffers.back());
      g_framebuffers.pop_back();
    } else {
      ++i;
    }
  }
}

// Retires the target and everything made over it (deferred past in-flight
// frames).
void RetireTarget(HostSurface &surface) {
  ++g_surface_generation;
  ForgetFramebuffers(&surface);
  if (surface.sample_slot != UINT32_MAX)
    Video::FreeTextureSlotLocked(surface.sample_slot);
  surface.sample_slot = UINT32_MAX;
  Video::RetireLocked(std::move(surface.sample_view));
  Video::RetireLocked(std::move(surface.texture));
}

// At least 'min_width' x 'min_height' (see SetTilingExtentLocked), at 'scale'.
bool BuildTarget(HostSurface &surface, const d3d::SurfaceDesc &desc, uint32_t min_width,
                 uint32_t min_height, RenderScale scale) {
  surface.size_word = desc.Size;
  surface.info_word = desc.SurfaceInfo;
  surface.format_word = desc.Format;
  surface.width = std::max(d3d::SurfaceWidth(surface.size_word), min_width);
  surface.height = std::max(d3d::SurfaceHeight(surface.size_word), min_height);
  surface.scale = scale;
  std::fill(std::begin(surface.profile_labels), std::end(surface.profile_labels), UINT32_MAX);
  const uint32_t host_width = surface.scale.Size(surface.width);
  const uint32_t host_height = surface.scale.Size(surface.height);
  surface.depth = d3d::IsDepthFormat(surface.format_word);
  if (surface.depth) {
    surface.format = DepthTargetFormat();
  } else {
    surface.format = SurfaceColorFormat(desc);
  }
  surface.samples = SurfaceSampleCount(desc, surface.format);

  plume::RenderDevice *device = Video::Device();
  const plume::RenderMultisampling multisampling(surface.samples);
  // Sampled too: resolves read them, and so does scan-out of a bound target.
  if (surface.depth) {
    surface.texture = device->createTexture(plume::RenderTextureDesc::DepthTarget(
        host_width, host_height, surface.format, multisampling));
  } else {
    surface.texture = device->createTexture(plume::RenderTextureDesc::ColorTarget(
        host_width, host_height, surface.format, multisampling));
  }
  surface.layout = plume::RenderTextureLayout::UNKNOWN;
  surface.generation = g_next_generation++;
  if (!TextureCreated(surface.texture.get())) {
    surface.texture.reset();
    if (!ReportDeviceLost(device)) {
      RGPU_ERROR("Surface {:08X}: {}x{} {} target creation failed", surface.guest_va,
                 host_width, host_height, surface.depth ? "depth" : "color");
    }
    return false;
  }
  surface.texture->setName(surface.depth ? "renut depth surface" : "renut color surface");
  RGPU_DEBUG("Surface {:08X}: {}x{} (x{:.3g} scale) {} format {} x{} samples", surface.guest_va,
             surface.width, surface.height, surface.scale.Factor(), surface.depth ? "depth" : "color",
             u32(surface.format), u32(surface.samples));
  return true;
}

} // namespace

RenderScale ResolutionScale() {
  return g_resolution_scale;
}

void LatchResolutionScaleLocked() {
  ++g_surface_generation;
  LatchGraphicsOptionsLocked();
  const std::string resolution = REXCVAR_GET(renut_resolution);
  // Codes of RenderScale.
  const RenderScale scale{resolution == "480p"    ? 1u
                          : resolution == "1080p" ? 2u
                          : resolution == "1440p" ? 3u
                          : resolution == "4K"    ? 4u
                                                  : 0u};
  if (!g_resolution_latched || scale != g_resolution_scale) {
    RGPU_INFO("Render resolution {}x{} ({:.3g}x)", scale.Size(GuestScreenWidth()), scale.Size(720),
              scale.Factor());
    g_resolution_scale = scale;
    g_resolution_latched = true;
  }
}

void SetTilingExtentLocked(uint32_t width, uint32_t height) {
  g_tiling_width = width;
  g_tiling_height = height;
}

// A surface stops using its target (destroyed, or its header now describes
// another view); the last user retires it.
void UnuseTarget(uint64_t key) {
  ++g_surface_generation;
  auto users = g_target_users.find(key);
  if (users == g_target_users.end() || --users->second)
    return;
  g_target_users.erase(users);
  if (auto it = g_targets.find(key); it != g_targets.end()) {
    RetireTarget(*it->second);
    g_targets.erase(it);
  }
}

HostSurface *FindHostSurfaceLocked(u32 surface_va) {
  auto key = g_surface_keys.find(surface_va);
  if (key == g_surface_keys.end())
    return nullptr;
  auto it = g_targets.find(key->second);
  return it != g_targets.end() ? it->second.get() : nullptr;
}

void ColorAliasesLocked(const HostSurface *surface, std::vector<HostSurface *> &out) {
  out.clear();
  if (!surface || surface->depth)
    return;
  // TargetKey: the view in the low 32 bits, the host format above.
  constexpr uint64_t kViewMask = 0xFFFFFFFFull;
  uint64_t own = 0;
  bool found = false;
  for (const auto &[key, target] : g_targets) {
    if (target.get() == surface) {
      own = key;
      found = true;
      break;
    }
  }
  if (!found)
    return;
  for (const auto &[key, target] : g_targets) {
    if (target && target.get() != surface && target->texture && !target->depth &&
        (key & kViewMask) == (own & kViewMask))
      out.push_back(target.get());
  }
}

HostSurface *GetHostSurfaceLocked(u32 surface_va) {
  if (!surface_va)
    return nullptr;
  // Targets made before the first Swap are of the configured scale too.
  if (!g_resolution_latched)
    LatchResolutionScaleLocked();
  const auto *desc = GuestPtr<const d3d::SurfaceDesc>(surface_va + sizeof(d3d::Resource));
  if (!desc)
    return nullptr;
  u32 desc_words[6];
  static_assert(sizeof(desc_words) == sizeof(d3d::SurfaceDesc));
  std::memcpy(desc_words, desc, sizeof(desc_words));
  SurfaceMemo &memo = g_surface_memo[(surface_va * 0x9E3779B1u) >> 24];
  if (memo.generation == g_surface_generation && memo.va == surface_va &&
      memo.tiling[0] == g_tiling_width && memo.tiling[1] == g_tiling_height &&
      std::memcmp(memo.desc, desc_words, sizeof(desc_words)) == 0) {
    // As below, for a target that covers the surface already.
    memo.surface->size_word = desc->Size;
    memo.surface->info_word = desc->SurfaceInfo;
    memo.surface->format_word = desc->Format;
    return memo.surface;
  }

  const bool depth = d3d::IsDepthFormat(desc->Format);
  const plume::RenderFormat format = depth ? DepthTargetFormat() : SurfaceColorFormat(*desc);
  // The render resolution, or the finer one graphics_options gives it.
  const RenderScale scale = SurfaceRenderScale(*desc, g_resolution_scale);
  // ... and the sample count renut_msaa gives it.
  const uint64_t key = TargetKey(*desc, depth, format, scale, SurfaceSampleCount(*desc, format));
  auto [bound, first_use] = g_surface_keys.try_emplace(surface_va, key);
  if (first_use) {
    ++g_target_users[key];
  } else if (bound->second != key) {
    // The header was rewritten (or the VA reused without Destroy reaching
    // us): the surface is a view of other EDRAM now.
    UnuseTarget(bound->second);
    bound->second = key;
    ++g_target_users[key];
  }

  auto &slot = g_targets[key];
  // The view's target covers every surface over it, and every tile while
  // tiling (SetTilingExtentLocked).
  uint32_t min_width = std::max(g_tiling_width, d3d::SurfaceWidth(desc->Size));
  uint32_t min_height = std::max(g_tiling_height, d3d::SurfaceHeight(desc->Size));
  if (slot && slot->texture) {
    // The surface bound now: its guest size (Targets::guest_width) and D3D
    // format (D24S8 or D24FS8 for a depth resolve).
    slot->size_word = desc->Size;
    slot->info_word = desc->SurfaceInfo;
    slot->format_word = desc->Format;
    if (slot->width >= min_width && slot->height >= min_height && slot->scale == scale) {
      memo.generation = g_surface_generation;
      memo.va = surface_va;
      memo.tiling[0] = g_tiling_width;
      memo.tiling[1] = g_tiling_height;
      std::memcpy(memo.desc, desc_words, sizeof(desc_words));
      memo.surface = slot.get();
      return slot.get();
    }
    // A larger surface over the view, or a larger tiling: grow it, keeping
    // the size the others gave it (what it held is lost). (Another scale is
    // another target: TargetKey.)
    min_width = std::max(min_width, slot->width);
    min_height = std::max(min_height, slot->height);
    static uint32_t logged = 0;
    if (logged++ < 32) {
      RGPU_WARN("Surface {:08X}: target rebuilt {}x{} -> {}x{}{}: what it held is lost", surface_va,
                slot->width, slot->height, min_width, min_height,
                slot->scale != scale ? " (scale changed)" : "");
    }
    RetireTarget(*slot);
  } else if (!slot) {
    slot = std::make_unique<HostSurface>();
    slot->guest_va = surface_va;
  }
  if (!BuildTarget(*slot, *desc, min_width, min_height, scale))
    return nullptr;
  // No defined content yet; D3D12 wants a new target discarded (or cleared)
  // before anything else uses it, and a resolve may read it before any draw.
  if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
    TransitionSurfaceLocked(slot.get(), slot->depth ? plume::RenderTextureLayout::DEPTH_WRITE
                                                    : plume::RenderTextureLayout::COLOR_WRITE);
    cmd->discardTexture(slot->texture.get());
  }
  return slot.get();
}

void CoverHostExtentLocked(HostSurface *surface, uint32_t host_width, uint32_t host_height) {
  if (!surface || !surface->texture)
    return;
  // The guest size, at the target's own scale, whose host size covers 'host'.
  const auto covering = [surface](uint32_t host, uint32_t guest) {
    guest = std::max(guest, uint32_t(float(host) / surface->scale.Factor()));
    while (surface->scale.Size(guest) < host)
      ++guest;
    return guest;
  };
  const uint32_t width = covering(host_width, surface->width);
  const uint32_t height = covering(host_height, surface->height);
  if (width == surface->width && height == surface->height)
    return;

  const plume::RenderMultisampling multisampling(surface->samples);
  const uint32_t new_width = surface->scale.Size(width);
  const uint32_t new_height = surface->scale.Size(height);
  plume::RenderDevice *device = Video::Device();
  std::unique_ptr<plume::RenderTexture> texture =
      surface->depth ? device->createTexture(plume::RenderTextureDesc::DepthTarget(
                           new_width, new_height, surface->format, multisampling))
                     : device->createTexture(plume::RenderTextureDesc::ColorTarget(
                           new_width, new_height, surface->format, multisampling));
  if (!TextureCreated(texture.get())) {
    if (!ReportDeviceLost(device)) {
      RGPU_ERROR("Surface {:08X}: growing the {} target to {}x{} failed", surface->guest_va,
                 surface->depth ? "depth" : "color", new_width, new_height);
    }
    return;
  }
  texture->setName(surface->depth ? "renut depth surface" : "renut color surface");
  RGPU_DEBUG("Surface {:08X}: {}x{} grown to {}x{} to cover a framebuffer of {}x{}",
             surface->guest_va, surface->width, surface->height, width, height, host_width,
             host_height);

  // What it held stays in the corner it covered. (A depth copy of part of a
  // subresource: Vulkan's image copies allow it; this runs on Vulkan only.)
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (cmd && surface->layout != plume::RenderTextureLayout::UNKNOWN) {
    TransitionSurfaceLocked(surface, plume::RenderTextureLayout::COPY_SOURCE);
    cmd->barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::COPY_DEST));
    const plume::RenderBox box(0, 0, int32_t(surface->scale.Size(surface->width)),
                               int32_t(surface->scale.Size(surface->height)));
    cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(texture.get()),
                           plume::RenderTextureCopyLocation::Subresource(surface->texture.get()),
                           0, 0, 0, &box);
    surface->layout = plume::RenderTextureLayout::COPY_DEST;
  } else {
    surface->layout = plume::RenderTextureLayout::UNKNOWN;
  }

  ForgetFramebuffers(surface);
  if (surface->sample_slot != UINT32_MAX)
    Video::FreeTextureSlotLocked(surface->sample_slot);
  surface->sample_slot = UINT32_MAX;
  Video::RetireLocked(std::move(surface->sample_view));
  Video::RetireLocked(std::move(surface->texture));
  surface->texture = std::move(texture);
  surface->width = width;
  surface->height = height;
  surface->generation = g_next_generation++;
}

void SurfaceMemoryLocked(size_t &targets, uint64_t &bytes) {
  targets = 0;
  bytes = 0;
  for (const auto &[key, surface] : g_targets) {
    if (!surface || !surface->texture)
      continue;
    ++targets;
    const uint64_t texel = plume::RenderFormatSize(surface->format);
    bytes += uint64_t(surface->scale.Size(surface->width)) * surface->scale.Size(surface->height) *
             texel * surface->samples;
  }
}

void ReleaseHostSurfaceLocked(u32 surface_va) {
  auto it = g_surface_keys.find(surface_va);
  if (it == g_surface_keys.end())
    return;
  const uint64_t key = it->second;
  g_surface_keys.erase(it);
  UnuseTarget(key);  // (a new generation)
}

uint32_t SurfaceSampleSlotLocked(HostSurface *surface) {
  if (!surface || !surface->texture)
    return kInvalidDescriptor;
  if (surface->sample_slot != UINT32_MAX)
    return surface->sample_slot;
  // The depth format views the depth plane.
  surface->sample_view = surface->texture->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(surface->format));
  if (!surface->sample_view)
    return kInvalidDescriptor;
  const uint32_t slot = Video::AllocateTextureSlotLocked();
  if (slot == kInvalidDescriptor)
    return kInvalidDescriptor;
  Video::SetTextureSlotLocked(slot, surface->texture.get(), surface->sample_view.get());
  surface->sample_slot = slot;
  return slot;
}

void TransitionSurfaceLocked(HostSurface *surface, plume::RenderTextureLayout layout) {
  if (!surface || !surface->texture || surface->layout == layout)
    return;
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!cmd)
    return;
  cmd->barriers(layout == plume::RenderTextureLayout::COPY_SOURCE ||
                        layout == plume::RenderTextureLayout::COPY_DEST ||
                        layout == plume::RenderTextureLayout::RESOLVE_SOURCE ||
                        layout == plume::RenderTextureLayout::RESOLVE_DEST
                    ? plume::RenderBarrierStage::COPY
                    : plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(surface->texture.get(), layout));
  surface->layout = layout;
}

plume::RenderFramebuffer *GetFramebufferLocked(HostSurface *const *colors, uint32_t color_count,
                                               HostSurface *depth) {
  FramebufferKey key;
  for (uint32_t i = 0; i < color_count && i < d3d::kMaxRenderTargets; ++i) {
    key.colors[i] = colors[i];
    key.color_generations[i] = colors[i] ? colors[i]->generation : 0;
  }
  key.depth = depth;
  key.depth_generation = depth ? depth->generation : 0;
  for (auto &cached : g_framebuffers) {
    if (cached.key == key)
      return cached.framebuffer.get();
  }

  const plume::RenderTexture *attachments[d3d::kMaxRenderTargets] = {};
  uint32_t count = 0;
  for (uint32_t i = 0; i < color_count && i < d3d::kMaxRenderTargets; ++i) {
    if (!colors[i] || !colors[i]->texture)
      break;  // host framebuffers need contiguous color attachments
    attachments[count++] = colors[i]->texture.get();
  }
  plume::RenderFramebufferDesc desc(attachments, count,
                                    depth && depth->texture ? depth->texture.get() : nullptr);
  auto framebuffer = Video::Device()->createFramebuffer(desc);
  if (!framebuffer) {
    RGPU_ERROR("Framebuffer creation failed ({} color targets, depth {})", count, depth != nullptr);
    return nullptr;
  }
  g_framebuffers.push_back({key, std::move(framebuffer)});
  return g_framebuffers.back().framebuffer.get();
}

} // namespace renut::gpu
