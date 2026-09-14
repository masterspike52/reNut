#include "mnk_controls.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <rex/input/input.h>
#include <rex/system/xtypes.h>

#include "imgui.h"
#include "renut_logging.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#if defined(_MSC_VER)
#include <stdlib.h>
#define RENUT_BSWAP32(x) _byteswap_ulong(x)
#else
#define RENUT_BSWAP32(x) __builtin_bswap32(x)
#endif

// =============================================================================
// Bind table
//
// One entry per bindable action. The macro drives three things that must stay
// in step: the cvar definitions, the enum used to index them, and the list the
// overlay renders.
//
// Values are key names as understood by rex::ui::ParseVirtualKey ("W", "Shift",
// "LMB", "Up", ...). A bind may list several keys separated by ',' or '|', so
// one action can sit on more than one physical input (e.g. "LMB,Space").
// =============================================================================
// clang-format off
#define RENUT_MNK_BINDS(X)                                                                        \
  X(mnk_bind_forward,     "W",      "Move forward",              "Movement")                      \
  X(mnk_bind_back,        "S",      "Move back",                 "Movement")                      \
  X(mnk_bind_left,        "A",      "Move left",                 "Movement")                      \
  X(mnk_bind_right,       "D",      "Move right",                "Movement")                      \
  X(mnk_bind_walk,        "Shift",  "Walk (hold)",               "Movement")                      \
  X(mnk_bind_look_up,     "",       "Look up",                   "Camera")                        \
  X(mnk_bind_look_down,   "",       "Look down",                 "Camera")                        \
  X(mnk_bind_look_left,   "",       "Look left",                 "Camera")                        \
  X(mnk_bind_look_right,  "",       "Look right",                "Camera")                        \
  X(mnk_bind_a,           "Space",  "A button",                  "Buttons")                       \
  X(mnk_bind_b,           "E",      "B button",                  "Buttons")                       \
  X(mnk_bind_x,           "R",      "X button",                  "Buttons")                       \
  X(mnk_bind_y,           "F",      "Y button",                  "Buttons")                       \
  X(mnk_bind_lb,          "Q",      "Left bumper",               "Buttons")                       \
  X(mnk_bind_rb,          "C",      "Right bumper",              "Buttons")                       \
  X(mnk_bind_lt,          "RMB",    "Left trigger",              "Buttons")                       \
  X(mnk_bind_rt,          "LMB",    "Right trigger",             "Buttons")                       \
  X(mnk_bind_l3,          "V",      "Left stick click",          "Buttons")                       \
  X(mnk_bind_r3,          "MMB",    "Right stick click",         "Buttons")                       \
  X(mnk_bind_dpad_up,     "Up",     "D-pad up",                  "D-pad")                         \
  X(mnk_bind_dpad_down,   "Down",   "D-pad down",                "D-pad")                         \
  X(mnk_bind_dpad_left,   "Left",   "D-pad left",                "D-pad")                         \
  X(mnk_bind_dpad_right,  "Right",  "D-pad right",               "D-pad")                         \
  X(mnk_bind_start,       "Escape", "Start (pause menu)",        "System")                        \
  X(mnk_bind_select,      "Tab",    "Back button",               "System")
// clang-format on

// -----------------------------------------------------------------------------
// cvars
// -----------------------------------------------------------------------------
REXCVAR_DEFINE_BOOL(mnk_controls, false, "Nuts&Bolts/Controls",
                    "Drive movement and camera from the keyboard and mouse. Writes straight into "
                    "the game's own movement input, so there is no controller emulation, no "
                    "deadzone and no stick ramp in the way. A connected pad keeps working.");

// rexglue signs exactly one profile in, on slot 0, and XamInputGetState reports
// every other slot as disconnected, so in practice this is always 0 today. It
// stays a knob for the day a second profile can sign in.
REXCVAR_DEFINE_INT32(mnk_pad_slot, 0, "Nuts&Bolts/Controls",
                     "Which player slot the keyboard and mouse drive (0 = player one).")
    .range(0, 3);

REXCVAR_DEFINE_INT32(mnk_look_sensitivity, 50, "Nuts&Bolts/Controls",
                     "Mouse look sensitivity. 50 = 1.0x; every step is 2%.")
    .range(1, 400);

REXCVAR_DEFINE_BOOL(mnk_invert_look, false, "Nuts&Bolts/Controls",
                    "Invert the mouse look Y axis.");

REXCVAR_DEFINE_INT32(mnk_walk_scale, 45, "Nuts&Bolts/Controls",
                     "Movement speed while the walk bind is held, as a percentage of full tilt.")
    .range(5, 100);

REXCVAR_DEFINE_BOOL(mnk_lock_cursor, true, "Nuts&Bolts/Controls",
                    "Hide the cursor and lock it to the window while the game has input. Turn off "
                    "if you want the cursor free (mouse look stops working).");

#define RENUT_MNK_DEFINE_BIND(name, def, label, section) \
  REXCVAR_DEFINE_STRING(name, def, "Nuts&Bolts/Controls", label);
RENUT_MNK_BINDS(RENUT_MNK_DEFINE_BIND)
#undef RENUT_MNK_DEFINE_BIND

