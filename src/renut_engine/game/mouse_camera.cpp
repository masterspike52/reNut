#include "renut_engine/game/mouse_camera.h"

#include <rex/hid/input_system.h>
#include <rex/input_keys.h>
#include <rex/module.h>
#include <rex/ppc/hook.h>
#include <rex/setting.h>
#include <rex/types.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/ui_system.h>
#include <rex/utility.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <rexglue_guide/guide.h>

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/game/vehicle_drivers.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

REX_EXTERN(__imp__sub_822C0448);  // camera update
REX_EXTERN(__imp__sub_821B42B8);  // yaw input
REX_EXTERN(__imp__sub_821B4050);  // pitch input
REX_EXTERN(__imp__sub_821B4128);  // stick snap-turn check

namespace {

struct MouseCameraSettings final : rex::SettingOwner {
  MouseCameraSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
  REX_SETTING(bool, renut_mouse_camera, false,
              "The mouse turns the camera directly in every camera mode, with no deadzone "
              "or stick response curve. Click the game window to capture the mouse; Esc "
              "releases it.");
  REX_SETTING(f64, renut_mouse_sensitivity, 1.0,
              "How far the camera turns per mouse movement (renut_mouse_camera).", rex::Range{0.05, 10.0});
  REX_SETTING(bool, renut_mouse_invert_y, false, "Moving the mouse up looks down (renut_mouse_camera).");
};
MouseCameraSettings &g_mouse = *new MouseCameraSettings();  // never freed, registered by RenutApp::OnConfigure

// Guest data.
constexpr uint32_t kPlayerProfiles = 0x82E1B280;    // dword_82E1B280[player]
constexpr uint32_t kFrameDeltaSeconds = 0x82E19924;  // g_frameDeltaSeconds
// Input device (inputDeviceCreatePlayerPad): type at + 8 (3: a player's pad),
// its player's profile at + 368.
constexpr uint32_t kDeviceType = 8;
constexpr uint32_t kDevicePlayerPad = 3;
constexpr uint32_t kDeviceProfile = 368;

// Turn input per pixel of mouse motion per second of frame (sensitivity 1).
constexpr double kMouseScale = 0.0015;

uint32_t Load32(const uint8_t *base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return rex::byte_swap(v);
}

float LoadFloat(const uint8_t *base, uint32_t va) {
  const uint32_t bits = Load32(base, va);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// Mouse motion since a camera last took it (the UI thread adds, the game
// thread takes).
std::mutex g_motion_mutex;
float g_motion_x = 0.0f, g_motion_y = 0.0f;

// Whether the mouse is captured; the camera update reads it.
std::atomic<bool> g_captured{false};

// Captures the mouse on a left click in the game window (UI thread); Esc, focus
// loss, an overlay needing the cursor or renut_mouse_camera off releases it.
class MouseCapture : public rex::ui::IUIEventListener {
public:
  void Attach(rex::ui::IUISystem &ui) {
    ui_ = &ui;
    input_ = REX_INTERFACE(rex::hid::IInputSystem);
    guard_ = ui.AddEventListener(*this);
  }

  void Detach() {
    if (!ui_)
      return;
    Release();
    guard_.Reset();
    ui_ = nullptr;
    input_ = nullptr;
  }

  rex::ui::IUISystem *ui() const { return ui_; }

  void OnInputEvent(rex::InputEvent &e) override {
    using Type = rex::InputEvent::Type;
    switch (e.type) {
    case Type::kButtonDown:
      if (!g_captured.load(std::memory_order_relaxed) && g_mouse.renut_mouse_camera.Get() &&
          e.button == rex::MouseButton::kLeft && ui_->IsFocused() &&
          !renut::gpu::overlay::WantsCursor() && !rexglue_guide::IsOpen()) {
        Capture();
      }
      break;
    case Type::kMotion: {
      if (!g_captured.load(std::memory_order_relaxed))
        return;
      // The Guide takes the mouse too: the cursor comes back for it.
      if (!g_mouse.renut_mouse_camera.Get() || renut::gpu::overlay::WantsCursor() ||
          rexglue_guide::IsOpen()) {
        Release();
        return;
      }
      std::lock_guard lock(g_motion_mutex);
      g_motion_x += e.dx;
      g_motion_y += e.dy;
      break;
    }
    case Type::kKeyDown:
      if (e.key == rex::VirtualKey::kEscape)
        Release();
      break;
    default:
      break;
    }
  }

  // The UI system takes a focus change after its listeners, and a listener can't go
  // during a delivery, so both wait for the UI thread's next task.
  void OnUIEvent(const rex::ui::UIEvent &e) override {
    using Type = rex::ui::UIEvent::Type;
    if (e.GetType() == Type::kFocus) {
      ui_->PostTask([this] {
        if (ui_ && !ui_->IsFocused())
          Release();
      });
    } else if (e.GetType() == Type::kClose) {
      ui_->PostTask([this] { Detach(); });
    }
  }

private:
  // The pointer locked (relative motion), hidden; Release frees it.
  void Capture() {
    if (!input_)
      return;
    input_->SetPointerMode(rex::PointerMode::kLocked);
    {
      std::lock_guard lock(g_motion_mutex);
      g_motion_x = g_motion_y = 0.0f;
    }
    g_captured.store(true, std::memory_order_release);
  }

  void Release() {
    if (!g_captured.exchange(false, std::memory_order_acq_rel))
      return;
    if (input_)
      input_->SetPointerMode(rex::PointerMode::kFree);
    std::lock_guard lock(g_motion_mutex);
    g_motion_x = g_motion_y = 0.0f;
  }

  rex::ui::IUISystem *ui_ = nullptr;
  rex::hid::IInputSystem *input_ = nullptr;
  rex::ScopeGuard guard_;
};

MouseCapture &g_capture = *new MouseCapture();  // never freed: no listener removal at exit

// The motion player 1's camera takes this frame, set for the camera update's
// duration.
struct Frame {
  bool active = false;
  float x = 0.0f, y = 0.0f;
};
thread_local Frame t_frame;
thread_local bool t_in_snap_check = false;

bool IsPlayerOnePad(const uint8_t *base, uint32_t device) {
  return device && Load32(base, device + kDeviceType) == kDevicePlayerPad &&
         Load32(base, device + kDeviceProfile) == Load32(base, kPlayerProfiles);
}

// Turn input for this frame's motion along one axis (0 without motion).
double MouseInput(const uint8_t *base, float motion) {
  const float dt = LoadFloat(base, kFrameDeltaSeconds);
  if (motion == 0.0f || !(dt > 0.0f))
    return 0.0;
  return double(motion) * g_mouse.renut_mouse_sensitivity.Get() * kMouseScale / dt;
}

} // namespace

namespace renut::game {

void AttachMouseCamera(rex::ui::IUISystem &ui) {
  ui.PostTask([&ui] {
    if (!g_capture.ui())
      g_capture.Attach(ui);
  });
}

void DetachMouseCamera() {
  rex::ui::IUISystem *ui = g_capture.ui();
  if (ui && !ui->RunSynchronous([] { g_capture.Detach(); }))
    g_capture.Detach();
}

bool MouseCaptured() {
  return g_captured.load(std::memory_order_acquire);
}

} // namespace renut::game

// void sub_822C0448(camera, input device, double dt, ..., mode, ...)
REX_HOOK_RAW(sub_822C0448) {
  Frame frame;
  if (g_mouse.renut_mouse_camera.Get() && g_captured.load(std::memory_order_acquire) &&
      IsPlayerOnePad(base, ctx.r4.u32)) {
    std::lock_guard lock(g_motion_mutex);
    frame = {true, g_motion_x, g_motion_y};
    g_motion_x = g_motion_y = 0.0f;
  }
  t_frame = frame;
  // The Remote Control: player 1's camera follows the vehicle it drives.
  renut::game::VehicleDriversCamera(ctx, base);
  __imp__sub_822C0448(ctx, base);
  t_frame = {};
}

// double sub_821B42B8(device): yaw input; -1 turns the camera right as the
// stick pushed right does.
REX_HOOK_RAW(sub_821B42B8) {
  if (!t_frame.active || t_in_snap_check || t_frame.x == 0.0f) {
    __imp__sub_821B42B8(ctx, base);
    return;
  }
  ctx.f1.f64 = -MouseInput(base, t_frame.x);
}

// double sub_821B4050(device): pitch input; +1 as the stick pushed up.
REX_HOOK_RAW(sub_821B4050) {
  if (!t_frame.active || t_frame.y == 0.0f) {
    __imp__sub_821B4050(ctx, base);
    return;
  }
  const double input = -MouseInput(base, t_frame.y);
  ctx.f1.f64 = g_mouse.renut_mouse_invert_y.Get() ? -input : input;
}

// int sub_821B4128(double dt, camera, state, device): the stick snap turn;
// it reads the stick only.
REX_HOOK_RAW(sub_821B4128) {
  t_in_snap_check = true;
  __imp__sub_821B4128(ctx, base);
  t_in_snap_check = false;
}
