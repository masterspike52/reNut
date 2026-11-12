#include "renut_engine/gpu/overlay/app_integration.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <utility>

#include <rex/gpu/graphics_system.h>
#include <rex/platform/native_window.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/ui_system.h>
#include <rex/utility.h>

#include "renut_engine/gpu/backend/native_window.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/overlay/overlay_drawer.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"
#include "renut_engine/gpu/backend/video.h"

namespace renut::gpu {

namespace {

// Records the SDK's menus (F3, F4, console, achievements, its dialogs), never
// freed: a task already posted may still run after shutdown.
OverlayDrawer &g_recorder = *new OverlayDrawer();

// Records the SDK's menus on the UI thread for Present to draw.
void RecordSdkMenus(rex::ui::IUISystem &ui) {
  uint32_t width = 0, height = 0;
  if (!ui.HasDialogs() || !GetRenderWindowPixelSize(width, height) || !width || !height) {
    OverlayDrawer::ClearLatest();
    return;
  }
  const uint64_t recorded = OverlayDrawer::FramesRecorded();
  // On the UI thread it builds the frame there and then, and records it through g_recorder.
  ui.DrawOverlays(g_recorder);
  if (OverlayDrawer::FramesRecorded() == recorded)
    OverlayDrawer::ClearLatest();
}

// Records the SDK's menus and drives Video::PresentOverlayOnly, ticking from
// its own thread so the UI thread's event processing isn't starved.
class OverlayPump {
public:
  void Start(rex::ui::IUISystem &ui) {
    Stop();
    stop_.store(false, std::memory_order_release);
    pending_.store(false, std::memory_order_release);
    thread_ = std::thread([this, &ui] {
      while (!stop_.load(std::memory_order_acquire)) {
        if (!pending_.exchange(true, std::memory_order_acq_rel)) {
          ui.PostTask([this, &ui] {
            RecordSdkMenus(ui);
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

// The window's size changes, on the UI thread: the swapchain follows at the next present.
class ResizeListener final : public rex::ui::IUIEventListener {
public:
  void OnUIEvent(const rex::ui::UIEvent &event) override {
    if (event.GetType() == rex::ui::UIEvent::Type::kResize)
      Video::RequestResize();
  }
  void OnInputEvent(rex::InputEvent &) override {}
};

ResizeListener g_resize_listener;
rex::ScopeGuard g_resize_guard;

} // namespace

bool StartRenderer(rex::ui::IUISystem &ui, rex::gpu::IGraphicsSystem &graphics) {
  const rex::Result<rex::NativeWindow> window = ui.GetNativeWindow();
  if (!window)
    return false;
  SetNativeWindow(*window);
  Video::SetGraphicsSystem(&graphics);
  if (!Video::CreateHostDevice())
    return false;
  g_resize_guard = ui.AddEventListener(g_resize_listener);
  // reNut's own overlay (overlay_ui.h), built on the UI thread with the SDK's
  // (without a game window it has no input).
  overlay::Start([&ui](std::function<void()> build) { ui.PostTask(std::move(build)); });

  // The SDK's own menus: recorded on the UI thread by the pump, drawn here
  // from the latest recording, so Present never waits for the UI thread.
  Video::SetOverlayHook([](plume::RenderCommandList *cmd, plume::RenderFramebuffer *framebuffer,
                           uint32_t width, uint32_t height) {
    OverlayDrawer::DrawLatestLocked(cmd, framebuffer, width, height);
  });

  g_pump.Start(ui);
  return true;
}

void StopGuestRendering() {
  render_thread::Stop();
  render_thread::Drain();
}

void ShutdownRenderer() {
  g_pump.Stop();
  overlay::Stop();
  g_resize_guard.Reset();
  Video::BeginShutdown();
  Video::SetGraphicsSystem(nullptr);
}

} // namespace renut::gpu