namespace {

using rex::ui::VirtualKey;

enum BindId : std::size_t {
#define RENUT_MNK_BIND_ENUM(name, def, label, section) kBind_##name,
  RENUT_MNK_BINDS(RENUT_MNK_BIND_ENUM)
#undef RENUT_MNK_BIND_ENUM
      kBindCount
};

using BindGetter = const std::string& (*)();

const BindGetter kBindGetters[kBindCount] = {
#define RENUT_MNK_BIND_GETTER(name, def, label, section) \
  []() -> const std::string& { return REXCVAR_GET(name); },
    RENUT_MNK_BINDS(RENUT_MNK_BIND_GETTER)
#undef RENUT_MNK_BIND_GETTER
};

const renut::mnk::BindInfo kBindInfo[kBindCount] = {
#define RENUT_MNK_BIND_INFO(name, def, label, section) {#name, label, section},
    RENUT_MNK_BINDS(RENUT_MNK_BIND_INFO)
#undef RENUT_MNK_BIND_INFO
};

// -----------------------------------------------------------------------------
// Xbox pad button bits, as the game's own button word uses them. sub_8239FF20
// takes XINPUT_GAMEPAD.wButtons verbatim and adds two synthetic bits for the
// triggers past their threshold, so our word has to use the same layout.
// -----------------------------------------------------------------------------
constexpr uint32_t kBtnDpadUp = 0x0001;
constexpr uint32_t kBtnDpadDown = 0x0002;
constexpr uint32_t kBtnDpadLeft = 0x0004;
constexpr uint32_t kBtnDpadRight = 0x0008;
constexpr uint32_t kBtnStart = 0x0010;
constexpr uint32_t kBtnBack = 0x0020;
constexpr uint32_t kBtnLThumb = 0x0040;
constexpr uint32_t kBtnRThumb = 0x0080;
constexpr uint32_t kBtnLShoulder = 0x0100;
constexpr uint32_t kBtnRShoulder = 0x0200;
constexpr uint32_t kBtnA = 0x1000;
constexpr uint32_t kBtnB = 0x2000;
constexpr uint32_t kBtnX = 0x4000;
constexpr uint32_t kBtnY = 0x8000;
constexpr uint32_t kBtnLTrigger = 0x10000;  // synthetic, set by sub_8239FF20
constexpr uint32_t kBtnRTrigger = 0x20000;  // synthetic, set by sub_8239FF20

// Mouse pixels per frame that reach full stick tilt at 1.0x sensitivity.
constexpr float kPixelsFullTilt = 25.0f;
// Mouse motion past full tilt is carried into the next frame instead of being
// dropped, so a fast flick still turns the whole way. Capped so a single huge
// jump (alt-tab, cursor warp) can't keep the camera spinning.
constexpr float kMaxLookCarry = 3.0f;

// =============================================================================
// Host-side keyboard/mouse state.
//
// Written on the UI thread from window events, read on the guest thread from
// the pad hook, hence the mutex. We never mark events handled -- the ImGui
// drawer sits above us at z-order 64 and gets first refusal already, and
// swallowing events here would break the SDK's own binds.
// =============================================================================
class HostInput final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  void Attach(rex::ui::Window* window) {
    if (window_ == window) {
      return;
    }
    Detach();
    window_ = window;
    if (window_) {
      window_->AddInputListener(this, 1);  // below the ImGui drawer (64)
      window_->AddListener(this);
    }
  }

  void Detach() {
    if (!window_) {
      return;
    }
    SetCursorLocked(false);
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
    window_ = nullptr;
  }

  // ---- guest-thread reads ---------------------------------------------------

  bool IsBindHeld(BindId id) const {
    const std::string& text = kBindGetters[id]();
    if (text.empty()) {
      return false;
    }
    std::lock_guard lock(mutex_);
    bool held = false;
    ForEachKey(text, [&](VirtualKey vk) {
      const uint16_t idx = static_cast<uint16_t>(vk);
      if (idx < kKeyCount && key_down_[idx]) {
        held = true;
        return true;
      }
      return false;
    });
    return held;
  }

  // Consumes the accumulated mouse motion, in pixels.
  void TakeMouseDelta(float& dx, float& dy) {
    std::lock_guard lock(mutex_);
    dx = static_cast<float>(mouse_dx_);
    dy = static_cast<float>(mouse_dy_);
    mouse_dx_ = 0;
    mouse_dy_ = 0;
  }

  void ClearKeys() {
    std::lock_guard lock(mutex_);
    std::memset(key_down_, 0, sizeof(key_down_));
    mouse_dx_ = 0;
    mouse_dy_ = 0;
  }

  bool has_focus() const {
    std::lock_guard lock(mutex_);
    return has_focus_;
  }

  bool attached() const { return window_ != nullptr; }

  // ---- rebind capture -------------------------------------------------------

  void BeginCapture() {
    std::lock_guard lock(mutex_);
    capturing_ = true;
    captured_.clear();
  }

  void CancelCapture() {
    std::lock_guard lock(mutex_);
    capturing_ = false;
    captured_.clear();
  }

  bool IsCapturing() const {
    std::lock_guard lock(mutex_);
    return capturing_;
  }

