/**
 * @file    gpu/backend/present.cpp
 * @brief   End of frame: swapchain acquire, guest image scan-out, overlay,
 *          submit and present.
 */
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include <plume_render_interface.h>

#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/command_recorder.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/backend/native_window.h"
#include "renut_engine/gpu/backend/smaa.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/render/graphics_options.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/backend/video_internal.h"

REXCVAR_DEFINE_BOOL(renut_vsync, true, "Nuts&Bolts/Graphics",
                    "Show each frame at the display's refresh: no tearing, and the game runs at "
                    "the refresh rate (frame_cap then only limits below it). Off shows frames at "
                    "once, with tearing.");

namespace renut::gpu {

namespace {

// renut_vsync onto the swapchain (both backends start with it on). Vulkan
// makes the change by recreating the swapchain, which needsResize asks for.
void ApplyVsync(VideoState &s) {
  static bool applied = true;
  const bool vsync = REXCVAR_GET(renut_vsync);
  if (vsync == applied)
    return;
  s.swap_chain->setVsyncEnabled(vsync);
  applied = vsync;
  RGPU_INFO("Vsync {}", vsync ? "on" : "off");
}

void RebuildSwapChain(VideoState &s) {
  // Submit rather than abandon an open list: plume tracks layouts on record.
  if (s.list_open)
    detail::SubmitAndAdvance(s, nullptr, nullptr);
  detail::WaitAllSubmitted(s);
  s.swap_framebuffers.clear();
  if (!s.swap_chain->resize() || !detail::BuildSwapChainTargets(s)) {
    if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
      RGPU_ERROR("Swapchain resize failed");  // 0x0 while minimized is expected
  }
}

// The most of the game's frame trimmed off either axis to fill the output.
constexpr float kMaxTrim = 0.15f;

struct FrameFit {
  plume::RenderViewport viewport;
  float visible[2] = {1.0f, 1.0f};  // the share of the frame shown on each axis
};

// The frame fills the output (a window that is not the frame's shape, say a
// maximized one) without bars: it covers it, keeping its proportions, and the
// part that overflows is trimmed evenly from both edges. Past kMaxTrim, the
// rest is fitted with bars.
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

void RecordScanOut(VideoState &s, const PresentImage &image, uint32_t out_w, uint32_t out_h) {
  plume::RenderCommandList *cmd = s.command_list;
  if (image.layout && *image.layout != plume::RenderTextureLayout::SHADER_READ) {
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderTextureBarrier(image.texture, plume::RenderTextureLayout::SHADER_READ));
    *image.layout = plume::RenderTextureLayout::SHADER_READ;
  }

  const uint32_t src_w = image.source_width ? image.source_width : image.width;
  const uint32_t src_h = image.source_height ? image.source_height : image.height;
  const FrameFit fit = FitFrame(out_w, out_h, src_w, src_h);
  const plume::RenderViewport &vp = fit.viewport;
  cmd->setViewports(vp);
  cmd->setScissors(plume::RenderRect(int32_t(vp.x), int32_t(vp.y), int32_t(vp.x + vp.width),
                                     int32_t(vp.y + vp.height)));

  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(s.present_pipeline.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);

