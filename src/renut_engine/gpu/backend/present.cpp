#include <algorithm>
#include <chrono>
#include <mutex>

#include <plume_render_interface.h>

#include <rex/gpu/graphics_system.h>
#include <rex/setting.h>

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/command_recorder.h"
#include "renut_engine/gpu/backend/frame_blur.h"
#include "renut_engine/gpu/backend/smaa.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/render/graphics_options.h"
#include "renut_engine/gpu/overlay/overlay_drawer.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/backend/video_internal.h"

namespace {
struct PresentSettings final : rex::SettingOwner {
  PresentSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
  REX_SETTING(bool, renut_vsync, true,
              "Show each frame at the display's refresh: no tearing, and the game runs at "
              "the refresh rate (frame_cap then only limits below it). Off shows frames at "
              "once, with tearing.");
};
PresentSettings &g_present_settings = *new PresentSettings();  // never freed, registered by RenutApp::OnConfigure
} // namespace

namespace renut::gpu {

namespace {

// renut_vsync onto the swapchain (both backends start with it on). Vulkan
// makes the change by recreating the swapchain, which needsResize asks for.
void ApplyVsync(VideoState &s) {
  static bool applied = true;
  const bool vsync = g_present_settings.renut_vsync.Get();
  if (vsync == applied)
    return;
  s.swap_chain->setVsyncEnabled(vsync);
  applied = vsync;
}

void RebuildSwapChain(VideoState &s) {
  // Submit rather than abandon an open list: plume tracks layouts on record.
  if (s.list_open)
    detail::SubmitAndAdvance(s, nullptr, nullptr);
  detail::WaitAllSubmitted(s);
  s.swap_framebuffers.clear();
  if (s.swap_chain->resize())
    detail::BuildSwapChainTargets(s);
}

// The most of the game's frame trimmed off either axis to fill the output.
constexpr float kMaxTrim = 0.15f;

struct FrameFit {
  plume::RenderViewport viewport;
  float visible[2] = {1.0f, 1.0f};  // the share of the frame shown on each axis
};

// Covers the output keeping the frame's proportions, trimming the overflow evenly
// from both edges; past kMaxTrim the rest is fitted with bars.
FrameFit FitFrame(uint32_t out_w, uint32_t out_h, uint32_t src_w, uint32_t src_h) {
  FrameFit fit;
  fit.viewport = plume::RenderViewport(0.0f, 0.0f, float(out_w), float(out_h));
  if (src_w == 0 || src_h == 0 || out_w == 0 || out_h == 0)
    return fit;
  const float out_aspect = float(out_w) / float(out_h);
  float src_aspect = float(src_w) / float(src_h);
  if (src_aspect > out_aspect) {
    fit.visible[0] = std::max(out_aspect / src_aspect, 1.0f - kMaxTrim);
    src_aspect *= fit.visible[0];
  } else {
    fit.visible[1] = std::max(src_aspect / out_aspect, 1.0f - kMaxTrim);
    src_aspect /= fit.visible[1];
  }
  float w = float(out_w), h = float(out_h);
  if (src_aspect > out_aspect)
    h = w / src_aspect;
  else
    w = h * src_aspect;
  fit.viewport = plume::RenderViewport((float(out_w) - w) * 0.5f, (float(out_h) - h) * 0.5f, w, h);
  return fit;
}

// The part of an image the output shows (scale and translate of it in the image's uv), and where it goes.
struct ShownPart {
  FrameFit fit;
  float scale[2] = {1.0f, 1.0f};
  float translate[2] = {0.0f, 0.0f};
};

ShownPart Shown(const PresentImage &image, uint32_t out_w, uint32_t out_h) {
  const uint32_t src_w = image.source_width ? image.source_width : image.width;
  const uint32_t src_h = image.source_height ? image.source_height : image.height;
  ShownPart shown;
  shown.fit = FitFrame(out_w, out_h, src_w, src_h);
  // The frame's share of the texture, less what the fit trims from its edges.
  const float frame_u = image.width ? float(src_w) / float(image.width) : 1.0f;
  const float frame_v = image.height ? float(src_h) / float(image.height) : 1.0f;
  shown.scale[0] = frame_u * shown.fit.visible[0];
  shown.scale[1] = frame_v * shown.fit.visible[1];
  shown.translate[0] = frame_u * (1.0f - shown.fit.visible[0]) * 0.5f;
  shown.translate[1] = frame_v * (1.0f - shown.fit.visible[1]) * 0.5f;
  return shown;
}

void RecordScanOut(VideoState &s, const PresentImage &image, const ShownPart &shown) {
  plume::RenderCommandList *cmd = s.command_list;
  if (image.layout && *image.layout != plume::RenderTextureLayout::SHADER_READ) {
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderTextureBarrier(image.texture, plume::RenderTextureLayout::SHADER_READ));
    *image.layout = plume::RenderTextureLayout::SHADER_READ;
  }

