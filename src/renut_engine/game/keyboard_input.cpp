#include "renut_engine/game/keyboard_input.h"

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
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <rexfabric/host_input.h>
#include <rexglue_guide/guide.h>

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/game/mouse_camera.h"
#include "renut_engine/game/vehicle_drivers.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

REX_EXTERN(__imp__sub_821CB628);  // padStatePush

namespace {

struct KeyboardSettings final : rex::SettingOwner {
  KeyboardSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
  REX_SETTING(bool, renut_keyboard, false,
              "Bound keys and mouse buttons (with the mouse captured) play player 1's "
              "controller: every action, alongside the controller. Binds are key names: "
              "A-Z, 0-9, F1-F24, Space, Return, Escape, Tab, Shift, Control, Alt, "
              "Left/Right/Up/Down, LMB/RMB/MMB, Numpad0-9, ... Empty unbinds.");
  REX_SETTING(std::string, renut_key_forward, "W", "Move forward (left stick up).");
  REX_SETTING(std::string, renut_key_back, "S", "Move back (left stick down).");
  REX_SETTING(std::string, renut_key_left, "A", "Move left (left stick left).");
  REX_SETTING(std::string, renut_key_right, "D", "Move right (left stick right).");
  REX_SETTING(std::string, renut_key_walk, "Control", "Held with the movement keys: half-strength stick (walk).");
  REX_SETTING(std::string, renut_key_a, "Space", "Controller A (jump).");
  REX_SETTING(std::string, renut_key_b, "RMB", "Controller B.");
  REX_SETTING(std::string, renut_key_x, "LMB", "Controller X.");
  REX_SETTING(std::string, renut_key_y, "E", "Controller Y.");
  REX_SETTING(std::string, renut_key_lb, "Q", "Controller left bumper.");
  REX_SETTING(std::string, renut_key_rb, "R", "Controller right bumper.");
  REX_SETTING(std::string, renut_key_lt, "Shift", "Controller left trigger.");
  REX_SETTING(std::string, renut_key_rt, "F", "Controller right trigger.");
  REX_SETTING(std::string, renut_key_start, "Escape", "Controller Start (Escape also frees the mouse).");
  REX_SETTING(std::string, renut_key_back_button, "Tab", "Controller Back.");
  REX_SETTING(std::string, renut_key_ls, "C", "Left stick click.");
  REX_SETTING(std::string, renut_key_rs, "V", "Right stick click.");
  REX_SETTING(std::string, renut_key_dpad_up, "Up", "D-pad up.");
  REX_SETTING(std::string, renut_key_dpad_down, "Down", "D-pad down.");
  REX_SETTING(std::string, renut_key_dpad_left, "Left", "D-pad left.");
  REX_SETTING(std::string, renut_key_dpad_right, "Right", "D-pad right.");
  REX_SETTING(std::string, renut_key_guide, "G",
              "Controller Guide button: opens the Guide (RexLiveless) for player 1.");
};
KeyboardSettings &g_keys = *new KeyboardSettings();  // never freed, registered by RenutApp::OnConfigure

// A key name's virtual key (rex.input's names), kNone without the input system.
rex::VirtualKey ParseKey(rex::hid::IInputSystem *input, std::string_view name) {
  return input ? input->ParseVirtualKey(name) : rex::VirtualKey::kNone;
}

// Guest data.
constexpr uint32_t kPadInputs = 0x82E0B158;       // g_padInputs[port]
constexpr uint32_t kConnectedPadMask = 0x82E0B14C;  // g_connectedPadMask
constexpr uint32_t kPadPortBits = 0x821051D8;     // g_padPortBits[port]
constexpr uint32_t kPlayerProfiles = 0x82E1B280;  // dword_82E1B280[player]: player slots
constexpr uint32_t kPorts = 4;
constexpr uint32_t kPadRecord = 52;               // g_padInputs[port] + 52: the port's record
// Player slot + 128: his port (TU3; 108 before it). sub_821DD268 finds the
// player of a port by it.
constexpr uint32_t kPlayerPort = 128;
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
  std::string (*key)();
  uint32_t buttons;  // pad buttons
  uint32_t move;     // movement keys
};

