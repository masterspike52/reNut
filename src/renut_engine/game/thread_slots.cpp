#include <rex/kernel/thread_registry.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>

#include <cstdint>

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
    const rex::kernel::ThreadPin thread = REX_RUNTIME()->GetThreadRegistry().Find(thread_id);
    if (thread && thread->IsRunning())
      continue;
    Store32(base, entry + kEntryHandleOffset, 0);
    Store32(base, entry + kEntryThreadIdOffset, kFreeThreadId);
    ++freed;
  }
  return freed;
}

} // namespace

// int sub_8220F350(DWORD thread_id): the thread's slot; when the table is full,
// exited threads' entries are freed and it asks again.
REX_HOOK_RAW(sub_8220F350) {
  const uint32_t thread_id = ctx.r3.u32;
  __imp__sub_8220F350(ctx, base);
  if (ctx.r3.u32 != kNoSlot)
    return;
  if (!FreeExitedThreads(base))
    return;
  ctx.r3.u64 = thread_id;
  __imp__sub_8220F350(ctx, base);
}
