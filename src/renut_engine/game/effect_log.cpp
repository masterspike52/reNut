/**
 * @file    game/effect_log.cpp
 * @brief   renut_log_effects: logs the game's particle effects as it plays
 *          them, by name, for part scripts' fx.play (part_scripts.h).
 *
 * N&B (IDA): sub_82276BA8(effect system, const u32 *aid, params, ...) spawns
 * a particle effect (sub_823DC870 and 16 other callers: vehicles, blocks,
 * footsteps, weapons...). Effect names are game asset refs: the tables
 * gameAssetRefGetAid (0x82254D10) searches, a list at 0x82F84B78 of
 * {?, next, count, entries}, entries {char name[64], u32 aid}; the log looks
 * the aid up there. Each effect is logged the first time it plays.
 */
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include "renut_engine/app/renut_logging.h"

REXCVAR_DEFINE_BOOL(renut_log_effects, false, "Nuts&Bolts/Debug",
                    "Logs each particle effect the game plays (the first time), with the name a part "
                    "script passes to fx.play.");

REX_EXTERN(__imp__sub_82276BA8);  // spawns a particle effect

namespace {

constexpr uint32_t kAssetRefs = 0x82F84B78;
constexpr uint32_t kAssetRefEntrySize = 68;
constexpr uint32_t kAssetRefAid = 64;

uint32_t Load32(const uint8_t *base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return rex::byte_swap(v);
}

// The asset ref name of 'aid', or empty.
std::string AssetRefName(const uint8_t *base, uint32_t aid) {
  for (uint32_t table = Load32(base, kAssetRefs); table; table = Load32(base, table + 4)) {
    const uint32_t count = Load32(base, table + 8), entries = Load32(base, table + 12);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t entry = entries + i * kAssetRefEntrySize;
      if (Load32(base, entry + kAssetRefAid) == aid)
        return std::string(reinterpret_cast<const char *>(base + entry),
                           strnlen(reinterpret_cast<const char *>(base + entry), kAssetRefAid));
    }
  }
  return {};
}

// The asset types effects come in (the id's top byte).
const char *EffectType(uint32_t aid) {
  switch (aid >> 24) {
  case 0x1B: return "fx emitter";
  case 0x45: return "GPU particle effect";
  case 0x47: return "blobs droplet effect";
  case 0x48: return "explosion effect";
  case 0x4A: return "3D GPU particle effect";
  case 0x4B: return "composite effect";
  default: return "effect";
  }
}

std::mutex g_mutex;
std::unordered_set<uint32_t> g_logged;

} // namespace

// sub_82276BA8(effect system, const u32 *aid, params, ...): spawns an effect.
REX_HOOK_RAW(sub_82276BA8) {
  if (REXCVAR_GET(renut_log_effects) && ctx.r4.u32) {
    const uint32_t aid = Load32(base, ctx.r4.u32);
    bool first = false;
    {
      std::lock_guard lock(g_mutex);
      first = aid && g_logged.insert(aid).second;
    }
    if (first) {
      const std::string name = AssetRefName(base, aid);
      // fx.play takes the name, or the id as "0x…" when it has none.
      if (name.empty())
        RNUT_INFO("Effect played: \"0x{:08X}\" ({})", aid, EffectType(aid));
      else
        RNUT_INFO("Effect played: \"{}\" ({}, asset 0x{:08X})", name, EffectType(aid), aid);
    }
  }
  __imp__sub_82276BA8(ctx, base);
}
