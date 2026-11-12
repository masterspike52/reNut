// Hooks for the game's own C++ on guest functions, with the guest's registers and memory at hand.
//
// Mods hook through RexFabricApi. The game's own code works with PPCContext anyway, so it hooks here instead,
// in the same chains: a game hook added after the mods load runs after their hooks on the same function. A hook
// runs on the guest thread making the call; Next runs the rest of the chain and the original function, and a hook
// that returns without it replaces the function.
//
//   rexfabric::HookGuest(0x821F0B18, [](rexfabric::GuestCall& call) {
//     BeforeView(call.ctx(), call.base());
//     call.Next();
//     AfterView(call.ctx(), call.base());
//   });

#pragma once

#include <cstdint>
#include <functional>

#include <rex/ppc/context.h>
#include <rexfabric/api.h>

namespace rexfabric {

class GuestCall {
 public:
  explicit GuestCall(RexFabricCall* call) : call_(call) {}

  PPCContext& ctx() const;
  uint8_t* base() const;
  // The hooked function.
  uint32_t address() const;
  // Runs the rest of the chain and the original function.
  void Next();

 private:
  RexFabricCall* call_;
};

using GuestHookFn = std::function<void(GuestCall& call)>;

// 0 when the address isn't hookable. The hook belongs to Host().
RexFabricHookId HookGuest(uint32_t address, GuestHookFn fn);
bool UnhookGuest(RexFabricHookId id);

}  // namespace rexfabric