  bool TakeCaptured(std::string& out) {
    std::lock_guard lock(mutex_);
    if (captured_.empty()) {
      return false;
    }
    out = std::move(captured_);
    captured_.clear();
    return true;
  }

  // ---- cursor lock ----------------------------------------------------------

  void SetCursorLocked(bool locked) {
    if (!window_ || locked == cursor_locked_) {
      return;
    }
    cursor_locked_ = locked;
    if (locked) {
      window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
      window_->CaptureMouse();
      std::lock_guard lock(mutex_);
      mouse_dx_ = 0;
      mouse_dy_ = 0;
    } else {
      window_->ReleaseMouse();
      window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    }
  }

  bool cursor_locked() const { return cursor_locked_; }

  // ---- WindowInputListener --------------------------------------------------

  void OnKeyDown(rex::ui::KeyEvent& e) override { SetKey(e.virtual_key(), true); }
  void OnKeyUp(rex::ui::KeyEvent& e) override { SetKey(e.virtual_key(), false); }

  void OnMouseDown(rex::ui::MouseEvent& e) override { SetKey(ButtonToKey(e.button()), true); }
  void OnMouseUp(rex::ui::MouseEvent& e) override { SetKey(ButtonToKey(e.button()), false); }

  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (!cursor_locked_ || !window_) {
      return;
    }
    // Locked: measure how far the cursor drifted from the window centre, then
    // put it back. The recentre generates another move event with a zero
    // delta, so this doesn't feed back on itself.
    const int32_t cx = static_cast<int32_t>(window_->GetActualLogicalWidth() / 2);
    const int32_t cy = static_cast<int32_t>(window_->GetActualLogicalHeight() / 2);
    const int32_t dx = e.x() - cx;
    const int32_t dy = e.y() - cy;
    if (dx == 0 && dy == 0) {
      return;
    }
    {
      std::lock_guard lock(mutex_);
      mouse_dx_ += dx;
      mouse_dy_ += dy;
    }
    CenterCursor(cx, cy);
  }

  // ---- WindowListener -------------------------------------------------------

  void OnLostFocus(rex::ui::UISetupEvent&) override {
    {
      std::lock_guard lock(mutex_);
      has_focus_ = false;
    }
    ClearKeys();
    SetCursorLocked(false);
  }

  void OnGotFocus(rex::ui::UISetupEvent&) override {
    std::lock_guard lock(mutex_);
    has_focus_ = true;
  }

  void OnClosing(rex::ui::UIEvent&) override { Detach(); }

 private:
  static constexpr size_t kKeyCount = 256;

  static VirtualKey ButtonToKey(rex::ui::MouseEvent::Button button) {
    switch (button) {
      case rex::ui::MouseEvent::Button::kLeft: return VirtualKey::kLButton;
      case rex::ui::MouseEvent::Button::kRight: return VirtualKey::kRButton;
      case rex::ui::MouseEvent::Button::kMiddle: return VirtualKey::kMButton;
      case rex::ui::MouseEvent::Button::kX1: return VirtualKey::kXButton1;
      case rex::ui::MouseEvent::Button::kX2: return VirtualKey::kXButton2;
      default: return VirtualKey::kNone;
    }
  }

  // Walk the ',' / '|' separated key list in a bind's cvar value. `fn` returns
  // true to stop early.
  template <typename Fn>
  static void ForEachKey(const std::string& text, Fn&& fn) {
    size_t i = 0;
    while (i < text.size()) {
      size_t j = text.find_first_of(",|", i);
      if (j == std::string::npos) {
        j = text.size();
      }
      size_t a = i, b = j;
      while (a < b && (text[a] == ' ' || text[a] == '\t')) ++a;
      while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t')) --b;
      if (b > a) {
        VirtualKey vk = rex::ui::ParseVirtualKey(std::string_view(text).substr(a, b - a));
        if (vk != VirtualKey::kNone && fn(vk)) {
          return;
        }
      }
      i = (j == text.size()) ? j : j + 1;
    }
  }

  void SetKey(VirtualKey vk, bool down) {
    const uint16_t idx = static_cast<uint16_t>(vk);
    if (idx == 0 || idx >= kKeyCount) {
      return;
    }
    std::lock_guard lock(mutex_);
    if (capturing_) {
      // Swallow the press for the rebind dialog. Escape cancels; the key is
      // still released normally so nothing sticks.
      if (down) {
        if (vk == VirtualKey::kEscape) {
          capturing_ = false;
        } else {
          std::string name = rex::ui::VirtualKeyToString(vk);
          if (!name.empty()) {
            captured_ = std::move(name);
            capturing_ = false;
          }
        }
      }
      return;
    }
    key_down_[idx] = down;
  }

  void CenterCursor(int32_t cx, int32_t cy) {
#if defined(_WIN32)
    HWND hwnd = reinterpret_cast<HWND>(window_->GetNativeWindowHandle());
    if (!hwnd) {
      return;
    }
    POINT pt = {static_cast<LONG>(cx), static_cast<LONG>(cy)};
    ClientToScreen(hwnd, &pt);
    SetCursorPos(pt.x, pt.y);
#else
    (void)cx;
    (void)cy;
#endif
  }

  rex::ui::Window* window_ = nullptr;
  bool cursor_locked_ = false;  // UI thread + frame tick only

  mutable std::mutex mutex_;
  bool key_down_[kKeyCount] = {};
  int32_t mouse_dx_ = 0;
  int32_t mouse_dy_ = 0;
  bool has_focus_ = true;
  bool capturing_ = false;
  std::string captured_;
};

