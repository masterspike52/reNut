#pragma once

#include <rex/ppc/hook.h>
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

// Guest scratch space under the thread's own r1, for imports called as Fn(g.ctx(), base, ...).
// r1 comes back when this goes out of scope; each import opens its own call frame below it.
class Guest {
 public:
  Guest(PPCContext &ctx, uint8_t *base)
      : ctx_(ctx), base_(base), saved_r1_(ctx.r1.u64), low_(ctx.r1.u32 - kGap) {
    ctx_.r1.u32 = low_;
  }
  ~Guest() { ctx_.r1.u64 = saved_r1_; }
  Guest(const Guest &) = delete;
  Guest &operator=(const Guest &) = delete;

  // Zeroed scratch space for the calls made through this.
  uint32_t Alloc(uint32_t size) {
    low_ -= (size + 15) & ~15u;
    std::memset(base_ + low_, 0, size);
    ctx_.r1.u32 = low_;
    return low_;
  }

  // A copy of 'data' in the scratch space.
  uint32_t Push(const void *data, uint32_t size) {
    const uint32_t va = Alloc(size);
    std::memcpy(base_ + va, data, size);
    return va;
  }

  PPCContext &ctx() { return ctx_; }

 private:
  static constexpr uint32_t kGap = 0x100;
  PPCContext &ctx_;
  uint8_t *base_;
  uint64_t saved_r1_;
  uint32_t low_;
};

}  // namespace renut::guest
