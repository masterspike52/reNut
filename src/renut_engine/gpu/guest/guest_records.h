/**
 * @file    gpu/guest/guest_records.h
 * @brief   Host records for guest D3D objects, living in guest-visible memory.
 *
 * The renderer creates the D3D objects the game asks for itself. Each record
 * starts with the X360 struct the game reads (D3DResource header, fetch
 * constant...) and continues with the host state behind it, so the guest VA
 * the game holds is the record. Records come from one o1heap over a block of
 * the guest physical window, and a registry marks which VAs are ours: any
 * other VA (bundle-embedded resources, sentinels) is foreign.
 */
#pragma once

#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#include <rex/system/kernel_state.h>
#include <rex/types.h>

namespace renut::gpu {

enum class RecordKind : u32 {
  VertexShader,
  PixelShader,
  VertexDeclaration,
  Texture,
  Surface,
  VertexBuffer,
  IndexBuffer,
};

namespace records {

// Reserves the heap block. Once, before the first Create.
bool Init();

void *AllocRaw(size_t size);
void FreeRaw(void *host);
void Register(u32 guest_va, RecordKind kind);
void Unregister(u32 guest_va);
bool Lookup(u32 guest_va, RecordKind *out_kind);
// Changes whenever a record is unregistered: lookups cached under an older
// value may name a destroyed record.
uint64_t Generation();

// A block of the guest physical window for renderer-owned guest memory (the
// Begin*Vertices scratch). Never freed. 0 on failure.
u32 AllocPhysicalBlock(u32 size);

} // namespace records

// T must start with its X360 prefix and have 'RecordKind kind' and 'u32 self_va'.
template <typename T, typename... Args>
T *CreateRecord(RecordKind kind, Args &&...args) {
  static_assert(std::is_destructible_v<T>);
  void *host = records::AllocRaw(sizeof(T));
  if (!host)
    return nullptr;
  std::memset(host, 0, sizeof(T));  // defined X360 bytes before the caller fills them
  T *record = new (host) T(std::forward<Args>(args)...);
  record->kind = kind;
  record->self_va = REX_KERNEL_MEMORY()->HostToGuestVirtual(host);
  records::Register(record->self_va, kind);
  return record;
}

template <typename T>
void DestroyRecord(T *record) {
  if (!record)
    return;
  records::Unregister(record->self_va);
  record->~T();
  records::FreeRaw(record);
}

// Our record at guest_va if it is one of 'kind', else nullptr.
template <typename T>
T *FindRecord(u32 guest_va, RecordKind kind) {
  RecordKind found;
  if (!guest_va || !records::Lookup(guest_va, &found) || found != kind)
    return nullptr;
  return REX_KERNEL_MEMORY()->TranslateVirtual<T *>(guest_va);
}

} // namespace renut::gpu
