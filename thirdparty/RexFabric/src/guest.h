// Guest memory and registers, as the front ends see them.

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/kernel/title_runtime.h>
#include <rex/ppc/context.h>

namespace rexfabric::guest {

using Register = PPCRegister PPCContext::*;

inline constexpr Register kGprs[32] = {&PPCContext::r0, &PPCContext::r1, &PPCContext::r2, &PPCContext::r3, &PPCContext::r4, &PPCContext::r5, &PPCContext::r6, &PPCContext::r7, &PPCContext::r8, &PPCContext::r9, &PPCContext::r10, &PPCContext::r11, &PPCContext::r12, &PPCContext::r13, &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18, &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23, &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28, &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
inline constexpr Register kFprs[32] = {&PPCContext::f0, &PPCContext::f1, &PPCContext::f2, &PPCContext::f3, &PPCContext::f4, &PPCContext::f5, &PPCContext::f6, &PPCContext::f7, &PPCContext::f8, &PPCContext::f9, &PPCContext::f10, &PPCContext::f11, &PPCContext::f12, &PPCContext::f13, &PPCContext::f14, &PPCContext::f15, &PPCContext::f16, &PPCContext::f17, &PPCContext::f18, &PPCContext::f19, &PPCContext::f20, &PPCContext::f21, &PPCContext::f22, &PPCContext::f23, &PPCContext::f24, &PPCContext::f25, &PPCContext::f26, &PPCContext::f27, &PPCContext::f28, &PPCContext::f29, &PPCContext::f30, &PPCContext::f31};

// Null until the runtime has guest memory.
inline uint8_t* Translate(uint32_t address) {
  rex::kernel::TitleRuntime* runtime = REX_RUNTIME();
  if (!runtime) return nullptr;
  const rex::memory::GuestMembase membase = runtime->GetMembase();
  return membase.GetBase() ? membase.TranslateVirtual(address) : nullptr;
}

template <typename T>
T Read(uint32_t address) {
  using Bits = std::conditional_t<sizeof(T) == 1, uint8_t, std::conditional_t<sizeof(T) == 2, uint16_t, std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>>>;
  Bits bits;
  std::memcpy(&bits, Translate(address), sizeof(T));
  if constexpr (sizeof(T) > 1) bits = std::byteswap(bits);
  return std::bit_cast<T>(bits);
}

template <typename T>
void Write(uint32_t address, T value) {
  using Bits = std::conditional_t<sizeof(T) == 1, uint8_t, std::conditional_t<sizeof(T) == 2, uint16_t, std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>>>;
  Bits bits = std::bit_cast<Bits>(value);
  if constexpr (sizeof(T) > 1) bits = std::byteswap(bits);
  std::memcpy(Translate(address), &bits, sizeof(T));
}

}  // namespace rexfabric::guest