HostInput g_input;

// Mouse motion that didn't fit in one frame's stick deflection, carried over.
float g_look_carry_x = 0.0f;
float g_look_carry_y = 0.0f;

// True while the game (not an overlay) owns input.
//
// rex::input::InputSystem exposes no getter for this. SetActiveCallback pushes a
// predicate down into each InputDriver and only the drivers read it back, via
// InputDriver::is_active(). Rather than replace that callback -- there is one
// slot, so overriding it would disable the SDK's own overlay gating -- ask ImGui
// the same question the SDK's callback in rex_app.cpp asks.
bool GameOwnsInput() {
  // Before the drawer is up there is no context, and the game has input.
  if (!ImGui::GetCurrentContext()) {
    return true;
  }
  const ImGuiIO& io = ImGui::GetIO();
  // Keyboard as well as mouse, unlike the SDK's callback: these are mouse and
  // keyboard controls, so typing into a console must not also drive the game.
  return !io.WantCaptureMouse && !io.WantCaptureKeyboard;
}

// Set while reNut's controls overlay is up.
bool g_overlay_open = false;

// Set by renutMnk_PadConnectMask each frame: true when the XInputGetState(0..3)
// scan found no physical pad at all, which is the only case where we stand in
// for one. Read by renutMnk_PadPollResult later in the same sweep.
bool g_no_physical_pad = false;

bool ControlsActive() {
  const bool enabled = REXCVAR_GET(mnk_controls);
  const bool attached = g_input.attached();
  const bool focused = g_input.has_focus();
  const bool no_overlay = !g_overlay_open;
  const bool not_capturing = !g_input.IsCapturing();
  const bool game_owns = GameOwnsInput();

  const bool active = enabled && attached && focused && no_overlay && not_capturing && game_owns;

  // Every one of these terms fails the same silent way from the outside: the
  // controls simply do nothing. Log which combination is in force, but only when
  // it changes -- the guest thread calls this on every pad poll, so logging
  // unconditionally would flood the file.
  static std::atomic<uint32_t> last_state{~0u};
  const uint32_t state = (enabled ? 1u : 0u) | (attached ? 2u : 0u) | (focused ? 4u : 0u) |
                         (no_overlay ? 8u : 0u) | (not_capturing ? 16u : 0u) |
                         (game_owns ? 32u : 0u);
  if (last_state.exchange(state) != state) {
    RNUT_INFO("mnk gate {}: cvar={} attached={} focus={} no_overlay={} not_capturing={} game_owns={}",
              active ? "ACTIVE" : "blocked", enabled, attached, focused, no_overlay, not_capturing,
              game_owns);
  }

  return active;
}

uint32_t PadSlot() {
  return static_cast<uint32_t>(std::clamp(REXCVAR_GET(mnk_pad_slot), 0, 3));
}

// ---- guest memory (big-endian) ----------------------------------------------

uint8_t* GuestBase() {
  auto* ks = rex::system::kernel_state();
  return ks ? ks->memory()->virtual_membase() : nullptr;
}

inline uint32_t RdBE32(uint8_t* base, uint32_t addr) {
  return RENUT_BSWAP32(*reinterpret_cast<uint32_t*>(base + addr));
}

inline void WrBE32(uint8_t* base, uint32_t addr, uint32_t value) {
  *reinterpret_cast<uint32_t*>(base + addr) = RENUT_BSWAP32(value);
}

inline void WrBEF32(uint8_t* base, uint32_t addr, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  WrBE32(base, addr, bits);
}

// -----------------------------------------------------------------------------
// One frame of pad state, as sub_8239FF20 lays it out on the stack before
// handing it to sub_821FBD90:
//
//   +0x00 u32   buttons pressed this frame (new & ~old)
//   +0x04 u32   buttons released this frame (old & ~new)
//   +0x08 u32   buttons held (new & old)
//   +0x0C u32   buttons raw (new)
//   +0x10 f32   left stick X    -1..1
//   +0x14 f32   left stick Y    -1..1
//   +0x18 f32   right stick X   -1..1
//   +0x1C f32   right stick Y   -1..1
//   +0x20 f32   left trigger     0..1
//   +0x24 f32   right trigger    0..1
// -----------------------------------------------------------------------------
constexpr uint32_t kBlkPressed = 0x00;
constexpr uint32_t kBlkReleased = 0x04;
constexpr uint32_t kBlkHeld = 0x08;
constexpr uint32_t kBlkRaw = 0x0C;
constexpr uint32_t kBlkLStickX = 0x10;
constexpr uint32_t kBlkLStickY = 0x14;
constexpr uint32_t kBlkRStickX = 0x18;
constexpr uint32_t kBlkRStickY = 0x1C;
constexpr uint32_t kBlkLTrigger = 0x20;
constexpr uint32_t kBlkRTrigger = 0x24;

