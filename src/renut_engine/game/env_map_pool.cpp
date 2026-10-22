/**
 * @file    game/env_map_pool.cpp
 * @brief   Room for more shared environment maps (co-op in Showdown Town).
 *
 * N&B (IDA): a model texture slot named EnvMapShared gets an environment map
 * entry per distinct texture name (sub_823097C8; the name without its "top" or
 * "mip" ending, sub_8230A108), shared and counted. A new one is taken from a
 * pool made at boot, 1024 entries of 76 bytes (sub_822B7468:
 * dword_82FADE64 = sub_8231EAA8(1024, 76); + 12 free count, + 16 the free
 * entries), by sub_8230A8B8, which uses the entry without checking: with none
 * left it writes through a null pointer (the crash at the first co-op match:
 * guest 0x2C from sub_82213AB8, on the loader thread). Showdown Town loaded as
 * an online match, from the online lobby's scene, needs more than 1024.
 *
 * Here that pool has 4096 entries (sub_8231EAA8's other caller, sub_823AD990,
 * makes 80 of 140 bytes), about 230 KB more, and running low is logged so the
 * number can be checked.
 */
#include <rex/hook.h>
#include <rex/types.h>

#include <cstdint>

#include "renut_engine/app/renut_logging.h"
#include "renut_engine/game/guest_call.h"

REX_EXTERN(__imp__sub_8231EAA8);  // object pool (count, size)
REX_EXTERN(__imp__sub_8230A8B8);  // a new shared environment map entry

namespace {

constexpr uint32_t kEnvMapPool = 0x82FADE64;
constexpr uint32_t kPoolFree = 12;
constexpr uint32_t kGameCount = 1024;
constexpr uint32_t kEntrySize = 76;
constexpr uint32_t kCount = 4096;
constexpr uint32_t kLowWater = 256;

bool g_warned = false;

}  // namespace

REX_HOOK_RAW(sub_8231EAA8) {
  if (ctx.r3.u32 == kGameCount && ctx.r4.u32 == kEntrySize) {
    ctx.r3.u64 = kCount;
    RNUT_INFO("Shared environment maps: room for {} (the game's {})", kCount, kGameCount);
  }
  __imp__sub_8231EAA8(ctx, base);
}

REX_HOOK_RAW(sub_8230A8B8) {
  __imp__sub_8230A8B8(ctx, base);
  const uint32_t pool = renut::guest::Load32(base, kEnvMapPool);
  if (!pool)
    return;
  const uint32_t free = renut::guest::Load32(base, pool + kPoolFree);
  if (free < kLowWater && !g_warned) {
    g_warned = true;
    RNUT_WARN("Shared environment maps: only {} of {} left", free, kCount);
  }
}
