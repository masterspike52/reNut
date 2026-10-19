/**
 * @file    game/mouse_camera.cpp
 * @brief   The mouse turns the game camera, for every camera mode.
 *
 * N&B's camera (IDA): sub_822F0B20(camera, input device, dt, ...) updates the
 * camera of a player once a frame; the active mode's update (a virtual call
 * through camera + 376) reads the player's input through two getters:
 * - sub_821E4368(device): yaw input, -(right stick X) past a 0.1 deadzone,
 *   rescaled to [-1, 1], flipped with the profile's invert X (flags[7] & 0x40);
 * - sub_821E4100(device): pitch input, right stick Y likewise (invert Y,
 *   flags[7] & 0x80), remapped through sub_8225EE90.
 * Each mode turns by input * its own rate * dt (the on-foot chase camera,
 * sub_822F31E8 -> sub_822F5EA8, through a spring). sub_821E41D8, run by the
 * camera update, turns a quick full push of the stick (past 0.9 and back
 * within 0.4 s) into a snap turn.
 *
 * With renut_mouse_camera, a click in the game window captures the mouse
 * (relative mode: hidden cursor, motion only; reNut's own, no SDK input
 * emulation) until Esc, a focus loss or an overlay window that needs the
 * cursor. While it is captured, player 1's camera takes the mouse motion of
 * the frame instead of the stick:
 * input = motion * sensitivity / dt,
 * so a mode turns by motion * sensitivity * its rate whatever the frame rate,
 * with no deadzone or response curve. The snap-turn check keeps reading the
 * stick. A frame without mouse motion reads the stick as before.
 *
 * The window's input comes through the SDK's rex::ui::Window, on Windows and
 * Linux alike: SDL, which owns the window, lives in rexruntime, and the
 * executable's own SDL symbols would reach a second, uninitialised copy.
 */
#include "renut_engine/game/mouse_camera.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "renut_engine/gpu/overlay/overlay_ui.h"

REXCVAR_DEFINE_BOOL(renut_mouse_camera, false, "Nuts&Bolts/Input",
                    "The mouse turns the camera directly in every camera mode, with no deadzone "
                    "or stick response curve. Click the game window to capture the mouse; Esc "
                    "releases it.");
REXCVAR_DEFINE_DOUBLE(renut_mouse_sensitivity, 1.0, "Nuts&Bolts/Input",
                      "How far the camera turns per mouse movement (renut_mouse_camera).")
    .range(0.05, 10.0);
REXCVAR_DEFINE_BOOL(renut_mouse_invert_y, false, "Nuts&Bolts/Input",
                    "Moving the mouse up looks down (renut_mouse_camera).");

REX_EXTERN(__imp__sub_822F0B20);  // camera update
REX_EXTERN(__imp__sub_821E4368);  // yaw input
REX_EXTERN(__imp__sub_821E4100);  // pitch input
REX_EXTERN(__imp__sub_821E41D8);  // stick snap-turn check

namespace {

// Guest data.
constexpr uint32_t kPlayerProfiles = 0x82FAD9F0;    // dword_82FAD9F0[player]
constexpr uint32_t kFrameDeltaSeconds = 0x82FAC644;  // g_frameDeltaSeconds
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

// The game window's mouse, on the UI thread (where the window sends its
// events): a left click in the focused window takes it; Esc, losing focus, an
// overlay window that needs the cursor, or turning renut_mouse_camera off gives
// it back. The events go on to the SDK's other listeners.
class MouseCapture : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
public:
  void Attach(rex::ui::Window *window) {
    window_ = window;
    window->AddInputListener(this, kZOrder);
    window->AddListener(this);
  }

  void Detach() {
    if (!window_)
      return;
    Release();
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
    window_ = nullptr;
  }

  rex::ui::Window *window() const { return window_; }

  void OnMouseDown(rex::ui::MouseEvent &e) override {
    if (!g_captured.load(std::memory_order_relaxed) && REXCVAR_GET(renut_mouse_camera) &&
        e.button() == rex::ui::MouseEvent::Button::kLeft && window_->HasFocus() &&
        !renut::gpu::overlay::WantsCursor()) {
      Capture();
    }
  }

