/**
 * @file    gpu/overlay/app_integration.cpp
 */
#include "renut_engine/gpu/overlay/app_integration.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <utility>

#include <rex/runtime.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include "renut_engine/gpu/backend/native_window.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/overlay/overlay_drawer.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"
#include "renut_engine/gpu/backend/video.h"

namespace renut::gpu {

namespace {

// The SDK's menus (F3, F4, console, achievements), drawn by its ImGuiDrawer
// on the UI thread, which owns their ImGui; OverlayDrawer records the frame
// for Present to draw.
void RecordSdkMenus(rex::ui::ImGuiDrawer *drawer) {
  uint32_t width = 0, height = 0;
  if (!drawer || !drawer->HasDialogs() || !GetRenderWindowPixelSize(width, height) || !width ||
      !height) {
    OverlayDrawer::ClearLatest();
    return;
  }
  const uint64_t recorded = OverlayDrawer::FramesRecorded();
  rex::ui::AppUIDrawContext ctx(width, height);
  drawer->Draw(ctx);
  if (OverlayDrawer::FramesRecorded() == recorded)
    OverlayDrawer::ClearLatest();
}

// Records the SDK's menus and drives Video::PresentOverlayOnly for the whole
// session. Ticks come from their own thread: a UI-thread tick that re-queues
// itself would starve the UI thread's event processing.
class OverlayPump {
public:
  void Start(rex::ui::WindowedAppContext &app_context,
             std::function<rex::ui::ImGuiDrawer *()> imgui_drawer) {
    Stop();
    stop_.store(false, std::memory_order_release);
    pending_.store(false, std::memory_order_release);
    thread_ = std::thread([this, &app_context, imgui_drawer = std::move(imgui_drawer)] {
      while (!stop_.load(std::memory_order_acquire)) {
        if (!pending_.exchange(true, std::memory_order_acq_rel)) {
          app_context.CallInUIThreadDeferred([this, imgui_drawer] {
            RecordSdkMenus(imgui_drawer());
            Video::PresentOverlayOnly();
            pending_.store(false, std::memory_order_release);
          });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
      }
    });
  }

  void Stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable())
      thread_.join();
  }

  ~OverlayPump() { Stop(); }

private:
  std::thread thread_;
  std::atomic<bool> stop_{true};
  std::atomic<bool> pending_{false};
};

OverlayPump g_pump;

} // namespace

void ConfigureRuntime(rex::RuntimeConfig &config) {
  config.graphics = nullptr;
  config.gpu_plugin.clear();
}

std::unique_ptr<rex::ui::ImmediateDrawer> CreateOverlayDrawer() {
  return std::make_unique<OverlayDrawer>();
}

bool StartRenderer(rex::ui::WindowedAppContext &app_context,
                   std::function<rex::ui::ImGuiDrawer *()> imgui_drawer) {
  if (!Video::CreateHostDevice()) {
    RGPU_ERROR("Renderer initialisation failed");
    return false;
  }
  // reNut's own overlay (overlay_ui.h), built on the UI thread with the SDK's.
  if (!overlay::Start([&app_context](std::function<void()> build) {
        app_context.CallInUIThreadDeferred(std::move(build));
      }))
    RGPU_WARN("Overlay input unavailable: no game window");

  // The SDK's own menus: recorded on the UI thread by the pump, drawn here
  // from the latest recording, so Present never waits for the UI thread.
  Video::SetOverlayHook([](plume::RenderCommandList *cmd, plume::RenderFramebuffer *framebuffer,
                           uint32_t width, uint32_t height) {
    OverlayDrawer::DrawLatestLocked(cmd, framebuffer, width, height);
  });

  g_pump.Start(app_context, std::move(imgui_drawer));
  return true;
}

void OnWindowResized() {
  Video::RequestResize();
}

void ShutdownRenderer() {
  g_pump.Stop();
  overlay::Stop();
  Video::BeginShutdown();
}

} // namespace renut::gpu