  UtilityPush push{};
  // The frame's share of the texture, less what the fit trims from its edges.
  const float frame_u = image.width ? float(src_w) / float(image.width) : 1.0f;
  const float frame_v = image.height ? float(src_h) / float(image.height) : 1.0f;
  push.scale[0] = frame_u * fit.visible[0];
  push.scale[1] = frame_v * fit.visible[1];
  push.translate[0] = frame_u * (1.0f - fit.visible[0]) * 0.5f;
  push.translate[1] = frame_v * (1.0f - fit.visible[1]) * 0.5f;
  push.texture_index = image.descriptor;
  push.sampler_index = kLinearClampSamplerSlot;
  push.param0 = s.output_gamma;
  // 1: FXAA (renut_anti_aliasing; SMAA ran before, ApplySmaaLocked).
  push.param1 = PostAntiAliasing() == kPostAntiAliasingFxaa ? 1.0f : 0.0f;
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
}

// Records the back-buffer pass and presents. Caller holds s.mutex.
void PresentLocked(VideoState &s, const PresentImage *image) {
  // The swapchain is the render thread's again once the recording thread has
  // presented the frame before (an acquire before that present would get the
  // image being presented).
  {
    render_thread::WaitScope wait;
    CommandRecorder::WaitForCall(s.present_call);
    s.present_call = 0;
  }
  // Logs, once, why the GPU stopped if an earlier submission faulted.
  ReportDeviceLost(s.device.get());
  if (!s.swap_chain) {
    // Back from the background (Android): a swapchain on the new surface.
    plume::RenderWindow window{};
    if (s.suspended.load(std::memory_order_acquire) || !GetNativeRenderWindow(window) ||
        !detail::CreateSwapChain(s, window)) {
      if (s.list_open)
        detail::SubmitAndAdvance(s, nullptr, nullptr);
      return;
    }
  }
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
  static const uint32_t kProfileLabel = gpu_profile::Label("present and overlays");
  gpu_profile::Mark(s, kProfileLabel);
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

  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(back, plume::RenderTextureLayout::COLOR_WRITE));
  cmd->setFramebuffer(back_fb);
  // Letterbox bars (and the whole output when there is no guest image).
  cmd->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));

  if (has_image)
    RecordScanOut(s, scan, out_w, out_h);

  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(out_w), float(out_h)));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(out_w), int32_t(out_h)));
  // The SDK's own menus, then reNut's overlay over them.
  if (s.overlay_hook)
    s.overlay_hook(cmd, back_fb, out_w, out_h);
  overlay::DrawLocked(cmd, out_w, out_h);

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

std::mutex g_suspend_mutex;
std::condition_variable g_suspend_cv;

// A guest that has not presented for this long is stalled (loading, movie
// decode); the UI pump then presents the overlay on its own.
constexpr int64_t kGuestStallMs = 100;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

void Video::SetSuspended(bool suspended) {
  auto &s = State();
  if (!suspended) {
    {
      std::lock_guard lock(g_suspend_mutex);
      s.suspended.store(false, std::memory_order_release);
    }
    g_suspend_cv.notify_all();
    RGPU_INFO("Foreground: resuming");
    return;
  }
  s.suspended.store(true, std::memory_order_release);
  FlushPipelineCache();  // Android may kill a background app
  std::lock_guard lock(s.mutex);
  if (!s.swap_chain)
    return;
  // The surface goes away right after this: nothing may still use it.
  if (s.list_open)
    detail::SubmitAndAdvance(s, nullptr, nullptr);
  detail::WaitAllSubmitted(s);
  s.swap_framebuffers.clear();
  s.present_semaphores.clear();
  s.swap_chain.reset();
  RGPU_INFO("Background: swapchain released, game paused");
}

void Video::SetPaused(bool paused) {
  auto &s = State();
  {
    std::lock_guard lock(g_suspend_mutex);
    s.paused.store(paused, std::memory_order_release);
  }
  if (!paused)
    g_suspend_cv.notify_all();
}

void Video::Present(const PresentImage *image) {
  auto &s = State();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  if (s.suspended.load(std::memory_order_acquire) || s.paused.load(std::memory_order_acquire)) {
    // Holding the guest's present pauses the game (background, side menu).
    std::unique_lock lock(g_suspend_mutex);
    g_suspend_cv.wait(lock, [&s] {
      return (!s.suspended.load(std::memory_order_acquire) &&
              !s.paused.load(std::memory_order_acquire)) ||
             s.shutting_down.load(std::memory_order_acquire);
    });
  }
  std::lock_guard lock(s.mutex);
  if (!s.host_ready || s.shutting_down.load(std::memory_order_acquire))
    return;
  PresentLocked(s, image);
  s.last_guest_present_ms.store(NowMs(), std::memory_order_release);
}

void Video::PresentOverlayOnly() {
  auto &s = State();
  if (s.shutting_down.load(std::memory_order_acquire) || s.suspended.load(std::memory_order_acquire))
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
  if (s.guest_ready) {
    static uint32_t logged = 0;
    if (logged++ < 32) {
      RGPU_WARN("Present: the game has not presented for {} ms; the UI presents {}",
                now - s.last_guest_present_ms.load(std::memory_order_acquire),
                image.texture ? "its last frame" : "black (no last frame)");
    }
  }
  PresentLocked(s, image.texture ? &image : nullptr);
}

} // namespace renut::gpu
