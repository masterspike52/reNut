#include <rexfabric/guest_hooks.h>

#include <memory>
#include <mutex>
#include <unordered_map>

#include <rexfabric/rexfabric.h>

#include "hooks.h"

namespace rexfabric {
namespace {

// The game's hooks, kept for as long as they're attached. A chain may still hold a removed hook's user pointer
// for a guest thread already walking it, so removed functions are never freed either.
std::mutex g_mutex;
std::unordered_map<RexFabricHookId, GuestHookFn*> g_hooks;

void Run(RexFabricCall* call, void* user) {
  GuestCall guest(call);
  (*static_cast<GuestHookFn*>(user))(guest);
}

}  // namespace

PPCContext& GuestCall::ctx() const { return *call_->ctx; }
uint8_t* GuestCall::base() const { return call_->base; }
uint32_t GuestCall::address() const { return call_->address; }
void GuestCall::Next() { CallNext(*call_); }

RexFabricHookId HookGuest(uint32_t address, GuestHookFn fn) {
  auto* held = new GuestHookFn(std::move(fn));
  const RexFabricHookId id = Hooks::Get().Add(Host(), address, Run, held);
  if (!id) {
    delete held;
    return 0;
  }
  std::lock_guard lock(g_mutex);
  g_hooks[id] = held;
  return id;
}

bool UnhookGuest(RexFabricHookId id) {
  {
    std::lock_guard lock(g_mutex);
    g_hooks.erase(id);
  }
  return Hooks::Get().Remove(id);
}

}  // namespace rexfabric
