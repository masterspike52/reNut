// A starting point for a native mod: hook one guest function, look at its arguments, and let it run.
//
// Find the address of the function you want in a disassembly of the game's XEX. Everything the mod can do is
// in rexfabric/api.h.

#include <cstdio>

#include <rexfabric/api.h>

namespace {

// The guest function to hook. Replace with a real address from your game.
constexpr uint32_t kTarget = 0x82000000;

const RexFabricApi* api = nullptr;
RexFabricMod* self = nullptr;

void OnTarget(RexFabricCall* call, void*) {
  // Arguments arrive in r3 to r10. Change them with set_gpr before call_next to change what the game sees.
  const uint64_t first = api->get_gpr(call, 3);
  // Runs the original function (and any later hooks). Leave this out to replace the function entirely.
  api->call_next(call);
  // The result is in r3 now; set_gpr(call, 3, ...) changes what the caller gets.
  char line[64];
  std::snprintf(line, sizeof(line), "called with r3 = %llX", static_cast<unsigned long long>(first));
  api->log(self, REXFABRIC_LOG_DEBUG, line);
}

}  // namespace

extern "C" REXFABRIC_MOD_EXPORT int RexFabricModInit(const RexFabricApi* host, RexFabricMod* mod) {
  if (host->version < 1) return 0;
  api = host;
  self = mod;
  if (!api->hook(self, kTarget, OnTarget, nullptr)) {
    api->log(self, REXFABRIC_LOG_ERROR, "the target isn't a hookable function");
    return 0;
  }
  return 1;
}
