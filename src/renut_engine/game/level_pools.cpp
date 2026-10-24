/**
 * @file    game/level_pools.cpp
 * @brief   N&B's level textures in its section memory pools: their host
 *          textures go with the pools' memory.
 *
 * dword_82E198D4 (sized per section) and dword_82E198D8 (1 KB blocks) are
 * pools (made by sub_823330E8: memory at +12, size at +4) that XACT
 * (sub_82299800 / sub_822998B0) and file streaming (sub_823335D0 /
 * sub_82333668) allocate from on their own threads. The main thread replaces
 * them at section changes (sub_823333F0, from sub_82392B60), which destroys
 * the pair it finds; the getter (sub_82333388) builds them through it when
 * they are missing. The game guards all of it with one lock of its own
 * (0x82E198E4), so the section load race of the first release (two threads
 * destroying the same pool) is gone.
 *
 * The pools also hold the data of N&B's level textures: headers the game sets
 * up itself (XGSetTextureHeader) over pool blocks, which D3D never creates and
 * so never destroys. The game ends them by freeing their memory: a streamed
 * block back to its pool (sub_82333240, from the streaming's sub_82333668),
 * or the whole pool at a section change. Those frees release the renderer's
 * host textures over that memory (ReleaseHostTexturesInMemoryLocked), after
 * the draws queued before them. A block's size is noted at its allocation
 * (sub_8225B118).
 */
#include <rex/hook.h>
#include <rex/types.h>

#include <rex/system/kernel_state.h>

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/render/textures.h"

namespace {

constexpr uint32_t kPoolSlots[2] = {0x82E198D4, 0x82E198D8};
constexpr uint32_t kPoolMemoryOffset = 12;
constexpr uint32_t kPoolSizeOffset = 4;

struct Pool {
  uint32_t va = 0;
  uint32_t memory = 0;
  uint32_t size = 0;
};

// Pool blocks allocated and not yet freed: their size, by address. Never held
// across a call into the game (whose pool lock its callers may hold).
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

uint32_t Load(uint8_t *base, uint32_t va) {
  return rex::byte_swap(*reinterpret_cast<const uint32_t *>(base + va));
}

bool InPool(const Pool &pool, uint32_t address) {
  return address >= pool.memory && address - pool.memory < pool.size;
}

} // namespace

REX_EXTERN(__imp__sub_823333F0);
REX_EXTERN(__imp__sub_8225B118);
REX_EXTERN(__imp__sub_82333240);

// void sub_823333F0(size, count): destroys the pair it finds and, given a
// size, makes it anew (under the game's pool lock). The pools are read before
// it: only the main thread's section change replaces a pair (the getter builds
// one only when there is none).
REX_HOOK_RAW(sub_823333F0) {
  Pool old[2];
  for (uint32_t slot = 0; slot < 2; ++slot) {
    if (const uint32_t va = Load(base, kPoolSlots[slot]))
      old[slot] = {va, Load(base, va + kPoolMemoryOffset), Load(base, va + kPoolSizeOffset)};
  }
  __imp__sub_823333F0(ctx, base);
  for (const Pool &pool : old) {
    if (!pool.va)
      continue;
    ReleaseTexturesIn(pool.memory, pool.size);
    std::lock_guard blocks(g_blocks_mutex);
    std::erase_if(g_block_sizes, [&](const auto &entry) { return InPool(pool, entry.first); });
  }
}

// void *sub_8225B118(pool, size): allocates from a pool.
REX_HOOK_RAW(sub_8225B118) {
  const uint32_t size = ctx.r4.u32;
  __imp__sub_8225B118(ctx, base);
  if (const uint32_t block = ctx.r3.u32) {
    std::lock_guard blocks(g_blocks_mutex);
    g_block_sizes[block] = size;
  }
}

// void sub_82333240(pool, void *block): frees a block back to its pool (the
// streaming's free, sub_82333668, under the game's pool lock).
REX_HOOK_RAW(sub_82333240) {
  const uint32_t block = ctx.r4.u32;
  uint32_t size = 0;
  {
    std::lock_guard blocks(g_blocks_mutex);
    if (auto it = g_block_sizes.find(block); it != g_block_sizes.end()) {
      size = it->second;
      g_block_sizes.erase(it);
    }
  }
  __imp__sub_82333240(ctx, base);
  ReleaseTexturesIn(block, size);
}