#define RENUT_BIND(cvar, buttons, move) \
  Bind{[]() -> std::string { return g_keys.cvar.Get(); }, buttons, move}

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
// Every key's state by virtual key, for mods (rex.input).
std::atomic<bool> g_key_down[256];
// For mods: the mouse buttons (left, right, middle, side 1, side 2) whether or not the mouse is
// captured, and the cursor over the window (0..1 across and down; -1 before it has been over it).
std::atomic<bool> g_mouse_down[5];
std::atomic<float> g_cursor_x{-1.0f}, g_cursor_y{-1.0f}, g_window_aspect{0.0f};
// The mouse's motion in physical pixels since a mod last took it (rex.input.mouse_delta).
std::atomic<int32_t> g_mouse_dx{0}, g_mouse_dy{0};
// A mod holds the game's input (rex.input.hold_guest): player 1's pad entries reach it empty.
std::atomic<bool> g_guest_held{false};
// The entries held from the game, as the pad gave them, for mods (HeldPadEntry).
std::mutex g_held_mutex;
uint8_t g_held_entry[4][40] = {};
uint32_t g_held_port = 4;

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
class KeyboardListener : public rex::ui::IUIEventListener {
public:
  // Added before the mouse camera's listener, so its events come first.
  void Attach(rex::ui::IUISystem &ui) {
    ui_ = &ui;
    input_ = REX_INTERFACE(rex::hid::IInputSystem);
    guard_ = ui.AddEventListener(*this);
  }

  void Detach() {
    if (!ui_)
      return;
    Clear();
    guard_.Reset();
    ui_ = nullptr;
    input_ = nullptr;
  }

  rex::ui::IUISystem *ui() const { return ui_; }

  void OnInputEvent(rex::InputEvent &e) override {
    using Type = rex::InputEvent::Type;
    switch (e.type) {
    case Type::kKeyDown: Set(e.key, true); break;
    case Type::kKeyUp: Set(e.key, false); break;
    // Before the mouse camera's listener: the click that captures the mouse
    // finds it free, and plays nothing.
    case Type::kButtonDown:
      Cursor(e);
      ModButton(e.button, true);
      if (renut::game::MouseCaptured())
        Set(MouseKey(e.button), true);
      break;
    case Type::kButtonUp:
      Cursor(e);
      ModButton(e.button, false);
      Set(MouseKey(e.button), false);
      break;
    case Type::kMotion:
      Cursor(e);
      g_mouse_dx.fetch_add(int32_t(std::lround(e.dx)), std::memory_order_relaxed);
      g_mouse_dy.fetch_add(int32_t(std::lround(e.dy)), std::memory_order_relaxed);
      break;
    default: break;
    }
  }

  // The UI system takes a focus change after its listeners, and a listener can't go
  // during a delivery, so both wait for the UI thread's next task.
  void OnUIEvent(const rex::ui::UIEvent &e) override {
    using Type = rex::ui::UIEvent::Type;
    if (e.GetType() == Type::kFocus) {
      ui_->PostTask([this] {
        if (ui_ && !ui_->IsFocused())
          Clear();
      });
    } else if (e.GetType() == Type::kClose) {
      ui_->PostTask([this] { Detach(); });
    }
  }

private:
  static rex::VirtualKey MouseKey(rex::MouseButton button) {
    switch (button) {
    case rex::MouseButton::kLeft: return rex::VirtualKey::kLButton;
    case rex::MouseButton::kRight: return rex::VirtualKey::kRButton;
    case rex::MouseButton::kMiddle: return rex::VirtualKey::kMButton;
    case rex::MouseButton::kX1: return rex::VirtualKey::kXButton1;
    case rex::MouseButton::kX2: return rex::VirtualKey::kXButton2;
    default: return rex::VirtualKey::kNone;
    }
  }

  void Set(rex::VirtualKey key, bool down) {
    const size_t index = size_t(key);
    if (key == rex::VirtualKey::kNone || index >= held_.size())
      return;
    held_[index] = down;
    g_key_down[index].store(down, std::memory_order_relaxed);
    Update();
  }

