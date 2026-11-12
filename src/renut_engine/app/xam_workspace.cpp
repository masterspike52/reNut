// The SDK swaps XAM's 384 KiB title workspace for the XEX's 512 KiB one without telling XAM, which then
// allocates from a freed heap. These keep XAM's workspace calls on the live one; the rest go to the SDK.

#include <rex/kernel/guest_allocator.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr uint32_t kXamAllocWorkspace = 0x10000000;
constexpr uint32_t kXamAllocSystem = 0x04000000;
constexpr uint32_t kXamAllocNoZero = 0x00100000;
constexpr uint32_t kXamAllocPhysical = 0x40000000;

using Func = void (*)(PPCContext &, uint8_t *);

// The SDK's own export: the executable defines the override, so the runtime library has the original.
Func Original(const char *name) {
#if defined(_WIN32)
  HMODULE modules[512];
  DWORD bytes = 0;
  if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) {
    for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i) {
      if (modules[i] == GetModuleHandleW(nullptr))
        continue;
      if (auto found = reinterpret_cast<Func>(GetProcAddress(modules[i], name)))
        return found;
    }
  }
  return nullptr;
#else
  return reinterpret_cast<Func>(dlsym(RTLD_NEXT, name));
#endif
}

void CallOriginal(const char *name, Func &cache, PPCContext &ctx, uint8_t *base) {
  if (!cache)
    cache = Original(name);
  if (cache)
    cache(ctx, base);
}

std::mutex g_mutex;
std::unordered_set<uint32_t> g_blocks;  // live-workspace blocks handed out here

// XamAlloc's workspace path, as the SDK's, on the workspace the runtime has now.
uint32_t AllocWorkspace(uint32_t flags, uint32_t size, uint32_t out_ptr, uint8_t *base) {
  rex::kernel::TitleRuntime *runtime = REX_RUNTIME();
  if (!runtime)
    return rex::kernel::kXEUnexpected;
  std::lock_guard lock(g_mutex);
  rex::Result<uint32_t> address = runtime->GetGuestWorkspace().Allocate(size, !(flags & kXamAllocNoZero));
  if (!address)
    return rex::kernel::kXEOutOfMemory;
  g_blocks.insert(*address);
  const uint32_t value = rex::byte_swap(*address);
  std::memcpy(base + out_ptr, &value, sizeof(value));
  return rex::kernel::X_E_SUCCESS;
}

// The flags the SDK sends to the workspace: 0, or the workspace flag without the system one.
bool UsesWorkspace(uint32_t flags) {
  return !(flags & kXamAllocSystem) && (flags == 0 || (flags & kXamAllocWorkspace));
}

}  // namespace

// XamAlloc(flags, size, u32* out)
REX_HOOK_RAW(__imp__XamAlloc) {
  static Func original = nullptr;
  const uint32_t flags = ctx.r3.u32, size = ctx.r4.u32, out_ptr = ctx.r5.u32;
  if (!out_ptr || !UsesWorkspace(flags))
    return CallOriginal("__imp__XamAlloc", original, ctx, base);
  std::memset(base + out_ptr, 0, sizeof(uint32_t));
  ctx.r3.u64 = AllocWorkspace(flags, size, out_ptr, base);
}

// XamAllocEx(attributes, flags, size, u32* out)
REX_HOOK_RAW(__imp__XamAllocEx) {
  static Func original = nullptr;
  const uint32_t flags = ctx.r4.u32, size = ctx.r5.u32, out_ptr = ctx.r6.u32;
  if (!out_ptr || (flags & kXamAllocPhysical) || !UsesWorkspace(flags))
    return CallOriginal("__imp__XamAllocEx", original, ctx, base);
  std::memset(base + out_ptr, 0, sizeof(uint32_t));
  ctx.r3.u64 = AllocWorkspace(flags, size, out_ptr, base);
}

// XamFree(address)
REX_HOOK_RAW(__imp__XamFree) {
  static Func original = nullptr;
  const uint32_t address = ctx.r3.u32;
  {
    std::lock_guard lock(g_mutex);
    if (address && g_blocks.erase(address)) {
      if (rex::kernel::TitleRuntime *runtime = REX_RUNTIME())
        runtime->GetGuestWorkspace().Free(address);
      return;
    }
  }
  CallOriginal("__imp__XamFree", original, ctx, base);
}
