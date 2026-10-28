/**
 * @file    online/rexliveless.cpp
 * @brief   online.h when online play runs on RexLiveless (the Rexglue Guide,
 *          thirdparty/rexglue_guide) instead of reNut's EOS layer.
 *
 * The Guide answers the game's Live and network imports itself, so reNut's
 * hooks on the XDK wrappers (identity, sessions, transport, qos, friends) are
 * not built and nothing here tracks the game's session: cheats stay as set.
 *
 * XStringVerify is the one the Guide can't answer: it is XDK code in the game,
 * a request to LIVE's string service (built by the XDK's request library,
 * XLiveBase 0x5000C in TU3) that nothing serves. N&B checks a blueprint's name
 * with it before sending (Sharing), and fails the send without an answer, so
 * every string passes.
 */

#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/system/kernel_state.h>
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

// DWORD XStringVerify(DWORD flags, const char *locale, WORD num_strings,
//                     const STRING_DATA *strings, DWORD results_size,
//                     STRING_VERIFY_RESPONSE *results, XOVERLAPPED *overlapped)
// STRING_VERIFY_RESPONSE is packed: WORD count at +0, HRESULT *results at +2
// (N&B's send screen, sub_825B5AC0, reads **(response + 2) == S_OK). The
// HRESULTs (S_OK, every string fine) go in the caller's buffer after it.
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
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, status);
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