  void Clear() {
    held_.reset();
    for (std::atomic<bool> &key : g_key_down)
      key.store(false, std::memory_order_relaxed);
    for (std::atomic<bool> &button : g_mouse_down)
      button.store(false, std::memory_order_relaxed);
    Update();
  }

  // Event positions and the window's size are physical pixels.
  void Cursor(const rex::InputEvent &e) {
    const rex::PointerState pointer = input_ ? input_->GetPointer() : rex::PointerState{};
    const uint32_t w = pointer.window_width;
    const uint32_t h = pointer.window_height;
    if (!w || !h)
      return;
    g_cursor_x.store(float(e.x) / float(w), std::memory_order_relaxed);
    g_cursor_y.store(float(e.y) / float(h), std::memory_order_relaxed);
    g_window_aspect.store(float(w) / float(h), std::memory_order_relaxed);
  }

  static void ModButton(rex::MouseButton button, bool down) {
    int index = -1;
    switch (button) {
    case rex::MouseButton::kLeft: index = 0; break;
    case rex::MouseButton::kRight: index = 1; break;
    case rex::MouseButton::kMiddle: index = 2; break;
    case rex::MouseButton::kX1: index = 3; break;
    case rex::MouseButton::kX2: index = 4; break;
    default: return;
    }
    g_mouse_down[index].store(down, std::memory_order_relaxed);
  }

  // The binds' keys are looked up as they are now, so a changed bind takes
  // effect from the next key.
  void Update() {
    uint32_t buttons = 0, move = 0;
    for (const Bind &bind : kBinds) {
      const std::string name = bind.key();
      if (name.empty())
        continue;
      const size_t index = size_t(ParseKey(input_, name));
      if (index && index < held_.size() && held_[index]) {
        buttons |= bind.buttons;
        move |= bind.move;
      }
    }
    g_buttons.store(buttons, std::memory_order_relaxed);
    g_move.store(move, std::memory_order_relaxed);
    UpdateGuide(buttons, move);
  }

  // Feeds the keys to the Guide as player 1's pad; the Guide key opens it and
  // never reaches the game.
  void UpdateGuide(uint32_t buttons, uint32_t move) {
    if (!g_keys.renut_keyboard.Get() || renut::gpu::overlay::WantsCursor()) {
      rexglue_guide::ClearKeyboardPad();
      return;
    }
    constexpr uint16_t kGuideButton = 0x0400;  // XINPUT_GAMEPAD_GUIDE
    const std::string guide_key = g_keys.renut_key_guide.Get();
    const size_t guide = guide_key.empty() ? 0 : size_t(ParseKey(input_, guide_key));
    const bool guide_held = guide && guide < held_.size() && held_[guide];
    const float x = float(!!(move & kMoveRight)) - float(!!(move & kMoveLeft));
    const float y = float(!!(move & kMoveForward)) - float(!!(move & kMoveBack));
    rexglue_guide::SetKeyboardPad(0, uint16_t(buttons & 0xFFFF) | (guide_held ? kGuideButton : 0), x, y);
  }

  rex::ui::IUISystem *ui_ = nullptr;
  rex::hid::IInputSystem *input_ = nullptr;
  rex::ScopeGuard guard_;
  std::bitset<256> held_;
};

KeyboardListener &g_listener = *new KeyboardListener();  // never freed: no listener removal at exit

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

