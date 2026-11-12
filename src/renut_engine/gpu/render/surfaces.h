#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include <rex/types.h>

#include "renut_engine/gpu/render/render_scale.h"

namespace renut::gpu {

struct HostSurface {
  u32 guest_va = 0;  // the first surface over the view
  // Header words of the surface over the view bound most recently.
  u32 size_word = 0;
  u32 info_word = 0;
  u32 format_word = 0;

  // In guest pixels; the host target is 'scale' times that on each axis
  // (ResolutionScale when it was built).
  uint32_t width = 0;
  uint32_t height = 0;
  RenderScale scale;
  bool depth = false;
  // One of the shadow maps (IsShadowMapSurface): what is resolved from it is
  // a shadow map too, whose compares renut_shadow_softness filters.
  bool shadow_map = false;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  plume::RenderSampleCounts samples = plume::RenderSampleCount::COUNT_1;

  std::unique_ptr<plume::RenderTexture> texture;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  uint64_t generation = 0;  // bumped on rebuild, part of framebuffer keys

  // The view and bindless slot shaders read the target through (depth
  // resolves); made on first use, see SurfaceSampleSlotLocked.
  std::unique_ptr<plume::RenderTextureView> sample_view;
  uint32_t sample_slot = UINT32_MAX;
};

// Host pixels per guest pixel of every render target and resolve destination,
// from renut_resolution; LatchResolutionScaleLocked latches it once a frame.
RenderScale ResolutionScale();
void LatchResolutionScaleLocked();

// The host target for the D3DSurface at 'surface_va', created or rebuilt as
// needed. nullptr for 0 or when the target cannot be created.
HostSurface *GetHostSurfaceLocked(u32 surface_va);

// Grows the target of 'surface' to at least host_width x host_height, keeping
// its content in the top-left corner (Vulkan only, on the open command list).
void CoverHostExtentLocked(HostSurface *surface, uint32_t host_width, uint32_t host_height);

// Only if one exists already.
HostSurface *FindHostSurfaceLocked(u32 surface_va);

// The other color targets over the same EDRAM view as 'surface' (base tile,
// pitch, samples and scale) in another host format.
void ColorAliasesLocked(const HostSurface *surface, std::vector<HostSurface *> &out);

// Predicated tiling: surfaces used while tiling get targets covering every tile,
// 'width' x 'height' being the tiles' bottom-right corner; 0 x 0 ends tiling.
void SetTilingExtentLocked(uint32_t width, uint32_t height);

// The surface no longer uses its target (guest D3DResource_Destroy); the
// target is retired when no surface over its view remains.
void ReleaseHostSurfaceLocked(u32 surface_va);

// Bindless slot of the surface's target (the depth plane for depth targets,
// multisampled when the target is), or kInvalidDescriptor.
uint32_t SurfaceSampleSlotLocked(HostSurface *surface);

// Moves 'surface' into 'layout' with a barrier on the open command list.
void TransitionSurfaceLocked(HostSurface *surface, plume::RenderTextureLayout layout);

// Framebuffer over up to 4 color targets plus depth; cached per target set.
plume::RenderFramebuffer *GetFramebufferLocked(HostSurface *const *colors, uint32_t color_count,
                                               HostSurface *depth);
} // namespace renut::gpu
