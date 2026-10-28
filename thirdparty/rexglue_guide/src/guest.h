#pragma once
// Helpers for the XAM/XNet functions the Guide defines in place of the
// rexglue runtime's (see xam_live.cpp, net.cpp).

#include <cstdint>

#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

namespace rexglue_guide::guest {

using Func = void (*)(PPCContext&, uint8_t*);

// The runtime's own implementation of an import, by symbol name.
Func Original(const char* name);

inline void CallOriginal(const char* name, PPCContext& ctx, uint8_t* base) {
  if (Func f = Original(name)) f(ctx, base);
}

inline uint8_t* At(uint32_t address) {
  return address ? REX_KERNEL_MEMORY()->TranslateVirtual(address) : nullptr;
}

inline uint32_t Load32(const uint8_t* p) {
  return rex::memory::load_and_swap<uint32_t>(p);
}
inline uint16_t Load16(const uint8_t* p) {
  return rex::memory::load_and_swap<uint16_t>(p);
}
inline uint64_t Load64(const uint8_t* p) {
  return rex::memory::load_and_swap<uint64_t>(p);
}
inline void Store32(uint8_t* p, uint32_t v) {
  rex::memory::store_and_swap<uint32_t>(p, v);
}
inline void Store16(uint8_t* p, uint16_t v) {
  rex::memory::store_and_swap<uint16_t>(p, v);
}
inline void Store64(uint8_t* p, uint64_t v) {
  rex::memory::store_and_swap<uint64_t>(p, v);
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

}  // namespace rexglue_guide::guest
