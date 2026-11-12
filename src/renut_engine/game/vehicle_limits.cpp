#include <rex/ppc/hook.h>
#include <rex/setting.h>
#include <rex/types.h>

#include <cstdint>

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/game/guest_call.h"
#include "renut_engine/online/online.h"

REX_EXTERN(__imp__sub_8254CEF0);  // spawns a vehicle from its blueprint
REX_EXTERN(__imp__sub_8250FD28);  // a blueprint's per-part flags (parts left out)
REX_EXTERN(__imp__sub_82633BA0);  // whether the world lets the editor's cursor move

namespace {

struct VehicleLimitSettings final : rex::SettingOwner {
  VehicleLimitSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
  REX_SETTING(bool, unlimited_vehicle_building, false,
              "Vehicles in the garage can span 128 cells a side (not 19) and hold 2048 parts "
              "(not 250). Off in online multiplayer unless the host allows cheats.");
};
VehicleLimitSettings &g_limits = *new VehicleLimitSettings();  // never freed, registered by RenutApp::OnConfigure

using renut::guest::Load16;

constexpr int32_t kGameSpan = 19;
constexpr int32_t kMaxSpan = 128;
constexpr uint32_t kMaxParts = 2048;
// The parts the spawn functions' stack arrays hold.
constexpr uint32_t kStackParts = 250;

bool Enabled() {
  return g_limits.unlimited_vehicle_building.Get() &&
         (!renut::online::InSession() || renut::online::SessionAllowsCheats());
}

int32_t CellX(uint32_t cell) {
  return int32_t(cell) >> 21;
}

int32_t CellY(uint32_t cell) {
  return int32_t(cell << 11) >> 22;
}

int32_t CellZ(uint32_t cell) {
  return int32_t(cell << 21) >> 21;
}

uint32_t PackCell(int32_t x, int32_t y, int32_t z) {
  return (uint32_t(x) << 21) | ((uint32_t(y) & 0x3FF) << 11) | (uint32_t(z) & 0x7FF);
}

// Lets a part count past 250 pass the game's check (r11 > 250 fails).
void AllowParts(PPCRegister &count) {
  if (Enabled() && count.u32 <= kMaxParts)
    count.u64 = 0;
}

// Stack room a spawn function needs for a blueprint of 'parts' past its arrays'
// 250: 8 bytes a part (sub_8250FD28's sort, the largest).
uint32_t SpawnRoom(uint32_t parts) {
  return parts <= kStackParts ? 0 : (parts * 8 + 0x10F) & ~0xFu;
}

// After a spawn function ran with room: what spilled over the registers it
// saved is undone by putting the caller's back, keeping its result.
void RestoreCaller(PPCContext &ctx, const PPCContext &caller) {
  const uint64_t result = ctx.r3.u64;
  ctx = caller;
  ctx.r3.u64 = result;
}

}  // namespace

// sub_82633230, before the span test (cells: largest r11, smallest r10): true
// when the span fits kMaxSpan, skipping the game's 19-cell pull-back.
bool VehicleSize_fits(PPCRegister &r10, PPCRegister &r11) {
  if (!Enabled())
    return false;
  const uint32_t high = r11.u32, low = r10.u32;
  return CellX(high) - CellX(low) < kMaxSpan && CellY(high) - CellY(low) < kMaxSpan &&
         CellZ(high) - CellZ(low) < kMaxSpan;
}

// After 0x826334F0: the packed limit (r11 x, y and z; r10 y and z).
void VehicleSize_limit(PPCRegister &r10, PPCRegister &r11) {
  if (!Enabled())
    return;
  r10.u64 = PackCell(0, kMaxSpan, kMaxSpan);
  r11.u64 = PackCell(kMaxSpan, kMaxSpan, kMaxSpan);
}

// After 0x82633510 and 0x82633580: 19 less a cell's z (the packed limit
// covers x and y).
void VehicleSize_limit_z(PPCRegister &r9) {
  if (Enabled())
    r9.u64 = uint32_t(int32_t(r9.u32) + kMaxSpan - kGameSpan);
}

void VehicleSize_limit_z2(PPCRegister &r11) {
  if (Enabled())
    r11.u64 = uint32_t(int32_t(r11.u32) + kMaxSpan - kGameSpan);
}

// sub_825ECBF8 (0x825ECC6C), sub_825ECD28 (0x825ECDD0) and sub_82630628
// (0x826309A8): the part count against 250.
void VehicleParts_join(PPCRegister &r11) {
  AllowParts(r11);
}

void VehicleParts_merge(PPCRegister &r11) {
  AllowParts(r11);
}

void VehicleParts_tip(PPCRegister &r11) {
  AllowParts(r11);
}

// BOOL sub_82633BA0(editor, const u32 *cell before): true lets the cursor's
// move stand.
REX_HOOK_RAW(sub_82633BA0) {
  if (Enabled()) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_82633BA0(ctx, base);
}

// sub_824984D0, after 0x824985E4 loads the blueprint's part count (r10): the
// picture's arrays hold 250.
void BlueprintPicture_parts(PPCRegister &r10) {
  if (r10.u32 > kStackParts)
    r10.u64 = kStackParts;
}

// sub_8254CEF0(spawner): the blueprint at + 0x60, its part count the u16 at + 0.
REX_HOOK_RAW(sub_8254CEF0) {
  const uint32_t blueprint = renut::guest::Load32(base, ctx.r3.u32 + 0x60);
  const uint32_t room = SpawnRoom(blueprint ? Load16(base, blueprint) : 0);
  if (!room) {
    __imp__sub_8254CEF0(ctx, base);
    return;
  }
  const PPCContext caller = ctx;
  ctx.r1.u32 -= room;
  __imp__sub_8254CEF0(ctx, base);
  RestoreCaller(ctx, caller);
}

// sub_8250FD28(challenge, blueprint, flags out).
REX_HOOK_RAW(sub_8250FD28) {
  const uint32_t room = SpawnRoom(ctx.r4.u32 ? Load16(base, ctx.r4.u32) : 0);
  if (!room) {
    __imp__sub_8250FD28(ctx, base);
    return;
  }
  const PPCContext caller = ctx;
  ctx.r1.u32 -= room;
  __imp__sub_8250FD28(ctx, base);
  RestoreCaller(ctx, caller);
}
