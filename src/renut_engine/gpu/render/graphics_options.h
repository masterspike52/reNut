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

// The sample counts targets can have under renut_msaa as last latched, a bit
// each (1, 2, 4, 8); any thread.
uint32_t SampleCountsInUse();

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
// index: 0 none, 1 bilinear, 2-7 four taps kShadowSpreadTexels[code] texels out.
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