// Whether the keys play now: renut_keyboard on, and no overlay window nor the
// Guide (RexLiveless) taking them.
bool KeysActive() {
  return g_keys.renut_keyboard.Get() && !renut::gpu::overlay::WantsCursor() &&
         !rexglue_guide::IsOpen();
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

void AttachKeyboard(rex::ui::IUISystem &ui) {
  ui.PostTask([&ui] {
    if (!g_listener.ui())
      g_listener.Attach(ui);
  });
}

void DetachKeyboard() {
  rex::ui::IUISystem *ui = g_listener.ui();
  if (ui && !ui->RunSynchronous([] { g_listener.Detach(); }))
    g_listener.Detach();
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

uint32_t KeyboardButtons() {
  return KeysActive() ? g_buttons.load(std::memory_order_relaxed) & 0xFFFF : 0;
}

bool HeldPadEntry(uint32_t port, uint8_t (&entry)[40]) {
  if (!g_guest_held.load(std::memory_order_relaxed))
    return false;
  std::lock_guard lock(g_held_mutex);
  if (port != g_held_port)
    return false;
  std::memcpy(entry, g_held_entry[port], sizeof(entry));
  return true;
}

bool CursorPosition(float &x, float &y, float &aspect) {
  x = g_cursor_x.load(std::memory_order_relaxed);
  y = g_cursor_y.load(std::memory_order_relaxed);
  aspect = g_window_aspect.load(std::memory_order_relaxed);
  return x >= 0.0f && aspect > 0.0f;
}

void InstallModInput() {
  rexfabric::HostInput input;
  input.resolve_key = [](std::string_view name) {
    return uint16_t(ParseKey(REX_INTERFACE(rex::hid::IInputSystem), name));
  };
  input.key_down = [](uint16_t key) { return key < 256 && g_key_down[key].load(std::memory_order_relaxed); };
  input.mouse_button_down = [](int button) {
    return button >= 0 && button < 5 && g_mouse_down[button].load(std::memory_order_relaxed);
  };
  input.accepts_input = [] { return !renut::gpu::overlay::WantsCursor() && !rexglue_guide::IsOpen(); };
  input.consume_mouse_delta = [] {
    return std::pair<int32_t, int32_t>(g_mouse_dx.exchange(0, std::memory_order_relaxed),
                                       g_mouse_dy.exchange(0, std::memory_order_relaxed));
  };
  input.consume_wheel = [] { return renut::gpu::overlay::ConsumeMouseWheel(); };
  input.set_guest_input_held = [](bool held) { g_guest_held.store(held, std::memory_order_relaxed); };
  input.flush = [] {
    g_mouse_dx.store(0, std::memory_order_relaxed);
    g_mouse_dy.store(0, std::memory_order_relaxed);
    renut::gpu::overlay::ConsumeMouseWheel();
  };
  rexfabric::SetHostInput(std::move(input));
}

} // namespace renut::game

// BOOL padStatePush(record, entry): makes 'entry' the record's current one.
REX_HOOK_RAW(sub_821CB628) {
  // A vehicle the Remote Control drives takes the player's
  // (vehicle_drivers.cpp).
  if (renut::game::VehicleDriversPadPush(ctx, base))
    return;
  const uint32_t record = ctx.r3.u32;
  const uint32_t entry = ctx.r4.u32;
  // Nothing held (keyboard off, or an overlay window taking the keys) still
  // merges once after keys were, so their release is seen.
  const bool active = KeysActive();
  const uint32_t port = entry ? PortOfRecord(base, record) : kPorts;
  if (port == kPorts) {
    __imp__sub_821CB628(ctx, base);
    return;
  }
  // A mod holding the game's input: player 1's port pushes an empty entry, put back after (mods read it held).
  if (g_guest_held.load(std::memory_order_relaxed) && port == KeyboardPort(base)) {
    uint8_t held[kEntrySize];
    std::memcpy(held, base + entry, kEntrySize);
    {
      std::lock_guard lock(g_held_mutex);
      std::memcpy(g_held_entry[port], held, kEntrySize);
      g_held_port = port;
    }
    std::memset(base + entry, 0, kEntrySize);
    __imp__sub_821CB628(ctx, base);
    std::memcpy(base + entry, held, kEntrySize);
    return;
  }
  // Only the keyboard's port plays the keys; keys still held when it changes
  // (player 1 joins on another port) are let go on the old one.
  const bool keyboard_port = active && port == KeyboardPort(base);
  const uint32_t buttons = keyboard_port ? g_buttons.load(std::memory_order_relaxed) : 0;
  const uint32_t move = keyboard_port ? g_move.load(std::memory_order_relaxed) : 0;
  uint32_t &previous = g_previous[port];
  if (!buttons && !(move & ~kMoveWalk) && !previous) {
    __imp__sub_821CB628(ctx, base);
    return;
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

  __imp__sub_821CB628(ctx, base);
  std::memcpy(base + entry, saved, kEntrySize);
}