// Pad-state object layout (the `padObject + 0x34` the game passes around):
//   +0x00 u32   index of the frame currently committed (ring of 2)
//   +0x04 + 0x28 * index   the frame block above
constexpr uint32_t kPadStateOff = 0x34;   // padObject -> pad state
constexpr uint32_t kFrameStride = 0x28;
constexpr uint32_t kFrameBase = 0x04;

// =============================================================================
// Keystrokes
//
// The pad state above drives gameplay, but it isn't what the front end runs on:
// every XUI screen (the main menu, the pause menu, the file select) navigates
// off XInputGetKeystroke, which is an edge-triggered queue of VK_PAD_* codes and
// a completely separate path from the analog state. Feeding one and not the
// other is exactly why the game boots to the "press start" screen -- polled
// state -- and then goes deaf at the main menu.
//
// So we keep our own queue: one entry per press, release and auto-repeat of a
// bound action, plus the left stick's eight-way direction, which is what menu
// lists actually scroll on.
// =============================================================================
using rex::input::X_INPUT_KEYSTROKE_KEYDOWN;
using rex::input::X_INPUT_KEYSTROKE_KEYUP;
using rex::input::X_INPUT_KEYSTROKE_REPEAT;

// Roughly the dashboard's own feel: a beat before a held direction starts
// repeating, then steady. Fast enough to scroll a long list, slow enough that a
// single tap never moves two rows.
constexpr auto kRepeatDelay = std::chrono::milliseconds(400);
constexpr auto kRepeatRate = std::chrono::milliseconds(110);

// Bounded so a screen that stops draining the queue can't grow it forever.
constexpr size_t kMaxQueued = 32;

enum PadIdx : size_t {
  kPadA, kPadB, kPadX, kPadY,
  kPadLB, kPadRB, kPadLT, kPadRT,
  kPadStart, kPadBack, kPadL3, kPadR3,
  kPadDUp, kPadDDown, kPadDLeft, kPadDRight,
  kPadLStick,  // holds the current 8-way direction as its vk
  kPadIdxCount
};

struct Keystroke {
  uint16_t vk;
  uint16_t flags;
};

struct PadKeyState {
  bool held = false;
  uint16_t vk = 0;
  std::chrono::steady_clock::time_point pressed_at;
  std::chrono::steady_clock::time_point last_event_at;
};

std::mutex g_keystroke_mutex;
std::deque<Keystroke> g_keystrokes;
PadKeyState g_pad_states[kPadIdxCount];

void PushKeystroke(uint16_t vk, uint16_t flags) {
  if (g_keystrokes.size() >= kMaxQueued) {
    g_keystrokes.pop_front();
  }
  g_keystrokes.push_back({vk, flags});
}

// Press/release edge for a plain button.
void UpdateEdge(PadIdx idx, uint16_t vk, bool down,
                std::chrono::steady_clock::time_point now) {
  auto& s = g_pad_states[idx];
  if (down && !s.held) {
    s.held = true;
    s.vk = vk;
    s.pressed_at = now;
    s.last_event_at = now;
    PushKeystroke(vk, X_INPUT_KEYSTROKE_KEYDOWN);
  } else if (!down && s.held) {
    s.held = false;
    PushKeystroke(s.vk, X_INPUT_KEYSTROKE_KEYUP);
  }
}

// The stick reports as one of eight directions; changing direction releases the
// old one and presses the new, which is what list navigation expects.
void UpdateStickDir(uint16_t dir, std::chrono::steady_clock::time_point now) {
  auto& s = g_pad_states[kPadLStick];
  if (dir == s.vk) {
    return;
  }
  if (s.held) {
    PushKeystroke(s.vk, X_INPUT_KEYSTROKE_KEYUP);
    s.held = false;
  }
  s.vk = dir;
  if (dir != 0) {
    s.held = true;
    s.pressed_at = now;
    s.last_event_at = now;
    PushKeystroke(dir, X_INPUT_KEYSTROKE_KEYDOWN);
  }
}

