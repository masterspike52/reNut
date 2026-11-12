#include "renut_engine/gpu/render/graphics_options.h"

#include <atomic>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include <rex/setting.h>
#include <rex/types.h>

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/guest/widescreen.h"
#include "renut_engine/gpu/render/ambient_occlusion.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/render/menu_colour.h"

namespace {
struct GraphicsOptionSettings final : rex::SettingOwner {
  GraphicsOptionSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
  REX_SETTING(std::string, renut_shadow_resolution, "Resolution",
              "Shadow map resolution, as a multiple of the console's: Resolution follows "
              "renut_resolution; 1x is the console's; 2x, 3x and 4x are sharper.",
              rex::Allowed("Resolution", "1x", "2x", "3x", "4x"));
  REX_SETTING(std::string, renut_ambient_occlusion, "Half",
              "Ambient occlusion (SSAO) resolution: Half the screen's, as on the console, "
              "or Full.",
              rex::Allowed("Half", "Full"));
  REX_SETTING(std::string, renut_reflection_resolution, "Game",
              "Resolution of the water reflections: the game's (a quarter of the screen "
              "on each axis, at the render resolution's scale) or 2x (half the screen: "
              "sharper reflections, four times the pixels to draw).",
              rex::Allowed("Game", "2x"));
  REX_SETTING(std::string, renut_shadow_softness, "Game",
              "Shadow edges: the game's (each of its shadow taps either lit or not: hard, "
              "stepped edges), Soft (each tap filtered over the shadow map's texels), "
              "Softer or Softest (each over a wider area, the same size in the world near "
              "and far: softer edges, more GPU time).",
              rex::Allowed("Game", "Soft", "Softer", "Softest"));
  REX_SETTING(std::string, renut_ssao, "Off",
              "Ambient occlusion: soft shading where surfaces meet, in corners and creases, "
              "worked out from the scene's depth (N&B's own SSAO is never drawn). Off, Low, "
              "Medium (more samples) or High (more still, at the full resolution).",
              rex::Allowed("Off", "Low", "Medium", "High"));
  REX_SETTING(std::string, renut_anisotropic_filtering, "Game",
              "Anisotropic texture filtering: the game's own, or at least 2x, 4x, 8x or "
              "16x.",
              rex::Allowed("Game", "2x", "4x", "8x", "16x"));
  REX_SETTING(std::string, renut_post_process_precision, "High",
              "Precision of the bloom, depth of field and SSAO buffers: Game keeps the "
              "console's 8 bits (faint glow shows as blobs sliding over dark areas), High "
              "uses 16 bits.",
              rex::Allowed("Game", "High"));
  REX_SETTING(std::string, renut_depth_of_field, "Game",
              "Depth of field blur on what is out of focus: the game's own, Reduced (half "
              "as strong) or Off.",
              rex::Allowed("Game", "Reduced", "Off"));
  REX_SETTING(std::string, renut_bloom, "Game",
              "Glow around bright light and sky: the game's own, Reduced (half as strong) "
              "or Off.",
              rex::Allowed("Game", "Reduced", "Off"));
  REX_SETTING(std::string, renut_msaa, "Game",
              "Multisample anti-aliasing of the scene: the game's own, Off, or 2x, 4x or "
              "8x. More samples smooth edges more but cost GPU time and memory, most at "
              "high resolutions. Applies from the next frame.",
              rex::Allowed("Game", "Off", "2x", "4x", "8x"));
  REX_SETTING(std::string, renut_anti_aliasing, "Off",
              "Anti-aliasing of the finished frame, on top of MSAA: Off, FXAA (fast; "
              "smooths every edge, the HUD's too, and softens the image a little) or SMAA "
              "(follows edges' shapes: smoother edges, text and textures kept sharp, a "
              "little more GPU time).",
              rex::Allowed("Off", "FXAA", "SMAA"));
  REX_SETTING(bool, renut_alpha_to_coverage, false,
              "Smooth the cut-out edges of leaves, grass, fences and other alpha-tested "
              "surfaces with the scene's multisampling (needs MSAA on: Game, 2x, 4x or 8x).");
};
GraphicsOptionSettings &g_options = *new GraphicsOptionSettings();  // never freed, registered by RenutApp::OnConfigure
} // namespace

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

