#include <rex/kernel/heap.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <cstdint>
#include <cstring>
#include "renut_engine/game/guest_call.h"


REX_EXTERN(__imp__sub_825AA5B0);
REX_EXTERN(__imp__sub_825A6DB0);
REX_EXTERN(__imp__sub_825A75C8); 

// Called.
REX_IMPORT(__imp__sub_824FC1B8, ElementShow, u32(u32, u32));
REX_IMPORT(__imp__sub_823B26F0, ElementEnable, u32(u32, u32));
REX_IMPORT(__imp__sub_82A25780, XuiTextElementSetText, u32(u32, u32));
REX_IMPORT(__imp__sub_825A8AF0, ChallengeResultsSetMode, u32(u32, u32));
REX_IMPORT(__imp__sub_82528500, ChallengeRestartHud, u32(u32));

namespace {

using renut::guest::Load32;
using renut::guest::Store32;


constexpr uint32_t kWorld = 0x82E19AB4;
constexpr uint32_t kOnline = 0x82E19934;
constexpr uint32_t kWorldScene = 5552;
constexpr uint32_t kSceneChallengeMan = 7008;
constexpr uint32_t kSceneSession = 464;
constexpr uint32_t kManChallenge = 12;
constexpr uint32_t kManNext = 16;
constexpr uint32_t kManState = 20;
constexpr uint32_t kManRestart = 652;
constexpr uint32_t kManPrizePending = 712;
constexpr uint32_t kManFade = 764;
constexpr uint32_t kStateRestart = 20;
constexpr float kRestartFade = 3.5f;
constexpr uint32_t kViewReplayIcon = 304;
constexpr uint32_t kViewReplayButton = 308;
constexpr uint32_t kViewReplayText = 312;
constexpr uint32_t kMode = 396;
constexpr uint32_t kSubPage = 413;
constexpr uint32_t kPage = 448;
constexpr uint32_t kRevealing = 552;
constexpr uint32_t kSavingReplay = 556;
constexpr uint32_t kOpening = 568;
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


uint32_t ChallengeMan(const uint8_t *base) {
  const uint32_t world = Load32(base, kWorld);
  if (!world)
    return 0;
  const uint32_t scene = Load32(base, world + kWorldScene);
  return scene ? scene + kSceneChallengeMan : 0;
}

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

uint32_t Label(uint8_t *base) {
  static uint32_t va = 0;
  if (va)
    return va;
  va = REX_RUNTIME()->GetGuestHeap().SystemHeapAlloc(uint32_t(sizeof(kLabel) * 2));
  if (!va)
    return 0;
  for (size_t i = 0; i < sizeof(kLabel); ++i)
    renut::guest::Store16(base, uint32_t(va + 2 * i), uint8_t(kLabel[i]));
  return va;
}

uint32_t g_shown_on = 0;

void Show(PPCContext &ctx, uint8_t *base, uint32_t results) {
  renut::guest::Guest g(ctx, base);
  auto &f = g.ctx();
  ElementEnable(f, base, Load32(base, results + kViewReplayButton), 1);
  ElementShow(f, base, Load32(base, results + kViewReplayIcon), 1);
  ElementShow(f, base, Load32(base, results + kViewReplayText), 1);
  if (const uint32_t label = Label(base))
    XuiTextElementSetText(f, base, Load32(base, results + kViewReplayText), label);
  g_shown_on = results;
}

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
    ChallengeRestartHud(g.ctx(), base, scene + kSceneSession);
  }
  ChallengeResultsSetMode(g.ctx(), base, results, kModeClosed);
}

}  

REX_HOOK_RAW(sub_825AA5B0) {
  const uint32_t results = ctx.r3.u32;
  const bool layout = ctx.r5.u32 == 0;
  __imp__sub_825AA5B0(ctx, base);
  if (g_shown_on == results)
    g_shown_on = 0;
  if (layout && Offered(base, results))
    Show(ctx, base, results);
}

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
