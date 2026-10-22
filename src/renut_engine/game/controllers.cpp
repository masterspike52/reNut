/**
 * @file    game/controllers.cpp
 * @brief   Player 1 is the controller that presses A first; 2-4 can be off.
 *
 * The kernel has a signed-in profile for user 0 only (XamUserGetSigninState,
 * XamUserGetSigninInfo, XamUserGetXUID: no such user for 1-3). N&B acts on any
 * controller that reports in, so a second one (a second pad, or the same pad
 * seen twice) is a player without a profile, and the game asks for a sign-in.
 *
 * All of N&B's controller access goes through its three XInput wrappers
 * (IDA: XInputGetState 0x8220F200 from inputPollAllPads, XInputGetKeystroke
 * 0x821FD318 from the XUI readers, XInputSetState 0x8228F1B8 from
 * rumbleSendPort). Here:
 * - player 1 is the first controller to press A or START (in its state or as
 *   a keystroke), for as long as it stays connected; until one has, the first
 *   connected controller's state, and every controller's keystrokes;
 * - players 2-4 are the other connected controllers in host slot order, or,
 *   with renut_disable_extra_controllers on, not connected (their keystrokes
 *   are dropped);
 * - the host slots are probed whenever the game reads player 1's state, and
 *   keystrokes and rumble go through the same mapping.
 * The XUI menus read keystrokes of any user (XUSER_INDEX_ANY, or
 * XINPUT_FLAG_ANYUSER: sub_82279D80, sub_823E9600, sub_823E8C98), which the
 * SDK answers from host slot 0 alone, as user 0; those are read here from
 * every host slot in turn. The bound keys' keystrokes (keyboard_input.cpp)
 * come first, as player 1's.
 *
 * Logged (category renut_input), as it changes: the host slots with a
 * controller, which claims player 1 and when it is lost, each player's
 * controller or why it has none (none connected, or players 2-4 off), the
 * first ignored press of a controller that is no player, and the game making a
 * new player for a user (a profile signing in, or one it took for another:
 * that player has to press START again).
 */
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "renut_engine/game/controllers.h"
#include "renut_engine/game/keyboard_input.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(renut_disable_extra_controllers, true, "Nuts&Bolts/Input",
                    "Only player 1 (the first controller to press A) plays; the other controllers "
                    "are not connected. Off: they are players 2-4, and the game asks them to sign "
                    "in (only player 1 has a profile).");

REX_EXTERN(__imp__rex_XInputGetState);
REX_EXTERN(__imp__rex_XInputGetKeystroke);
REX_EXTERN(__imp__rex_XInputSetState);
REX_EXTERN(__imp__sub_826EC1E8);  // makes the game's player for a user

