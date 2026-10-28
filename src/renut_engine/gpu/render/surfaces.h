/**
 * @file    gpu/render/surfaces.h
 * @brief   Host render targets behind guest D3DSurfaces.
 *
 * N&B's own D3DDevice_CreateSurface still builds the surface (and does its
 * EDRAM bookkeeping); the renderer attaches a host color or depth target to
 * the EDRAM view the surface describes (base, pitch, samples, format) the
 * first time a surface over it is bound, sized and formatted from the surface
 * header. Surfaces over the same view share the target, as they share EDRAM
 * on the GPU; content lives in it while any of them does, which is what the
 * game relies on between Clear, draws and Resolve.
 * All functions require State().mutex.
 */
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

  // gpu_profile labels of draws with it as the first target: color only,
  // color with depth, depth only. Made on first use.
  uint32_t profile_labels[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
};

// Render resolution: every host render target, and every texture a resolve
// writes, is this many times its guest size on each axis (1 = the game's own
// 720p, 3 = 2160p, 2/3 = 480p). The renderer works in guest pixels and scales
// where it records host viewports, scissors, clears and copies; shaders see a
// scaled texture's guest size (the scale is in its descriptor index, see
// kResolutionScaleShift) and the guest pixel position.
// From the renut_resolution cvar, latched once per frame by
// LatchResolutionScaleLocked (at Swap) so a frame never mixes scales. Targets
// and resolve destinations of another scale are rebuilt on their next use.
RenderScale ResolutionScale();
void LatchResolutionScaleLocked();

// The host target for the D3DSurface at 'surface_va', created or rebuilt as
// needed. nullptr for 0 or when the target cannot be created.
HostSurface *GetHostSurfaceLocked(u32 surface_va);

// Grows the target of 'surface' so its host size is at least host_width x
// host_height, keeping what it held (copied into the top-left corner). For
// Vulkan, whose framebuffer takes the first color target's size and needs every
// attachment to cover it; the console and D3D12 render a pass within the
// smallest one. Uses the open command list; Vulkan only (a partial depth copy).
void CoverHostExtentLocked(HostSurface *surface, uint32_t host_width, uint32_t host_height);

// Only if one exists already.
HostSurface *FindHostSurfaceLocked(u32 surface_va);

// The other color targets over the same EDRAM view as 'surface' (base tile,
// pitch, samples and scale), in another host format: on the console one
// memory, so what a clear writes through one is what the others then hold.
void ColorAliasesLocked(const HostSurface *surface, std::vector<HostSurface *> &out);

// Predicated tiling (D3DDevice_BeginTiling .. EndTiling): the game draws the
// frame into surfaces the size of one EDRAM tile, and the GPU replays the
// draws once per tile with the tile's screen offset, resolving each tile into
// its part of the destination. The host has no EDRAM to fit, so a surface
// used while tiling is backed by a target covering every tile (and keeps that
// size), which the draws and the per-tile resolves address in screen space.
// 'width' x 'height' is the bottom-right corner of the tile rects; 0 x 0 ends
// tiling.
void SetTilingExtentLocked(uint32_t width, uint32_t height);

// The surface no longer uses its target (guest D3DResource_Destroy); the
// target is retired when no surface over its view remains.
void ReleaseHostSurfaceLocked(u32 surface_va);

// Bindless slot of the surface's target (the depth plane for depth targets,
// multisampled when the target is), or kInvalidDescriptor.
uint32_t SurfaceSampleSlotLocked(HostSurface *surface);

// Moves 'surface' into 'layout' with a barrier on the open command list.
void TransitionSurfaceLocked(HostSurface *surface, plume::RenderTextureLayout layout);

// For the memory report: host render targets and the video memory they take
// (estimated from format, size and samples).
void SurfaceMemoryLocked(size_t &targets, uint64_t &bytes);

// Framebuffer over up to 4 color targets plus depth; cached per target set.
plume::RenderFramebuffer *GetFramebufferLocked(HostSurface *const *colors, uint32_t color_count,
                                               HostSurface *depth);

} // namespace renut::gpu
