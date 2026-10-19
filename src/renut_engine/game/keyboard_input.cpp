/**
 * @file    game/keyboard_input.cpp
 * @brief   Bound keys and mouse buttons play player 1's pad, for every action.
 *
 * N&B's input (IDA): inputPollAllPads -> padApplyXInputState (0x8239FF20)
 * turns each port's XInput state into a pad record in g_padInputs[port] + 52:
 * a current index at + 0 and two 40-byte entries at + 4 + 40 * index:
 *   + 0 pressed (now & ~before), + 4 released (before & ~now),
 *   + 8 held (now & before), + 12 now: XINPUT_GAMEPAD wButtons, with
 *       0x10000 / 0x20000 for the left / right trigger past 32;
 *   + 16 / + 20 left stick x / y, + 24 / + 28 right stick (padFilterStick:
 *       -1..1, y up, length at most 1); + 32 / + 36 triggers (0..1).
 * Every record takes a new entry through padStatePush (sub_821FBD90). The game
 * reads player 1's input from his port's record:
 * - in play, through his input device (inputDeviceCreatePlayerPad, type 3),
 *   whose update copies g_padInputs[player + 108]'s current entry into its own
 *   record (inputDeviceUpdatePlayerPad); the game reads the device's record,
 *   button bits inline, everywhere;
 * - before there is a player (title, menus), from the ports' records directly
 *   (inputDeviceAnyPadPressed: the port pressing Start becomes player 1's).
 *
 * With renut_keyboard, the push of player 1's port's entry (the first
 * connected port, or port 0, while there is no player 1) takes the entry with
 * the bound keys merged in: their buttons ORed into the pad's (the edges
 * recomputed from both), the movement keys as a full-strength left stick, the
 * trigger keys as full triggers. The controller keeps working alongside.
 *
 * The XUI menus read keystrokes instead (XInputGetKeystroke: sub_82279D80,
 * sub_823E9600, sub_823E8C98 -> XuiProcessInput). The keys make player 1's:
 * VK_PAD_* down and up as they are pressed and let go, and repeats while held
 * (as XInput's), the movement keys as the left stick's directions;
 * controllers.cpp hands them out before the controllers' own.
 *
 * Keys come from the SDK's window (rex::ui::Window listeners, on the UI thread,
 * Windows and Linux alike); the binds are key names (rex::ui::ParseVirtualKey:
 * "Space", "LMB", "Shift", ...) in Nuts&Bolts/Keyboard cvars, read on the UI
 * thread, where the settings window edits them. Mouse buttons count only while
 * the mouse is captured (mouse_camera.cpp), and the click that captures it
 * doesn't.
 */
#include "renut_engine/game/keyboard_input.h"

#include "renut_engine/app/renut_logging.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "renut_engine/game/mouse_camera.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

REXCVAR_DEFINE_BOOL(renut_keyboard, false, "Nuts&Bolts/Keyboard",
                    "Bound keys and mouse buttons (with the mouse captured) play player 1's "
                    "controller: every action, alongside the controller. Binds are key names: "
                    "A-Z, 0-9, F1-F24, Space, Return, Escape, Tab, Shift, Control, Alt, "
                    "Left/Right/Up/Down, LMB/RMB/MMB, Numpad0-9, ... Empty unbinds.");
REXCVAR_DEFINE_STRING(renut_key_forward, "W", "Nuts&Bolts/Keyboard", "Move forward (left stick up).");
REXCVAR_DEFINE_STRING(renut_key_back, "S", "Nuts&Bolts/Keyboard", "Move back (left stick down).");
REXCVAR_DEFINE_STRING(renut_key_left, "A", "Nuts&Bolts/Keyboard", "Move left (left stick left).");
REXCVAR_DEFINE_STRING(renut_key_right, "D", "Nuts&Bolts/Keyboard", "Move right (left stick right).");
REXCVAR_DEFINE_STRING(renut_key_walk, "Control", "Nuts&Bolts/Keyboard",
                      "Held with the movement keys: half-strength stick (walk).");
