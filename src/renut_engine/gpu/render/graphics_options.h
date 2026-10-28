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
 * - renut_reflection_resolution: the water reflection's targets (320x180,
 *   4x multisampled, made by sub_8237BF58) render at the scene's scale, or 2x
 *   that (half the screen). Its resolve destination takes the source's scale,
 *   which the water's fetches divide out (kResolutionScaleShift).
 * - renut_shadow_softness: N&B shadows a pixel with binary depth compares:
 *   its materials take 8 taps of a rotated Poisson disk per sun cascade
 *   (sub_8237C6B8 fills the 32x32 rotation noise), its deferred passes one,
 *   each `depth >= reference` (Sge) or `>` (Sgt) on the fetched depth. The
 *   recompiler keeps each depth-like fetch's coordinate, texture and sampler
 *   for the register lanes it lands in (renutTapLanes in
 *   shader_recompiler.cpp) and re-emits a compare of several such lanes as
 *   renutShadowCompare (shader_common.h), which for a texture flagged here
 *   (kShadowFilterShift on the descriptor index of a resolve from a shadow
 *   map) filters the compare over the texels around the tap, and gives the
 *   binary compare otherwise. The deferred passes' lone compares, which the
 *   game takes as yes or no, stay binary.
 * - renut_alpha_to_coverage: an alpha-tested draw (RB_COLORCONTROL alpha test
 *   GREATER or GEQUAL) into a multisampled target without blending turns its
 *   test into coverage: the shader writes alpha sharpened around the reference
 *   (kAlphaTestToCoverage on the alpha test function, alphaTest in
 *   shader_common.h) instead of discarding, and the pipeline has alpha to
 *   coverage on.
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

// Whether 'desc' is one of the shadow maps (whose resolves take the shadow
// filter, see renut_shadow_softness).
bool IsShadowMapSurface(const d3d::SurfaceDesc &desc);

// renut_shadow_softness, as a level: off (the game's binary compares), Soft,
// Softer, Softest.
constexpr uint32_t kShadowFilterOff = 0;
constexpr uint32_t kShadowFilterBilinear = 1;
constexpr uint32_t kShadowFilterWide = 2;
constexpr uint32_t kShadowFilterWidest = 3;
uint32_t ShadowFilter();

// The filter a shadow map's compares get, in bits 29-31 of its 2D descriptor
// index (RENUT_SHADOW_FILTER_SHIFT in shader_common.h): 0 none, 1 a bilinear
// compare, 2 to 7 four of them kShadowSpreadTexels[code] guest texels out.
constexpr uint32_t kShadowFilterShift = 29;
constexpr uint32_t kShadowFilterCodeBilinear = 1;
constexpr float kShadowSpreadTexels[8] = {0.0f, 0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};

// renut_alpha_to_coverage.
bool AlphaToCoverage();

// renut_ssao (ambient_occlusion.h): 0 off, 1 low, 2 medium, 3 high.
uint32_t AmbientOcclusionQuality();

// On SharedConstants::alpha_test_func (alphaTest in shader_common.h): the
// test becomes coverage (RENUT_ALPHA_TO_COVERAGE).
constexpr uint32_t kAlphaTestToCoverage = 0x10;

} // namespace renut::gpu