namespace {

const rex::LogCategoryId kInputLog = rex::RegisterLogCategory("renut_input");
#define RINPUT_INFO(...) REXLOG_CAT_INFO(kInputLog, __VA_ARGS__)
#define RINPUT_WARN(...) REXLOG_CAT_WARN(kInputLog, __VA_ARGS__)

constexpr uint32_t kUserCount = 4;
constexpr uint32_t kUserIndexAny = 0xFF;
constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorDeviceNotConnected = 1167;
constexpr uint32_t kErrorEmpty = 4306;
// XINPUT_STATE: packet number, then XINPUT_GAMEPAD (wButtons first).
constexpr uint32_t kStateSize = 16;
constexpr uint32_t kStateButtonsOffset = 4;
// XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START: the buttons that claim player 1.
constexpr uint16_t kClaimButtons = 0x1000 | 0x0010;
// XINPUT_KEYSTROKE: VirtualKey, Unicode, Flags (2 bytes each), UserIndex,
// HidCode (1 byte each).
constexpr uint32_t kKeystrokeSize = 8;
constexpr uint32_t kKeystrokeFlagsOffset = 4;
constexpr uint32_t kKeystrokeUserIndexOffset = 6;
constexpr uint16_t kVirtualKeyPadA = 0x5800;      // VK_PAD_A
constexpr uint16_t kVirtualKeyPadStart = 0x5814;  // VK_PAD_START
constexpr uint16_t kKeystrokeKeyDown = 1;         // XINPUT_KEYSTROKE_KEYDOWN
constexpr uint32_t kFlagAnyUser = 0x40000000;     // XINPUT_FLAG_ANYUSER
// Keystrokes of dropped controllers skipped in one XInputGetKeystroke call.
constexpr uint32_t kMaxSkippedKeystrokes = 16;
constexpr int kNoPad = -1;

// The host slot of the controller that pressed A first, while connected.
int g_claimed_pad = kNoPad;
// The host slot each player's controller is in (kNoPad: not connected), from
// the last probe.
int g_player_pad[kUserCount] = {kNoPad, kNoPad, kNoPad, kNoPad};

// What was last logged: the host slots with a controller, each player's
// status (and its controller's slot), and the slots whose ignored presses were.
enum class PlayerStatus { kUnknown, kConnected, kNoController, kTurnedOff };
uint32_t g_logged_connected = ~0u;
PlayerStatus g_logged_status[kUserCount] = {PlayerStatus::kUnknown, PlayerStatus::kNoController,
                                            PlayerStatus::kNoController,
                                            PlayerStatus::kNoController};
int g_logged_pad[kUserCount] = {kNoPad, kNoPad, kNoPad, kNoPad};
uint32_t g_logged_ignored = 0;
int g_logged_focus = -1;        // whether the game window had the focus
bool g_logged_press = false;    // player 1's first press, since its controller changed
// Player 1's buttons from the game's last read (Player1Buttons).
std::atomic<uint16_t> g_player1_buttons{0};

uint16_t Load16(const uint8_t *p) {
  return rex::byte_swap(*reinterpret_cast<const uint16_t *>(p));
}

void Store16(uint8_t *p, uint16_t value) {
  value = rex::byte_swap(value);
  std::memcpy(p, &value, 2);
}

std::string SlotsText(uint32_t mask) {
  std::string text;
  for (uint32_t pad = 0; pad < kUserCount; ++pad) {
    if (mask & (1u << pad))
      text += (text.empty() ? "" : ", ") + std::to_string(pad);
  }
  return text.empty() ? "none" : text;
}

// Whether one of this process's windows has the focus. Without it the SDK
// hands the game untouched controllers (its input drivers' active callback,
// rex_app.cpp), so a connected controller does nothing.
bool GameWindowFocused() {
#if defined(_WIN32)
  const HWND foreground = GetForegroundWindow();
  DWORD process = 0;
  if (!foreground || !GetWindowThreadProcessId(foreground, &process))
    return false;
  return process == GetCurrentProcessId();
#else
  return true;
#endif
}

// Logs the game window losing and getting back the focus.
void LogFocus() {
  const int focused = GameWindowFocused() ? 1 : 0;
  if (focused == g_logged_focus)
    return;
  if (!focused)
    RINPUT_WARN("Input: the game window is not focused: the controllers do nothing in the game "
                "until it is (click the game window)");
  else if (g_logged_focus == 0)
    RINPUT_INFO("Input: the game window is focused again: the controllers reach the game");
  g_logged_focus = focused;
  g_logged_press = false;
}

// Logs player 1's first press since its controller (or the focus) changed: its
// presses reach the game.
void LogPress(const uint8_t *state, int pad) {
  const uint16_t buttons = Load16(state + kStateButtonsOffset);
  if (g_logged_press || !buttons)
    return;
  g_logged_press = true;
  RINPUT_INFO("Input: player 1's controller (host slot {}) reaches the game (buttons {:04X})",
              pad, buttons);
}

// Logs the host slots with a controller when they change.
void LogConnected(uint32_t connected) {
  if (connected == g_logged_connected)
    return;
  if (connected == 0)
    RINPUT_WARN("Input: no controller connected (host slots: none)");
  else
    RINPUT_INFO("Input: controllers in host slots {} (before: {})", SlotsText(connected),
                g_logged_connected == ~0u ? "-" : SlotsText(g_logged_connected));
  g_logged_connected = connected;
  g_logged_ignored = 0;
}

// Players in order from the controllers connected at the last probe (bit = host slot).
void AssignPlayers(uint32_t connected) {
  if (g_claimed_pad != kNoPad && !(connected & (1u << g_claimed_pad))) {
    RINPUT_WARN("Input: player 1's controller (host slot {}) disconnected; until a controller "
                "presses A or START, player 1 is the first one connected",
                g_claimed_pad);
    g_claimed_pad = kNoPad;
  }
  uint32_t players = 0;
  if (g_claimed_pad != kNoPad)
    g_player_pad[players++] = g_claimed_pad;
  for (uint32_t pad = 0; pad < kUserCount; ++pad) {
    if ((connected & (1u << pad)) && int(pad) != g_claimed_pad)
      g_player_pad[players++] = int(pad);
  }
  for (; players < kUserCount; ++players)
    g_player_pad[players] = kNoPad;
}

// The controller in host slot 'pad' pressed A or START ('how': in its state,
// or as a keystroke): player 1 is its, unless one already claimed it.
void ClaimPlayer1(uint32_t pad, const char *how) {
  if (g_claimed_pad != kNoPad || pad >= kUserCount)
    return;
  RINPUT_INFO("Input: the controller in host slot {} pressed A or START ({}): it is player 1",
              pad, how);
  g_claimed_pad = int(pad);
  g_logged_ignored = 0;
  uint32_t connected = 1u << pad;
  for (int other : g_player_pad) {
    if (other != kNoPad)
      connected |= 1u << other;
  }
  AssignPlayers(connected);
}

bool PlayerEnabled(uint32_t user) {
  return user == 0 || !REXCVAR_GET(renut_disable_extra_controllers);
}

// The host slot of 'user''s controller, or kNoPad.
int PlayerPad(uint32_t user) {
  return user < kUserCount && PlayerEnabled(user) ? g_player_pad[user] : kNoPad;
}

// The player whose controller is in host slot 'pad', or kUserCount.
uint32_t PadPlayer(uint32_t pad) {
  for (uint32_t user = 0; user < kUserCount; ++user) {
    if (PlayerPad(user) == int(pad))
      return user;
  }
  return kUserCount;
}

// Logs whether the game gets a controller for 'user', and why not, when that
// changes.
void LogPlayer(uint32_t user) {
  const int pad = g_player_pad[user];
  PlayerStatus status = PlayerStatus::kConnected;
  if (pad == kNoPad)
    status = PlayerStatus::kNoController;
  else if (!PlayerEnabled(user))
    status = PlayerStatus::kTurnedOff;
  if (status == g_logged_status[user] && pad == g_logged_pad[user])
    return;
  g_logged_status[user] = status;
  g_logged_pad[user] = pad;
  if (user == 0)
    g_logged_press = false;
  switch (status) {
  case PlayerStatus::kConnected:
    if (user == 0 && g_claimed_pad == kNoPad)
      RINPUT_INFO("Input: player 1 is the controller in host slot {} (the first connected, until "
                  "one presses A or START)",
                  pad);
    else
      RINPUT_INFO("Input: player {} is the controller in host slot {}", user + 1, pad);
    break;
  case PlayerStatus::kNoController:
    if (user == 0)
      RINPUT_WARN("Input: player 1 has no controller: none is connected");
    else
      RINPUT_INFO("Input: player {} has no controller", user + 1);
    break;
  case PlayerStatus::kTurnedOff:
    RINPUT_INFO("Input: the controller in host slot {} is not given to the game as player {}: "
                "renut_disable_extra_controllers is on",
                pad, user + 1);
    break;
  default:
    break;
  }
}

// Logs the first ignored press of a controller that is no player.
void LogIgnored(uint32_t pad) {
  if (pad >= kUserCount || (g_logged_ignored & (1u << pad)))
    return;
  g_logged_ignored |= 1u << pad;
  RINPUT_INFO("Input: presses of the controller in host slot {} are ignored: it is no player "
              "(player 1 is host slot {}; renut_disable_extra_controllers {})",
              pad, g_player_pad[0], REXCVAR_GET(renut_disable_extra_controllers) ? "on" : "off");
}

// A successful keystroke from host slot 'pad': an A or START press there may
// claim player 1. Then the player it is from, or kUserCount for none; until
// player 1 is claimed, every controller's keystrokes are player 1's.
uint32_t KeystrokePlayer(const uint8_t *keystroke, uint32_t pad) {
  const uint16_t key = Load16(keystroke);
  if ((key == kVirtualKeyPadA || key == kVirtualKeyPadStart) &&
      (Load16(keystroke + kKeystrokeFlagsOffset) & kKeystrokeKeyDown)) {
    ClaimPlayer1(pad, "keystroke");
  }
  if (g_claimed_pad == kNoPad && REXCVAR_GET(renut_disable_extra_controllers))
    return 0;
  const uint32_t player = PadPlayer(pad);
  if (player >= kUserCount)
    LogIgnored(pad);
  return player;
}

} // namespace

