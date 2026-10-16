/**
 * @file    gpu/guest/callbacks.cpp
 */
#include "renut_engine/gpu/guest/callbacks.h"

#include <deque>
#include <mutex>
#include <vector>

#include <rex/hook.h>
#include <rex/ppc.h>

#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/render/textures.h"

namespace renut::gpu {

namespace {

struct Callback {
  uint64_t seq = 0;
  u32 function = 0;
  u32 context = 0;
};

std::mutex g_mutex;
std::deque<Callback> g_callbacks;  // inserted, not run yet, in order
uint64_t g_last_seq = 0;
// Set while a thread runs callbacks: they run one at a time, in order.
bool g_running = false;

} // namespace

void InsertCallback(u32 function, u32 context) {
  uint64_t seq;
  {
    std::lock_guard lock(g_mutex);
    seq = ++g_last_seq;
    g_callbacks.push_back({seq, function, context});
  }
  // The linear textures of the work before it are read now, not when the
  // command list is submitted: after the callback the game may rewrite them
  // (the XMV player's frames).
  render_thread::Queue([] { FlushTextureFillsLocked(); });
  render_thread::QueueCallbackMarker(seq);
}

uint64_t LastCallback() {
  std::lock_guard lock(g_mutex);
  return g_last_seq;
}

void RunReadyCallbacks() {
  const uint64_t reached = render_thread::CallbacksReached();
  std::vector<Callback> ready;
  {
    std::lock_guard lock(g_mutex);
    if (g_running || g_callbacks.empty() || g_callbacks.front().seq > reached)
      return;
    while (!g_callbacks.empty() && g_callbacks.front().seq <= reached) {
      ready.push_back(g_callbacks.front());
      g_callbacks.pop_front();
    }
    g_running = true;
  }
  for (const Callback &callback : ready) {
    PPCFunc *function = rex::runtime::ResolveIndirectFunction(callback.function);
    if (!function) {
      RGPU_ERROR("InsertCallback: no guest function at {:08X}", callback.function);
      continue;
    }
    rex::ppc::GuestToHostFunction<void>(function, callback.context);
  }
  {
    std::lock_guard lock(g_mutex);
    g_running = false;
  }
  render_thread::CallbacksRun(ready.back().seq);
}

} // namespace renut::gpu
