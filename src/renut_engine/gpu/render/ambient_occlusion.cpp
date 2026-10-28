/**
 * @file    gpu/render/ambient_occlusion.cpp
 */
#include "renut_engine/gpu/render/ambient_occlusion.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>
#include <rex/types.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/render/graphics_options.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/textures.h"

#include "host_shaders/ambient_occlusion_blur_ps.spirv.h"
#include "host_shaders/ambient_occlusion_composite_ps.spirv.h"
#include "host_shaders/ambient_occlusion_ps.spirv.h"
#if defined(_WIN32)
#include "host_shaders/ambient_occlusion_blur_ps.dxil.h"
#include "host_shaders/ambient_occlusion_composite_ps.dxil.h"
#include "host_shaders/ambient_occlusion_ps.dxil.h"
#endif

namespace renut::gpu {

namespace {

// The camera (meCalculateProjection, 0x822BC0E0): the vertical field of view
// in degrees (field_of_view.cpp), and the lens whose +0x64 is the near
// distance and +0x68 the far one, which the projection clamps to 5000.
constexpr u32 kFieldOfView = 0x82E1B6B4;
constexpr u32 kCameraLens = 0x82E1B5F0;
constexpr u32 kLensNear = 0x64;
constexpr u32 kLensFar = 0x68;
constexpr float kMaxFar = 5000.0f;

// The occlusion radius in world units (N&B's are metres) and its strength.
constexpr float kRadius = 1.5f;
constexpr float kIntensity = 1.2f;
// Smaller depth targets are previews, not the scene.
constexpr uint32_t kMinSceneWidth = 640;
constexpr uint32_t kMinSceneHeight = 360;

constexpr plume::RenderFormat kBufferFormat = plume::RenderFormat::R16G16_FLOAT;

// Matches AmbientOcclusionPush in shaders/host/ambient_occlusion.hlsli.
struct AmbientOcclusionPush {
  float scale[2];
  float translate[2];
  uint32_t depth_index;
  uint32_t input_index;
  uint32_t sampler_index;
  uint32_t flags;
  float texel[2];
  float view_ray[2];
  float near_z;
  float far_z;
  float radius;
  float intensity;
};
static_assert(sizeof(AmbientOcclusionPush) == 64);
constexpr uint32_t kFlagVerticalBlur = 0x100;

struct Buffer {
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  uint32_t slot = kInvalidDescriptor;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  uint32_t width = 0, height = 0;
};

struct CompositePipeline {
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  plume::RenderSampleCounts samples = plume::RenderSampleCount::COUNT_1;
  std::unique_ptr<plume::RenderPipeline> pipeline;
};

struct AmbientOcclusion {
  bool failed = false;  // the layout, shaders or pipelines could not be made; logged once
  bool built = false;
  std::unique_ptr<plume::RenderPipelineLayout> layout;
  std::unique_ptr<plume::RenderShader> ao_ps, blur_ps, composite_ps;
  std::unique_ptr<plume::RenderPipeline> ao, blur;
  std::vector<CompositePipeline> composites;
  Buffer buffers[2];
  uint64_t frame = 0;
  uint64_t applied_frame = UINT64_MAX;
};
AmbientOcclusion g_ao;

void Release(Buffer &b) {
  if (b.slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(b.slot);
  b.slot = kInvalidDescriptor;
  Video::RetireLocked(std::move(b.framebuffer));
  Video::RetireLocked(std::move(b.view));
  Video::RetireLocked(std::move(b.texture));
  b.layout = plume::RenderTextureLayout::UNKNOWN;
  b.width = b.height = 0;
}

bool Make(VideoState &s, Buffer &b, uint32_t width, uint32_t height, const char *name) {
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(width, height, 1, kBufferFormat);
  desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  b.texture = s.device->createTexture(desc);
  if (!TextureCreated(b.texture.get())) {
    b.texture.reset();
    return false;
  }
  b.texture->setName(name);
  b.view = b.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(kBufferFormat));
  b.slot = b.view ? Video::AllocateTextureSlotLocked() : kInvalidDescriptor;
  if (b.slot == kInvalidDescriptor) {
    Release(b);
    return false;
  }
  Video::SetTextureSlotLocked(b.slot, b.texture.get(), b.view.get());
  const plume::RenderTexture *targets[] = {b.texture.get()};
  b.framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1));
  if (!b.framebuffer) {
    Release(b);
    return false;
  }
  b.width = width;
  b.height = height;
  return true;
}

std::unique_ptr<plume::RenderPipeline> MakePipeline(VideoState &s, plume::RenderShader *ps,
                                                    plume::RenderFormat format,
                                                    plume::RenderSampleCounts samples,
                                                    const plume::RenderBlendDesc &blend, const char *name) {
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = g_ao.layout.get();
  desc.vertexShader = s.present_vs.get();
  desc.pixelShader = ps;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = format;
  desc.renderTargetBlend[0] = blend;
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  desc.multisampling = plume::RenderMultisampling(samples);
  auto pipeline = s.device->createGraphicsPipeline(desc);
  if (pipeline && GraphicsPipelineCreated(pipeline.get()))
    pipeline->setName(name);
  else
    pipeline.reset();
  return pipeline;
}

