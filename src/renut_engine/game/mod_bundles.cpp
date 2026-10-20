/**
 * @file    game/mod_bundles.cpp
 * @brief   Loads the bundles .nbpatch part mods add (their parts' models,
 *          collision, footprints and textures) with the game's common bundle.
 *
 * N&B (IDA): at boot sub_82363BB0 loads the common bundle,
 * assetDbBundleLoadBundle(0, &aid_bundle_banjox_common) (GAME:bundle\4f\685374,
 * a CAFF: every asset its manifest lists is opened and kept, sub_822C2B58),
 * then its stream archive, sub_822BDF68(0, &aid_streambundle_banjox_common,
 * wait 1) (GAME:bundle\50\685374, assets opened on demand). Each registers
 * the bundle as a provider of the asset manager (dword_82F7BFBC + 16), which
 * every open by asset id searches in order, and takes a slot of the bundle
 * table 0x82F7BB50 (128 of {u32 id, u32 references}); owner 0 is no level's,
 * so no level change lets them go. Vehicles stream their parts' models and
 * collision by id (Bundle/50) and streamed models find their textures by name
 * among the resident bundles, so a mod's parts can bring their own:
 * tools/renut_modtool writes a resident bundle (textures) and a stream
 * archive (models, collision, footprints) that the mod adds as new files.
 *
 * Here each mod bundle (mods/custom_parts.h ModBundles) is loaded the same
 * way right after the common bundle of its kind, while the game's allocator
 * is still set up for the common bundles (sub_82363BB0 switches it after).
 */
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "renut_engine/app/renut_logging.h"
#include "renut_engine/mods/custom_parts.h"

REX_EXTERN(__imp__assetDbBundleLoadBundle);
REX_EXTERN(__imp__sub_822BDF68);  // loads a stream archive (Bundle/50)

namespace {

constexpr uint32_t kCommonBundle = 0x4F685374;  // aid_bundle_banjox_common
constexpr uint32_t kCommonStream = 0x50685374;  // aid_streambundle_banjox_common
constexpr uint32_t kTypeBundle = 0x4F;
constexpr uint32_t kTypeStream = 0x50;
// The loaded bundles: {u32 id, u32 references} x 128.
constexpr uint32_t kBundleTable = 0x82F7BB50;
constexpr uint32_t kBundleSlots = 128;

uint32_t Load32(const uint8_t *base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return rex::byte_swap(v);
}

void Store32(uint8_t *base, uint32_t va, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(base + va, &value, 4);
}

bool IsLoaded(const uint8_t *base, uint32_t id) {
  for (uint32_t i = 0; i < kBundleSlots; ++i)
    if (Load32(base, kBundleTable + 8 * i) == id)
      return true;
  return false;
}

std::atomic<bool> g_bundles_loaded{false};
std::atomic<bool> g_streams_loaded{false};
uint32_t g_id_va = 0;  // guest word holding the id being loaded

// Loads the mod bundles of one kind (kTypeBundle or kTypeStream) through the
// game's own loader. The caller's registers are put back.
void LoadModBundles(PPCContext &ctx, uint8_t *base, uint32_t type) {
  const auto r3 = ctx.r3, r4 = ctx.r4, r5 = ctx.r5;
  for (const renut::mods::ModBundle &bundle : renut::mods::ModBundles()) {
    if (bundle.id >> 24 != type)
      continue;
    const char *folder = type == kTypeBundle ? "4f" : "50";
    if (IsLoaded(base, bundle.id)) {
      RNUT_INFO("mods: {}: bundle {}/{:06x} is already loaded", bundle.mod, folder, bundle.id & 0xFFFFFF);
      continue;
    }
    if (!g_id_va && !(g_id_va = REX_KERNEL_MEMORY()->SystemHeapAlloc(4))) {
      RNUT_ERROR("mods: no guest memory; the mods' bundles are not loaded");
      break;
    }
    Store32(base, g_id_va, bundle.id);
    ctx.r3.u64 = 0;  // no owner: kept for the session
    ctx.r4.u64 = g_id_va;
    if (type == kTypeBundle) {
      __imp__assetDbBundleLoadBundle(ctx, base);
    } else {
      ctx.r5.u64 = 1;  // wait until it is ready
      __imp__sub_822BDF68(ctx, base);
    }
    if (IsLoaded(base, bundle.id))
      RNUT_INFO("mods: {}: bundle {}/{:06x} loaded", bundle.mod, folder, bundle.id & 0xFFFFFF);
    else
      RNUT_ERROR("mods: {}: bundle {}/{:06x} didn't load (the bundle table is full, or the file is missing)",
                 bundle.mod, folder, bundle.id & 0xFFFFFF);
  }
  ctx.r3 = r3;
  ctx.r4 = r4;
  ctx.r5 = r5;
}

} // namespace

// assetDbBundleLoadBundle(owner, const u32 *id): loads a resident bundle
// (Bundle/4f) and waits for it.
REX_HOOK_RAW(assetDbBundleLoadBundle) {
  const uint32_t owner = ctx.r3.u32;
  const uint32_t id = ctx.r4.u32 ? Load32(base, ctx.r4.u32) : 0;
  __imp__assetDbBundleLoadBundle(ctx, base);
  if (id == kCommonBundle && !owner && !renut::mods::ModBundles().empty() && !g_bundles_loaded.exchange(true))
    LoadModBundles(ctx, base, kTypeBundle);
}

// sub_822BDF68(owner, const u32 *id, int wait): loads a stream archive
// (Bundle/50), waiting for it when 'wait'.
REX_HOOK_RAW(sub_822BDF68) {
  const uint32_t owner = ctx.r3.u32;
  const uint32_t id = ctx.r4.u32 ? Load32(base, ctx.r4.u32) : 0;
  __imp__sub_822BDF68(ctx, base);
  if (id == kCommonStream && !owner && !renut::mods::ModBundles().empty() && !g_streams_loaded.exchange(true))
    LoadModBundles(ctx, base, kTypeStream);
}
