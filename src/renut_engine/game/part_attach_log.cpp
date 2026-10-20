/**
 * @file    game/part_attach_log.cpp
 * @brief   renut_log_part_attach: logs vehicle parts attaching to a vehicle,
 *          to find why a part is left off (debugging custom parts).
 *
 * N&B (IDA): a part joins a vehicle through its descriptor's slot 5. The
 * generic attach is sub_8262FB90(part, vehicle, cell, orientation, ..., flags
 * r8) (non-zero when attached). The wheel's slot 5, sub_82641DC8(part,
 * vehicle, cell, orientation, ...), first matches the wheel's orientation to
 * the vehicle's other wheel (sub_82641E58(part, vehicle, orientation): the
 * orientation to use) and counts it at vehicle + 3540 when attached. A part's
 * class id is at part + 4.
 */
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include <cstring>

#include "renut_engine/app/renut_logging.h"

REXCVAR_DEFINE_BOOL(renut_log_part_attach, false, "Nuts&Bolts/Debug",
                    "Logs vehicle parts attaching to vehicles (wheels always, other parts when they fail).");

REX_EXTERN(__imp__sub_8262FB90);  // generic part attach
REX_EXTERN(__imp__sub_82641DC8);  // wheel attach (slot 5)
REX_EXTERN(__imp__sub_82641E58);  // wheel orientation match
REX_EXTERN(__imp__sub_82641E48);  // wheel detach (slot 6)
REX_EXTERN(__imp__sub_82641D50);  // wheel destroy (slot 2)

namespace {

uint32_t Load32(const uint8_t *base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return rex::byte_swap(v);
}

} // namespace

// sub_8262FB90(part, vehicle, cell, orientation, ..., flags r8): attaches a part.
REX_HOOK_RAW(sub_8262FB90) {
  const bool log = REXCVAR_GET(renut_log_part_attach);
  const uint32_t part = ctx.r3.u32, vehicle = ctx.r4.u32, cell = ctx.r5.u32, orientation = ctx.r6.u32,
                 flags = ctx.r8.u32;
  __imp__sub_8262FB90(ctx, base);
  if (log && !ctx.r3.u32)
    RNUT_INFO("Part attach FAILED: part {:08X} (class {}) on vehicle {:08X}, cell {:08X}, orientation {}, flags {:X}",
              part, part ? Load32(base, part + 4) : 0, vehicle, cell, orientation, flags);
}

// sub_82641E58(wheel, vehicle, orientation): the orientation the wheel takes,
// matched to the vehicle's other wheel.
REX_HOOK_RAW(sub_82641E58) {
  const bool log = REXCVAR_GET(renut_log_part_attach);
  const uint32_t wheel = ctx.r3.u32, vehicle = ctx.r4.u32, orientation = ctx.r5.u32;
  __imp__sub_82641E58(ctx, base);
  if (log)
    RNUT_INFO("Wheel orientation: wheel {:08X} on vehicle {:08X}: {} -> {} (wheel +576 {:08X}, +612 {:08X})", wheel,
              vehicle, orientation, ctx.r3.u32, Load32(base, wheel + 576), Load32(base, wheel + 612));
}

// sub_82641E48(wheel, vehicle, ...): a wheel leaves its vehicle.
REX_HOOK_RAW(sub_82641E48) {
  if (REXCVAR_GET(renut_log_part_attach))
    RNUT_INFO("Wheel detach: wheel {:08X} from vehicle {:08X} (wheels {})", ctx.r3.u32, ctx.r4.u32,
              ctx.r4.u32 ? base[ctx.r4.u32 + 3540] : 0);
  __imp__sub_82641E48(ctx, base);
}

// sub_82641D50(wheel): a wheel is destroyed.
REX_HOOK_RAW(sub_82641D50) {
  if (REXCVAR_GET(renut_log_part_attach))
    RNUT_INFO("Wheel destroyed: wheel {:08X}", ctx.r3.u32);
  __imp__sub_82641D50(ctx, base);
}

// sub_82641DC8(wheel, vehicle, cell, orientation, ...): a wheel's attach.
REX_HOOK_RAW(sub_82641DC8) {
  const bool log = REXCVAR_GET(renut_log_part_attach);
  const uint32_t wheel = ctx.r3.u32, vehicle = ctx.r4.u32, cell = ctx.r5.u32, orientation = ctx.r6.u32;
  const uint32_t wheels_before = vehicle ? base[vehicle + 3540] : 0;
  __imp__sub_82641DC8(ctx, base);
  if (log)
    RNUT_INFO("Wheel attach: wheel {:08X} (class {}) on vehicle {:08X}, cell {:08X}, orientation {}: {} (wheels {} -> {})",
              wheel, wheel ? Load32(base, wheel + 4) : 0, vehicle, cell, orientation,
              ctx.r3.u32 ? "attached" : "FAILED", wheels_before, vehicle ? base[vehicle + 3540] : 0);
}
