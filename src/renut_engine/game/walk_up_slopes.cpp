/**
 * @file    game/walk_up_slopes.cpp
 * @brief   On foot, slopes never make Banjo slide back down
 *          (walk_up_slopes).
 *
 * N&B (TU3, IDA): sub_82227F18, an on-foot body's ground update (from
 * sub_82227CC0), decides whether the ground is too steep to stand on: a
 * floor whose normal is less than about 35 degrees from up (dot < 0.82, and
 * past a 25 degree check) or a surface flagged slippery (its material's
 * 0x8000800, or 0x10 with its own angle) adds the frame's time to the body's
 * slide timer (+ 1852); any other ground sets it to 0. sub_823CE4F8 then sets
 * the body's IsSliding flag (body flag 77, + 280 byte 9 bit 0x20) once the
 * timer reaches 0.5 s, and the walk and stand states (sub_821F3118) go to the
 * body's slide state, which carries it down the slope.
 *
 * With the cvar on the update takes the "not steep" path every frame: timer 0,
 * sub_823CE4F8, + 1844 cleared, as the game does on level ground.
 */
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include <cstdint>

#include "renut_engine/game/guest_call.h"
#include "renut_engine/online/online.h"

REXCVAR_DEFINE_BOOL(walk_up_slopes, false, "Nuts&Bolts/Cheats",
                    "On foot, Banjo walks up slopes of any steepness instead of sliding back down. "
                    "Off in online multiplayer unless the host allows cheats.");

REX_EXTERN(__imp__sub_82227F18);
REX_IMPORT(__imp__sub_823CE4F8, UpdateSliding, void(u32));

namespace {

using renut::guest::Load32;
using renut::guest::Store32;

// The on-foot body (sub_82227F18's).
constexpr uint32_t kBodyBusy = 1840;    // nonzero: the update does nothing
constexpr uint32_t kBodyGround = 1784;  // the ground it stands on, 0 for none
constexpr uint32_t kBodySlideSet = 1844;
constexpr uint32_t kBodySlideTime = 1852;  // float seconds on steep ground

bool Enabled() {
  return REXCVAR_GET(walk_up_slopes) &&
         (!renut::online::InSession() || renut::online::SessionAllowsCheats());
}

}  // namespace

// void bodyGroundSlope(body*), once a frame for an on-foot body.
REX_HOOK_RAW(sub_82227F18) {
  if (!Enabled()) {
    __imp__sub_82227F18(ctx, base);
    return;
  }
  const uint32_t body = ctx.r3.u32;
  if (Load32(base, body + kBodyBusy) || !Load32(base, body + kBodyGround))
    return;
  Store32(base, body + kBodySlideTime, 0);  // 0.0f
  renut::guest::Guest g(ctx, base);
  UpdateSliding(g.frame(), base, body);
  Store32(base, body + kBodySlideSet, 0);
}
