#include <rex/kernel/heap.h>
#include <rex/kernel/thread_state.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>

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
// it go, once the draws queued so far are done with them. Game thread.
void ReleaseTexturesIn(uint32_t va, uint32_t size) {
  if (!va || !size)
    return;
  const uint32_t physical = rex::kernel::ThreadState::Get()->GetAddressSpace()->GetPhysicalAddress(va);
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

// void sub_823333F0(size, count): replaces the section pool pair; the old
// pools' host textures are released after it.
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
