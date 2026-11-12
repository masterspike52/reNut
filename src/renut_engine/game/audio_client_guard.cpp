#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

REX_EXTERN(__imp__sub_8264EE20);
REX_EXTERN(__imp__rex_XAUDIO__CXenonRenderer__SetCallback);

namespace {

constexpr uint32_t kRendererClientHandle = 0x18;
constexpr auto kCallbackWait = std::chrono::milliseconds(500);

std::timed_mutex g_callbackLock;
std::atomic<bool> g_clientLive{ false };

uint32_t GuestLoad32(const uint8_t* base, uint32_t va) {
    return rex::byte_swap(*reinterpret_cast<const uint32_t*>(base + va));
}

}

REX_HOOK_RAW(sub_8264EE20) {
    std::lock_guard lock(g_callbackLock);
    if (!g_clientLive.load(std::memory_order_relaxed)) {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_8264EE20(ctx, base);
}

REX_HOOK_RAW(rex_XAUDIO__CXenonRenderer__SetCallback) {
    const uint32_t renderer = ctx.r3.u32;
    std::unique_lock lock(g_callbackLock, std::defer_lock);
    (void)lock.try_lock_for(kCallbackWait);
    g_clientLive.store(false, std::memory_order_relaxed);
    __imp__rex_XAUDIO__CXenonRenderer__SetCallback(ctx, base);
    g_clientLive.store(GuestLoad32(base, renderer + kRendererClientHandle) != 0,
                       std::memory_order_relaxed);
}
