#include <rex/ppc/hook.h>
#include <rex/setting.h>
#include <rex/types.h>

#include <cstdint>

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/game/guest_call.h"
#include "renut_engine/online/online.h"

namespace {

struct WalkUpSlopesSettings final : rex::SettingOwner {
  WalkUpSlopesSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
  REX_SETTING(bool, walk_up_slopes, false,
              "On foot, Banjo walks up slopes of any steepness instead of sliding back down. "
              "Off in online multiplayer unless the host allows cheats.");
};
WalkUpSlopesSettings &g_walk_up_slopes = *new WalkUpSlopesSettings();  // never freed, registered by RenutApp::OnConfigure

}  // namespace

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
  return g_walk_up_slopes.walk_up_slopes.Get() &&
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
  UpdateSliding(g.ctx(), base, body);
  Store32(base, body + kBodySlideSet, 0);
}
