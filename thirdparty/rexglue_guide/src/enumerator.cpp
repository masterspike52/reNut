#include "enumerator.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <rex/kernel/object/object_table.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/context.h>

#include "guest.h"

namespace rexglue_guide::enumerator {
namespace {

constexpr uint32_t kHeaderSize = 24;  // X_KENUMERATOR, before the private data

}  // namespace

uint8_t* StaticEnumerator::AppendItem() {
  items_.resize(items_.size() + item_size());
  ++count_;
  return items_.data() + items_.size() - item_size();
}

uint32_t StaticEnumerator::WriteItems(uint32_t, uint8_t* buffer, uint32_t* written_count) {
  const uint32_t count = std::min(count_ - current_, items_per_enumerate());
  if (!count) return kNoMoreFiles;
  std::memcpy(buffer, items_.data() + size_t(current_) * item_size(), size_t(count) * item_size());
  current_ += count;
  if (written_count) *written_count = count;
  return 0;
}

uint32_t Create(std::unique_ptr<Enumerator> e, uint32_t user, uint32_t app, uint32_t open, uint32_t close,
                uint32_t flags, uint32_t extra, uint8_t** private_data) {
  if (private_data) *private_data = nullptr;
  rex::kernel::TitleRuntime* runtime = REX_RUNTIME();
  if (!runtime || !e) return 0;
  rex::kernel::IObjectTable& table = runtime->GetObjectTable();
  const uint32_t items = e->items_per_enumerate();
  const rex::kernel::ObjectBlock block =
      table.AllocateObjectBlock(kHeaderSize + extra, rex::kernel::ObjectType::kXAM, rex::kernel::ObjectBlockHeap::kSystem);
  uint8_t* header = guest::At(block.address);
  if (!header) return 0;
  std::memset(header, 0, kHeaderSize + extra);
  guest::Store32(header + 0, app);
  guest::Store32(header + 4, open);
  guest::Store32(header + 8, close);
  guest::Store32(header + 12, user);
  guest::Store32(header + 16, items);
  guest::Store32(header + 20, flags);
  const auto handle = table.AddXAMObject(std::move(e), {.block = block});
  if (!handle) return 0;
  if (private_data && extra) *private_data = header + kHeaderSize;
  return *handle;
}

}  // namespace rexglue_guide::enumerator

using rexglue_guide::enumerator::Enumerator;
using namespace rexglue_guide::guest;

namespace {

rex::kernel::Pinned<Enumerator> Find(uint32_t handle) {
  rex::kernel::TitleRuntime* runtime = REX_RUNTIME();
  if (!runtime) return {};
  return runtime->GetObjectTable().Get<Enumerator>(handle);
}

}  // namespace

// XamEnumerate(handle, flags, buffer, buffer size, DWORD* items returned, overlapped): the
// Guide's enumerators here, any other to the runtime's.
extern "C" REX_FUNC(__imp__XamEnumerate) {
  const rex::kernel::Pinned<Enumerator> e = Find(ctx.r3.u32);
  if (!e) return CallOriginal("__imp__XamEnumerate", ctx, base);
  const uint32_t buffer_ptr = ctx.r5.u32, buffer_length = ctx.r6.u32;
  const uint32_t returned_ptr = ctx.r7.u32, overlapped = ctx.r8.u32;
  uint8_t* buffer = At(buffer_ptr);
  uint32_t count = 0;
  uint32_t result = rex::kernel::X_ERROR_INSUFFICIENT_BUFFER;
  if (buffer && buffer_length >= e->item_size()) {
    std::memset(buffer, 0, buffer_length);
    result = e->WriteItems(buffer_ptr, buffer, &count);
  }
  if (uint8_t* returned = At(returned_ptr)) {
    Store32(returned, result == 0 ? count : 0);
    return Return(ctx, result);
  }
  if (overlapped) {
    CompleteOverlapped(overlapped, result, result, result == 0 ? count : 0);
    return Return(ctx, rex::kernel::X_ERROR_IO_PENDING);
  }
  Return(ctx, rex::kernel::X_ERROR_INVALID_PARAMETER);
}

// XamGetPrivateEnumStructureFromHandle(handle, void** object): the enumerator's guest block,
// referenced, as the title dereferences it (ObDereferenceObject) when it's done.
extern "C" REX_FUNC(__imp__XamGetPrivateEnumStructureFromHandle) {
  const rex::kernel::Pinned<Enumerator> e = Find(ctx.r3.u32);
  if (!e) return CallOriginal("__imp__XamGetPrivateEnumStructureFromHandle", ctx, base);
  uint8_t* out = At(ctx.r4.u32);
  if (!out) return Return(ctx, rex::kernel::X_ERROR_INVALID_PARAMETER);
  (void)REX_RUNTIME()->GetObjectTable().Reference(e.GetSlot());
  Store32(out, e.GetGuestAddress());
  Return(ctx, rex::kernel::X_ERROR_SUCCESS);
}