REXCVAR_DEFINE_STRING(renut_key_a, "Space", "Nuts&Bolts/Keyboard", "Controller A (jump).");
REXCVAR_DEFINE_STRING(renut_key_b, "RMB", "Nuts&Bolts/Keyboard", "Controller B.");
REXCVAR_DEFINE_STRING(renut_key_x, "LMB", "Nuts&Bolts/Keyboard", "Controller X.");
REXCVAR_DEFINE_STRING(renut_key_y, "E", "Nuts&Bolts/Keyboard", "Controller Y.");
REXCVAR_DEFINE_STRING(renut_key_lb, "Q", "Nuts&Bolts/Keyboard", "Controller left bumper.");
REXCVAR_DEFINE_STRING(renut_key_rb, "R", "Nuts&Bolts/Keyboard", "Controller right bumper.");
REXCVAR_DEFINE_STRING(renut_key_lt, "Shift", "Nuts&Bolts/Keyboard", "Controller left trigger.");
REXCVAR_DEFINE_STRING(renut_key_rt, "F", "Nuts&Bolts/Keyboard", "Controller right trigger.");
REXCVAR_DEFINE_STRING(renut_key_start, "Escape", "Nuts&Bolts/Keyboard",
                      "Controller Start (Escape also frees the mouse).");
REXCVAR_DEFINE_STRING(renut_key_back_button, "Tab", "Nuts&Bolts/Keyboard", "Controller Back.");
REXCVAR_DEFINE_STRING(renut_key_ls, "C", "Nuts&Bolts/Keyboard", "Left stick click.");
REXCVAR_DEFINE_STRING(renut_key_rs, "V", "Nuts&Bolts/Keyboard", "Right stick click.");
REXCVAR_DEFINE_STRING(renut_key_dpad_up, "Up", "Nuts&Bolts/Keyboard", "D-pad up.");
REXCVAR_DEFINE_STRING(renut_key_dpad_down, "Down", "Nuts&Bolts/Keyboard", "D-pad down.");
REXCVAR_DEFINE_STRING(renut_key_dpad_left, "Left", "Nuts&Bolts/Keyboard", "D-pad left.");
REXCVAR_DEFINE_STRING(renut_key_dpad_right, "Right", "Nuts&Bolts/Keyboard", "D-pad right.");

REX_EXTERN(__imp__sub_821FBD90);  // padStatePush