// The layout, shaders and the passes' own pipelines, once.
bool Build(VideoState &s) {
  if (g_ao.built)
    return true;
  if (g_ao.failed)
    return false;
  AmbientOcclusion &m = g_ao;

  plume::RenderDescriptorSetBuilder textures;  // the bindless tables, as BuildUtilityLayout's
  textures.begin();
  textures.addTexture(0, kTextureDescriptorCount);
  textures.end(true, kTextureDescriptorCount);
  plume::RenderDescriptorSetBuilder samplers;
  samplers.begin();
  samplers.addSampler(0, kSamplerDescriptorCount);
  samplers.end(true, kSamplerDescriptorCount);
  plume::RenderPipelineLayoutBuilder layout;
  layout.begin(false, true);
  layout.addDescriptorSet(textures);  // set 0 -> t0, space0
  layout.addDescriptorSet(samplers);  // set 1 -> s0, space1
  layout.addPushConstant(0, 2, sizeof(AmbientOcclusionPush),
                         plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);  // b0, space2
  layout.end();
  m.layout = layout.create(s.device.get());

  m.ao_ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(ambient_occlusion_ps), "ambient_occlusion_ps");
  m.blur_ps =
      CreateHostShader(s.device.get(), RENUT_HOST_SHADER(ambient_occlusion_blur_ps), "ambient_occlusion_blur_ps");
  m.composite_ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(ambient_occlusion_composite_ps),
                                    "ambient_occlusion_composite_ps");
  if (m.layout && m.ao_ps && m.blur_ps && m.composite_ps && s.present_vs) {
    const plume::RenderBlendDesc copy = plume::RenderBlendDesc::Copy();
    m.ao = MakePipeline(s, m.ao_ps.get(), kBufferFormat, plume::RenderSampleCount::COUNT_1, copy,
                        "renut ambient occlusion");
    m.blur = MakePipeline(s, m.blur_ps.get(), kBufferFormat, plume::RenderSampleCount::COUNT_1, copy,
                          "renut ambient occlusion blur");
  }
  if (!m.ao || !m.blur) {
    RGPU_ERROR("Ambient occlusion could not be set up; the scene is drawn without it");
    m.failed = true;
    return false;
  }
  m.built = true;
  return true;
}

// The composite for a scene target of 'format' and 'samples': the occlusion
// multiplied into its color (dest * source), alpha kept.
plume::RenderPipeline *Composite(VideoState &s, plume::RenderFormat format, plume::RenderSampleCounts samples) {
  for (CompositePipeline &c : g_ao.composites) {
    if (c.format == format && c.samples == samples)
      return c.pipeline.get();
  }
  plume::RenderBlendDesc multiply;
  multiply.blendEnabled = true;
  multiply.srcBlend = plume::RenderBlend::ZERO;
  multiply.dstBlend = plume::RenderBlend::SRC_COLOR;
  multiply.blendOp = plume::RenderBlendOperation::ADD;
  multiply.srcBlendAlpha = plume::RenderBlend::ZERO;
  multiply.dstBlendAlpha = plume::RenderBlend::ONE;
  multiply.blendOpAlpha = plume::RenderBlendOperation::ADD;
  multiply.renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::RED) |
                                   uint8_t(plume::RenderColorWriteEnable::GREEN) |
                                   uint8_t(plume::RenderColorWriteEnable::BLUE);
  CompositePipeline c;
  c.format = format;
  c.samples = samples;
  c.pipeline = MakePipeline(s, g_ao.composite_ps.get(), format, samples, multiply, "renut ambient occlusion composite");
  if (!c.pipeline) {
    RGPU_WARN("Ambient occlusion: no composite for scene format {} ({} samples)", uint32_t(format),
              uint32_t(samples));
  }
  g_ao.composites.push_back(std::move(c));
  return g_ao.composites.back().pipeline.get();
}

void Transition(plume::RenderCommandList *cmd, Buffer &b, plume::RenderTextureLayout layout) {
  if (b.layout == layout)
    return;
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(b.texture.get(), layout));
  b.layout = layout;
}

// One fullscreen pass over 'width' x 'height' of 'framebuffer'.
void Pass(VideoState &s, plume::RenderCommandList *cmd, plume::RenderFramebuffer *framebuffer, uint32_t width,
          uint32_t height, plume::RenderPipeline *pipeline, const AmbientOcclusionPush &push) {
  cmd->setFramebuffer(framebuffer);
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
  cmd->setGraphicsPipelineLayout(g_ao.layout.get());
  cmd->setPipeline(pipeline);
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
}

float LoadGuestFloat(u32 va) {
  const auto *value = GuestPtr<const be_f32>(va);
  return value ? float(*value) : 0.0f;
}

} // namespace

void AmbientOcclusionNewFrameLocked() {
  ++g_ao.frame;
}

