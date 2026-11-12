#include "renut_engine/gpu/backend/frame_blur.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>

#include <plume_render_interface.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/overlay/overlay_drawer.h"

#include "host_shaders/frame_blur_down_ps.spirv.h"
#include "host_shaders/frame_blur_ps.spirv.h"
#if defined(_WIN32)
#include "host_shaders/frame_blur_down_ps.dxil.h"
#include "host_shaders/frame_blur_ps.dxil.h"
#endif

namespace renut::gpu {

namespace {

constexpr plume::RenderFormat kFormat = plume::RenderFormat::R8G8B8A8_UNORM;
// The blur runs on the shown frame shrunk this many times; frame_blur_ps's sigma is kSigmaTaps of its taps.
constexpr uint32_t kShrink = 4;
constexpr float kSigmaTaps = 3.0f;
// It keeps running this long after the UI last asked, so a UI frame recorded with it never draws without it.
constexpr std::chrono::milliseconds kKeepFor(500);

struct Target {
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  uint32_t slot = kInvalidDescriptor;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  uint32_t width = 0, height = 0;
};

struct FrameBlur {
  bool failed = false;  // the pipelines could not be made
  bool built = false;
  std::unique_ptr<plume::RenderShader> down_ps, blur_ps;
  std::unique_ptr<plume::RenderPipeline> down, blur;
  Target a, b;  // a: the shrunk frame, then the result; b: between the two directions
  float sigma = 0.0f;
  bool asked = false;
  std::chrono::steady_clock::time_point asked_at;
};
FrameBlur g_blur;

// The largest sigma asked for since the last present; below 0, none.
std::atomic<float> g_wanted{-1.0f};

// The screen as drawn so far, for glass over the UI (ScreenTexture). The swapchain's images can't be copied from on
// every backend, so while glass wants it the UI pass draws into `ui`, which goes on the back buffer at the end.
constexpr plume::RenderFormat kScreenFormat = plume::RenderFormat::B8G8R8A8_UNORM;  // the swapchain's
struct Screen {
  Target ui;
  Target sharp;              // what's under each glass panel, copied from `ui` as the panel is drawn
  Target between, blurred;   // the two directions of its blur
  bool active = false;       // the UI pass draws into `ui` this frame
  float sigma = 12.0f;       // window pixels
};
Screen g_screen;
std::atomic<int64_t> g_screen_asked{0};  // steady_clock nanoseconds; 0: never
std::atomic<float> g_screen_sigma{12.0f};
std::atomic<bool> g_screen_ready{false};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

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

void ReleaseScreen() {
  Screen &m = g_screen;
  Release(m.ui);
  Release(m.sharp);
  Release(m.between);
  Release(m.blurred);
}

bool Make(VideoState &s, Target &t, uint32_t width, uint32_t height, const char *name,
          plume::RenderFormat format = kFormat) {
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(width, height, 1, format);
  desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  t.texture = s.device->createTexture(desc);
  if (!TextureCreated(t.texture.get())) {
    t.texture.reset();
    return false;
  }
  t.texture->setName(name);
  t.view = t.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(format));
  t.slot = t.view ? Video::AllocateTextureSlotLocked() : kInvalidDescriptor;
  if (t.slot == kInvalidDescriptor) {
    Release(t);
    return false;
  }
  Video::SetTextureSlotLocked(t.slot, t.texture.get(), t.view.get());
  const plume::RenderTexture *targets[] = {t.texture.get()};
  t.framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1));
  if (!t.framebuffer) {
    Release(t);
    return false;
  }
  t.width = width;
  t.height = height;
  return true;
}

std::unique_ptr<plume::RenderPipeline> MakePipeline(VideoState &s, plume::RenderShader *ps, const char *name) {
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

// The pipelines, once.
bool Build(VideoState &s) {
  FrameBlur &m = g_blur;
  if (m.built || m.failed)
    return m.built;
  m.down_ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(frame_blur_down_ps), "frame_blur_down_ps");
  m.blur_ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(frame_blur_ps), "frame_blur_ps");
  if (m.down_ps && m.blur_ps && s.present_vs) {
    m.down = MakePipeline(s, m.down_ps.get(), "renut frame blur shrink");
    m.blur = MakePipeline(s, m.blur_ps.get(), "renut frame blur");
  }
  // Without them the UI gets no blurred frame and does without.
  m.failed = !m.down || !m.blur;
  m.built = !m.failed;
  return m.built;
}

void Transition(plume::RenderCommandList *cmd, Target &t, plume::RenderTextureLayout layout,
                plume::RenderBarrierStages stage = plume::RenderBarrierStage::GRAPHICS) {
  if (t.layout == layout)
    return;
  cmd->barriers(stage, plume::RenderTextureBarrier(t.texture.get(), layout));
  t.layout = layout;
}