// Rebuild the queue for this frame from the current bind state. `up/down/
// left/right` are the movement binds, already read by the caller.
void UpdateKeystrokes(bool up, bool down, bool left, bool right) {
  using VK = rex::ui::VirtualKey;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(g_keystroke_mutex);

  auto edge = [&](PadIdx idx, BindId bind, VK vk) {
    UpdateEdge(idx, static_cast<uint16_t>(vk), g_input.IsBindHeld(bind), now);
  };
  edge(kPadA, kBind_mnk_bind_a, VK::kXInputPadA);
  edge(kPadB, kBind_mnk_bind_b, VK::kXInputPadB);
  edge(kPadX, kBind_mnk_bind_x, VK::kXInputPadX);
  edge(kPadY, kBind_mnk_bind_y, VK::kXInputPadY);
  edge(kPadLB, kBind_mnk_bind_lb, VK::kXInputPadLShoulder);
  edge(kPadRB, kBind_mnk_bind_rb, VK::kXInputPadRShoulder);
  edge(kPadLT, kBind_mnk_bind_lt, VK::kXInputPadLTrigger);
  edge(kPadRT, kBind_mnk_bind_rt, VK::kXInputPadRTrigger);
  edge(kPadStart, kBind_mnk_bind_start, VK::kXInputPadStart);
  edge(kPadBack, kBind_mnk_bind_select, VK::kXInputPadBack);
  edge(kPadL3, kBind_mnk_bind_l3, VK::kXInputPadLThumbPress);
  edge(kPadR3, kBind_mnk_bind_r3, VK::kXInputPadRThumbPress);
  edge(kPadDUp, kBind_mnk_bind_dpad_up, VK::kXInputPadDpadUp);
  edge(kPadDDown, kBind_mnk_bind_dpad_down, VK::kXInputPadDpadDown);
  edge(kPadDLeft, kBind_mnk_bind_dpad_left, VK::kXInputPadDpadLeft);
  edge(kPadDRight, kBind_mnk_bind_dpad_right, VK::kXInputPadDpadRight);

  uint16_t dir = 0;
  if (up && right) dir = static_cast<uint16_t>(VK::kXInputPadLThumbUpRight);
  else if (up && left) dir = static_cast<uint16_t>(VK::kXInputPadLThumbUpLeft);
  else if (down && right) dir = static_cast<uint16_t>(VK::kXInputPadLThumbDownRight);
  else if (down && left) dir = static_cast<uint16_t>(VK::kXInputPadLThumbDownLeft);
  else if (up) dir = static_cast<uint16_t>(VK::kXInputPadLThumbUp);
  else if (down) dir = static_cast<uint16_t>(VK::kXInputPadLThumbDown);
  else if (right) dir = static_cast<uint16_t>(VK::kXInputPadLThumbRight);
  else if (left) dir = static_cast<uint16_t>(VK::kXInputPadLThumbLeft);
  UpdateStickDir(dir, now);

  for (auto& s : g_pad_states) {
    if (!s.held || now - s.pressed_at < kRepeatDelay || now - s.last_event_at < kRepeatRate) {
      continue;
    }
    PushKeystroke(s.vk, X_INPUT_KEYSTROKE_REPEAT);
    s.last_event_at = now;
  }
}

// Drop everything and release anything held, so a key still down when the game
// stops owning input doesn't leave a phantom press queued behind it.
void ResetKeystrokes() {
  std::lock_guard lock(g_keystroke_mutex);
  g_keystrokes.clear();
  for (auto& s : g_pad_states) {
    s.held = false;
    s.vk = 0;
  }
}

bool PopKeystroke(Keystroke& out) {
  std::lock_guard lock(g_keystroke_mutex);
  if (g_keystrokes.empty()) {
    return false;
  }
  out = g_keystrokes.front();
  g_keystrokes.pop_front();
  return true;
}

}  // namespace

// =============================================================================
// Public interface
// =============================================================================
namespace renut::mnk {

void AttachWindow(rex::ui::Window* window) { g_input.Attach(window); }

void DetachWindow() { g_input.Detach(); }

bool Active() { return ControlsActive(); }

void SetOverlayOpen(bool open) { g_overlay_open = open; }

void BeginCapture() { g_input.BeginCapture(); }

void CancelCapture() { g_input.CancelCapture(); }

bool IsCapturing() { return g_input.IsCapturing(); }

bool TakeCaptured(std::string& out_key_name) { return g_input.TakeCaptured(out_key_name); }

const BindInfo* BindList(std::size_t& out_count) {
  out_count = kBindCount;
  return kBindInfo;
}

}  // namespace renut::mnk

// =============================================================================
// Midasm hook: stand in for a pad when there isn't one (sub_8239FC08, guest
// 0x8239FC6C -- right after the XInputGetState(0..3) scan finishes filling
// dword_82F9DFEC and before the disconnect sweep runs).
//
// Playing on keyboard alone with no pad plugged in otherwise leaves every slot
// reading as empty and the game sits in its "reconnect controller" state.
// dword_8212A96C is the per-slot mask table and holds {1, 2, 4, 8}.
//
// This only ever fills in for a *missing* pad: if the scan found any real one we
// leave the mask exactly as it is. Presenting a second pad alongside a real one
// makes rexglue titles decide nobody is signed in, and that breaks considerably
// more than it fixes. With a pad connected the keyboard just rides along on the
// slot mnk_pad_slot names, which needs no phantom device.
// =============================================================================
void renutMnk_PadConnectMask() {
  g_no_physical_pad = false;
  if (!REXCVAR_GET(mnk_controls)) {
    return;
  }
  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }
  constexpr uint32_t kConnectedMask = 0x82F9DFEC;
  if (RdBE32(base, kConnectedMask) != 0) {
    return;  // a real pad is present -- don't add a second one
  }
  g_no_physical_pad = true;
  WrBE32(base, kConnectedMask, 1u << PadSlot());
}

