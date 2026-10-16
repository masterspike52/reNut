/**
 * @file    game/level_pools.cpp
 * @brief   N&B's pair of section memory pools, made thread safe.
 *
 * dword_82FAC600 (sized per section) and dword_82FAC604 (1 KB blocks) are
 * pools (made by sub_823646E8: memory at +28, size at +20) that XACT
 * (sub_822C9FF0 / sub_822CA098) and file streaming (sub_8230B4F8 /
 * sub_8230A1E8) allocate from on their own threads. The main thread replaces
 * them at section changes (sub_82364A88, from sub_823C5F50 and
 * scriptExecuteMain, which also destroys them on its own with sub_82364BB8),
 * and their getter (sub_82364A20) builds them when they are missing. None of
 * it is locked: two threads destroying the same pool crash in meInternalFree
 * at a section load.
 *
 * - One lock covers the getter, the rebuild, the destroy, XACT's allocate and
 *   free, and the pool allocate (sub_8228D108). Under it the game takes only
 *   the pool's own lock and the heaps; nothing takes this lock while holding
 *   those, so no thread waits on another in a cycle. (Streaming's free runs
 *   under the pool's lock and is left out: the rebuilds wait for streaming to
 *   finish first.)
 * - A destroy of a pool that is gone already is skipped.
 * - scriptExecuteMain's destroy (the only one without the lock) first waits
 *   for streaming to finish, as sub_8230A098 does before the rebuilds, and
 *   takes both pools at once, so the getter never sees half of the pair.
 * - A pool the getter built while the script was between its destroy and its
 *   store of 0 is put back when the getter next finds the slot empty.
 * - XACT freeing a block of a pool since destroyed (its memory is gone) is
 *   skipped, rather than handed to XPhysicalFree.
 * - Allocating from a pool since destroyed allocates from its replacement.
 *
 * The pools also hold the data of N&B's level textures: headers the game sets
 * up itself (XGSetTextureHeader) over pool blocks, which D3D never creates and
 * so never destroys. The game ends them by freeing their memory: a streamed
 * block back to its pool (sub_823648D0, from the streaming's sub_8230A1E8),
 * or the whole pool at a section change. Those frees release the renderer's
 * host textures over that memory (ReleaseHostTexturesInMemoryLocked), after
 * the draws queued before them. A block's size is noted at its allocation.
 */
#include <rex/hook.h>
#include <rex/types.h>

#include <rex/system/kernel_state.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "renut_engine/app/renut_logging.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/render/textures.h"

