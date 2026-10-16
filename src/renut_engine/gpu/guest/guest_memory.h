/**
 * @file    gpu/guest/guest_memory.h
 * @brief   Guest virtual address translation for the renderer.
 */
#pragma once

#include <rex/system/kernel_state.h>
#include <rex/types.h>

namespace renut::gpu {

// Host pointer for a guest VA, or nullptr for VA 0.
template <typename T>
inline T *GuestPtr(u32 guest_address) {
  if (!guest_address)
    return nullptr;
  return REX_KERNEL_MEMORY()->TranslateVirtual<T *>(guest_address);
}

// Host pointer for a GPU physical address (fetch constants, index buffer
// addresses): the low 29 bits index physical memory.
template <typename T>
inline T *GuestPhysicalPtr(u32 physical_address) {
  return REX_KERNEL_MEMORY()->TranslatePhysical<T *>(physical_address);
}

} // namespace renut::gpu
