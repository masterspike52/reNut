#include <rex/ppc/hook.h>
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
REX_IMPORT(__imp__sub_82283640, FinishIntro, void());                    // Frontend_FinishIntro
REX_IMPORT(__imp__sub_82559BE0, TransitionAnimEvent, u32(u32, u32));    // Transition_OnAnimEvent(event, arg)

namespace {

using renut::guest::Load16;
using renut::guest::Load32;
using renut::guest::Store32;

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

// The jiggy wall transition. Its animation events only fire once the intro has handed over (g_loadingScreenReady).
constexpr uint32_t kLoadingScreenReady = 0x82E18114;
constexpr uint32_t kTransitionActive = 0x82E19EE0;  // > 0 while a transition covers the screen
constexpr uint32_t kTransitionShown = 0x82E19EE4;   // set once its "into" animation has started
constexpr uint32_t kIntoEvents = 0x82E19F10;        // transition_jiggywall_into_events
constexpr uint32_t kEventSizes = 0x821082A8;        // each event type's size, 12 bytes a type
constexpr uint32_t kEventTypes = 0x32;
constexpr uint32_t kEventType = 12;                 // event + 12: its type (u16); + 0: its size (u16)

// Plays the "into" animation's sound events, as AnimEvents_Fire walks them, through the transition's own handler.
void PlayIntoEvents(uint8_t *base) {
  const uint32_t events = Load32(base, kIntoEvents);
  for (uint32_t event = events; event && Load16(base, event + kEventType);) {
    const uint32_t size = Load16(base, event);
    const uint32_t type = Load16(base, event + kEventType);
    if (!size)
      break;
    if (type < kEventTypes && Load32(base, kEventSizes + 12 * type) == size)
      TransitionAnimEvent(event, 0);
    event += size;
  }
}

// A or Start on any controller, or on the bound keys.
uint32_t SkipButtonsHeld(PPCContext &ctx, uint8_t *base) {
  uint32_t buttons = renut::game::KeyboardButtons();
  renut::guest::Guest g(ctx, base);
  const uint32_t state = g.Alloc(kStateSize);
  for (uint32_t port = 0; port < kPorts; ++port) {
    if (GuestXInputGetState(g.ctx(), base, port, state) == kErrorSuccess)
      buttons |= Load16(base, state + kStateButtonsOffset);
  }
  return buttons & kSkipButtons;
}

// void Frontend_FinishIntro(): the boot's first jiggy wall closed during the Rare logo, so its start sound was
// held back; it plays here, as the wall comes into view.
void FinishIntro_hook() {
  uint8_t *base = rex::ppc::GetBoundThread().base;
  const bool was_ready = Load32(base, kLoadingScreenReady) != 0;
  FinishIntro();
  if (!was_ready && Load32(base, kLoadingScreenReady) && int32_t(Load32(base, kTransitionActive)) > 0 &&
      Load32(base, kTransitionShown))
    PlayIntoEvents(base);
}

} // namespace

REX_HOOK(sub_82283640, FinishIntro_hook);

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

// HRESULT IXMediaXmvPlayer_RenderNextFrame(player, flags, ...): a skip press
// during the logo sets its movie object's stop flag (+ 12).
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
      }
      g_held = held;
      g_held_known = true;
    }
  }
  __imp__rex_IXMediaXmvPlayer_RenderNextFrame(ctx, base);
}

// ULONG IXMediaXmvPlayer_Release(player): forgets the logo's player when it is
// released (the next player may reuse its address).
REX_HOOK_RAW(rex_IXMediaXmvPlayer_Release) {
  uint32_t logo = ctx.r3.u32;
  g_logo_player.compare_exchange_strong(logo, 0, std::memory_order_relaxed);
  __imp__rex_IXMediaXmvPlayer_Release(ctx, base);
}
