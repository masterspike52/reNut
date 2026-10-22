/**
 * @file    gpu/backend/smaa.cpp
 */
#include "renut_engine/gpu/backend/smaa.h"

#include <bit>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/log.h"

#include "host_shaders/smaa_blend_ps.spirv.h"
#include "host_shaders/smaa_edge_ps.spirv.h"
#include "host_shaders/smaa_weight_ps.spirv.h"
#if defined(_WIN32)
#include "host_shaders/smaa_blend_ps.dxil.h"
#include "host_shaders/smaa_edge_ps.dxil.h"
#include "host_shaders/smaa_weight_ps.dxil.h"
#endif

#include "thirdparty/smaa/AreaTex.h"
#include "thirdparty/smaa/SearchTex.h"

namespace renut::gpu {

namespace {

constexpr plume::RenderFormat kFormat = plume::RenderFormat::R8G8B8A8_UNORM;

// A texture the passes read through the bindless table, and draw into when it
// is a target.
struct Target {
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  uint32_t slot = kInvalidDescriptor;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  uint32_t width = 0, height = 0;
};

struct Smaa {
  bool failed = false;  // the pipelines or lookup textures could not be made; logged once
  bool built = false;
  std::unique_ptr<plume::RenderShader> edge_ps, weight_ps, blend_ps;
  std::unique_ptr<plume::RenderPipeline> edge, weight, blend;
  Target area, search;
  bool lookups_uploaded = false;
  Target edges, weights, output;  // the frame's size
};
Smaa g_smaa;

void Release(Target &t) {
  if (t.slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(t.slot);
  t.slot = kInvalidDescriptor;
  Video::RetireLocked(std::move(t.framebuffer));
  Video::RetireLocked(std::move(t.view));
  Video::RetireLocked(std::move(t.texture));
  t.layout = plume::RenderTextureLayout::UNKNOWN;
  t.width = t.height = 0;
}

bool Make(VideoState &s, Target &t, uint32_t width, uint32_t height, bool render_target,
          const char *name) {
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(width, height, 1, kFormat);
  if (render_target)
    desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  t.texture = s.device->createTexture(desc);
  if (!TextureCreated(t.texture.get())) {
    t.texture.reset();
    return false;
  }
  t.texture->setName(name);
  t.view = t.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(kFormat));
  t.slot = t.view ? Video::AllocateTextureSlotLocked() : kInvalidDescriptor;
  if (t.slot == kInvalidDescriptor) {
    Release(t);
    return false;
  }
  Video::SetTextureSlotLocked(t.slot, t.texture.get(), t.view.get());
  if (render_target) {
    const plume::RenderTexture *targets[] = {t.texture.get()};
    t.framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1));
    if (!t.framebuffer) {
      Release(t);
      return false;
    }
  }
  t.layout = plume::RenderTextureLayout::UNKNOWN;
  t.width = width;
  t.height = height;
  return true;
}

std::unique_ptr<plume::RenderPipeline> MakePipeline(VideoState &s, plume::RenderShader *ps,
                                                    const char *name) {
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.utility_layout.get();
  desc.vertexShader = s.present_vs.get();
  desc.pixelShader = ps;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = kFormat;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  auto pipeline = s.device->createGraphicsPipeline(desc);
  if (pipeline && GraphicsPipelineCreated(pipeline.get()))
    pipeline->setName(name);
  else
    pipeline.reset();
  return pipeline;
}

// The pipelines and lookup textures, once.
bool Build(VideoState &s) {
  if (g_smaa.built)
    return true;
  if (g_smaa.failed)
    return false;
  Smaa &m = g_smaa;
  m.edge_ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(smaa_edge_ps), "smaa_edge_ps");
  m.weight_ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(smaa_weight_ps), "smaa_weight_ps");
  m.blend_ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(smaa_blend_ps), "smaa_blend_ps");
  if (m.edge_ps && m.weight_ps && m.blend_ps && s.present_vs) {
    m.edge = MakePipeline(s, m.edge_ps.get(), "renut smaa edges");
    m.weight = MakePipeline(s, m.weight_ps.get(), "renut smaa weights");
    m.blend = MakePipeline(s, m.blend_ps.get(), "renut smaa blend");
  }
  if (!m.edge || !m.weight || !m.blend ||
      !Make(s, m.area, AREATEX_WIDTH, AREATEX_HEIGHT, false, "renut smaa area") ||
      !Make(s, m.search, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, false, "renut smaa search")) {
    RGPU_ERROR("SMAA could not be set up; frames are scanned out without it");
    m.failed = true;
    return false;
  }
  m.built = true;
  return true;
}

