#include <rex/ppc/hook.h>
#include <rex/setting.h>
#include <rex/types.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <rexglue_guide/guide.h>
#include "renut_engine/app/renut_settings.h"
#include "renut_engine/game/controllers.h"
#include "renut_engine/game/keyboard_input.h"

namespace {

struct ControllerSettings final : rex::SettingOwner {
  ControllerSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
  REX_SETTING(bool, renut_disable_extra_controllers, true, "Only player 1 (the first controller to press A) plays; the other controllers " "are not connected. Off: they are players 2-4, and the game asks them to sign " "in (only player 1 has a profile).");
};
ControllerSettings &g_controllers = *new ControllerSettings();  // never freed, registered by RenutApp::OnConfigure

}

REX_EXTERN(__imp__rex_XInputGetState);
REX_EXTERN(__imp__rex_XInputGetKeystroke);
REX_EXTERN(__imp__rex_XInputSetState);

namespace {

constexpr uint32_t kUserCount = 4;
constexpr uint32_t kUserIndexAny = 0xFF;
constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorDeviceNotConnected = 1167;
constexpr uint32_t kErrorEmpty = 4306;
constexpr uint32_t kStateSize = 16;
constexpr uint32_t kStateButtonsOffset = 4;
constexpr uint16_t kClaimButtons = 0x1000 | 0x0010;
constexpr uint16_t kGuideButton = 0x0400;
constexpr uint32_t kKeystrokeSize = 8;
constexpr uint32_t kKeystrokeFlagsOffset = 4;
constexpr uint32_t kKeystrokeUserIndexOffset = 6;
constexpr uint16_t kVirtualKeyPadA = 0x5800;
constexpr uint16_t kVirtualKeyPadStart = 0x5814;
constexpr uint16_t kKeystrokeKeyDown = 1;
constexpr uint32_t kFlagAnyUser = 0x40000000;
constexpr uint32_t kMaxSkippedKeystrokes = 16;
constexpr int kNoPad = -1;

int g_claimed_pad = kNoPad;
int g_player_pad[kUserCount] = {kNoPad, kNoPad, kNoPad, kNoPad};

uint16_t Load16(const uint8_t *p) {
  return rex::byte_swap(*reinterpret_cast<const uint16_t *>(p));
}

void Store16(uint8_t *p, uint16_t value) {
  value = rex::byte_swap(value);
  std::memcpy(p, &value, 2);
}

void AssignPlayers(uint32_t connected) {
  if (g_claimed_pad != kNoPad && !(connected & (1u << g_claimed_pad)))
    g_claimed_pad = kNoPad;
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

void ClaimPlayer1(uint32_t pad) {
  if (g_claimed_pad != kNoPad || pad >= kUserCount)
    return;
  g_claimed_pad = int(pad);
  uint32_t connected = 1u << pad;
  for (int other : g_player_pad) {
    if (other != kNoPad)
      connected |= 1u << other;
  }
  AssignPlayers(connected);
}

bool PlayerEnabled(uint32_t user) {
  return user == 0 || !g_controllers.renut_disable_extra_controllers.Get() ||rexglue_guide::SignedIn(int(user));
}

int PlayerPad(uint32_t user) {
  return user < kUserCount && PlayerEnabled(user) ? g_player_pad[user] : kNoPad;
}

uint32_t PadPlayer(uint32_t pad) {
  for (uint32_t user = 0; user < kUserCount; ++user) {
    if (PlayerPad(user) == int(pad))
      return user;
  }
  return kUserCount;
}

uint32_t KeystrokePlayer(const uint8_t *keystroke, uint32_t pad) {
  const uint16_t key = Load16(keystroke);
  if ((key == kVirtualKeyPadA || key == kVirtualKeyPadStart) &&
      (Load16(keystroke + kKeystrokeFlagsOffset) & kKeystrokeKeyDown)) {
    ClaimPlayer1(pad);
  }
  if (g_claimed_pad == kNoPad && g_controllers.renut_disable_extra_controllers.Get())
    return 0;
  return PadPlayer(pad);
}

void GetState(PPCContext &ctx, uint8_t *base);

}

REX_HOOK_RAW(rex_XInputGetState) {
  const uint32_t state = ctx.r4.u32;
  GetState(ctx, base);
  if (ctx.r3.u32 != kErrorSuccess || !state)
    return;
  if (rexglue_guide::IsOpen()) {
    std::memset(base + state + kStateButtonsOffset, 0, kStateSize - kStateButtonsOffset);
    return;
  }
  uint8_t *buttons = base + state + kStateButtonsOffset;
  Store16(buttons, Load16(buttons) & ~kGuideButton);
}

namespace {

void GetState(PPCContext &ctx, uint8_t *base) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  if (user >= kUserCount || !state) {
    __imp__rex_XInputGetState(ctx, base);
    return;
  }

  if (user == 0) {
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

    AssignPlayers(connected);
    for (uint32_t pad = 0; pad < kUserCount && g_claimed_pad == kNoPad; ++pad) {
      if ((connected & (1u << pad)) && (Load16(states[pad] + kStateButtonsOffset) & kClaimButtons))
        ClaimPlayer1(pad);
    }

    if (g_player_pad[0] == kNoPad) {
      ctx.r3.u64 = kErrorDeviceNotConnected;
      return;
    }

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

void GetKeystroke(PPCContext &ctx, uint8_t *base);

}

REX_HOOK_RAW(rex_XInputGetKeystroke) {
  if (!rexglue_guide::IsOpen()) {
    GetKeystroke(ctx, base);
    return;
  }
  const uint32_t user = ctx.r3.u32, flags = ctx.r4.u32, keystroke = ctx.r5.u32;
  constexpr int kMaxDrained = 64;
  for (int i = 0; i < kMaxDrained; ++i) {
    ctx.r3.u64 = user;
    ctx.r4.u64 = flags;
    ctx.r5.u64 = keystroke;
    GetKeystroke(ctx, base);
    if (ctx.r3.u32 != kErrorSuccess)
      break;
  }
  if (keystroke)
    std::memset(base + keystroke, 0, kKeystrokeSize);
  ctx.r3.u64 = kErrorEmpty;
}

namespace {

void GetKeystroke(PPCContext &ctx, uint8_t *base) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  const uint32_t keystroke = ctx.r5.u32;
  if (!keystroke || (user >= kUserCount && user != kUserIndexAny)) {
    __imp__rex_XInputGetKeystroke(ctx, base);
    return;
  }
  const bool any_user = user == kUserIndexAny || (flags & kFlagAnyUser);
  uint16_t key_virtual_key = 0, key_flags = 0;
  if ((any_user || user == 0) && renut::game::NextKeystroke(key_virtual_key, key_flags)) {
    uint8_t *out = base + keystroke;
    std::memset(out, 0, kKeystrokeSize);
    Store16(out, key_virtual_key);
    Store16(out + kKeystrokeFlagsOffset, key_flags);
    ctx.r3.u64 = kErrorSuccess;
    return;
  }
  auto next = [&](uint32_t pad) {
    ctx.r3.u64 = pad;
    ctx.r4.u64 = flags & ~kFlagAnyUser;
    ctx.r5.u64 = keystroke;
    __imp__rex_XInputGetKeystroke(ctx, base);
    return ctx.r3.u32;
  };

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

}

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

int renut::game::ControllerPlayer(int pad) {
  for (uint32_t user = 0; user < kUserCount; ++user) {
    if (g_player_pad[user] == pad)
      return int(user);
  }
  return -1;
}