// One fullscreen pass into 'dest', reading the part (scale, translate) of 'texture', left readable after. Only
// `scissor` of it is drawn, when given.
void Pass(VideoState &s, plume::RenderCommandList *cmd, Target &dest, plume::RenderPipeline *pipeline,
          uint32_t texture, const float scale[2], const float translate[2], float param0, float param1,
          const plume::RenderRect *scissor = nullptr) {
  Transition(cmd, dest, plume::RenderTextureLayout::COLOR_WRITE);
  cmd->setFramebuffer(dest.framebuffer.get());
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(dest.width), float(dest.height)));
  cmd->setScissors(scissor ? *scissor : plume::RenderRect(0, 0, int32_t(dest.width), int32_t(dest.height)));
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(pipeline);
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = scale[0];
  push.scale[1] = scale[1];
  push.translate[0] = translate[0];
  push.translate[1] = translate[1];
  push.texture_index = texture;
  push.sampler_index = kLinearClampSamplerSlot;
  push.param0 = param0;
  push.param1 = param1;
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  cmd->setFramebuffer(nullptr);
  Transition(cmd, dest, plume::RenderTextureLayout::SHADER_READ);
}

} // namespace

void WantFrameBlur(float sigma) {
  if (!std::isfinite(sigma))
    return;
  sigma = std::max(sigma, 0.0f);
  float seen = g_wanted.load(std::memory_order_relaxed);
  while (seen < sigma && !g_wanted.compare_exchange_weak(seen, sigma, std::memory_order_acq_rel)) {
  }
}

void BlurFrameLocked(VideoState &s, const PresentImage &image, const float scale[2], const float translate[2],
                     float shown_width) {
  FrameBlur &m = g_blur;
  const auto now = std::chrono::steady_clock::now();
  if (const float wanted = g_wanted.exchange(-1.0f, std::memory_order_acq_rel); wanted >= 0.0f) {
    m.sigma = wanted;
    m.asked = true;
    m.asked_at = now;
  }
  plume::RenderCommandList *cmd = s.command_list;
  const uint32_t width = std::max(1u, uint32_t(std::lround(float(image.width) * scale[0] / float(kShrink))));
  const uint32_t height = std::max(1u, uint32_t(std::lround(float(image.height) * scale[1] / float(kShrink))));
  if (!m.asked || now - m.asked_at > kKeepFor || !cmd || !image.texture || image.descriptor == kInvalidDescriptor ||
      !image.width || !image.height || !(shown_width > 0.0f) || !Build(s)) {
    SetBlurredFrameLocked(kInvalidDescriptor);
    return;
  }
  if (m.a.width != width || m.a.height != height) {
    Release(m.a);
    Release(m.b);
    if (!Make(s, m.a, width, height, "renut frame blur") || !Make(s, m.b, width, height, "renut frame blur between")) {
      Release(m.a);
      Release(m.b);
      SetBlurredFrameLocked(kInvalidDescriptor);
      return;
    }
  }
  if (image.layout && *image.layout != plume::RenderTextureLayout::SHADER_READ) {
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderTextureBarrier(image.texture, plume::RenderTextureLayout::SHADER_READ));
    *image.layout = plume::RenderTextureLayout::SHADER_READ;
  }
  const float whole[2] = {1.0f, 1.0f}, origin[2] = {0.0f, 0.0f};
  // Sigma from window pixels into the shrunk frame's texels, spread over the taps.
  const float spacing = m.sigma * float(width) / shown_width / kSigmaTaps;
  Pass(s, cmd, m.a, m.down.get(), image.descriptor, scale, translate, s.output_gamma, 0.0f);
  Pass(s, cmd, m.b, m.blur.get(), m.a.slot, whole, origin, spacing, 0.0f);
  Pass(s, cmd, m.a, m.blur.get(), m.b.slot, whole, origin, spacing, 1.0f);
  SetBlurredFrameLocked(m.a.slot);
}

void WantScreen(float sigma) {
  if (std::isfinite(sigma) && sigma > 0.0f)
    g_screen_sigma.store(sigma, std::memory_order_relaxed);
  g_screen_asked.store(NowNs(), std::memory_order_release);
}

bool ScreenReady() { return g_screen_ready.load(std::memory_order_acquire); }

