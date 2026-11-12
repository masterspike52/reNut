#pragma once

#include <rex/kernel/title_runtime.h>
#include <rex/types.h>

namespace renut::gpu {

// Host pointer for a guest VA, or nullptr for VA 0 or once the title has unloaded.
template <typename T>
inline T *GuestPtr(u32 guest_address) {
  rex::kernel::TitleRuntime *runtime = REX_RUNTIME();
  if (!guest_address || !runtime)  // no title: its memory is unmapped
    return nullptr;
  return runtime->GetMembase().TranslateVirtual<T *>(guest_address);
}

// Host pointer for a GPU physical address (fetch constants, index buffer
// addresses): the low 29 bits index physical memory, mapped at base + 4 GiB.
template <typename T>
inline T *GuestPhysicalPtr(u32 physical_address) {
  constexpr u64 kPhysicalHostOffset = 0x100000000ull;
  rex::kernel::TitleRuntime *runtime = REX_RUNTIME();
  if (!runtime)  // no title: its memory is unmapped
    return nullptr;
  return reinterpret_cast<T *>(runtime->GetMembase().GetBase() + kPhysicalHostOffset +
                               (physical_address & 0x1FFFFFFFu));
}

} // namespace renut::gpu
