/**
 * @file    gpu/render/graphics_options.h
 * @brief   Graphics quality beyond the console's (Nuts&Bolts/Graphics):
 *          shadow map and ambient occlusion resolution, anisotropic
 *          filtering.
 *
 * - renut_shadow_resolution: the shadow maps' render scale. The sun's two
 *   cascades are 1024x1024 and 768x768 D24S8 targets (512x512 in split
 *   screen; sub_822998C8), and the local lights render 512x512 D24S8 depth
 *   converted in a 512x512 16_16_FLOAT target into a 2048x2048 atlas
 *   (sub_823B3CE0); by default they follow the render resolution.
 * - renut_ambient_occlusion: the SSAO pass (sub_8235FA90) renders at half
 *   the screen, into 640x360 targets at EDRAM base 1864; Full renders it at
 *   the screen's resolution.
 * - renut_anisotropic_filtering: the least a texture's linear minification
 *   filters anisotropically.
 * - renut_post_process_precision: the 8_8_8_8 targets of half the screen or
 *   less (bloom, depth of field, SSAO) as 16-bit unorm on the host. The bloom
 *   (bright pass at 640x360 from the 8-bit scene, then 320x180 down to 80x45,
 *   added at weights 1, 1.2, 1.6 and 2) carries a faint glow in 1/255 steps;
 *   upscaled to the screen they show as blobs that slide over dark areas as
 *   the camera moves. A resolve's destination takes its source's host format,
 *   so the passes reading them see the finer values too.
 * - renut_depth_of_field: the fog object's full-screen composite
 *   (sub_82217628; VS 950f5ca8b083fa7c, PS a5e26fb9039da3bf in the main
 *   game) blends the sharp scene toward the blurred one by
 *   min(saturate(distance outside the focus range * c192.x near / c192.y
 *   far), c194.w), c194.w being the most blur (0.8). Reduced halves the
 *   scales and the cap in that draw's constants, Off zeroes them.
 * - renut_bloom: the glow composite (ps_981a6a58f2abfa42) adds the scene's
 *   four bloom levels at weights c0 = (1, 1.2, 1.6, 2); Reduced halves them in
 *   that draw's constants, Off zeroes them.
 * - renut_msaa: the sample count of the targets the game multisamples (its
 *   D3DMULTISAMPLE_TYPE in the surface header, SurfaceInfo bits 16-17; the
 *   scene and its depth). Game keeps it, Off renders them single-sampled,
 *   2x/4x/8x give them that many samples. Everything that reads those targets
 *   follows the host count: draws' pipelines take their first target's, color
 *   resolves are hardware resolves of any count, and the depth resolve reads
 *   sample 0 of a multisampled view. A count the GPU can't do for the
 *   target's format and the depth format steps down to one it can, so a
 *   draw's color and depth targets always agree.
 * - renut_anti_aliasing: post-process anti-aliasing of the finished frame,
 *   in the present pass (present_ps.hlsl) as it scans the game's front
 *   buffer out to the window: FXAA in the frame's own texels, or SMAA 1x in
 *   passes of its own before it (backend/smaa.h). The overlay is drawn after
 *   it, so only the game's image (HUD included) is smoothed.
 */
#pragma once

#include <cstdint>

#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/render/render_scale.h"

namespace renut::gpu {

// Reads the options; the rest use what it read. Once a frame, with the render
// resolution (LatchResolutionScaleLocked), so a frame never mixes them.
void LatchGraphicsOptionsLocked();

// The render scale of the target 'desc' describes, given the render
// resolution's.
RenderScale SurfaceRenderScale(const d3d::SurfaceDesc &desc, RenderScale resolution);

// The host format of the color target 'desc' describes.
plume::RenderFormat SurfaceColorFormat(const d3d::SurfaceDesc &desc);

// The host sample count of the target 'desc' describes, whose host format is
// 'format': the game's, or renut_msaa's for one the game multisamples.
plume::RenderSampleCounts SurfaceSampleCount(const d3d::SurfaceDesc &desc, plume::RenderFormat format);

// renut_anti_aliasing as last latched, for the present pass (any thread).
constexpr uint32_t kPostAntiAliasingOff = 0;
constexpr uint32_t kPostAntiAliasingFxaa = 1;
constexpr uint32_t kPostAntiAliasingSmaa = 2;
uint32_t PostAntiAliasing();

// The anisotropy linear minification gets at least (1 = the game's own).
uint32_t MinAnisotropy();

// How strong the depth of field blur is, as a share of the game's (1 = the
// game's own, 0 = off).
float DepthOfFieldStrength();

// How strong the bloom (glow) is, as a share of the game's (1 = the game's
// own, 0 = off).
float BloomStrength();

} // namespace renut::gpu