// SMAA's lookup textures as RGBA8 (the shaders read area.rg and search.r).
bool UploadLookups() {
  Smaa &m = g_smaa;
  if (m.lookups_uploaded)
    return true;
  std::vector<uint8_t> rgba(size_t(AREATEX_WIDTH) * AREATEX_HEIGHT * 4, 0);
  for (size_t i = 0; i < size_t(AREATEX_WIDTH) * AREATEX_HEIGHT; ++i) {
    rgba[i * 4 + 0] = areaTexBytes[i * 2 + 0];
    rgba[i * 4 + 1] = areaTexBytes[i * 2 + 1];
  }
  if (!Video::UploadRGBA8Locked(m.area.texture.get(), AREATEX_WIDTH, AREATEX_HEIGHT, rgba.data()))
    return false;
  rgba.assign(size_t(SEARCHTEX_WIDTH) * SEARCHTEX_HEIGHT * 4, 0);
  for (size_t i = 0; i < size_t(SEARCHTEX_WIDTH) * SEARCHTEX_HEIGHT; ++i)
    rgba[i * 4] = searchTexBytes[i];
  if (!Video::UploadRGBA8Locked(m.search.texture.get(), SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, rgba.data()))
    return false;
  // (UploadRGBA8Locked leaves them in SHADER_READ.)
  m.area.layout = m.search.layout = plume::RenderTextureLayout::SHADER_READ;
  m.lookups_uploaded = true;
  return true;
}

void Transition(plume::RenderCommandList *cmd, Target &t, plume::RenderTextureLayout layout) {
  if (t.layout == layout)
    return;
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(t.texture.get(), layout));
  t.layout = layout;
}

// One fullscreen pass into 'dest', reading 'texture' (and the slots in
// param0/param1).
void Pass(VideoState &s, plume::RenderCommandList *cmd, Target &dest, plume::RenderPipeline *pipeline,
          uint32_t texture, uint32_t extra0, uint32_t extra1, bool clear) {
  Transition(cmd, dest, plume::RenderTextureLayout::COLOR_WRITE);
  cmd->setFramebuffer(dest.framebuffer.get());
  if (clear)
    cmd->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(dest.width), float(dest.height)));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(dest.width), int32_t(dest.height)));
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(pipeline);
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = push.scale[1] = 1.0f;
  push.texture_index = texture;
  push.sampler_index = kLinearClampSamplerSlot;
  push.param0 = std::bit_cast<float>(extra0);
  push.param1 = std::bit_cast<float>(extra1);
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  cmd->setFramebuffer(nullptr);
}

} // namespace

PresentImage ApplySmaaLocked(VideoState &s, const PresentImage &image) {
  plume::RenderCommandList *cmd = s.command_list;
  if (!cmd || !image.texture || image.descriptor == kInvalidDescriptor || !image.width ||
      !image.height || !Build(s) || !UploadLookups()) {
    return image;
  }
  Smaa &m = g_smaa;
  if (m.output.width != image.width || m.output.height != image.height) {
    Release(m.edges);
    Release(m.weights);
    Release(m.output);
    if (!Make(s, m.edges, image.width, image.height, true, "renut smaa edges") ||
        !Make(s, m.weights, image.width, image.height, true, "renut smaa weights") ||
        !Make(s, m.output, image.width, image.height, true, "renut smaa output")) {
      Release(m.edges);
      Release(m.weights);
      Release(m.output);
      return image;
    }
  }
  static const uint32_t kProfileLabel = gpu_profile::Label("SMAA");
  gpu_profile::Mark(s, kProfileLabel);

  if (image.layout && *image.layout != plume::RenderTextureLayout::SHADER_READ) {
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderTextureBarrier(image.texture, plume::RenderTextureLayout::SHADER_READ));
    *image.layout = plume::RenderTextureLayout::SHADER_READ;
  }
  Pass(s, cmd, m.edges, m.edge.get(), image.descriptor, 0, 0, true);
  Transition(cmd, m.edges, plume::RenderTextureLayout::SHADER_READ);
  Pass(s, cmd, m.weights, m.weight.get(), m.edges.slot, m.area.slot, m.search.slot, true);
  Transition(cmd, m.weights, plume::RenderTextureLayout::SHADER_READ);
  Pass(s, cmd, m.output, m.blend.get(), image.descriptor, m.weights.slot, 0, false);

  PresentImage out = image;
  out.texture = m.output.texture.get();
  out.layout = &m.output.layout;
  out.descriptor = m.output.slot;
  return out;
}

}  // namespace renut::gpu