bool ApplyAmbientOcclusionLocked(HostSurface *depth, HostTexture *resolved, int32_t bottom, HostSurface *color) {
  const uint32_t quality = AmbientOcclusionQuality();
  if (!quality || !depth || !resolved || g_ao.applied_frame == g_ao.frame)
    return false;
  // The scene's depth: screen-sized float depth (the shadow maps are D24S8),
  // and with tiling the resolve that finishes it.
  if (depth->shadow_map || GpuTextureFormat(depth->format_word & 0x3F) != GpuTextureFormat::Fmt24_8_Float ||
      resolved->width < kMinSceneWidth || resolved->height < kMinSceneHeight || bottom < int32_t(resolved->height) ||
      resolved->slot == kInvalidDescriptor) {
    return false;
  }
  // The frame's first: later resolves come after the water and particles.
  g_ao.applied_frame = g_ao.frame;
  if (!color || color->depth || !color->texture || color->scale != depth->scale ||
      color->width < resolved->width || color->height < resolved->height) {
    return false;
  }

  const float fov = LoadGuestFloat(kFieldOfView);
  const auto *lens_pointer = GuestPtr<const be_u32>(kCameraLens);
  const u32 lens = lens_pointer ? u32(*lens_pointer) : 0;
  if (!lens)
    return false;
  const float near_z = LoadGuestFloat(lens + kLensNear);
  const float far_z = std::min(LoadGuestFloat(lens + kLensFar), kMaxFar);
  if (!(fov > 1.0f && fov < 179.0f) || !(near_z > 0.0f) || !(far_z > near_z))
    return false;

  auto &s = State();
  plume::RenderCommandList *cmd = s.command_list;
  if (!cmd || !Build(s))
    return false;
  plume::RenderPipeline *composite = Composite(s, color->format, color->samples);
  plume::RenderFramebuffer *color_framebuffer = GetFramebufferLocked(&color, 1, nullptr);
  if (!composite || !color_framebuffer)
    return false;

  // High at the scene's resolution, the others at half of it.
  const uint32_t host_width = resolved->alloc_width, host_height = resolved->alloc_height;
  const uint32_t divisor = quality >= 3 ? 1 : 2;
  const uint32_t ao_width = std::max(1u, host_width / divisor), ao_height = std::max(1u, host_height / divisor);
  AmbientOcclusion &m = g_ao;
  if (m.buffers[0].width != ao_width || m.buffers[0].height != ao_height) {
    Release(m.buffers[0]);
    Release(m.buffers[1]);
    if (!Make(s, m.buffers[0], ao_width, ao_height, "renut ambient occlusion") ||
        !Make(s, m.buffers[1], ao_width, ao_height, "renut ambient occlusion blur")) {
      Release(m.buffers[0]);
      Release(m.buffers[1]);
      return false;
    }
  }
  static const uint32_t kProfileLabel = gpu_profile::Label("ambient occlusion");
  gpu_profile::Mark(s, kProfileLabel);

  AmbientOcclusionPush push{};
  push.scale[0] = push.scale[1] = 1.0f;
  push.depth_index = resolved->slot;
  push.sampler_index = kPointClampSamplerSlot;
  push.flags = quality >= 3 ? 16 : quality == 2 ? 12 : 8;
  const float tan_y = std::tan(fov * 0.5f * 0.017453292f);
  push.view_ray[0] = tan_y * float(resolved->width) / float(resolved->height);
  push.view_ray[1] = tan_y;
  push.near_z = near_z;
  push.far_z = far_z;
  push.radius = kRadius;
  push.intensity = kIntensity;

  // The occlusion, from the depth texel by texel.
  push.texel[0] = 1.0f / float(host_width);
  push.texel[1] = 1.0f / float(host_height);
  Transition(cmd, m.buffers[0], plume::RenderTextureLayout::COLOR_WRITE);
  Pass(s, cmd, m.buffers[0].framebuffer.get(), ao_width, ao_height, m.ao.get(), push);
  // Blurred across, then down.
  push.texel[0] = 1.0f / float(ao_width);
  push.texel[1] = 1.0f / float(ao_height);
  push.flags = 0;
  push.input_index = m.buffers[0].slot;
  Transition(cmd, m.buffers[0], plume::RenderTextureLayout::SHADER_READ);
  Transition(cmd, m.buffers[1], plume::RenderTextureLayout::COLOR_WRITE);
  Pass(s, cmd, m.buffers[1].framebuffer.get(), ao_width, ao_height, m.blur.get(), push);
  push.flags = kFlagVerticalBlur;
  push.input_index = m.buffers[1].slot;
  Transition(cmd, m.buffers[1], plume::RenderTextureLayout::SHADER_READ);
  Transition(cmd, m.buffers[0], plume::RenderTextureLayout::COLOR_WRITE);
  Pass(s, cmd, m.buffers[0].framebuffer.get(), ao_width, ao_height, m.blur.get(), push);
  // Into the scene.
  push.flags = 0;
  push.input_index = m.buffers[0].slot;
  Transition(cmd, m.buffers[0], plume::RenderTextureLayout::SHADER_READ);
  TransitionSurfaceLocked(color, plume::RenderTextureLayout::COLOR_WRITE);
  Pass(s, cmd, color_framebuffer, host_width, host_height, composite, push);
  cmd->setFramebuffer(nullptr);
  return true;
}

} // namespace renut::gpu
