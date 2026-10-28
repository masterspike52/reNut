// Linux only (CMakeLists.txt): the freeze after the Microsoft logo.
//
// The logo is a movie. Its audio renderer (CCalAudioRenderer) starts XAudio
// when it opens and shuts it down in Close (sub_82A69AC0 → XAudioShutDown
// sub_8264BD00 → CEngine::~CEngine sub_8264C560). The engine's teardown first
// takes its render client away (CXenonRenderer::SetCallback(0), sub_8264FBF8:
// XAudioUnregisterRenderDriverClient), then stops its processing threads and
// frees its voices: it counts on no render callback running once the client
// is unregistered.
//
// The SDK doesn't wait for one. Its audio worker (AudioSystem::WorkerThreadMain)
// reads the client's callback under its global lock, lets the lock go and runs
// the callback (sub_8264EE20, CEngine::Process). One that is running when the
// movie ends submits its frame after the unregister, to a client slot without a
// driver, and AudioSystem::SubmitFrame reads the null driver holding the global
// lock. On Linux the SDK's signal handler returns from the fault unhandled, the
// instruction faults again for ever, and every guest thread waiting on that lock
// stops: the game freezes, the window keeps drawing. Windows is unaffected.
//
// So the render callback runs under a lock that SetCallback also holds: an
// unregister waits for a running callback to finish, and a callback the worker
// picked up before it skips once the client is gone. A register also holds it
// until the client's handle is written, which the callback submits to.

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

REX_EXTERN(__imp__sub_8264EE20);
REX_EXTERN(__imp__rex_XAUDIO__CXenonRenderer__SetCallback);

namespace {

// CXenonRenderer + 0x18: its render client's handle (0 when it has none).
constexpr uint32_t kRendererClientHandle = 0x18;

// A callback takes well under a frame (256 samples, 5.3 ms). XAudio's own
// lock (0x82D9F748) is held across SetCallback by XAudioShutDown, and a
// callback that waited on it would never finish: past this the client is
// taken away without waiting, as before.
constexpr auto kCallbackWait = std::chrono::milliseconds(500);

std::timed_mutex g_callbackLock;
std::atomic<bool> g_clientLive{ false };

uint32_t GuestLoad32(const uint8_t* base, uint32_t va) {
    return rex::byte_swap(*reinterpret_cast<const uint32_t*>(base + va));
}

}  // namespace

// The engine's render callback, CEngine::Process (sub_8264D098), on the SDK's
// audio worker.
REX_HOOK_RAW(sub_8264EE20) {
    std::lock_guard lock(g_callbackLock);
    if (!g_clientLive.load(std::memory_order_relaxed)) {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_8264EE20(ctx, base);
}

// HRESULT CXenonRenderer::SetCallback(CXenonRenderer*, callback): unregisters
// the client it has, then registers one for 'callback' unless it is 0.
REX_HOOK_RAW(rex_XAUDIO__CXenonRenderer__SetCallback) {
    const uint32_t renderer = ctx.r3.u32;
    std::unique_lock lock(g_callbackLock, std::defer_lock);
    if (!lock.try_lock_for(kCallbackWait))
        REXLOG_WARN("XAudio: the render callback still runs after {} ms; changing its client anyway",
                    kCallbackWait.count());
    g_clientLive.store(false, std::memory_order_relaxed);
    __imp__rex_XAUDIO__CXenonRenderer__SetCallback(ctx, base);
    g_clientLive.store(GuestLoad32(base, renderer + kRendererClientHandle) != 0,
                       std::memory_order_relaxed);
}
