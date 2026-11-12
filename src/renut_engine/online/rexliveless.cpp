#include <cstdint>
#include <cstring>

#include <rex/kernel/thread_registry.h>
#include <rex/module.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>

#include "renut_engine/online/online.h"

namespace {

void Store16(uint8_t *p, uint16_t v) {
  v = rex::byte_swap(v);
  std::memcpy(p, &v, 2);
}

void Store32(uint8_t *p, uint32_t v) {
  v = rex::byte_swap(v);
  std::memcpy(p, &v, 4);
}

} // namespace

// XStringVerify, which nothing serves: every string passes (packed response,
// WORD count at +0, HRESULT *results at +2 read by sub_825B5AC0, all S_OK).
REX_HOOK_RAW(rex_XStringVerify) {
  constexpr uint32_t kHeaderSize = 6;
  constexpr uint32_t kErrorInsufficientBuffer = 122;
  constexpr uint32_t kErrorIoPending = 997;
  const uint32_t count = ctx.r5.u32 & 0xFFFF;
  const uint32_t results_size = ctx.r7.u32;
  const uint32_t results = ctx.r8.u32;
  const uint32_t overlapped = ctx.r9.u32;
  uint32_t status = 0;
  if (!results || results_size < kHeaderSize + 4 * count) {
    status = kErrorInsufficientBuffer;
  } else {
    // The HRESULTs word-aligned when there's room, else straight after.
    const uint32_t at = results_size >= 8 + 4 * count ? 8 : kHeaderSize;
    uint8_t *out = base + results;
    std::memset(out, 0, at + 4 * count);
    Store16(out, uint16_t(count));
    Store32(out + 2, results + at);
  }
  if (overlapped) {
    REX_INTERFACE(rex::kernel::IOverlappedDispatcher)->Complete(overlapped, {status});
    ctx.r3.u64 = kErrorIoPending;
  } else {
    ctx.r3.u64 = status;
  }
}

namespace renut::online {

bool Enabled() { return false; }
std::string PlayerName() { return "Player"; }
void SetPlayerName(std::string_view) {}
uint64_t PlayerXuid() { return 0; }

bool InSession() { return false; }
bool HostingSession() { return false; }
std::string SessionHostId() { return {}; }

bool SessionAllowsCheats() { return true; }
bool CanAllowSessionCheats() { return true; }
void ForgetSessionCheats() {}

} // namespace renut::online
