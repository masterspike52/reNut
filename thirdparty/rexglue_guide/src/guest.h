#pragma once
// Helpers for the XAM/XNet functions the Guide defines in place of the
// rexglue runtime's (see xam_live.cpp, net.cpp).

#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>

#include <rex/kernel/heap.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/object/notification_broadcaster.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/io.h>
#include <rex/module.h>
#include <rex/ppc/context.h>

namespace rexglue_guide::guest {

using Func = void (*)(PPCContext&, uint8_t*);

// The runtime's own implementation of an import, by symbol name.
Func Original(const char* name);

inline void CallOriginal(const char* name, PPCContext& ctx, uint8_t* base) {
  if (Func f = Original(name)) f(ctx, base);
}

inline uint8_t* At(uint32_t address) {
  rex::kernel::TitleRuntime* runtime = REX_RUNTIME();
  return address && runtime ? runtime->GetMembase().TranslateVirtual(address) : nullptr;
}

template <typename T>
inline T LoadBE(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return std::byteswap(v);
}
template <typename T>
inline void StoreBE(uint8_t* p, T v) {
  v = std::byteswap(v);
  std::memcpy(p, &v, sizeof(T));
}

inline uint32_t Load32(const uint8_t* p) {
  return LoadBE<uint32_t>(p);
}
inline uint16_t Load16(const uint8_t* p) {
  return LoadBE<uint16_t>(p);
}
inline uint64_t Load64(const uint8_t* p) {
  return LoadBE<uint64_t>(p);
}
inline void Store32(uint8_t* p, uint32_t v) {
  StoreBE<uint32_t>(p, v);
}
inline void Store16(uint8_t* p, uint16_t v) {
  StoreBE<uint16_t>(p, v);
}
inline void Store64(uint8_t* p, uint64_t v) {
  StoreBE<uint64_t>(p, v);
}

inline void Return(PPCContext& ctx, uint32_t value) {
  ctx.r3.u64 = value;
}

// The n-th integer argument (0-based); beyond the eight in r3..r10 they sit
// in the caller's parameter save area.
inline uint32_t Arg(PPCContext& ctx, int n) {
  switch (n) {
    case 0: return ctx.r3.u32;
    case 1: return ctx.r4.u32;
    case 2: return ctx.r5.u32;
    case 3: return ctx.r6.u32;
    case 4: return ctx.r7.u32;
    case 5: return ctx.r8.u32;
    case 6: return ctx.r9.u32;
    case 7: return ctx.r10.u32;
    default: return Load32(At(ctx.r1.u32 + 0x54 + 8 * (n - 8)));
  }
}

// The running title's ID, 0 when none is loaded.
inline uint32_t RunningTitleId() {
  rex::kernel::TitleRuntime* runtime = REX_RUNTIME();
  return runtime ? runtime->GetTitleID() : 0;
}

// Sends an XNotification to the title's listeners; nothing when no title is loaded.
inline void Broadcast(uint32_t id, uint32_t data) {
  if (rex::kernel::TitleRuntime* runtime = REX_RUNTIME()) {
    runtime->GetNotificationBroadcaster().BroadcastNotification(id, data);
  }
}

// Signals (set) or clears (reset) a guest event by handle; nothing when it isn't one.
inline void SignalEvent(uint32_t handle, bool set) {
  rex::kernel::TitleRuntime* runtime = REX_RUNTIME();
  if (!runtime || !handle) return;
  if (auto ev = runtime->GetObjectTable().Get<rex::kernel::IGuestEvent>(handle)) {
    if (set) ev->Set();
    else ev->Reset();
  }
}

// Completes an XOVERLAPPED now: its result, extended error (the result when unset) and length.
// Off a guest thread the dispatcher's Complete can't run, so its writes are done here, the completion
// routine going to the thread that started the call (the XOVERLAPPED's context).
inline void CompleteOverlapped(uint32_t overlapped, uint32_t result, std::optional<uint32_t> extended = std::nullopt,
                               std::optional<uint32_t> length = std::nullopt) {
  auto* dispatcher = REX_INTERFACE(rex::kernel::IOverlappedDispatcher);
  if (!dispatcher) return;
  if (rex::kernel::KernelThread::IsInThread()) {
    dispatcher->Complete(overlapped, {.result = result, .extended_error = extended, .length = length});
    return;
  }
  rex::kernel::TitleRuntime* runtime = REX_RUNTIME();
  uint8_t* ov = At(overlapped);
  if (!runtime || !ov) return;
  const uint32_t written = length.value_or(result ? 0xFFFFFFFF : 0);
  dispatcher->Publish([&] {
    rex::kernel::XOverlappedSetExtendedError(ov, extended.value_or(result));
    rex::kernel::XOverlappedSetLength(ov, written);
    rex::kernel::XOverlappedSetResult(ov, result);
    SignalEvent(rex::kernel::XOverlappedGetEvent(ov), true);
  });
  if (const uint32_t routine = rex::kernel::XOverlappedGetCompletionRoutine(ov)) {
    if (auto thread = runtime->GetThreadRegistry().FindByHandle(rex::kernel::XOverlappedGetContext(ov))) {
      thread->EnqueueAPC(routine, result, written, overlapped);
    }
  }
}

inline uint32_t SystemHeapAlloc(uint32_t size) {
  rex::kernel::TitleRuntime* runtime = REX_RUNTIME();
  return runtime ? runtime->GetGuestHeap().SystemHeapAlloc(size) : 0;
}

inline void SystemHeapFree(uint32_t address) {
  rex::kernel::TitleRuntime* runtime = REX_RUNTIME();
  if (runtime && address) runtime->GetGuestHeap().SystemHeapFree(address);
}

}  // namespace rexglue_guide::guest
