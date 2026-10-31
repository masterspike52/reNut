/**
 * @file    game/startup_logo.cpp
 * @brief   A or Start skips the Microsoft Game Studios logo at startup.
 *
 * N&B's startup movies (IDA, TU3): sub_82283E78 loads
 * aid_xmv_banjox_attractsequence_mgslogo and aid_xmv_banjox_bootupsequence_1
 * into memory (meLoadMovie, 0x82283F78; the logo's data at dword_82E18104) and
 * starts the movie thread (sub_82284080). For each movie the thread makes a
 * movie object (dword_82E180F4 points at it meanwhile), opens a player from the
 * data into the object + 24 (XMediaCreateXmvPlayerFromMemory) and renders
 * frames (IXMediaXmvPlayer_RenderNextFrame) until the movie ends or the
 * object's + 12 is set, then releases the player.
 *
 * The boot-up sequence sets + 12 from its frame callback (sub_82284378: A or
 * Start on any port), but only once loading is done (dword_82E180F8, signalled
 * by sub_82283640 during the boot-up sequence; the main thread waits for the
 * logo to end before it loads on, at appMainBoot's wait on dword_82E18100). The
 * logo has no callback, so the game plays it to its end.
 *
 * Here a press of A or Start during the logo (any controller's, or the bound
 * keys' with renut_keyboard) sets the logo's + 12, and the thread goes on as
 * when the logo ends: the boot-up sequence next.
 */
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>

#include <atomic>
#include <cstdint>

#include <rexglue_guide/guide.h>

#include "renut_engine/game/guest_call.h"
#include "renut_engine/game/keyboard_input.h"

REX_EXTERN(__imp__rex_XMediaCreateXmvPlayerFromMemory);
REX_EXTERN(__imp__rex_IXMediaXmvPlayer_RenderNextFrame);
REX_EXTERN(__imp__rex_IXMediaXmvPlayer_Release);
REX_IMPORT(__imp__rex_XInputGetState, GuestXInputGetState, u32(u32, u32));

namespace {

using renut::guest::Load16;
using renut::guest::Load32;
using renut::guest::Store32;

const rex::LogCategoryId kLogoLog = rex::RegisterLogCategory("renut_startup");

// Guest data.
constexpr uint32_t kLogoData = 0x82E18104;     // the logo's movie data (meLoadMovie)
constexpr uint32_t kStartupMovie = 0x82E180F4;  // the movie thread's movie object
constexpr uint32_t kMovieStop = 12;             // movie object + 12: stop rendering
constexpr uint32_t kMoviePlayer = 24;           // movie object + 24: its player

constexpr uint32_t kPorts = 4;
constexpr uint32_t kErrorSuccess = 0;
// XINPUT_STATE: packet number, then XINPUT_GAMEPAD (wButtons first).
constexpr uint32_t kStateSize = 16;
constexpr uint32_t kStateButtonsOffset = 4;
// XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START, as the boot-up sequence's skip.
constexpr uint32_t kSkipButtons = 0x1000 | 0x0010;

// The logo's player, from its creation until it is skipped or released; 0
// otherwise.
std::atomic<uint32_t> g_logo_player{0};
// The skip buttons held at the logo's last frame: a press is one that was not
// held then (one held since before the logo skips nothing).
uint32_t g_held = 0;
bool g_held_known = false;

// A or Start on any controller, or on the bound keys.
uint32_t SkipButtonsHeld(PPCContext &ctx, uint8_t *base) {
  uint32_t buttons = renut::game::KeyboardButtons();
  renut::guest::Guest g(ctx, base);
  const uint32_t state = g.Alloc(kStateSize);
  for (uint32_t port = 0; port < kPorts; ++port) {
    if (GuestXInputGetState(g.frame(), base, port, state) == kErrorSuccess)
      buttons |= Load16(base, state + kStateButtonsOffset);
  }
  return buttons & kSkipButtons;
}

} // namespace

// HRESULT XMediaCreateXmvPlayerFromMemory(context, data, size, IXMediaXmvPlayer **player)
// The player made from the logo's data is the logo's.
REX_HOOK_RAW(rex_XMediaCreateXmvPlayerFromMemory) {
  const uint32_t data = ctx.r4.u32;
  const uint32_t out = ctx.r6.u32;
  __imp__rex_XMediaCreateXmvPlayerFromMemory(ctx, base);
  if (int32_t(ctx.r3.u32) < 0 || !out || !data || data != Load32(base, kLogoData))
    return;
  g_logo_player.store(Load32(base, out), std::memory_order_relaxed);
  g_held_known = false;
}

// HRESULT IXMediaXmvPlayer_RenderNextFrame(player, flags, ...), on the movie
// thread once per frame: a skip press during the logo stops it after this
// frame (the thread checks the stop after each one).
REX_HOOK_RAW(rex_IXMediaXmvPlayer_RenderNextFrame) {
  const uint32_t player = ctx.r3.u32;
  const uint32_t logo = g_logo_player.load(std::memory_order_relaxed);
  if (logo && player == logo) {
    const uint32_t movie = Load32(base, kStartupMovie);
    if (!movie || Load32(base, movie + kMoviePlayer) != player) {
      // Not the startup thread's logo any more.
      g_logo_player.store(0, std::memory_order_relaxed);
    } else if (!rexglue_guide::IsOpen()) {
      const uint32_t held = SkipButtonsHeld(ctx, base);
      if (g_held_known && (held & ~g_held)) {
        Store32(base, movie + kMovieStop, 1);
        g_logo_player.store(0, std::memory_order_relaxed);
        REXLOG_CAT_INFO(kLogoLog, "Startup: the Microsoft Game Studios logo skipped");
      }
      g_held = held;
      g_held_known = true;
    }
  }
  __imp__rex_IXMediaXmvPlayer_RenderNextFrame(ctx, base);
}

// ULONG IXMediaXmvPlayer_Release(player): the logo's player, released when the
// logo ends unskipped, is no longer the logo's (the boot-up sequence's player
// may take its address).
REX_HOOK_RAW(rex_IXMediaXmvPlayer_Release) {
  uint32_t logo = ctx.r3.u32;
  g_logo_player.compare_exchange_strong(logo, 0, std::memory_order_relaxed);
  __imp__rex_IXMediaXmvPlayer_Release(ctx, base);
}