namespace {

// Guest data.
constexpr uint32_t kPadInputs = 0x82F9DFF8;       // g_padInputs[port]
constexpr uint32_t kConnectedPadMask = 0x82F9DFEC;  // g_connectedPadMask
constexpr uint32_t kPadPortBits = 0x8212A96C;     // g_padPortBits[port]
constexpr uint32_t kPlayerProfiles = 0x82FAD9F0;  // dword_82FAD9F0[player]: player slots
constexpr uint32_t kPorts = 4;
constexpr uint32_t kPadRecord = 52;               // g_padInputs[port] + 52: the port's record
constexpr uint32_t kPlayerPort = 108;             // player slot + 108: his port
constexpr uint32_t kEntrySize = 40;

// Pad entry buttons (XINPUT_GAMEPAD, plus the triggers).
constexpr uint32_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004,
                   kDpadRight = 0x0008, kStart = 0x0010, kBack = 0x0020, kLeftThumb = 0x0040,
                   kRightThumb = 0x0080, kLeftShoulder = 0x0100, kRightShoulder = 0x0200,
                   kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000, kLeftTrigger = 0x10000,
                   kRightTrigger = 0x20000;

// Movement keys.
constexpr uint32_t kMoveForward = 1, kMoveBack = 2, kMoveLeft = 4, kMoveRight = 8, kMoveWalk = 16;

struct Bind {
  const std::string &(*key)();
  uint32_t buttons;  // pad buttons
  uint32_t move;     // movement keys
};

#define RENUT_BIND(cvar, buttons, move) \
  Bind{[]() -> const std::string & { return REXCVAR_GET(cvar); }, buttons, move}

const Bind kBinds[] = {
    RENUT_BIND(renut_key_forward, 0, kMoveForward),
    RENUT_BIND(renut_key_back, 0, kMoveBack),
    RENUT_BIND(renut_key_left, 0, kMoveLeft),
    RENUT_BIND(renut_key_right, 0, kMoveRight),
    RENUT_BIND(renut_key_walk, 0, kMoveWalk),
    RENUT_BIND(renut_key_a, kA, 0),
    RENUT_BIND(renut_key_b, kB, 0),
    RENUT_BIND(renut_key_x, kX, 0),
    RENUT_BIND(renut_key_y, kY, 0),
    RENUT_BIND(renut_key_lb, kLeftShoulder, 0),
    RENUT_BIND(renut_key_rb, kRightShoulder, 0),
    RENUT_BIND(renut_key_lt, kLeftTrigger, 0),
    RENUT_BIND(renut_key_rt, kRightTrigger, 0),
    RENUT_BIND(renut_key_start, kStart, 0),
    RENUT_BIND(renut_key_back_button, kBack, 0),
    RENUT_BIND(renut_key_ls, kLeftThumb, 0),
    RENUT_BIND(renut_key_rs, kRightThumb, 0),
    RENUT_BIND(renut_key_dpad_up, kDpadUp, 0),
    RENUT_BIND(renut_key_dpad_down, kDpadDown, 0),
    RENUT_BIND(renut_key_dpad_left, kDpadLeft, 0),
    RENUT_BIND(renut_key_dpad_right, kDpadRight, 0),
};

#undef RENUT_BIND

// What the held keys play, set on the UI thread, read by the game thread.
std::atomic<uint32_t> g_buttons{0};
std::atomic<uint32_t> g_move{0};

uint32_t Load32(const uint8_t *base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return rex::byte_swap(v);
}

void Store32(uint8_t *base, uint32_t va, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(base + va, &value, 4);
}

float LoadFloat(const uint8_t *base, uint32_t va) {
  const uint32_t bits = Load32(base, va);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void StoreFloat(uint8_t *base, uint32_t va, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  Store32(base, va, bits);
}

// The window's keys and mouse buttons, on the UI thread.
class KeyboardListener : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
public:
  void Attach(rex::ui::Window *window) {
    window_ = window;
    window->AddInputListener(this, kZOrder);
    window->AddListener(this);
  }

  void Detach() {
    if (!window_)
      return;
    Clear();
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
    window_ = nullptr;
  }

  rex::ui::Window *window() const { return window_; }

  void OnKeyDown(rex::ui::KeyEvent &e) override { Set(e.virtual_key(), true); }
  void OnKeyUp(rex::ui::KeyEvent &e) override { Set(e.virtual_key(), false); }

  // Before the mouse camera's listener: the click that captures the mouse
  // finds it free, and plays nothing.
  void OnMouseDown(rex::ui::MouseEvent &e) override {
    if (renut::game::MouseCaptured())
      Set(MouseKey(e.button()), true);
  }
  void OnMouseUp(rex::ui::MouseEvent &e) override { Set(MouseKey(e.button()), false); }

  void OnLostFocus(rex::ui::UISetupEvent &) override { Clear(); }
  void OnClosing(rex::ui::UIEvent &) override { Detach(); }

private:
  // Above the mouse camera's (1) and the app's own (0) listeners.
  static constexpr size_t kZOrder = 2;

  static rex::ui::VirtualKey MouseKey(rex::ui::MouseEvent::Button button) {
    switch (button) {
    case rex::ui::MouseEvent::Button::kLeft: return rex::ui::VirtualKey::kLButton;
    case rex::ui::MouseEvent::Button::kRight: return rex::ui::VirtualKey::kRButton;
    case rex::ui::MouseEvent::Button::kMiddle: return rex::ui::VirtualKey::kMButton;
    case rex::ui::MouseEvent::Button::kX1: return rex::ui::VirtualKey::kXButton1;
    case rex::ui::MouseEvent::Button::kX2: return rex::ui::VirtualKey::kXButton2;
    default: return rex::ui::VirtualKey::kNone;
    }
  }

  void Set(rex::ui::VirtualKey key, bool down) {
    const size_t index = size_t(key);
    if (key == rex::ui::VirtualKey::kNone || index >= held_.size())
      return;
    held_[index] = down;
    Update();
  }

  void Clear() {
    held_.reset();
    Update();
  }

  // The binds' keys are looked up as they are now, so a changed bind takes
  // effect from the next key.
  void Update() {
    uint32_t buttons = 0, move = 0;
    for (const Bind &bind : kBinds) {
      const std::string &name = bind.key();
      if (name.empty())
        continue;
      const size_t index = size_t(rex::ui::ParseVirtualKey(name));
      if (index && index < held_.size() && held_[index]) {
        buttons |= bind.buttons;
        move |= bind.move;
      }
    }
    g_buttons.store(buttons, std::memory_order_relaxed);
    g_move.store(move, std::memory_order_relaxed);
  }

  rex::ui::Window *window_ = nullptr;
  std::bitset<256> held_;
};

KeyboardListener g_listener;

// The port the keys play: player 1's, or before there is one, the first
// connected port (port 0 without any).
uint32_t KeyboardPort(const uint8_t *base) {
  if (const uint32_t player = Load32(base, kPlayerProfiles)) {
    const uint32_t port = Load32(base, player + kPlayerPort);
    if (port < kPorts)
      return port;
  }
  const uint32_t connected = Load32(base, kConnectedPadMask);
  for (uint32_t port = 0; port < kPorts; ++port)
    if (connected & Load32(base, kPadPortBits + 4 * port))
      return port;
  return 0;
}

// Which port's record 'record' is, or kPorts for another record (a device's).
uint32_t PortOfRecord(const uint8_t *base, uint32_t record) {
  for (uint32_t port = 0; port < kPorts; ++port)
    if (record == Load32(base, kPadInputs + 4 * port) + kPadRecord)
      return port;
  return kPorts;
}

// The keys' buttons as each port's last push took them (game thread), for
// their edges.
uint32_t g_previous[kPorts] = {};

// Whether the keys play now: renut_keyboard on and no overlay window taking
// them.
bool KeysActive() {
  return REXCVAR_GET(renut_keyboard) && !renut::gpu::overlay::WantsCursor();
}

// --- Keystrokes (XUI menus) -------------------------------------------------

// XINPUT_KEYSTROKE flags.
constexpr uint16_t kKeystrokeDown = 0x0001, kKeystrokeUp = 0x0002, kKeystrokeRepeat = 0x0004;
// XInput's repeat of a held key: first after this, then every kRepeatEvery.
constexpr auto kRepeatAfter = std::chrono::milliseconds(400);
constexpr auto kRepeatEvery = std::chrono::milliseconds(100);

// A key's pad keystroke: held while its buttons (or movement keys) are.
struct KeystrokeKey {
  uint16_t virtual_key;
  uint32_t buttons;
  uint32_t move;
};

const KeystrokeKey kKeystrokeKeys[] = {
    {0x5800, kA, 0},                // VK_PAD_A
    {0x5801, kB, 0},                // VK_PAD_B
    {0x5802, kX, 0},                // VK_PAD_X
    {0x5803, kY, 0},                // VK_PAD_Y
    {0x5804, kRightShoulder, 0},    // VK_PAD_RSHOULDER
    {0x5805, kLeftShoulder, 0},     // VK_PAD_LSHOULDER
    {0x5806, kLeftTrigger, 0},      // VK_PAD_LTRIGGER
    {0x5807, kRightTrigger, 0},     // VK_PAD_RTRIGGER
    {0x5810, kDpadUp, 0},           // VK_PAD_DPAD_UP
    {0x5811, kDpadDown, 0},         // VK_PAD_DPAD_DOWN
    {0x5812, kDpadLeft, 0},         // VK_PAD_DPAD_LEFT
    {0x5813, kDpadRight, 0},        // VK_PAD_DPAD_RIGHT
    {0x5814, kStart, 0},            // VK_PAD_START
    {0x5815, kBack, 0},             // VK_PAD_BACK
    {0x5816, kLeftThumb, 0},        // VK_PAD_LTHUMB_PRESS
    {0x5817, kRightThumb, 0},       // VK_PAD_RTHUMB_PRESS
    {0x5820, 0, kMoveForward},      // VK_PAD_LTHUMB_UP
    {0x5821, 0, kMoveBack},         // VK_PAD_LTHUMB_DOWN
    {0x5822, 0, kMoveRight},        // VK_PAD_LTHUMB_RIGHT
    {0x5823, 0, kMoveLeft},         // VK_PAD_LTHUMB_LEFT
};
constexpr size_t kKeystrokeKeyCount = sizeof(kKeystrokeKeys) / sizeof(kKeystrokeKeys[0]);

// What the keystrokes have told the game so far (game thread): which keys are
// down, and when each held one repeats next.
std::bitset<kKeystrokeKeyCount> g_keystroke_down;
std::chrono::steady_clock::time_point g_keystroke_repeat[kKeystrokeKeyCount];

} // namespace