// The water reflection's targets (320x180, 4x multisampled, sub_8237BF58): the
// only multisampled targets of a quarter of the screen or less.
bool IsReflection(u32 width, u32 height) {
  return width * 4 <= GuestScreenWidth() && height * 4 <= kGuestScreenHeight;
}

bool IsAmbientOcclusion(const d3d::SurfaceDesc &desc, u32 width, u32 height) {
  return !d3d::IsDepthFormat(desc.Format) && width == 640 && height == 360 &&
         (u32(desc.ColorInfo) & 0xFFF) == kAmbientOcclusionEdramBase;
}

// A quarter of the screen or less: the depth of field and bloom buffers
// (320x180 down to 20x11 at 1280x720), kept at the console's scale.
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

// The options as LatchGraphicsOptionsLocked last read them, so the draw path
// never parses the settings' strings.
struct Latched {
  bool shadow_follows_resolution = true;
  RenderScale shadow_scale;
  bool full_ambient_occlusion = false;
  uint32_t min_anisotropy = 1;
  bool high_precision_post = true;
  float depth_of_field = 1.0f;
  float bloom = 1.0f;
  uint32_t msaa = 0;  // 0: the game's; else the sample count (1 = off)
  bool reflection_2x = false;
  uint32_t shadow_filter = 0;  // kShadowFilter*
  bool alpha_to_coverage = false;
  uint32_t ambient_occlusion = 0;  // 0 off, 1 low, 2 medium, 3 high
};
Latched g_latched;
// renut_anti_aliasing as the present pass reads it (another thread).
std::atomic<uint32_t> g_post_anti_aliasing{0};
// The sample counts targets can have under renut_msaa, for the pipeline workers;
// every count until the first latch.
std::atomic<uint32_t> g_sample_counts_in_use{0xFu};

// The sample counts the GPU can render 'format' with, together with the depth
// format (asked once each).
std::unordered_map<uint32_t, plume::RenderSampleCounts> g_supported_samples;

plume::RenderSampleCounts SupportedSampleCounts(plume::RenderFormat format) {
  auto [it, inserted] = g_supported_samples.try_emplace(u32(format), plume::RenderSampleCount::COUNT_1);
  if (inserted) {
    if (plume::RenderDevice *device = Video::Device())
      it->second = device->getSampleCountsSupported(format) & device->getSampleCountsSupported(DepthTargetFormat());
  }
  return it->second;
}

// The game's sample count of the target 'desc' describes (D3DMULTISAMPLE_TYPE
// 0 none, 1 2x, 2 4x).
plume::RenderSampleCounts GuestSampleCount(const d3d::SurfaceDesc &desc) {
  switch ((u32(desc.SurfaceInfo) >> 16) & 3) {
  case 1:
    return plume::RenderSampleCount::COUNT_2;
  case 2:
    return plume::RenderSampleCount::COUNT_4;
  default:
    return plume::RenderSampleCount::COUNT_1;
  }
}

} // namespace

