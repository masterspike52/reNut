/**
 * @file    game/challenge_replay.cpp
 * @brief   REPLAY CHALLENGE on a challenge's results: the challenge again,
 *          without going back to its giver.
 *
 * N&B (IDA, TU3). The results are the ChallengeCountdown_c scene
 * (XuiScene_BanjoX_Ingame_ChallengeCountdown, scene 113; vtable 0x82159308;
 * aid_xuiscene_banjox_ingame_321go.xur in aid_xuipackage_banjox_challenge) in
 * its results modes, 4 and 5 (+396; sub_825A8AF0 sets the mode). Its press
 * handler (sub_825A6DB0) takes continueButton (A), saveReplayButton (B),
 * deviceSelectButton (RB) and toggleResultsButton (X); it also has a whole
 * view replay button on Y (viewReplayIcon +304, file://aid_texture_banjox_ui_
 * buttons_y; viewReplayButton +308; viewReplayText +312) the results never
 * use: sub_825AA5B0, which lays out the save replay and save location
 * buttons, always hides it. Here it is shown after those, as REPLAY
 * CHALLENGE, and its press does what the pause menu's restart does
 * (sub_8258E498), then closes the results (mode 0, as Continue ends).
 *
 * The restart (challenge manager, the world's + 5552 + 7008): + 16 (the next
 * challenge) = + 12 (this one), + 652 (restart) = 1, + 764 = 3.5 s of fade,
 * state (+ 20) 20 and sub_82528500 (the session's challenge HUD, for online
 * play). The manager's tick (challengeManTick, sub_821BD590) fades out at 20,
 * tears the challenge down at 21 and, with + 652, begins + 16 again
 * (sub_824FF978) instead of the exit (state 22) that Continue leads to.
 *
 * The exit is also where a challenge's first win spawns its prize (Jiggy or
 * bingo card, + 712: set by the win's results, sub_82508290, when the player
 * doesn't own it yet; spawned and cleared by state 22's sub_82503A28), which
 * a restart would skip: REPLAY CHALLENGE is offered only without one pending.
 */
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <cstdint>
#include <cstring>

#include "renut_engine/app/renut_logging.h"
#include "renut_engine/game/guest_call.h"

// Hooked.
REX_EXTERN(__imp__sub_825AA5B0);  // challenge results: the save replay / save location buttons
REX_EXTERN(__imp__sub_825A6DB0);  // challenge results: a button pressed
REX_EXTERN(__imp__sub_825A75C8);  // challenge results: update (scene, double seconds)

// Called.
REX_IMPORT(__imp__sub_824FC1B8, ElementShow, u32(u32, u32));
REX_IMPORT(__imp__sub_823B26F0, ElementEnable, u32(u32, u32));
REX_IMPORT(__imp__sub_82A25780, XuiTextElementSetText, u32(u32, u32));
REX_IMPORT(__imp__sub_825A8AF0, ChallengeResultsSetMode, u32(u32, u32));  // (scene, mode); 0 closes
REX_IMPORT(__imp__sub_82528500, ChallengeRestartHud, u32(u32));           // (session); online only

namespace {

using renut::guest::Load32;
using renut::guest::Store32;

// Guest data.
constexpr uint32_t kWorld = 0x82E19AB4;   // the running world (dword_82E19AB4)
constexpr uint32_t kOnline = 0x82E19934;  // byte: an online session
constexpr uint32_t kWorldScene = 5552;    // world + 5552: its scene
constexpr uint32_t kSceneChallengeMan = 7008;
constexpr uint32_t kSceneSession = 464;   // scene + 464: the session sub_82528500 takes

// The challenge manager.
constexpr uint32_t kManChallenge = 12;      // the running challenge's asset id
constexpr uint32_t kManNext = 16;           // the one to begin after this
constexpr uint32_t kManState = 20;
constexpr uint32_t kManRestart = 652;       // begin + 16 again at the end (state 21)
constexpr uint32_t kManPrizePending = 712;  // a first win's prize spawns at the exit
constexpr uint32_t kManFade = 764;          // float: the restart's fade out, seconds
constexpr uint32_t kStateRestart = 20;
constexpr float kRestartFade = 3.5f;        // as the pause menu's restart

// The results scene (ChallengeCountdown_c).
constexpr uint32_t kViewReplayIcon = 304;
constexpr uint32_t kViewReplayButton = 308;
constexpr uint32_t kViewReplayText = 312;
constexpr uint32_t kMode = 396;           // 4, 5: the results
constexpr uint32_t kSubPage = 413;        // byte: a leaderboard page is up
constexpr uint32_t kPage = 448;           // 0: the results; 1-3 the leaderboards
constexpr uint32_t kRevealing = 552;      // the results still being revealed
constexpr uint32_t kSavingReplay = 556;   // a replay being saved
constexpr uint32_t kOpening = 568;        // the results still opening
constexpr uint32_t kModeResults = 4;
constexpr uint32_t kModeResultsNoReplay = 5;
constexpr uint32_t kModeClosed = 0;

constexpr char kLabel[] = "REPLAY CHALLENGE";

uint8_t Load8(const uint8_t *base, uint32_t va) {
  return base[va];
}

void StoreFloat(uint8_t *base, uint32_t va, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  Store32(base, va, bits);
}

// The scene's challenge manager, or 0.
uint32_t ChallengeMan(const uint8_t *base) {
  const uint32_t world = Load32(base, kWorld);
  if (!world)
    return 0;
  const uint32_t scene = Load32(base, world + kWorldScene);
  return scene ? scene + kSceneChallengeMan : 0;
}

// REPLAY CHALLENGE is offered on the results' own page, once they are all
// shown, offline, with a challenge to restart and no prize waiting to spawn.
bool Offered(const uint8_t *base, uint32_t results) {
  const uint32_t mode = Load32(base, results + kMode);
  if (mode != kModeResults && mode != kModeResultsNoReplay)
    return false;
  if (Load32(base, results + kPage) || Load8(base, results + kSubPage) || Load32(base, results + kRevealing) ||
      Load32(base, results + kSavingReplay) || Load32(base, results + kOpening))
    return false;
  if (Load8(base, kOnline))
    return false;
  const uint32_t man = ChallengeMan(base);
  return man && Load32(base, man + kManChallenge) && !Load32(base, man + kManPrizePending);
}

// The label, as a wide (UTF-16, big-endian) string in guest memory, made once.
uint32_t Label(uint8_t *base) {
  static uint32_t va = 0;
  if (va)
    return va;
  va = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(sizeof(kLabel) * 2));
  if (!va)
    return 0;
  for (size_t i = 0; i < sizeof(kLabel); ++i)
    renut::guest::Store16(base, uint32_t(va + 2 * i), uint8_t(kLabel[i]));
  return va;
}