namespace renut::game {

void AttachKeyboard(rex::ui::Window *window) {
  if (!window)
    return;
  window->app_context().CallInUIThreadDeferred([window] {
    if (!g_listener.window())
      g_listener.Attach(window);
  });
}

void DetachKeyboard() {
  rex::ui::Window *window = g_listener.window();
  if (window)
    window->app_context().CallInUIThreadSynchronous([] { g_listener.Detach(); });
}

bool NextKeystroke(uint16_t &virtual_key, uint16_t &flags) {
  const bool active = KeysActive();
  const uint32_t buttons = active ? g_buttons.load(std::memory_order_relaxed) : 0;
  const uint32_t move = active ? g_move.load(std::memory_order_relaxed) : 0;
  const auto now = std::chrono::steady_clock::now();
  // Presses and releases first, then the earliest due repeat.
  size_t repeat = kKeystrokeKeyCount;
  for (size_t i = 0; i < kKeystrokeKeyCount; ++i) {
    const KeystrokeKey &key = kKeystrokeKeys[i];
    const bool held = (buttons & key.buttons) || (move & key.move);
    if (held != g_keystroke_down[i]) {
      g_keystroke_down[i] = held;
      g_keystroke_repeat[i] = now + kRepeatAfter;
      virtual_key = key.virtual_key;
      flags = held ? kKeystrokeDown : kKeystrokeUp;
      return true;
    }
    if (held && g_keystroke_repeat[i] <= now &&
        (repeat == kKeystrokeKeyCount || g_keystroke_repeat[i] < g_keystroke_repeat[repeat]))
      repeat = i;
  }
  if (repeat == kKeystrokeKeyCount)
    return false;
  g_keystroke_repeat[repeat] = now + kRepeatEvery;
  virtual_key = kKeystrokeKeys[repeat].virtual_key;
  flags = kKeystrokeDown | kKeystrokeRepeat;
  return true;
}

} // namespace renut::game