// =============================================================================
// Midasm hook: make the per-pad poll succeed (sub_8239FC08, guest 0x8239FC84 --
// on the `cmplwi r3, 0` that tests the XInputGetState result).
//
// r29 points at the pad object, whose first word is its user index; r1+0x50 is
// the XINPUT_STATE the call filled in. When our slot has no physical pad the
// call returns ERROR_DEVICE_NOT_CONNECTED and sub_8239FF20 is skipped entirely,
// so we hand it a zeroed but "successful" state and let the state builder run.
// renutMnk_ApplyPadState then fills that empty frame in with the real
// keyboard/mouse input.
// =============================================================================
void renutMnk_PadPollResult(PPCRegister& r1, PPCRegister& r3, PPCRegister& r29) {
  if (r3.u32 == 0) {
    return;  // a real pad answered; nothing to fake
  }
  if (!REXCVAR_GET(mnk_controls) || !g_no_physical_pad) {
    return;
  }
  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }
  if (RdBE32(base, r29.u32) != PadSlot()) {
    return;
  }
  std::memset(base + r1.u32 + 0x50, 0, 16);  // XINPUT_STATE
  r3.u32 = 0;                                // ERROR_SUCCESS
}

// =============================================================================
// Midasm hook: write our movement into the frame the game is about to commit
// (sub_8239FF20, guest 0x823A00A4 -- on the `bl sub_821FBD90` handoff).
//
// r3 = pad state object (padObject + 0x34), r4 = the 0x28-byte frame block.
//
// Everything the game reads for avatar movement, camera and button presses this
// frame comes out of this block, so this is the one place worth touching: no
// XInput round trip, no stick response curve, and the values land at full float
// precision instead of being quantised to a 16-bit stick axis.
//
// We merge rather than replace, so a physical pad plugged in at the same slot
// still drives the game: buttons are OR'd, and each stick keeps the pad's
// vector when we have nothing to say for it.
// =============================================================================
void renutMnk_ApplyPadState(PPCRegister& r3, PPCRegister& r4) {
  const bool active = ControlsActive();

  // This is the game's own once-per-frame pad poll, so the cursor lock rides
  // along with it rather than needing a tick of its own. Dropping held keys on
  // the way out matters: the ImGui overlays sit above us and mark their key
  // events handled, so a key still down when F4 opens never sees its key-up.
  static bool was_active = false;
  if (was_active && !active) {
    g_input.ClearKeys();
    ResetKeystrokes();
    g_look_carry_x = 0.0f;
    g_look_carry_y = 0.0f;
  }
  was_active = active;
  g_input.SetCursorLocked(active && REXCVAR_GET(mnk_lock_cursor));

  if (!active) {
    return;
  }
  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }

  const uint32_t pad_state = r3.u32;
  if (RdBE32(base, pad_state - kPadStateOff) != PadSlot()) {
    return;  // a different player's pad
  }
  const uint32_t blk = r4.u32;

  // ---- buttons -------------------------------------------------------------
  uint32_t buttons = 0;
  auto set_if = [&](BindId id, uint32_t bit) {
    if (g_input.IsBindHeld(id)) {
      buttons |= bit;
    }
  };
  set_if(kBind_mnk_bind_a, kBtnA);
  set_if(kBind_mnk_bind_b, kBtnB);
  set_if(kBind_mnk_bind_x, kBtnX);
  set_if(kBind_mnk_bind_y, kBtnY);
  set_if(kBind_mnk_bind_lb, kBtnLShoulder);
  set_if(kBind_mnk_bind_rb, kBtnRShoulder);
  set_if(kBind_mnk_bind_l3, kBtnLThumb);
  set_if(kBind_mnk_bind_r3, kBtnRThumb);
  set_if(kBind_mnk_bind_dpad_up, kBtnDpadUp);
  set_if(kBind_mnk_bind_dpad_down, kBtnDpadDown);
  set_if(kBind_mnk_bind_dpad_left, kBtnDpadLeft);
  set_if(kBind_mnk_bind_dpad_right, kBtnDpadRight);
  set_if(kBind_mnk_bind_start, kBtnStart);
  set_if(kBind_mnk_bind_select, kBtnBack);

  const bool lt_held = g_input.IsBindHeld(kBind_mnk_bind_lt);
  const bool rt_held = g_input.IsBindHeld(kBind_mnk_bind_rt);
  if (lt_held) {
    buttons |= kBtnLTrigger;
  }
  if (rt_held) {
    buttons |= kBtnRTrigger;
  }

  // Re-derive the edges from the merged word against the frame the game itself
  // used as "old", so pressed/released stay consistent with its bookkeeping.
  const uint32_t old_raw =
      RdBE32(base, pad_state + kFrameBase + kFrameStride * RdBE32(base, pad_state) + kBlkRaw);
  const uint32_t new_raw = RdBE32(base, blk + kBlkRaw) | buttons;

  WrBE32(base, blk + kBlkPressed, new_raw & ~old_raw);
  WrBE32(base, blk + kBlkReleased, old_raw & ~new_raw);
  WrBE32(base, blk + kBlkHeld, new_raw & old_raw);
  WrBE32(base, blk + kBlkRaw, new_raw);

  if (lt_held) {
    WrBEF32(base, blk + kBlkLTrigger, 1.0f);
  }
  if (rt_held) {
    WrBEF32(base, blk + kBlkRTrigger, 1.0f);
  }

  // ---- left stick: movement ------------------------------------------------
  const bool move_right = g_input.IsBindHeld(kBind_mnk_bind_right);
  const bool move_left = g_input.IsBindHeld(kBind_mnk_bind_left);
  const bool move_fwd = g_input.IsBindHeld(kBind_mnk_bind_forward);
  const bool move_back = g_input.IsBindHeld(kBind_mnk_bind_back);

  // The front end runs on keystrokes, not on any of the above, so feed that
  // queue from the same bind state (see UpdateKeystrokes).
  UpdateKeystrokes(move_fwd, move_back, move_left, move_right);

  float mx = 0.0f;
  float my = 0.0f;
  if (move_right) mx += 1.0f;
  if (move_left) mx -= 1.0f;
  if (move_fwd) my += 1.0f;
  if (move_back) my -= 1.0f;

  if (mx != 0.0f || my != 0.0f) {
    // Normalise so diagonals aren't faster than cardinals -- the thing an
    // 8-way digital input mapped onto an analog stick always gets wrong.
    const float len = std::sqrt(mx * mx + my * my);
    mx /= len;
    my /= len;
    if (g_input.IsBindHeld(kBind_mnk_bind_walk)) {
      const float scale = static_cast<float>(REXCVAR_GET(mnk_walk_scale)) / 100.0f;
      mx *= scale;
      my *= scale;
    }
    WrBEF32(base, blk + kBlkLStickX, mx);
    WrBEF32(base, blk + kBlkLStickY, my);
  }

  // ---- right stick: camera -------------------------------------------------
  float dx = 0.0f;
  float dy = 0.0f;
  g_input.TakeMouseDelta(dx, dy);

  const float sensitivity = static_cast<float>(REXCVAR_GET(mnk_look_sensitivity)) / 50.0f;
  const float per_pixel = sensitivity / kPixelsFullTilt;

  float lx = g_look_carry_x + dx * per_pixel;
  float ly = g_look_carry_y + (-dy) * per_pixel;  // mouse up = look up

  // Keyboard look binds, for anyone who wants them, are full tilt.
  if (g_input.IsBindHeld(kBind_mnk_bind_look_right)) lx += 1.0f;
  if (g_input.IsBindHeld(kBind_mnk_bind_look_left)) lx -= 1.0f;
  if (g_input.IsBindHeld(kBind_mnk_bind_look_up)) ly += 1.0f;
  if (g_input.IsBindHeld(kBind_mnk_bind_look_down)) ly -= 1.0f;

  const float rx = std::clamp(lx, -1.0f, 1.0f);
  const float ry = std::clamp(ly, -1.0f, 1.0f);

  // Whatever the stick couldn't express this frame rides along to the next one,
  // so a flick faster than one frame of full tilt still turns the whole way.
  // Carried in un-inverted space; invert only ever touches what we write out.
  g_look_carry_x = std::clamp(lx - rx, -kMaxLookCarry, kMaxLookCarry);
  g_look_carry_y = std::clamp(ly - ry, -kMaxLookCarry, kMaxLookCarry);

  if (rx != 0.0f || ry != 0.0f) {
    WrBEF32(base, blk + kBlkRStickX, rx);
    WrBEF32(base, blk + kBlkRStickY, REXCVAR_GET(mnk_invert_look) ? -ry : ry);
  }
}