// The results scene REPLAY CHALLENGE is shown on now, or 0. The results lay
// their buttons out (sub_825AA5B0) while they are still revealed, when it
// can't be offered yet, and not again when the reveal ends: the update shows
// it once it can be, and again after the game hides it (another page, a
// replay saving).
uint32_t g_shown_on = 0;

void Show(PPCContext &ctx, uint8_t *base, uint32_t results) {
  renut::guest::Guest g(ctx, base);
  auto &f = g.frame();
  ElementEnable(f, base, Load32(base, results + kViewReplayButton), 1);
  ElementShow(f, base, Load32(base, results + kViewReplayIcon), 1);
  ElementShow(f, base, Load32(base, results + kViewReplayText), 1);
  if (const uint32_t label = Label(base))
    XuiTextElementSetText(f, base, Load32(base, results + kViewReplayText), label);
  g_shown_on = results;
}

// The pause menu's restart (sub_8258E498), from the results.
void Restart(PPCContext &ctx, uint8_t *base, uint32_t results) {
  const uint32_t man = ChallengeMan(base);
  if (!man)
    return;
  StoreFloat(base, man + kManFade, kRestartFade);
  Store32(base, man + kManRestart, 1);
  Store32(base, man + kManNext, Load32(base, man + kManChallenge));
  renut::guest::Guest g(ctx, base);
  if (Load32(base, man + kManState) != kStateRestart) {
    Store32(base, man + kManState, kStateRestart);
    const uint32_t scene = man - kSceneChallengeMan;
    ChallengeRestartHud(g.frame(), base, scene + kSceneSession);
  }
  ChallengeResultsSetMode(g.frame(), base, results, kModeClosed);
  RNUT_INFO("Challenge {:08X}: replayed from its results", Load32(base, man + kManChallenge));
}

}  // namespace

// void sub_825AA5B0(results, bool show, bool remember): shows or hides the save
// replay and save location buttons, and always hides view replay. From the
// results' layout (remember 0) REPLAY CHALLENGE is shown after it when it can
// be; while a replay saves (remember 1) it stays hidden with the others. Either
// way the update shows it again once it can be.
REX_HOOK_RAW(sub_825AA5B0) {
  const uint32_t results = ctx.r3.u32;
  const bool layout = ctx.r5.u32 == 0;
  __imp__sub_825AA5B0(ctx, base);
  if (g_shown_on == results)
    g_shown_on = 0;
  if (layout && Offered(base, results))
    Show(ctx, base, results);
}

// void sub_825A75C8(results, double seconds): the results' update, which ends
// their reveal. REPLAY CHALLENGE appears as soon as it can be offered.
REX_HOOK_RAW(sub_825A75C8) {
  const uint32_t results = ctx.r3.u32;
  __imp__sub_825A75C8(ctx, base);
  if (!Offered(base, results)) {
    if (g_shown_on == results)
      g_shown_on = 0;
    return;
  }
  if (g_shown_on != results)
    Show(ctx, base, results);
}

// bool sub_825A6DB0(results, pressed button handle, ...): the results' buttons.
// The view replay button, while offered, replays the challenge.
REX_HOOK_RAW(sub_825A6DB0) {
  const uint32_t results = ctx.r3.u32;
  const uint32_t pressed = ctx.r4.u32;
  if (pressed && pressed == Load32(base, results + kViewReplayButton) && Offered(base, results)) {
    Restart(ctx, base, results);
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_825A6DB0(ctx, base);
}