// BOOL padStatePush(record, entry): makes 'entry' the record's current one.
REX_HOOK_RAW(sub_821FBD90) {
  const uint32_t record = ctx.r3.u32;
  const uint32_t entry = ctx.r4.u32;
  // Nothing held (keyboard off, or an overlay window taking the keys) still
  // merges once after keys were, so their release is seen.
  const bool active = KeysActive();
  const uint32_t port = entry ? PortOfRecord(base, record) : kPorts;
  if (port == kPorts) {
    __imp__sub_821FBD90(ctx, base);
    return;
  }
  // Only the keyboard's port plays the keys; keys still held when it changes
  // (player 1 joins on another port) are let go on the old one.
  const bool keyboard_port = active && port == KeyboardPort(base);
  const uint32_t buttons = keyboard_port ? g_buttons.load(std::memory_order_relaxed) : 0;
  const uint32_t move = keyboard_port ? g_move.load(std::memory_order_relaxed) : 0;
  uint32_t &previous = g_previous[port];
  if (!buttons && !(move & ~kMoveWalk) && !previous) {
    __imp__sub_821FBD90(ctx, base);
    return;
  }

  static bool logged = false;
  if (!logged) {
    logged = true;
    RNUT_INFO("Keyboard: keys reach pad port {}", port);
  }

  uint8_t saved[kEntrySize];
  std::memcpy(saved, base + entry, kEntrySize);

  // The edges from the pad and the keys together.
  const uint32_t pad_now = Load32(base, entry + 12);
  const uint32_t pad_before = (pad_now & ~Load32(base, entry + 0)) | Load32(base, entry + 4);
  const uint32_t now = pad_now | buttons;
  const uint32_t before = pad_before | previous;
  Store32(base, entry + 0, now & ~before);
  Store32(base, entry + 4, before & ~now);
  Store32(base, entry + 8, now & before);
  Store32(base, entry + 12, now);
  previous = buttons;

  // Movement keys: the left stick at full strength (half walking), in place
  // of the pad's.
  const float x = float(!!(move & kMoveRight)) - float(!!(move & kMoveLeft));
  const float y = float(!!(move & kMoveForward)) - float(!!(move & kMoveBack));
  if (x != 0.0f || y != 0.0f) {
    const float scale = ((move & kMoveWalk) ? 0.5f : 1.0f) / std::sqrt(x * x + y * y);
    StoreFloat(base, entry + 16, x * scale);
    StoreFloat(base, entry + 20, y * scale);
  }
  if (buttons & kLeftTrigger)
    StoreFloat(base, entry + 32, 1.0f);
  if (buttons & kRightTrigger)
    StoreFloat(base, entry + 36, 1.0f);

  __imp__sub_821FBD90(ctx, base);
  std::memcpy(base + entry, saved, kEntrySize);
}