namespace {

constexpr uint32_t kPoolSlots[2] = {0x82FAC600, 0x82FAC604};
// sub_8230B4F8 counts the stream requests in flight; sub_8230A1E8 counts them
// out after its free.
constexpr uint32_t kStreamsInFlightVa = 0x82FADEA8;
constexpr uint32_t kPoolMemoryOffset = 28;
constexpr uint32_t kPoolSizeOffset = 20;
constexpr auto kStreamWaitLimit = std::chrono::seconds(5);

struct Pool {
  uint32_t va = 0;
  uint32_t memory = 0;
  uint32_t size = 0;
  uint32_t slot = 0;  // index into kPoolSlots
};

std::recursive_mutex g_mutex;
thread_local int t_depth = 0;  // this thread holds g_mutex
std::vector<Pool> g_live;
// The latest destroyed pools: their memory, for XACT frees that arrive late,
// and their slot, for allocations from them.
std::vector<Pool> g_destroyed;
constexpr size_t kDestroyedKept = 16;

class Locked {
public:
  Locked() : lock_(g_mutex) { ++t_depth; }
  ~Locked() { --t_depth; }

private:
  std::unique_lock<std::recursive_mutex> lock_;
};

// Pool blocks allocated and not yet freed: their size, by address. Its own
// lock: the pool free runs under the pool's lock, which the allocation takes
// under g_mutex, so g_mutex cannot be taken there.
std::mutex g_blocks_mutex;
std::unordered_map<uint32_t, uint32_t> g_block_sizes;

// The game freed 'size' bytes of guest memory at 'va': the host textures over
// it go, once the draws queued so far are done with them.
void ReleaseTexturesIn(uint32_t va, uint32_t size) {
  if (!va || !size)
    return;
  const uint32_t physical = REX_KERNEL_MEMORY()->GetPhysicalAddress(va);
  if (physical == UINT32_MAX)
    return;
  renut::gpu::render_thread::Queue(
      [physical, size] { renut::gpu::ReleaseHostTexturesInMemoryLocked(physical, size); });
}

// The first few of each warning.
bool LogLimited(uint32_t &count) {
  return count++ < 8;
}

uint32_t Load(uint8_t *base, uint32_t va) {
  return rex::byte_swap(*reinterpret_cast<const uint32_t *>(base + va));
}

void Store(uint8_t *base, uint32_t va, uint32_t value) {
  *reinterpret_cast<uint32_t *>(base + va) = rex::byte_swap(value);
}

Pool *FindLive(uint32_t va) {
  auto it = std::find_if(g_live.begin(), g_live.end(), [va](const Pool &p) { return p.va == va; });
  return it != g_live.end() ? &*it : nullptr;
}

bool InPool(const Pool &pool, uint32_t address) {
  return address >= pool.memory && address - pool.memory < pool.size;
}

// A pool in a slot that was made before anything here saw it counts as live.
bool IsLive(uint8_t *base, uint32_t va) {
  return va && (FindLive(va) || va == Load(base, kPoolSlots[0]) || va == Load(base, kPoolSlots[1]));
}

// The pools the slots hold, from their headers.
void RegisterSlots(uint8_t *base) {
  for (uint32_t slot = 0; slot < 2; ++slot) {
    const uint32_t va = Load(base, kPoolSlots[slot]);
    if (!va || FindLive(va))
      continue;
    Pool pool{va, Load(base, va + kPoolMemoryOffset), Load(base, va + kPoolSizeOffset), slot};
    // Its memory is in use again: no late free of a destroyed pool lands there.
    std::erase_if(g_destroyed, [&](const Pool &old) {
      return old.memory < pool.memory + pool.size && pool.memory < old.memory + old.size;
    });
    g_live.push_back(pool);
  }
}

// A slot left empty by scriptExecuteMain's store of 0 over a pool the getter
// had just built: the pool is still there, so it goes back in.
void AdoptOrphans(uint8_t *base) {
  for (uint32_t slot = 0; slot < 2; ++slot) {
    if (Load(base, kPoolSlots[slot]))
      continue;
    const uint32_t other = Load(base, kPoolSlots[slot ^ 1]);
    for (const Pool &pool : g_live) {
      if (pool.slot == slot && pool.va != other) {
        Store(base, kPoolSlots[slot], pool.va);
        static uint32_t logged = 0;
        if (LogLimited(logged))
          RNUT_WARN("Section pool {:08X} was dropped from its slot while in use; restored", pool.va);
        break;
      }
    }
  }
}

// What sub_8230A098 waits for before the main thread replaces the pools.
void WaitForStreams(uint8_t *base) {
  const auto start = std::chrono::steady_clock::now();
  while (Load(base, kStreamsInFlightVa) != 0) {
    if (std::chrono::steady_clock::now() - start > kStreamWaitLimit) {
      RNUT_WARN("Section pools destroyed with {} stream requests still in flight",
                Load(base, kStreamsInFlightVa));
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

} // namespace

REX_EXTERN(__imp__sub_82364A20);
REX_EXTERN(__imp__sub_82364A88);
REX_EXTERN(__imp__sub_82364BB8);
REX_EXTERN(__imp__sub_822C9FF0);
REX_EXTERN(__imp__sub_822CA098);
REX_EXTERN(__imp__sub_8228D108);
REX_EXTERN(__imp__sub_823648D0);

namespace {

// Destroys a live pool and empties the slot holding it. g_mutex held.
void DestroyLocked(PPCContext &ctx, uint8_t *base, uint32_t va) {
  Pool pool;
  if (Pool *live = FindLive(va)) {
    pool = *live;
  } else {
    pool = {va, Load(base, va + kPoolMemoryOffset), Load(base, va + kPoolSizeOffset),
            va == Load(base, kPoolSlots[1]) ? 1u : 0u};
  }
  ctx.r3.u64 = va;
  ctx.r4.u64 = 1;
  __imp__sub_82364BB8(ctx, base);
  ReleaseTexturesIn(pool.memory, pool.size);
  {
    std::lock_guard blocks(g_blocks_mutex);
    std::erase_if(g_block_sizes, [&](const auto &entry) { return InPool(pool, entry.first); });
  }
  std::erase_if(g_live, [va](const Pool &p) { return p.va == va; });
  for (uint32_t slot : kPoolSlots) {
    if (Load(base, slot) == va)
      Store(base, slot, 0);
  }
  g_destroyed.push_back(pool);
  if (g_destroyed.size() > kDestroyedKept)
    g_destroyed.erase(g_destroyed.begin());
}

} // namespace

// void *sub_82364A20(bool block_pool): the pool, built at 1 MB when missing.
REX_HOOK_RAW(sub_82364A20) {
  Locked locked;
  AdoptOrphans(base);
  __imp__sub_82364A20(ctx, base);
}

// void sub_82364A88(size, count): destroys the pair and makes it anew.
REX_HOOK_RAW(sub_82364A88) {
  Locked locked;
  AdoptOrphans(base);
  __imp__sub_82364A88(ctx, base);
  RegisterSlots(base);
}

// void *sub_82364BB8(pool, bool): destroys a pool (and frees its header).
REX_HOOK_RAW(sub_82364BB8) {
  const uint32_t va = ctx.r3.u32;
  // Only scriptExecuteMain calls it without the lock (sub_82364A88 holds it).
  const bool from_script = t_depth == 0;
  if (from_script)
    WaitForStreams(base);
  Locked locked;
  if (!IsLive(base, va)) {
    static uint32_t logged = 0;
    if (LogLimited(logged))
      RNUT_WARN("Section pool {:08X} destroyed twice; skipped", va);
    return;
  }
  DestroyLocked(ctx, base, va);
  if (from_script) {
    // The script destroys both, one after the other: take the other now.
    for (uint32_t slot : kPoolSlots) {
      if (const uint32_t other = Load(base, slot))
        DestroyLocked(ctx, base, other);
    }
  }
  ctx.r3.u64 = va;
}

// void *sub_822C9FF0(XACT, size): XACT's allocate, from the pool when it fits.
REX_HOOK_RAW(sub_822C9FF0) {
  Locked locked;
  __imp__sub_822C9FF0(ctx, base);
}

// void sub_822CA098(XACT, void *block): XACT's free, to the pool holding the
// block or to XPhysicalFree.
REX_HOOK_RAW(sub_822CA098) {
  Locked locked;
  const uint32_t block = ctx.r4.u32;
  const bool live = std::any_of(g_live.begin(), g_live.end(),
                                [block](const Pool &p) { return InPool(p, block); });
  if (!live && std::any_of(g_destroyed.begin(), g_destroyed.end(),
                           [block](const Pool &p) { return InPool(p, block); })) {
    static uint32_t logged = 0;
    if (LogLimited(logged))
      RNUT_WARN("XACT freed {:08X} from a section pool already destroyed; skipped", block);
    return;
  }
  __imp__sub_822CA098(ctx, base);
}

// void *sub_8228D108(pool, size): allocates from a pool.
REX_HOOK_RAW(sub_8228D108) {
  Locked locked;
  const uint32_t va = ctx.r3.u32;
  if (!IsLive(base, va)) {
    auto old = std::find_if(g_destroyed.rbegin(), g_destroyed.rend(),
                            [va](const Pool &p) { return p.va == va; });
    const uint32_t slot = old != g_destroyed.rend() ? old->slot : 0;
    const uint32_t size = ctx.r4.u32;
    ctx.r3.u64 = slot;
    sub_82364A20(ctx, base);
    static uint32_t logged = 0;
    if (LogLimited(logged))
      RNUT_WARN("Allocation from destroyed section pool {:08X} moved to {:08X}", va, ctx.r3.u32);
    ctx.r4.u64 = size;
  }
  const uint32_t size = ctx.r4.u32;
  __imp__sub_8228D108(ctx, base);
  if (const uint32_t block = ctx.r3.u32) {
    std::lock_guard blocks(g_blocks_mutex);
    g_block_sizes[block] = size;
  }
}

// void sub_823648D0(pool, void *block): frees a block back to its pool (the
// streaming's free, sub_8230A1E8, under the pool's lock).
REX_HOOK_RAW(sub_823648D0) {
  const uint32_t block = ctx.r4.u32;
  uint32_t size = 0;
  {
    std::lock_guard blocks(g_blocks_mutex);
    if (auto it = g_block_sizes.find(block); it != g_block_sizes.end()) {
      size = it->second;
      g_block_sizes.erase(it);
    }
  }
  __imp__sub_823648D0(ctx, base);
  ReleaseTexturesIn(block, size);
}