uint16_t renut::controllers::Player1Buttons() {
  return g_player1_buttons.load(std::memory_order_relaxed);
}

// DWORD XInputGetState(DWORD user, XINPUT_STATE *state)
REX_HOOK_RAW(rex_XInputGetState) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  if (user >= kUserCount || !state) {
    __imp__rex_XInputGetState(ctx, base);
    return;
  }
  if (user == 0) {
    // Probe every host slot into the caller's state, keeping each connected
    // controller's; player 1's goes back last.
    uint8_t states[kUserCount][kStateSize];
    uint32_t connected = 0;
    for (uint32_t pad = 0; pad < kUserCount; ++pad) {
      ctx.r3.u64 = pad;
      ctx.r4.u64 = state;
      __imp__rex_XInputGetState(ctx, base);
      if (ctx.r3.u32 != kErrorSuccess)
        continue;
      std::memcpy(states[pad], base + state, kStateSize);
      connected |= 1u << pad;
    }
    LogFocus();
    LogConnected(connected);
    AssignPlayers(connected);
    for (uint32_t pad = 0; pad < kUserCount && g_claimed_pad == kNoPad; ++pad) {
      if ((connected & (1u << pad)) && (Load16(states[pad] + kStateButtonsOffset) & kClaimButtons))
        ClaimPlayer1(pad, "state");
    }
    for (uint32_t player = 0; player < kUserCount; ++player)
      LogPlayer(player);
    if (g_player_pad[0] == kNoPad) {
      g_player1_buttons = 0;
      ctx.r3.u64 = kErrorDeviceNotConnected;
      return;
    }
    LogPress(states[g_player_pad[0]], g_player_pad[0]);
    g_player1_buttons = Load16(states[g_player_pad[0]] + kStateButtonsOffset);
    std::memcpy(base + state, states[g_player_pad[0]], kStateSize);
    ctx.r3.u64 = kErrorSuccess;
    return;
  }
  const int pad = PlayerPad(user);
  if (pad == kNoPad) {
    ctx.r3.u64 = kErrorDeviceNotConnected;
    return;
  }
  ctx.r3.u64 = uint32_t(pad);
  __imp__rex_XInputGetState(ctx, base);
}

