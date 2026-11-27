#include "renut_engine/game/audio_driver.h"

#include <rex/ppc/context.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <semaphore>

#include "renut_engine/game/guest_call.h"

REX_IMPORT(__imp__rex_XAUDIO__CXenonRenderer__SetCallback, RendererSetCallback, u32(u32, u32));
REX_IMPORT(__imp__rex_XAUDIO__CXenonRenderer__Process, RendererProcess, u32(u32, u32));
REX_IMPORT(__imp__rex_XAUDIO__CXenonRenderer__CaptureXBMAudioFrame, RendererCapture, u32(u32, u32));
REX_IMPORT(__imp__sub_8264EE20, EngineRender, u32(u32));  // the engine's render callback: rex_XAUDIO__CEngine__Process
REX_IMPORT(__imp__ExCreateThread, ExCreateThread, u32(u32, u32, u32, u32, u32, u32, u32));
REX_IMPORT(__imp__NtClose, NtClose, u32(u32));
REX_IMPORT(__imp__KeResetEvent, KeResetEvent, u32(u32));
REX_IMPORT(__imp__rex_XAUDIO__CEngine__Initialize, EngineInitialize, u32(u32, u32));

namespace renut::game::audio_driver {
namespace {

using renut::guest::Load32;

constexpr uint32_t kEngineRender = 0x8264EE20;
constexpr uint32_t kEngine = 0x82E1AA04;         // the XAudio engine, set once its Initialize has returned
constexpr uint32_t kRendererArg = 0x0C;          // CXenonRenderer: its callback's argument
constexpr uint32_t kRendererClient = 0x18;       // and its kernel client handle, 0 while reNut renders it
constexpr uint32_t kPacketInfo = 0x14 - 8;       // Process's packet: 12 bytes the capture reads, samples at + 8
constexpr uint32_t kPumpContext = 0x52415544;    // sub_8264EE20 started as reNut's audio thread, not as a callback
constexpr auto kCreditWait = std::chrono::milliseconds(20);

// The engine's events, which its Initialize sets up by writing their headers: {address, event type it writes}.
// The kernel keeps a host object per address, so a signal left by the last engine would outlive the rewrite.
struct EngineEvent {
  uint32_t address;
  uint8_t type;
};
constexpr EngineEvent kEngineEvents[] = {
    {0x82E14DF0, 1},  // a processing thread finished its frame
    {0x82E14E14, 1},  // wakes the processing threads
    {0x82E14E34, 0},  // the title is terminating
};

using Frame = std::array<float, rex::apu::kAudioFrameSamples>;

rex::apu::IAudioSystem *g_audio = nullptr;
rex::apu::AudioStreamID g_stream{};
uint32_t g_queued = 0;
// One credit for each frame the stream has room for: every credit taken submits exactly one frame.
std::counting_semaphore<64> g_credits{0};

std::mutex g_mutex;  // held while the engine renders, so SetCallback(0) waits for a render in progress
std::atomic<uint32_t> g_renderer{0};
uint32_t g_callback_arg = 0;
std::atomic<bool> g_submitted{false};
std::atomic<bool> g_failed{false};  // reNut's audio thread didn't start: the kernel's render clients are used
std::once_flag g_started;

bool Ready() { return g_audio && !g_failed.load(); }

void Submit(const Frame &frame) {
  (void)g_audio->Submit(g_stream, frame);
  g_submitted = true;
}

// The engine's mastering frame: 6 channels one after another, 256 big-endian floats each.
void SubmitGuestFrame(const uint8_t *base, uint32_t samples) {
  Frame frame;
  for (uint32_t i = 0; i < frame.size(); ++i)
    frame[i] = std::bit_cast<float>(Load32(base, samples + 4 * i));
  Submit(frame);
}

// On the guest thread registering the first engine: reNut's audio thread, sub_8264EE20 with kPumpContext.
void StartPump() {
  const rex::ppc::BoundThread bound = rex::ppc::GetBoundThread();
  renut::guest::Guest g(*bound.ctx, bound.base);
  const uint32_t handle = g.Alloc(4);
  if (ExCreateThread(handle, 0, 0, 0, kEngineRender, kPumpContext, 0) != 0) {
    g_failed = true;
    return;
  }
  NtClose(Load32(bound.base, handle));
}

// The audio thread: one engine render per frame the stream plays, or silence while no engine is registered.
uint32_t Pump(const rex::ppc::BoundThread &bound) {
  static const Frame kSilence{};
  g_credits.release(g_queued);
  for (;;) {
    if (bound.ctx->stop_requested && bound.ctx->stop_requested->load(std::memory_order_acquire))
      return 0;
    if (!g_credits.try_acquire_for(kCreditWait))
      continue;
    g_submitted = false;
    {
      std::lock_guard lock(g_mutex);
      if (g_renderer.load() && Load32(bound.base, kEngine))
        EngineRender(g_callback_arg);
    }
    if (!g_submitted)
      Submit(kSilence);
  }
}

// u32 sub_8264EE20(arg): the engine's render callback, and reNut's audio thread when started with kPumpContext.
uint32_t EngineRender_hook(uint32_t context) {
  if (context != kPumpContext)
    return EngineRender(context);
  return Pump(rex::ppc::GetBoundThread());
}

// u32 CXenonRenderer::SetCallback(renderer, callback): reNut takes the engine's render callback instead of the kernel.
uint32_t SetCallback_hook(uint32_t renderer, uint32_t callback) {
  if (!Ready() || (callback && callback != kEngineRender))
    return RendererSetCallback(renderer, callback);
  const uint8_t *base = rex::ppc::GetBoundThread().base;
  if (Load32(base, renderer + kRendererClient))
    RendererSetCallback(renderer, 0);  // a kernel client from before goes
  if (callback)
    std::call_once(g_started, StartPump);
  if (!Ready())
    return RendererSetCallback(renderer, callback);
  std::lock_guard lock(g_mutex);
  if (callback) {
    g_callback_arg = Load32(base, renderer + kRendererArg);
    g_renderer = renderer;
  } else if (g_renderer.load() == renderer) {
    g_renderer = 0;
    g_callback_arg = 0;
  }
  return 0;
}

// u32 CXenonRenderer::Process(renderer, packet): the mastering frame goes to reNut's stream, not the kernel's.
uint32_t Process_hook(uint32_t renderer, uint32_t packet) {
  const rex::ppc::BoundThread bound = rex::ppc::GetBoundThread();
  uint8_t *base = bound.base;
  if (!Ready() || Load32(base, renderer + kRendererClient))
    return RendererProcess(renderer, packet);
  if (!packet)
    return 0;
  renut::guest::Guest g(*bound.ctx, base);
  const uint32_t info = g.Alloc(12);
  std::memcpy(base + info, base + packet + kPacketInfo, 12);
  RendererCapture(renderer, info);
  SubmitGuestFrame(base, Load32(base, info + 8));
  return 0;
}

// u32 CEngine::Initialize(engine, params): a new engine's events start unsignalled, as a rewritten header does on a
// 360. Without it, the movie engine's last "frame finished" makes the next engine's processing thread quit.
uint32_t EngineInitialize_hook(uint32_t engine, uint32_t params) {
  const uint8_t *base = rex::ppc::GetBoundThread().base;
  for (const EngineEvent &event : kEngineEvents)
    if (base[event.address] == event.type)  // an event from an earlier engine, not untouched memory
      KeResetEvent(event.address);
  return EngineInitialize(engine, params);
}

} // namespace

void Attach(rex::apu::IAudioSystem &audio) {
  if (g_audio)
    return;
  auto stream = audio.OpenStream([] { g_credits.release(); });
  if (!stream)
    return;
  g_stream = *stream;
  g_queued = audio.GetSettings().max_queued_frames.Get();
  g_audio = &audio;
}

} // namespace renut::game::audio_driver

REX_HOOK(sub_8264EE20, renut::game::audio_driver::EngineRender_hook);
REX_HOOK(rex_XAUDIO__CXenonRenderer__SetCallback, renut::game::audio_driver::SetCallback_hook);
REX_HOOK(rex_XAUDIO__CXenonRenderer__Process, renut::game::audio_driver::Process_hook);
REX_HOOK(rex_XAUDIO__CEngine__Initialize, renut::game::audio_driver::EngineInitialize_hook);
