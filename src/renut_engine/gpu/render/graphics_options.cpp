/**
 * @file    gpu/render/graphics_options.cpp
 */
#include "renut_engine/gpu/render/graphics_options.h"

#include <cstdlib>
#include <string>

#include <rex/cvar.h>
#include <rex/types.h>

#include "renut_engine/gpu/guest/widescreen.h"
#include "renut_engine/gpu/render/format.h"

REXCVAR_DEFINE_STRING(renut_shadow_resolution, "Resolution", "Nuts&Bolts/Graphics",
                      "Shadow map resolution, as a multiple of the console's: Resolution follows "
                      "renut_resolution; 1x is the console's; 2x, 3x and 4x are sharper.")
    .allowed({"Resolution", "1x", "2x", "3x", "4x"});

REXCVAR_DEFINE_STRING(renut_ambient_occlusion, "Half", "Nuts&Bolts/Graphics",
                      "Ambient occlusion (SSAO) resolution: Half the screen's, as on the console, "
                      "or Full.")
    .allowed({"Half", "Full"});

REXCVAR_DEFINE_STRING(renut_anisotropic_filtering, "Game", "Nuts&Bolts/Graphics",
                      "Anisotropic texture filtering: the game's own, or at least 2x, 4x, 8x or "
                      "16x.")
    .allowed({"Game", "2x", "4x", "8x", "16x"});

REXCVAR_DEFINE_STRING(renut_post_process_precision, "High", "Nuts&Bolts/Graphics",
                      "Precision of the bloom, depth of field and SSAO buffers: Game keeps the "
                      "console's 8 bits (faint glow shows as blobs sliding over dark areas), High "
                      "uses 16 bits.")
    .allowed({"Game", "High"});

REXCVAR_DEFINE_STRING(renut_depth_of_field, "Game", "Nuts&Bolts/Graphics",
                      "Depth of field blur on what is out of focus: the game's own, Reduced (half "
                      "as strong) or Off.")
    .allowed({"Game", "Reduced", "Off"});

namespace renut::gpu {

namespace {

// D3DFORMAT low bits of D3DFMT_D24S8 (the shadow maps; the scene's depth is
// D24FS8).
constexpr u32 kFormatD24S8 = 22;
// The SSAO targets' EDRAM base (sub_8235FA90).
constexpr u32 kAmbientOcclusionEdramBase = 1864;
// The game's screen height (widescreen.h).
constexpr u32 kGuestScreenHeight = 720;

bool IsShadowMap(const d3d::SurfaceDesc &desc, u32 width, u32 height) {
  if (width != height)
    return false;
  if ((u32(desc.Format) & 0x3F) == kFormatD24S8)
    return width == 512 || width == 768 || width == 1024;
  // The local lights' depth, converted for their atlas.
  return !d3d::IsDepthFormat(desc.Format) && width == 512 &&
         GpuColorFormat((u32(desc.ColorInfo) >> 16) & 0xF) == GpuColorFormat::Fmt16_16_Float;
}

bool IsAmbientOcclusion(const d3d::SurfaceDesc &desc, u32 width, u32 height) {
  return !d3d::IsDepthFormat(desc.Format) && width == 640 && height == 360 &&
         (u32(desc.ColorInfo) & 0xFFF) == kAmbientOcclusionEdramBase;
}

// A quarter of the screen or less: the post-processing buffers (depth of
// field and bloom, 320x180 down to 20x11 at 1280x720). Their blurs average a
// few taps 2 of their own texels apart, made for buffers box-downsampled from
// the screen; rendered at a higher scale, the buffers keep detail those taps
// step over, so edges come out as ghost copies rather than blurred.
bool IsPostProcessBuffer(const d3d::SurfaceDesc &desc, u32 width, u32 height) {
  return !d3d::IsDepthFormat(desc.Format) && width * 4 <= GuestScreenWidth() &&
         height * 4 <= kGuestScreenHeight;
}

// Half the screen or less, single-sampled, 8_8_8_8: the bloom's bright pass
// (640x360) and the post-processing buffers below it, and the SSAO targets.
bool IsLowPrecisionPostTarget(const d3d::SurfaceDesc &desc) {
  const u32 width = d3d::SurfaceWidth(desc.Size);
  const u32 height = d3d::SurfaceHeight(desc.Size);
  return !d3d::IsDepthFormat(desc.Format) && ((u32(desc.SurfaceInfo) >> 16) & 3) == 0 &&
         width * 2 <= GuestScreenWidth() && height * 2 <= kGuestScreenHeight &&
         GpuColorFormat((u32(desc.ColorInfo) >> 16) & 0xF) == GpuColorFormat::Fmt8_8_8_8;
}

// The options as LatchGraphicsOptionsLocked last read them: the draw path
// asks for every target and sampler of every draw, too often for the
// settings' strings.
struct Latched {
  bool shadow_follows_resolution = true;
  RenderScale shadow_scale;
  bool full_ambient_occlusion = false;
  uint32_t min_anisotropy = 1;
  bool high_precision_post = true;
  float depth_of_field = 1.0f;
};
Latched g_latched;

} // namespace

void LatchGraphicsOptionsLocked() {
  const std::string &shadows = REXCVAR_GET(renut_shadow_resolution);
  g_latched.shadow_follows_resolution = shadows == "Resolution" || shadows.empty();
  g_latched.shadow_scale = {};
  if (!g_latched.shadow_follows_resolution)
    RenderScale::FromRatio(uint32_t(std::atoi(shadows.c_str())), 1, g_latched.shadow_scale);
  g_latched.full_ambient_occlusion = REXCVAR_GET(renut_ambient_occlusion) == "Full";
  const int anisotropy = std::atoi(REXCVAR_GET(renut_anisotropic_filtering).c_str());
  g_latched.min_anisotropy = anisotropy >= 2 ? uint32_t(anisotropy) : 1;
  g_latched.high_precision_post = REXCVAR_GET(renut_post_process_precision) != "Game";
  const std::string &depth_of_field = REXCVAR_GET(renut_depth_of_field);
  g_latched.depth_of_field = depth_of_field == "Off" ? 0.0f : depth_of_field == "Reduced" ? 0.5f : 1.0f;
}

RenderScale SurfaceRenderScale(const d3d::SurfaceDesc &desc, RenderScale resolution) {
  if ((u32(desc.SurfaceInfo) >> 16) & 3)
    return resolution;  // multisampled: the scene's
  const u32 width = d3d::SurfaceWidth(desc.Size);
  const u32 height = d3d::SurfaceHeight(desc.Size);
  if (IsShadowMap(desc, width, height))
    return g_latched.shadow_follows_resolution ? resolution : g_latched.shadow_scale;
  if (g_latched.full_ambient_occlusion && IsAmbientOcclusion(desc, width, height))
    return resolution.Times(2);
  if (IsPostProcessBuffer(desc, width, height))
    return {};  // the console's
  return resolution;
}

plume::RenderFormat SurfaceColorFormat(const d3d::SurfaceDesc &desc) {
  // 16-bit unorm keeps 8-bit's clamping to [0, 1], which the game's blends
  // and shaders were made for.
  if (g_latched.high_precision_post && IsLowPrecisionPostTarget(desc))
    return plume::RenderFormat::R16G16B16A16_UNORM;
  return ColorTargetFormat(GpuColorFormat((u32(desc.ColorInfo) >> 16) & 0xF));
}

uint32_t MinAnisotropy() {
  return g_latched.min_anisotropy;
}

float DepthOfFieldStrength() {
  return g_latched.depth_of_field;
}

} // namespace renut::gpu