// DWORD XInputGetKeystroke(DWORD user, DWORD flags, XINPUT_KEYSTROKE *keystroke)
REX_HOOK_RAW(rex_XInputGetKeystroke) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  const uint32_t keystroke = ctx.r5.u32;
  if (!keystroke || (user >= kUserCount && user != kUserIndexAny)) {
    __imp__rex_XInputGetKeystroke(ctx, base);
    return;
  }
  const bool any_user = user == kUserIndexAny || (flags & kFlagAnyUser);
  // The bound keys' keystrokes, player 1's.
  uint16_t key_virtual_key = 0, key_flags = 0;
  if ((any_user || user == 0) && renut::game::NextKeystroke(key_virtual_key, key_flags)) {
    uint8_t *out = base + keystroke;
    std::memset(out, 0, kKeystrokeSize);
    Store16(out, key_virtual_key);
    Store16(out + kKeystrokeFlagsOffset, key_flags);
    ctx.r3.u64 = kErrorSuccess;
    return;
  }
  // The next keystroke of host slot 'pad' (the SDK reads slot 0 alone for any
  // user, so never asked for one).
  auto next = [&](uint32_t pad) {
    ctx.r3.u64 = pad;
    ctx.r4.u64 = flags & ~kFlagAnyUser;
    ctx.r5.u64 = keystroke;
    __imp__rex_XInputGetKeystroke(ctx, base);
    return ctx.r3.u32;
  };
  // Any player's keystroke, or before player 1 is claimed player 1's: the next
  // from every host slot in turn (an A or START press claims), dropping those
  // of controllers that are no player, or not this one.
  if (any_user || (user == 0 && g_claimed_pad == kNoPad)) {
    uint32_t result = kErrorDeviceNotConnected;
    for (uint32_t pad = 0; pad < kUserCount; ++pad) {
      for (uint32_t skipped = 0; skipped <= kMaxSkippedKeystrokes; ++skipped) {
        const uint32_t status = next(pad);
        if (status != kErrorSuccess) {
          if (status == kErrorEmpty)
            result = kErrorEmpty;
          break;
        }
        result = kErrorEmpty;
        const uint32_t player = KeystrokePlayer(base + keystroke, pad);
        if (player < kUserCount && (any_user || player == user)) {
          base[keystroke + kKeystrokeUserIndexOffset] = uint8_t(player);
          ctx.r3.u64 = kErrorSuccess;
          return;
        }
      }
    }
    ctx.r3.u64 = result;
    return;
  }
  const int pad = PlayerPad(user);
  if (pad == kNoPad) {
    ctx.r3.u64 = kErrorDeviceNotConnected;
    return;
  }
  if (next(uint32_t(pad)) == kErrorSuccess)
    base[keystroke + kKeystrokeUserIndexOffset] = uint8_t(user);
}

