/**
 * @file    game/vehicle_limits.cpp
 * @brief   The garage's limits on a vehicle's size and part count
 *          (unlimited_vehicle_building).
 *
 * N&B (TU3, IDA):
 * - Size. The vehicle editor (entitySceneControlEditVehicle's editor object,
 *   the cursor's cell at + 0x1A8; cells pack x, y and z in 11, 10 and 11
 *   signed bits) keeps its cursor in sub_82633230: the vehicle's cells (from 0)
 *   together with the held part's must span fewer than 19 on each axis
 *   (0x826333EC, 0x82633408, 0x82633424), or the cursor is pulled back until
 *   they span 19 (0x826334E4 and 0x826334F0 load the packed limit,
 *   0x82633510 and 0x82633580 subtract from it) and the tip "can't move the
 *   cursor any further" (29) shows. A saved blueprint keeps each block's cell
 *   in signed bytes (sub_8254E518), so a vehicle can span 128 cells and still
 *   save. A cursor move also stands only if the world lets it
 *   (sub_82633BA0, from sub_82632E48: the held part's shape cast through the
 *   world, sub_825EC8C8, or with none a line along the vehicle, sub_825EB270),
 *   so in a garage of walls (Mumbo's) the room ends the range first.
 * - Parts. Joining a vehicle to another (sub_825ECBF8: placing a part, and the
 *   editor's check of the held part every frame) and merging two
 *   (sub_825ECD28) fail when both vehicles' parts (+ 0xDA0) and the ones
 *   carried come to more than 250; picking a part up shows the tip "reached the
 *   maximum" (34) past 250 (sub_82630628). Spawning a blueprint (sub_8254CEF0)
 *   keeps a flag per part on its stack, room for 250, filled by sub_8250FD28,
 *   whose own stack arrays (per-part weights, a sort of them) hold about as
 *   many: past 250 parts both write over the registers they saved, and past
 *   their frames. The blueprint's picture (sub_824984D0, drawn by
 *   sub_82497588) holds three asset references a part in arrays of 250 inside
 *   its object (+ 0x84, + 0x46C, + 0x854): past 250 parts they run into each
 *   other and past the object, and the picture's parts list breaks.
 *
 * With the cvar on, a vehicle may span 128 cells and hold 2048 parts, and the
 * cursor goes through the garage's walls (parts still can't overlap the
 * vehicle: sub_825ECBF8 checks that every frame). The two
 * spawn functions run with room under their caller's stack for what spills out
 * of their arrays, and the caller's registers are put back afterwards, and the
 * picture draws a blueprint's first 250 parts, for any blueprint of more than
 * 250 parts whatever the cvar: a big vehicle saved with it on still loads with
 * it off.
 */
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include <cstdint>

#include "renut_engine/game/guest_call.h"
#include "renut_engine/online/online.h"

REXCVAR_DEFINE_BOOL(unlimited_vehicle_building, false, "Nuts&Bolts/Cheats",
                    "Vehicles in the garage can span 128 cells a side (not 19) and hold 2048 parts "
                    "(not 250). Off in online multiplayer unless the host allows cheats.");

REX_EXTERN(__imp__sub_8254CEF0);  // spawns a vehicle from its blueprint
REX_EXTERN(__imp__sub_8250FD28);  // a blueprint's per-part flags (parts left out)
REX_EXTERN(__imp__sub_82633BA0);  // whether the world lets the editor's cursor move

namespace {

using renut::guest::Load16;

constexpr int32_t kGameSpan = 19;
constexpr int32_t kMaxSpan = 128;
constexpr uint32_t kMaxParts = 2048;
// The parts the spawn functions' stack arrays hold.
constexpr uint32_t kStackParts = 250;

bool Enabled() {
  return REXCVAR_GET(unlimited_vehicle_building) &&
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

// The room a spawn function needs under its caller's stack for a blueprint of
// 'parts': 8 bytes a part, the most any of its arrays takes (sub_8250FD28's
// sort); none within the arrays.
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

// sub_82633230, before the span test: the vehicle's and the held part's cells
// together (largest r11, smallest r10). True goes to the cursor clamp that
// keeps it in that box (0x82633600); a span past the limit goes on to the
// game's pull-back, which the hooks below make pull back to it.
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