plume::RenderFramebuffer *BeginScreenLocked(VideoState &s, uint32_t width, uint32_t height) {
  Screen &m = g_screen;
  m.active = false;
  const int64_t asked = g_screen_asked.load(std::memory_order_acquire);
  const bool wanted = asked != 0 && NowNs() - asked < std::chrono::nanoseconds(kKeepFor).count();
  plume::RenderCommandList *cmd = s.command_list;
  if (!wanted || !cmd || !width || !height || !Build(s)) {
    // Its targets go once glass stops asking, as they're the window's size.
    if (!wanted && m.ui.texture)
      ReleaseScreen();
    g_screen_ready.store(false, std::memory_order_release);
    return nullptr;
  }
  if (m.ui.width != width || m.ui.height != height) {
    ReleaseScreen();
    if (!Make(s, m.ui, width, height, "renut screen", kScreenFormat) ||
        !Make(s, m.sharp, width, height, "renut screen copy", kScreenFormat) ||
        !Make(s, m.between, width, height, "renut screen blur between") ||
        !Make(s, m.blurred, width, height, "renut screen blur")) {
      ReleaseScreen();
      g_screen_ready.store(false, std::memory_order_release);
      return nullptr;
    }
  }
  Transition(cmd, m.ui, plume::RenderTextureLayout::COLOR_WRITE);
  m.sigma = g_screen_sigma.load(std::memory_order_relaxed);
  m.active = true;
  g_screen_ready.store(true, std::memory_order_release);
  return m.ui.framebuffer.get();
}

bool ScreenActiveLocked() { return g_screen.active; }

uint32_t ScreenSlotLocked(bool blurred) {
  const Screen &m = g_screen;
  if (!m.active)
    return kInvalidDescriptor;
  return blurred ? m.blurred.slot : m.sharp.slot;
}

bool CaptureScreenLocked(VideoState &s, plume::RenderCommandList *cmd, const plume::RenderRect &region) {
  Screen &m = g_screen;
  if (!m.active || !cmd)
    return false;
  const int32_t width = int32_t(m.ui.width), height = int32_t(m.ui.height);
  const plume::RenderRect inside(std::clamp(region.left, 0, width), std::clamp(region.top, 0, height),
                                 std::clamp(region.right, 0, width), std::clamp(region.bottom, 0, height));
  if (inside.right <= inside.left || inside.bottom <= inside.top)
    return false;
  // The blur reads its seven taps either side, kSigmaTaps to a sigma, so the copy reaches that far past the region.
  const float spacing = m.sigma / kSigmaTaps;
  const int32_t reach = int32_t(std::ceil(spacing * 7.0f)) + 2;
  const plume::RenderRect copy(std::max(inside.left - reach, 0), std::max(inside.top - reach, 0),
                               std::min(inside.right + reach, width), std::min(inside.bottom + reach, height));
  const plume::RenderRect across(inside.left, copy.top, inside.right, copy.bottom);

  cmd->setFramebuffer(nullptr);
  Transition(cmd, m.ui, plume::RenderTextureLayout::COPY_SOURCE, plume::RenderBarrierStage::COPY);
  Transition(cmd, m.sharp, plume::RenderTextureLayout::COPY_DEST, plume::RenderBarrierStage::COPY);
  const plume::RenderBox box(copy.left, copy.top, copy.right, copy.bottom);
  cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(m.sharp.texture.get()),
                         plume::RenderTextureCopyLocation::Subresource(m.ui.texture.get()), uint32_t(copy.left),
                         uint32_t(copy.top), 0, &box);
  Transition(cmd, m.sharp, plume::RenderTextureLayout::SHADER_READ);
  const float whole[2] = {1.0f, 1.0f}, origin[2] = {0.0f, 0.0f};
  Pass(s, cmd, m.between, g_blur.blur.get(), m.sharp.slot, whole, origin, spacing, 0.0f, &across);
  Pass(s, cmd, m.blurred, g_blur.blur.get(), m.between.slot, whole, origin, spacing, 1.0f, &inside);

  // Back to drawing the UI where it left off.
  Transition(cmd, m.ui, plume::RenderTextureLayout::COLOR_WRITE);
  cmd->setFramebuffer(m.ui.framebuffer.get());
  return true;
}

void FinishScreenLocked(VideoState &s, plume::RenderFramebuffer *back_fb, uint32_t width, uint32_t height) {
  Screen &m = g_screen;
  plume::RenderCommandList *cmd = s.command_list;
  if (!m.active || !cmd)
    return;
  m.active = false;
  cmd->setFramebuffer(nullptr);
  Transition(cmd, m.ui, plume::RenderTextureLayout::SHADER_READ);
  // The present pipeline with gamma 1 and no FXAA is a straight copy onto the back buffer.
  cmd->setFramebuffer(back_fb);
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(s.present_pipeline.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = push.scale[1] = 1.0f;
  push.texture_index = m.ui.slot;
  push.sampler_index = kPointClampSamplerSlot;
  push.param0 = 1.0f;
  push.param1 = 0.0f;
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
}

}  // namespace renut::gpu