// DWORD XInputSetState(DWORD user, XINPUT_VIBRATION *vibration)
REX_HOOK_RAW(rex_XInputSetState) {
  const uint32_t user = ctx.r3.u32;
  if (user >= kUserCount) {
    __imp__rex_XInputSetState(ctx, base);
    return;
  }
  const int pad = PlayerPad(user);
  if (pad == kNoPad) {
    ctx.r3.u64 = kErrorDeviceNotConnected;
    return;
  }
  ctx.r3.u64 = uint32_t(pad);
  __imp__rex_XInputSetState(ctx, base);
}

// The game's player for a user (r3), made when its profile signs in, and made
// anew by the profile refresh (sub_82209B88) when it takes the profile for
// another (its gamertag changed): the old player and its controller are
// dropped then.
REX_HOOK_RAW(sub_826EC1E8) {
  static uint32_t made[kUserCount] = {};
  const uint32_t user = ctx.r3.u32;
  __imp__sub_826EC1E8(ctx, base);
  if (user >= kUserCount)
    return;
  if (made[user]++ == 0)
    RINPUT_INFO("Input: the game made its player {} (profile signed in)", user + 1);
  else
    RINPUT_WARN("Input: the game made player {} anew (its profile changed, or its gamertag "
                "did): the old player was dropped with its controller, and the new one may need "
                "START pressed",
                user + 1);
}
