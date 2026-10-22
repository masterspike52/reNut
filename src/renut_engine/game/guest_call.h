/**
 * @file    game/guest_call.h
 * @brief   Calling the game's own functions from a hook, and its memory.
 *
 * Guest: an isolated context for calls made through REX_IMPORT callables
 * (rex::CallFrame), whose stack pointer sits under the scratch space handed out
 * (out-parameters, structs), leaving a callee room to write its caller's
 * parameter area. Big-endian loads and stores of guest memory.
 */
#pragma once

#include <rex/hook.h>
#include <rex/types.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace renut::guest {

inline uint32_t Load32(const uint8_t *base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return rex::byte_swap(v);
}

inline uint16_t Load16(const uint8_t *base, uint32_t va) {
  uint16_t v;
  std::memcpy(&v, base + va, 2);
  return rex::byte_swap(v);
}

inline void Store32(uint8_t *base, uint32_t va, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(base + va, &value, 4);
}

inline void Store16(uint8_t *base, uint32_t va, uint16_t value) {
  value = rex::byte_swap(value);
  std::memcpy(base + va, &value, 2);
}

// The guest return addresses above a hook, for logs: the link register, then
// each frame's saved one (N&B's prologs store LR at -8 from the caller's stack
// pointer, then stwu r1, so 0(r1) is the back chain). Guest addresses, as IDA
// shows them.
inline std::string GuestBacktrace(const PPCContext &ctx, const uint8_t *base, int depth = 12) {
  auto code = [](uint32_t va) { return va >= 0x82000000u && va < 0x83000000u; };
  std::string out;
  char buffer[16];
  if (code(ctx.lr)) {
    std::snprintf(buffer, sizeof(buffer), "%08X", uint32_t(ctx.lr));
    out += buffer;
  }
  uint32_t sp = ctx.r1.u32;
  for (int i = 0; i < depth && sp; ++i) {
    const uint32_t next = Load32(base, sp);
    if (next <= sp)
      break;
    sp = next;
    const uint32_t lr = Load32(base, sp - 8);
    if (!code(lr))
      break;
    std::snprintf(buffer, sizeof(buffer), "%s%08X", out.empty() ? "" : " < ", lr);
    out += buffer;
  }
  return out;
}

class Guest {
 public:
  Guest(PPCContext &ctx, uint8_t *base) : frame_(ctx), base_(base), low_(ctx.r1.u32 - kGap) {
    frame_.ctx.r1.u32 = low_ - kCalleeArea;
  }

  // Zeroed scratch space for the calls made through this.
  uint32_t Alloc(uint32_t size) {
    low_ -= (size + 15) & ~15u;
    std::memset(base_ + low_, 0, size);
    frame_.ctx.r1.u32 = low_ - kCalleeArea;
    return low_;
  }

  // A copy of 'data' in the scratch space.
  uint32_t Push(const void *data, uint32_t size) {
    const uint32_t va = Alloc(size);
    std::memcpy(base_ + va, data, size);
    return va;
  }

  rex::CallFrame &frame() { return frame_; }

 private:
  static constexpr uint32_t kGap = 0x100;
  static constexpr uint32_t kCalleeArea = 0x60;
  rex::CallFrame frame_;
  uint8_t *base_;
  uint32_t low_;
};

}  // namespace renut::guest