// =============================================================================
// Override: XInputGetKeystroke (guest 0x821FD318).
//
// The XUI front end -- main menu, pause menu, file select -- navigates off this
// and never looks at the analog pad state, so injecting movement alone gets you
// as far as the "press start" screen and no further.
//
// r3 = dwUserIndex, r4 = dwFlags, r5 = pKeystroke (guest X_INPUT_KEYSTROKE).
//
// When we have something queued we answer it ourselves. Otherwise the original
// runs, and if it comes back DEVICE_NOT_CONNECTED while we're standing in for a
// missing pad we soften that to EMPTY: "no keystroke waiting" keeps the menu
// alive, "no controller" makes it stop listening.
// =============================================================================
REX_EXTERN(__imp__rex_XInputGetKeystroke);

// The X_ERROR_* macros expand to a cast through rex::X_RESULT.
using rex::X_RESULT;

REX_HOOK_RAW(rex_XInputGetKeystroke) {
  const uint32_t keystroke_ptr = ctx.r5.u32;

  if (ControlsActive() && keystroke_ptr) {
    Keystroke ks;
    if (PopKeystroke(ks)) {
      uint8_t* out = base + keystroke_ptr;
      out[0] = static_cast<uint8_t>(ks.vk >> 8);      // virtual_key (BE)
      out[1] = static_cast<uint8_t>(ks.vk);
      out[2] = 0;                                     // unicode
      out[3] = 0;
      out[4] = static_cast<uint8_t>(ks.flags >> 8);   // flags (BE)
      out[5] = static_cast<uint8_t>(ks.flags);
      out[6] = static_cast<uint8_t>(PadSlot());       // user_index
      out[7] = 0;                                     // hid_code
      ctx.r3.u64 = X_ERROR_SUCCESS;
      return;
    }
  }

  __imp__rex_XInputGetKeystroke(ctx, base);

  if (REXCVAR_GET(mnk_controls) && g_no_physical_pad &&
      ctx.r3.u32 == static_cast<uint32_t>(X_ERROR_DEVICE_NOT_CONNECTED)) {
    ctx.r3.u64 = static_cast<uint32_t>(X_ERROR_EMPTY);
  }
}