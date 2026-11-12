#include <rex/kernel/guest_memory.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "renut_engine/mods/custom_parts.h"

REX_EXTERN(__imp__rex_assetDbBundleLoadBundle);
REX_EXTERN(__imp__sub_8228BAC8);  // loads a stream archive (Bundle/50)

namespace {

constexpr uint32_t kCommonBundle = 0x4F685374;  // aid_bundle_banjox_common
constexpr uint32_t kCommonStream = 0x50685374;  // aid_streambundle_banjox_common
constexpr uint32_t kTypeBundle = 0x4F;
constexpr uint32_t kTypeStream = 0x50;
// The loaded bundles: {u32 id, u32 references} x 128.
constexpr uint32_t kBundleTable = 0x82DE8D70;
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
  const auto r3 = ctx.r3, r4 = ctx.r4, r5 = ctx.r5, r6 = ctx.r6, r7 = ctx.r7;
  for (const renut::mods::ModBundle &bundle : renut::mods::ModBundles()) {
    if (bundle.id >> 24 != type || IsLoaded(base, bundle.id))
      continue;
    // No guest memory: the mods' bundles are not loaded.
    if (rex::kernel::GuestMemory *memory = REX_GUEST_MEMORY(); !g_id_va && memory)
      g_id_va =
          memory->Allocate(4, 0x20, rex::kernel::GuestProtect::kReadWrite, rex::kernel::GuestHeap::kSystem).value_or(0);
    if (!g_id_va)
      break;
    Store32(base, g_id_va, bundle.id);
    ctx.r3.u64 = 0;  // no owner: kept for the session
    ctx.r4.u64 = g_id_va;
    if (type == kTypeBundle) {
      // As boot loads the common bundle: its assets opened, no DLC check,
      // from GAME:.
      ctx.r5.u64 = 1;
      ctx.r6.u64 = 0;
      ctx.r7.u64 = 0;
      __imp__rex_assetDbBundleLoadBundle(ctx, base);
    } else {
      ctx.r5.u64 = 1;  // wait until it is ready
      __imp__sub_8228BAC8(ctx, base);
    }
  }
  ctx.r3 = r3;
  ctx.r4 = r4;
  ctx.r5 = r5;
  ctx.r6 = r6;
  ctx.r7 = r7;
}

} // namespace

// assetDbBundleLoadBundle(owner, const u32 *id, open, dlc, update): loads a
// resident bundle (Bundle/4f) and waits for it.
REX_HOOK_RAW(rex_assetDbBundleLoadBundle) {
  const uint32_t owner = ctx.r3.u32;
  const uint32_t id = ctx.r4.u32 ? Load32(base, ctx.r4.u32) : 0;
  __imp__rex_assetDbBundleLoadBundle(ctx, base);
  if (id == kCommonBundle && !owner && !renut::mods::ModBundles().empty() && !g_bundles_loaded.exchange(true))
    LoadModBundles(ctx, base, kTypeBundle);
}

// sub_8228BAC8(owner, const u32 *id, int wait): loads a stream archive
// (Bundle/50), waiting for it when 'wait'.
REX_HOOK_RAW(sub_8228BAC8) {
  const uint32_t owner = ctx.r3.u32;
  const uint32_t id = ctx.r4.u32 ? Load32(base, ctx.r4.u32) : 0;
  __imp__sub_8228BAC8(ctx, base);
  if (id == kCommonStream && !owner && !renut::mods::ModBundles().empty() && !g_streams_loaded.exchange(true))
    LoadModBundles(ctx, base, kTypeStream);
}
