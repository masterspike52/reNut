/**
 * @file    gpu/guest/guest_records.cpp
 */
#include "renut_engine/gpu/guest/guest_records.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <o1heap.h>
#include <rex/system/xmemory.h>

#include "renut_engine/gpu/log.h"

namespace renut::gpu::records {

namespace {

// Records are small (headers + a few host pointers); 64 MiB is ample.
constexpr u32 kHeapSize = 64u << 20;

std::mutex g_heap_mutex;  // o1heap is not internally synchronised
O1HeapInstance *g_heap = nullptr;
bool g_oom_logged = false;

std::shared_mutex g_registry_mutex;
std::unordered_map<u32, RecordKind> g_registry;
std::atomic<uint64_t> g_generation{0};

} // namespace

u32 AllocPhysicalBlock(u32 size) {
  // The physical window (64 KiB pages) maps host and guest addresses 1:1 in
  // offset, so a host pointer into it round-trips to a guest VA.
  rex::memory::BaseHeap *heap = REX_KERNEL_MEMORY()->LookupHeapByType(true, 64u * 1024u);
  if (!heap) {
    RGPU_ERROR("No guest physical heap");
    return 0;
  }
  u32 guest_va = 0;
  if (!heap->Alloc(size, 64u * 1024u,
                   rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                   rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, true,
                   &guest_va)) {
    RGPU_ERROR("Reserving {} bytes of guest physical memory failed", size);
    return 0;
  }
  return guest_va;
}

bool Init() {
  std::lock_guard lock(g_heap_mutex);
  if (g_heap)
    return true;

  auto *memory = REX_KERNEL_MEMORY();
  const u32 guest_va = AllocPhysicalBlock(kHeapSize);
  if (!guest_va) {
    RGPU_ERROR("Record heap: reserving {} bytes failed", kHeapSize);
    return false;
  }
  g_heap = o1heapInit(memory->TranslateVirtual<void *>(guest_va), kHeapSize);
  if (!g_heap) {
    RGPU_ERROR("Record heap: o1heapInit failed");
    return false;
  }
  return true;
}

void *AllocRaw(size_t size) {
  std::lock_guard lock(g_heap_mutex);
  if (!g_heap) {
    RGPU_ERROR("Record heap used before Init");
    return nullptr;
  }
  void *p = o1heapAllocate(g_heap, size ? size : 1);
  if (!p && !g_oom_logged) {
    RGPU_ERROR("Record heap exhausted ({} bytes requested)", size);
    g_oom_logged = true;
  }
  return p;
}

void FreeRaw(void *host) {
  if (!host)
    return;
  std::lock_guard lock(g_heap_mutex);
  if (g_heap)
    o1heapFree(g_heap, host);
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