  void OnMouseMove(rex::ui::MouseEvent &e) override {
    if (!g_captured.load(std::memory_order_relaxed))
      return;
    if (!REXCVAR_GET(renut_mouse_camera) || renut::gpu::overlay::WantsCursor()) {
      Release();
      return;
    }
    std::lock_guard lock(g_motion_mutex);
    g_motion_x += e.dx();
    g_motion_y += e.dy();
  }

  void OnKeyDown(rex::ui::KeyEvent &e) override {
    if (e.virtual_key() == rex::ui::VirtualKey::kEscape)
      Release();
  }

  void OnLostFocus(rex::ui::UISetupEvent &) override { Release(); }
  void OnClosing(rex::ui::UIEvent &) override { Detach(); }

private:
  // Above the app's own listener (0), so its key binds don't hide Esc.
  static constexpr size_t kZOrder = 1;

  void Capture() {
    cursor_before_ = window_->GetCursorVisibility();
    window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    if (!window_->SetRelativeMouseMode(true)) {
      window_->SetCursorVisibility(cursor_before_);
      return;
    }
    {
      std::lock_guard lock(g_motion_mutex);
      g_motion_x = g_motion_y = 0.0f;
    }
    g_captured.store(true, std::memory_order_release);
  }

  void Release() {
    if (!g_captured.exchange(false, std::memory_order_acq_rel))
      return;
    window_->SetRelativeMouseMode(false);
    window_->SetCursorVisibility(cursor_before_);
    std::lock_guard lock(g_motion_mutex);
    g_motion_x = g_motion_y = 0.0f;
  }

  rex::ui::Window *window_ = nullptr;
  rex::ui::Window::CursorVisibility cursor_before_ = rex::ui::Window::CursorVisibility::kVisible;
};

MouseCapture g_capture;

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
  return double(motion) * REXCVAR_GET(renut_mouse_sensitivity) * kMouseScale / dt;
}

} // namespace

namespace renut::game {

void AttachMouseCamera(rex::ui::Window *window) {
  if (!window)
    return;
  window->app_context().CallInUIThreadDeferred([window] {
    if (!g_capture.window())
      g_capture.Attach(window);
  });
}

void DetachMouseCamera() {
  rex::ui::Window *window = g_capture.window();
  if (window)
    window->app_context().CallInUIThreadSynchronous([] { g_capture.Detach(); });
}

bool MouseCaptured() {
  return g_captured.load(std::memory_order_acquire);
}

} // namespace renut::game

// void sub_822F0B20(camera, input device, double dt, ..., mode, ...)
REX_HOOK_RAW(sub_822F0B20) {
  Frame frame;
  if (REXCVAR_GET(renut_mouse_camera) && g_captured.load(std::memory_order_acquire) &&
      IsPlayerOnePad(base, ctx.r4.u32)) {
    std::lock_guard lock(g_motion_mutex);
    frame = {true, g_motion_x, g_motion_y};
    g_motion_x = g_motion_y = 0.0f;
  }
  t_frame = frame;
  __imp__sub_822F0B20(ctx, base);
  t_frame = {};
}

// double sub_821E4368(device): yaw input; -1 turns the camera right as the
// stick pushed right does.
REX_HOOK_RAW(sub_821E4368) {
  if (!t_frame.active || t_in_snap_check || t_frame.x == 0.0f) {
    __imp__sub_821E4368(ctx, base);
    return;
  }
  ctx.f1.f64 = -MouseInput(base, t_frame.x);
}

// double sub_821E4100(device): pitch input; +1 as the stick pushed up.
REX_HOOK_RAW(sub_821E4100) {
  if (!t_frame.active || t_frame.y == 0.0f) {
    __imp__sub_821E4100(ctx, base);
    return;
  }
  const double input = -MouseInput(base, t_frame.y);
  ctx.f1.f64 = REXCVAR_GET(renut_mouse_invert_y) ? -input : input;
}

// int sub_821E41D8(double dt, camera, state, device): the stick snap turn;
// it reads the stick only.
REX_HOOK_RAW(sub_821E41D8) {
  t_in_snap_check = true;
  __imp__sub_821E41D8(ctx, base);
  t_in_snap_check = false;
}