  const plume::RenderViewport &vp = shown.fit.viewport;
  cmd->setViewports(vp);
  cmd->setScissors(plume::RenderRect(int32_t(vp.x), int32_t(vp.y), int32_t(vp.x + vp.width),
                                     int32_t(vp.y + vp.height)));

  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(s.present_pipeline.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);

  UtilityPush push{};
  push.scale[0] = shown.scale[0];
  push.scale[1] = shown.scale[1];
  push.translate[0] = shown.translate[0];
  push.translate[1] = shown.translate[1];
  push.texture_index = image.descriptor;
  push.sampler_index = kLinearClampSamplerSlot;
  push.param0 = s.output_gamma;
  // 1: FXAA (renut_anti_aliasing; SMAA ran before, ApplySmaaLocked).
  push.param1 = PostAntiAliasing() == kPostAntiAliasingFxaa ? 1.0f : 0.0f;
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  // UI drawn over it may sample it (GameFrameTexture).
  const float uv[4] = {push.translate[0], push.translate[1], push.translate[0] + push.scale[0],
                       push.translate[1] + push.scale[1]};
  SetGameFrameLocked(image.descriptor, uv);
}

// Waits for the display; present wait is turned off after kMaxTimeouts 100 ms
// timeouts in a row (presents that never report reaching the display).
void WaitForDisplay(VideoState &s) {
  constexpr uint32_t kMaxTimeouts = 10;
  static uint32_t timeouts = 0;
  const auto start = std::chrono::steady_clock::now();
  s.swap_chain->wait();
  if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(90)) {
    timeouts = 0;
    return;
  }
  if (++timeouts == kMaxTimeouts)
    s.present_wait = false;
}

