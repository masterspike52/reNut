#include "input.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include <chrono>
#include <mutex>

#include <rex/ui/windowed_app_context.h>

#include <rexglue_guide/guide.h>

#include "guest.h"
#include "imgui.h"

namespace rexglue_guide {
namespace {

constexpr float kRepeatDelay = 0.35f;
constexpr float kRepeatRate = 0.09f;
constexpr float kStick = 0.5f;

// The last XINPUT_GAMEPAD the game read for each user.
struct GameRead {
  uint16_t buttons = 0;
  uint8_t lt = 0;
  int16_t lx = 0, ly = 0;
  std::chrono::steady_clock::time_point read_at;
};
std::mutex g_game_pads_mutex;
GameRead g_game_pads[4];

// The app's keyboard pad (SetKeyboardPad), under g_game_pads_mutex.
struct KeyboardRead {
  int player = -1;  // -1: none
  uint16_t buttons = 0;
  float lx = 0.0f, ly = 0.0f;  // y up
};
KeyboardRead g_keyboard_pad;

// XINPUT_GAMEPAD buttons as the Guide's SDL button indices.
constexpr std::pair<uint16_t, SDL_GamepadButton> kXInputButtons[] = {
    {0x1000, SDL_GAMEPAD_BUTTON_SOUTH},          {0x2000, SDL_GAMEPAD_BUTTON_EAST},
    {0x4000, SDL_GAMEPAD_BUTTON_WEST},           {0x8000, SDL_GAMEPAD_BUTTON_NORTH},
    {0x0020, SDL_GAMEPAD_BUTTON_BACK},           {0x0400, SDL_GAMEPAD_BUTTON_GUIDE},
    {0x0010, SDL_GAMEPAD_BUTTON_START},          {0x0040, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {0x0080, SDL_GAMEPAD_BUTTON_RIGHT_STICK},    {0x0100, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {0x0200, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {0x0001, SDL_GAMEPAD_BUTTON_DPAD_UP},
    {0x0002, SDL_GAMEPAD_BUTTON_DPAD_DOWN},      {0x0004, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {0x0008, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};

// The host's own SDL may be a separate copy (a static SDL in the exe), whose
// gamepads nothing else opens; polling them here works either way. Events
// stay off since nobody drains this copy's queue. Called on one thread only
// (the UI thread when the app has one, see Input::Latest).
SDL_Gamepad* ActivePad() {
  static const bool started = [] {
    if (SDL_WasInit(SDL_INIT_GAMEPAD)) return true;
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return false;
    SDL_SetJoystickEventsEnabled(false);
    SDL_SetGamepadEventsEnabled(false);
    return true;
  }();
  if (!started) return nullptr;
  SDL_UpdateGamepads();

  static std::vector<SDL_Gamepad*> open;
  static SDL_Gamepad* active = nullptr;
  for (auto it = open.begin(); it != open.end();) {
    if (SDL_GamepadConnected(*it)) {
      ++it;
      continue;
    }
    if (*it == active) active = nullptr;
    SDL_CloseGamepad(*it);
    it = open.erase(it);
  }
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  for (int i = 0; ids && i < count; ++i) {
    const bool known = std::any_of(open.begin(), open.end(),
                                   [&](SDL_Gamepad* g) { return SDL_GetGamepadID(g) == ids[i]; });
    if (!known) {
      if (SDL_Gamepad* g = SDL_OpenGamepad(ids[i])) open.push_back(g);
    }
  }
  SDL_free(ids);
  // Follow whichever pad was touched last.
  for (SDL_Gamepad* g : open) {
    if (g == active) continue;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
      if (SDL_GetGamepadButton(g, SDL_GamepadButton(b))) {
        active = g;
        break;
      }
    }
  }
  if (!active && !open.empty()) active = open.front();
  return active;
}

}  // namespace

Input::Pad Input::Sample() {
  Pad p;
  SDL_Gamepad* pad = ActivePad();
  if (!pad) return p;
  for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; ++b) {
    p.buttons[b] = SDL_GetGamepadButton(pad, SDL_GamepadButton(b));
  }
  p.lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
  p.ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
  p.lt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f;
  return p;
}

// The Guide's own SDL pad (whichever was touched last). Off the UI thread:
// the pad as last read there, with the next read queued (one at a time); a
// frame late at most.
Input::Pad Input::SdlPad() const {
  if (!ui_ || ui_->IsInUIThread()) return Sample();
  if (!sampled_->queued.exchange(true)) {
    const bool posted = ui_->CallInUIThread([sampled = sampled_] {
      const Pad p = Sample();
      std::lock_guard lock(sampled->mutex);
      sampled->pad = p;
      sampled->queued = false;
    });
    if (!posted) sampled_->queued = false;
  }
  std::lock_guard lock(sampled_->mutex);
  return sampled_->pad;
}

// A controller as the game last read it (XamInputGetState below), or all of
// them together (-1), from the last quarter second. Whatever controller the
// game can use, the Guide can too, even when the Guide's own SDL copy can't
// see it (a DualSense over Bluetooth in reNut). The Guide button only comes
// through with the runtime's guide_button cvar on.
Input::Pad Input::GamePad(int controller, bool* fresh) {
  constexpr auto kFresh = std::chrono::milliseconds(250);
  Pad p;
  if (fresh) *fresh = false;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(g_game_pads_mutex);
  for (int c = 0; c < 4; ++c) {
    const GameRead& g = g_game_pads[c];
    if ((controller >= 0 && c != controller) || now - g.read_at > kFresh) continue;
    if (fresh) *fresh = true;
    for (const auto& [bit, button] : kXInputButtons) p.buttons[button] |= (g.buttons & bit) != 0;
    // XInput's Y axis points up, SDL's down.
    const float lx = g.lx / 32767.0f, ly = -g.ly / 32767.0f;
    if (std::abs(lx) > std::abs(p.lx)) p.lx = lx;
    if (std::abs(ly) > std::abs(p.ly)) p.ly = ly;
    p.lt = std::max(p.lt, g.lt / 255.0f);
  }
  return p;
}

// The app's keyboard pad; *attached: there is one.
Input::Pad Input::KeyboardPad(bool* attached) {
  Pad p;
  std::lock_guard lock(g_game_pads_mutex);
  if (attached) *attached = g_keyboard_pad.player >= 0;
  if (g_keyboard_pad.player < 0) return p;
  for (const auto& [bit, button] : kXInputButtons) p.buttons[button] = (g_keyboard_pad.buttons & bit) != 0;
  p.lx = g_keyboard_pad.lx;
  p.ly = -g_keyboard_pad.ly;  // SDL's y points down
  return p;
}

int Input::KeyboardPlayer() {
  std::lock_guard lock(g_game_pads_mutex);
  return g_keyboard_pad.player;
}

Input::Pad Input::Merge(const Pad& a, const Pad& b) {
  Pad p = a;
  for (int i = 0; i < 32; ++i) p.buttons[i] |= b.buttons[i];
  if (std::abs(b.lx) > std::abs(p.lx)) p.lx = b.lx;
  if (std::abs(b.ly) > std::abs(p.ly)) p.ly = b.ly;
  p.lt = std::max(p.lt, b.lt);
  return p;
}

// The controller that pressed the Guide button this frame. The game's reads
// say which controller it was; the SDL pad's press only counts when they
// don't carry the button (no guide_button), as the same press shows up in
// both, a frame apart.
int Input::GuideFrom(const Pad& sdl) {
  constexpr double kSamePress = 0.3;
  int from = -1;
  bool game_held = false;
  for (int c = 0; c < 4; ++c) {
    bool fresh = false;
    const bool down = GamePad(c, &fresh).buttons[SDL_GAMEPAD_BUTTON_GUIDE] && fresh;
    if (down && !prev_guide_[c] && from < 0) from = c;
    game_held |= down;
    prev_guide_[c] = down;
  }
  if (from >= 0) last_game_guide_ = clock_;
  const bool sdl_down = sdl.buttons[SDL_GAMEPAD_BUTTON_GUIDE];
  const bool recent = last_game_guide_ >= 0.0 && clock_ - last_game_guide_ < kSamePress;
  if (from < 0 && sdl_down && !prev_sdl_guide_ && !game_held && !recent) from = kGuideFromSdl;
  prev_sdl_guide_ = sdl_down;
  const bool keyboard_down = KeyboardPad().buttons[SDL_GAMEPAD_BUTTON_GUIDE];
  if (from < 0 && keyboard_down && !prev_keyboard_guide_) from = kGuideFromKeyboard;
  prev_keyboard_guide_ = keyboard_down;
  return from;
}

bool Input::Repeat::Tick(bool down, float dt) {
  if (!down) {
    held = -1.0f;
    return false;
  }
  if (held < 0.0f) {
    held = 0.0f;
    next = kRepeatDelay;
    return true;
  }
  held += dt;
  if (held >= next) {
    next += kRepeatRate;
    return true;
  }
  return false;
}

NavInput Input::Poll(float dt, bool text_entry, int controller) {
  NavInput in;
  clock_ += dt;
  const Pad sdl = SdlPad();
  in.guide_from = GuideFrom(sdl);
  // The driving controller's pad alone, while the game reads it; otherwise
  // every pad.
  bool fresh = false;
  const Pad own = controller >= 0 && controller < 4 ? GamePad(controller, &fresh) : Pad{};
  const Pad pads = fresh ? own : Merge(sdl, GamePad(-1));
  // The app's keyboard pad joins in unless another controller drives the
  // Guide, and never while typing (its keys are letters then). It counts as
  // the keyboard, not a pad, for the hints shown.
  bool keyboard_attached = false;
  const Pad keys = KeyboardPad(&keyboard_attached);
  const Pad pad = !fresh && !text_entry ? Merge(pads, keys) : pads;
  const bool* now = pad.buttons;
  const float lx = pad.lx, ly = pad.ly, lt = pad.lt;
  auto pressed = [&](SDL_GamepadButton b) { return now[b] && !prev_[b]; };

  in.accept = pressed(SDL_GAMEPAD_BUTTON_SOUTH);
  in.back = pressed(SDL_GAMEPAD_BUTTON_EAST);
  in.x = pressed(SDL_GAMEPAD_BUTTON_WEST);
  in.y = pressed(SDL_GAMEPAD_BUTTON_NORTH);
  in.lb = pressed(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
  in.rb = pressed(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
  in.lb_held = now[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER];
  in.rb_held = now[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER];
  in.start = pressed(SDL_GAMEPAD_BUTTON_START);
  in.ls = pressed(SDL_GAMEPAD_BUTTON_LEFT_STICK);
  in.lt = lt > 0.5f && prev_lt_ <= 0.5f;

  bool pad_dir[4] = {now[SDL_GAMEPAD_BUTTON_DPAD_UP] || ly < -kStick,
                     now[SDL_GAMEPAD_BUTTON_DPAD_DOWN] || ly > kStick,
                     now[SDL_GAMEPAD_BUTTON_DPAD_LEFT] || lx < -kStick,
                     now[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] || lx > kStick};
  // A real pad touched: buttons or stick (not the keyboard pad's).
  bool pad_any = std::abs(pads.lx) > kStick || std::abs(pads.ly) > kStick;
  for (int b = 0; b < 32 && !pad_any; ++b) pad_any = pads.buttons[b];
  bool keys_any = std::abs(keys.lx) > kStick || std::abs(keys.ly) > kStick;
  for (int b = 0; b < 32 && !keys_any; ++b) keys_any = keys.buttons[b];

  // Keyboard. Letters only navigate when nothing is being typed.
  auto key = [](ImGuiKey k) { return ImGui::IsKeyDown(k); };
  auto key_pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
  const bool kb_dir[4] = {key(ImGuiKey_UpArrow), key(ImGuiKey_DownArrow), key(ImGuiKey_LeftArrow),
                          key(ImGuiKey_RightArrow)};
  if (key_pressed(ImGuiKey_Escape)) in.back = true;
  if (!text_entry) {
    if (key_pressed(ImGuiKey_Enter) || key_pressed(ImGuiKey_KeypadEnter)) in.accept = true;
    if (key_pressed(ImGuiKey_Backspace)) in.back = true;
    if (key_pressed(ImGuiKey_PageUp)) in.lb = true;
    if (key_pressed(ImGuiKey_PageDown)) in.rb = true;
    // The app's key binds may use these letters (its keyboard pad has the
    // buttons then).
    if (!keyboard_attached) {
      if (key_pressed(ImGuiKey_Q)) in.lb = true;
      if (key_pressed(ImGuiKey_E)) in.rb = true;
      if (key_pressed(ImGuiKey_X)) in.x = true;
      if (key_pressed(ImGuiKey_Y)) in.y = true;
    }
  }
  const bool kb_any = std::any_of(std::begin(kb_dir), std::end(kb_dir), [](bool b) { return b; }) ||
                      keys_any || in.accept || in.back || ImGui::GetIO().InputQueueCharacters.Size > 0;
  if (pad_any) pad_used_ = true;
  if (kb_any && !pad_any) pad_used_ = false;
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) pad_used_ = false;

  in.up = up_.Tick(pad_dir[0] || kb_dir[0], dt);
  in.down = down_.Tick(pad_dir[1] || kb_dir[1], dt);
  in.left = left_.Tick(pad_dir[2] || kb_dir[2], dt);
  in.right = right_.Tick(pad_dir[3] || kb_dir[3], dt);
  in.pad_used = pad_used_;
  // Held keys keep the game's input held too, so a key that closed the Guide
  // doesn't reach the game.
  in.pad_held = pad_any || keys_any || lt > 0.3f;

  std::copy(now, now + 32, std::begin(prev_));
  prev_lt_ = lt;
  return in;
}

void SetKeyboardPad(int player, uint16_t buttons, float lx, float ly) {
  std::lock_guard lock(g_game_pads_mutex);
  g_keyboard_pad = {player >= 0 && player < 4 ? player : 0, buttons, lx, ly};
}

void ClearKeyboardPad() {
  std::lock_guard lock(g_game_pads_mutex);
  g_keyboard_pad = {};
}

}  // namespace rexglue_guide

// XamInputGetState(user, flags, XINPUT_STATE*): the runtime's, with what each
// user's pad read kept for the Guide (Input::RuntimePad). XINPUT_STATE:
// packet number, then XINPUT_GAMEPAD (buttons, triggers, sticks, big-endian).
extern "C" REX_FUNC(__imp__XamInputGetState) {
  using namespace rexglue_guide::guest;
  const uint32_t user = ctx.r3.u32;
  const uint32_t state_ptr = ctx.r5.u32;
  CallOriginal("__imp__XamInputGetState", ctx, base);
  const uint8_t* state = At(state_ptr);
  if (ctx.r3.u32 != 0 || !state || user >= 4) return;
  using rexglue_guide::g_game_pads;
  std::lock_guard lock(rexglue_guide::g_game_pads_mutex);
  auto& pad = g_game_pads[user];
  pad.buttons = Load16(state + 4);
  pad.lt = state[6];
  pad.lx = int16_t(Load16(state + 8));
  pad.ly = int16_t(Load16(state + 10));
  pad.read_at = std::chrono::steady_clock::now();
}
