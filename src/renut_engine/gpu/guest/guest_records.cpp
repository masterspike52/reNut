#include "renut_engine/gpu/guest/guest_records.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <rex/kernel/guest_allocator.h>
#include <rex/kernel/guest_memory.h>
#include <rex/kernel/title_runtime.h>

namespace renut::gpu::records {

namespace {

// Records are small (headers + a few host pointers); 64 MiB is ample.
constexpr u32 kHeapSize = 64u << 20;
constexpr u32 kPageSize64K = 64u * 1024u;

std::mutex g_heap_mutex;
// Never freed: the title memory it sits in goes away first (as reblue's HostHeap).
rex::kernel::GuestAllocator *g_heap = nullptr;

std::shared_mutex g_registry_mutex;
std::unordered_map<u32, RecordKind> g_registry;
std::atomic<uint64_t> g_generation{0};

} // namespace

u32 AllocPhysicalBlock(u32 size) {
  // The physical window (64 KiB pages) maps host and guest addresses 1:1 in
  // offset, so a host pointer into it round-trips to a guest VA.
  rex::kernel::GuestMemory *memory = REX_GUEST_MEMORY();
  if (!memory)
    return 0;
  const rex::Result<u32> guest_va = memory->Allocate(size, 64u * 1024u, rex::kernel::GuestProtect::kReadWrite,
                                                     rex::kernel::GuestHeap::kPhysical);
  return guest_va ? *guest_va : 0;
}

bool Init() {
  std::lock_guard lock(g_heap_mutex);
  if (g_heap)
    return true;

  rex::kernel::GuestMemory *memory = REX_GUEST_MEMORY();
  if (!memory)
    return false;
  // Physical 64 KiB pages, so a host pointer into it round-trips to a guest VA.
  auto allocator = rex::kernel::GuestAllocator::Create(*memory, kHeapSize, kPageSize64K, /*physical=*/true,
                                                       kPageSize64K, 0, rex::kernel::GuestPool::kTitle);
  if (!allocator)
    return false;
  g_heap = (*allocator).release();
  return true;
}

void *AllocRaw(size_t size) {
  std::lock_guard lock(g_heap_mutex);
  if (!g_heap)
    return nullptr;  // used before Init
  return g_heap->Allocate(static_cast<u32>(size ? size : 1));  // nullptr when the heap is exhausted
}

void FreeRaw(void *host) {
  if (!host)
    return;
  std::lock_guard lock(g_heap_mutex);
  if (g_heap)
    g_heap->Free(host);
}

void Register(u32 guest_va, RecordKind kind) {
  std::unique_lock lock(g_registry_mutex);
  g_registry[guest_va] = kind;
}

void Unregister(u32 guest_va) {
  std::unique_lock lock(g_registry_mutex);
  g_registry.erase(guest_va);
  g_generation.fetch_add(1, std::memory_order_release);
}

uint64_t Generation() {
  return g_generation.load(std::memory_order_acquire);
}

bool Lookup(u32 guest_va, RecordKind *out_kind) {
  std::shared_lock lock(g_registry_mutex);
  auto it = g_registry.find(guest_va);
  if (it == g_registry.end())
    return false;
  *out_kind = it->second;
  return true;
}

} // namespace renut::gpu::records