void LatchGraphicsOptionsLocked() {
  const std::string shadows = g_options.renut_shadow_resolution.Get();
  g_latched.shadow_follows_resolution = shadows == "Resolution" || shadows.empty();
  g_latched.shadow_scale = {};
  if (!g_latched.shadow_follows_resolution)
    RenderScale::FromRatio(uint32_t(std::atoi(shadows.c_str())), 1, g_latched.shadow_scale);
  g_latched.full_ambient_occlusion = g_options.renut_ambient_occlusion.Get() == "Full";
  const int anisotropy = std::atoi(g_options.renut_anisotropic_filtering.Get().c_str());
  g_latched.min_anisotropy = anisotropy >= 2 ? uint32_t(anisotropy) : 1;
  g_latched.high_precision_post = g_options.renut_post_process_precision.Get() != "Game";
  const std::string depth_of_field = g_options.renut_depth_of_field.Get();
  g_latched.depth_of_field = depth_of_field == "Off" ? 0.0f : depth_of_field == "Reduced" ? 0.5f : 1.0f;
  const std::string bloom = g_options.renut_bloom.Get();
  g_latched.bloom = bloom == "Off" ? 0.0f : bloom == "Reduced" ? 0.5f : 1.0f;
  const std::string msaa = g_options.renut_msaa.Get();
  g_latched.msaa = msaa == "Off" ? 1u : uint32_t(std::atoi(msaa.c_str()));
  g_sample_counts_in_use.store(g_latched.msaa == 0 ? 1u | 2u | 4u : 1u | g_latched.msaa,
                               std::memory_order_relaxed);
  g_latched.reflection_2x = g_options.renut_reflection_resolution.Get() == "2x";
  const std::string softness = g_options.renut_shadow_softness.Get();
  g_latched.shadow_filter = softness == "Softest" ? kShadowFilterWidest
                            : softness == "Softer" ? kShadowFilterWide
                            : softness == "Soft"   ? kShadowFilterBilinear
                                                   : kShadowFilterOff;
  g_latched.alpha_to_coverage = g_options.renut_alpha_to_coverage.Get();
  const std::string ssao = g_options.renut_ssao.Get();
  g_latched.ambient_occlusion = ssao == "High" ? 3u : ssao == "Medium" ? 2u : ssao == "Low" ? 1u : 0u;
  AmbientOcclusionNewFrameLocked();
  const std::string anti_aliasing = g_options.renut_anti_aliasing.Get();
  g_post_anti_aliasing.store(anti_aliasing == "FXAA"   ? kPostAntiAliasingFxaa
                             : anti_aliasing == "SMAA" ? kPostAntiAliasingSmaa
                                                       : kPostAntiAliasingOff,
                             std::memory_order_relaxed);
  LatchMenuColourLocked();
}

uint32_t PostAntiAliasing() {
  return g_post_anti_aliasing.load(std::memory_order_relaxed);
}

uint32_t SampleCountsInUse() {
  return g_sample_counts_in_use.load(std::memory_order_relaxed);
}

bool IsShadowMapSurface(const d3d::SurfaceDesc &desc) {
  return !((u32(desc.SurfaceInfo) >> 16) & 3) &&
         IsShadowMap(desc, d3d::SurfaceWidth(desc.Size), d3d::SurfaceHeight(desc.Size));
}

RenderScale SurfaceRenderScale(const d3d::SurfaceDesc &desc, RenderScale resolution) {
  const u32 width = d3d::SurfaceWidth(desc.Size);
  const u32 height = d3d::SurfaceHeight(desc.Size);
  if ((u32(desc.SurfaceInfo) >> 16) & 3) {
    // Multisampled: the scene's, or twice it for the reflection.
    if (g_latched.reflection_2x && IsReflection(width, height))
      return resolution.Times(2);
    return resolution;
  }
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

plume::RenderSampleCounts SurfaceSampleCount(const d3d::SurfaceDesc &desc, plume::RenderFormat format) {
  const plume::RenderSampleCounts game = GuestSampleCount(desc);
  if (game == plume::RenderSampleCount::COUNT_1 || g_latched.msaa == 0)
    return game;
  if (g_latched.msaa == 1)
    return plume::RenderSampleCount::COUNT_1;
  // The most samples up to the setting's that both this format and the depth
  // format can do, so a draw's color and depth targets agree.
  plume::RenderSampleCounts samples = g_latched.msaa;
  const plume::RenderSampleCounts supported = SupportedSampleCounts(format);
  while (samples > 1 && !(supported & samples))
    samples >>= 1;
  return samples;
}

uint32_t MinAnisotropy() {
  return g_latched.min_anisotropy;
}

float DepthOfFieldStrength() {
  return g_latched.depth_of_field;
}

float BloomStrength() {
  return g_latched.bloom;
}

uint32_t ShadowFilter() {
  return g_latched.shadow_filter;
}

bool AlphaToCoverage() {
  return g_latched.alpha_to_coverage;
}

uint32_t AmbientOcclusionQuality() {
  return g_latched.ambient_occlusion;
}

} // namespace renut::gpu
