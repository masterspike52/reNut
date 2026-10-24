/**
 * @file    game/thread_slots.cpp
 * @brief   N&B's thread slots, given back when their threads have exited.
 *
 * N&B keeps a table of the threads it has seen (sub_8220F350; filled in by
 * sub_823ADCB8): 27 entries of {handle, thread ID, slot} at 0x82E0BAD0, the
 * first 6 its own threads', the other 21 given to any other thread the first
 * time it asks for its slot. Its allocator picks the thread's heap by that slot
 * (meInternalAlloc_82332A08: dword_82E0AE70[slot]). Nothing gives an entry
 * back: on the console a thread's ID is used again once it has exited, but the
 * SDK gives every thread a new ID. Short-lived threads (each area change makes
 * some) use the 21 entries up over a long session; the next new thread then
 * gets slot 27, which has no heap, and its first allocation reads through a
 * null heap (guest 0x24): the crash at an area change after an hour or so.
 *
 * When the game finds the table full (slot 27), the entries of threads that
 * have exited are made free here and it asks again. A full table costs a
 * second look only then.
 */
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/types.h>

#include <cstdint>

#include "renut_engine/app/renut_logging.h"

REX_EXTERN(__imp__sub_8220F350);  // a thread's slot, by thread ID

namespace {

constexpr uint32_t kTableVa = 0x82E0BAD0;
constexpr uint32_t kEntrySize = 12;
constexpr uint32_t kEntryHandleOffset = 0;
constexpr uint32_t kEntryThreadIdOffset = 4;
constexpr uint32_t kEntryCount = 27;
constexpr uint32_t kFirstSharedEntry = 6;
constexpr uint32_t kNoSlot = 27;  // what sub_8220F350 returns with the table full
constexpr uint32_t kFreeThreadId = 0xFFFFFFFF;

uint32_t Load32(const uint8_t *base, uint32_t va) {
  return rex::byte_swap(*reinterpret_cast<const uint32_t *>(base + va));
}

void Store32(uint8_t *base, uint32_t va, uint32_t value) {
  *reinterpret_cast<uint32_t *>(base + va) = rex::byte_swap(value);
}

// Frees the shared entries of threads that have exited; how many.
uint32_t FreeExitedThreads(uint8_t *base) {
  uint32_t freed = 0;
  for (uint32_t i = kFirstSharedEntry; i < kEntryCount; ++i) {
    const uint32_t entry = kTableVa + i * kEntrySize;
    const uint32_t thread_id = Load32(base, entry + kEntryThreadIdOffset);
    if (thread_id == kFreeThreadId)
      continue;
    const auto thread = REX_KERNEL_STATE()->GetThreadByID(thread_id);
    if (thread && thread->is_running())
      continue;
    Store32(base, entry + kEntryHandleOffset, 0);
    Store32(base, entry + kEntryThreadIdOffset, kFreeThreadId);
    ++freed;
  }
  return freed;
}

} // namespace

// int sub_8220F350(DWORD thread_id): the thread's slot, an entry taken for it
// the first time.
REX_HOOK_RAW(sub_8220F350) {
  const uint32_t thread_id = ctx.r3.u32;
  __imp__sub_8220F350(ctx, base);
  if (ctx.r3.u32 != kNoSlot)
    return;
  const uint32_t freed = FreeExitedThreads(base);
  RNUT_INFO("Threads: the game's thread table was full; {} entries of exited threads freed",
            freed);
  if (!freed)
    return;
  ctx.r3.u64 = thread_id;
  __imp__sub_8220F350(ctx, base);
}