// Records the back-buffer pass and presents. Caller holds s.mutex.
void PresentLocked(VideoState &s, const PresentImage *image) {
  // Waits for the recording thread's previous present before using the swapchain.
  {
    render_thread::WaitScope wait;
    CommandRecorder::WaitForCall(s.present_call);
    s.present_call = 0;
    // Then for the display, so frames are made one per refresh rather than in
    // bursts.
    if (s.present_wait)
      WaitForDisplay(s);
  }
  // Notes the device lost if an earlier submission faulted.
  ReportDeviceLost(s.device.get());
  ApplyVsync(s);
  if (s.swap_chain->needsResize() || s.resize_requested.exchange(false, std::memory_order_acq_rel))
    RebuildSwapChain(s);
  if (s.swap_framebuffers.empty()) {
    // Minimized or failed resize: keep the frame ring moving without a present.
    if (s.list_open)
      detail::SubmitAndAdvance(s, nullptr, nullptr);
    return;
  }

  plume::RenderCommandSemaphore *acquire = s.acquire_semaphores[s.frame].get();
  uint32_t image_index = 0;
  auto acquire_image = [&] {
    render_thread::WaitScope wait;
    return s.swap_chain->acquireTexture(acquire, &image_index);
  };
  if (!acquire_image()) {
    RebuildSwapChain(s);
    acquire = s.acquire_semaphores[s.frame].get();
    if (s.swap_framebuffers.empty() || !acquire_image()) {
      if (s.list_open)
        detail::SubmitAndAdvance(s, nullptr, nullptr);
      return;
    }
  }

  if (!Video::EnsureCommandListLocked())
    return;
  plume::RenderCommandList *cmd = s.command_list;
  // SMAA's passes come before the back buffer's (their own targets).
  PresentImage scan;
  const bool has_image = image && image->texture && image->descriptor != kInvalidDescriptor;
  if (has_image)
    scan = PostAntiAliasing() == kPostAntiAliasingSmaa ? ApplySmaaLocked(s, *image) : *image;
  plume::RenderTexture *back = s.swap_chain->getTexture(image_index);
  plume::RenderFramebuffer *back_fb = s.swap_framebuffers[image_index].get();
  const uint32_t out_w = s.swap_chain->getWidth();
  const uint32_t out_h = s.swap_chain->getHeight();
  // The blurred copy for glass and the Guide, which are drawn over this frame, also before the back buffer's pass.
  ShownPart shown;
  if (has_image) {
    shown = Shown(scan, out_w, out_h);
    BlurFrameLocked(s, scan, shown.scale, shown.translate, shown.fit.viewport.width);
  } else {
    SetBlurredFrameLocked(kInvalidDescriptor);
  }

  // Glass showing the screen as drawn so far has the UI drawn where it can be copied, then put on the back buffer.
  plume::RenderFramebuffer *screen_fb = BeginScreenLocked(s, out_w, out_h);
  plume::RenderFramebuffer *target = screen_fb ? screen_fb : back_fb;

  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(back, plume::RenderTextureLayout::COLOR_WRITE));
  cmd->setFramebuffer(target);
  // Letterbox bars (and the whole output when there is no guest image).
  cmd->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));

  if (has_image)
    RecordScanOut(s, scan, shown);
  else
    SetGameFrameLocked(kInvalidDescriptor, nullptr);

  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(out_w), float(out_h)));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(out_w), int32_t(out_h)));
  // The SDK's own menus, then reNut's overlay over them.
  if (s.overlay_hook)
    s.overlay_hook(cmd, target, out_w, out_h);
  overlay::DrawLocked(cmd, out_w, out_h);
  if (screen_fb)
    FinishScreenLocked(s, back_fb, out_w, out_h);

  cmd->setFramebuffer(nullptr);
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));

  plume::RenderCommandSemaphore *present_semaphore = s.present_semaphores[image_index].get();
  const uint32_t submitted = s.frame;
  detail::SubmitAndAdvance(s, acquire, present_semaphore);
  const auto present = [swap_chain = s.swap_chain.get(), image_index, present_semaphore,
                        resize = &s.resize_requested] {
    plume::RenderCommandSemaphore *waits[] = {present_semaphore};
    if (!swap_chain->present(image_index, waits, 1))
      resize->store(true, std::memory_order_release);
  };
  if (CommandRecorder *recorder = s.recorders[submitted]) {
    // After the submission, on the recording thread (where a vsync'd present
    // holds until the display takes the frame).
    s.present_call = recorder->RecordCall(present);
  } else {
    // With vsync the present can hold until the display takes a frame.
    render_thread::WaitScope wait;
    present();
  }
}

} // namespace

namespace {

// A guest that has not presented for this long is stalled (loading, movie
// decode); the UI pump then presents the overlay on its own.
constexpr int64_t kGuestStallMs = 100;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

void Video::Present(const PresentImage *image) {
  auto &s = State();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::lock_guard lock(s.mutex);
  if (!s.host_ready || s.shutting_down.load(std::memory_order_acquire))
    return;
  PresentLocked(s, image);
  s.last_guest_present_ms.store(NowMs(), std::memory_order_release);
  if (rex::gpu::IGraphicsSystem *graphics = s.graphics.load(std::memory_order_acquire))
    graphics->ReportFrame();
}

void Video::PresentOverlayOnly() {
  auto &s = State();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  const int64_t now = NowMs();
  if (s.guest_ready &&
      now - s.last_guest_present_ms.load(std::memory_order_acquire) < kGuestStallMs)
    return;
  // The render thread holds the mutex while it waits on the UI thread for the
  // overlay, so never block on it here.
  std::unique_lock lock(s.mutex, std::try_to_lock);
  if (!lock.owns_lock() || !s.host_ready || s.shutting_down.load(std::memory_order_acquire))
    return;
  // The last guest frame stays up while the guest stalls.
  const PresentImage image =
      s.guest_ready && s.last_front_buffer ? FrontBufferImageLocked(s.last_front_buffer) : PresentImage{};
  PresentLocked(s, image.texture ? &image : nullptr);
}

} // namespace renut::gpu
