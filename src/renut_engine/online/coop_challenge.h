#pragma once

#include <rex/ppc/hook.h>
#include <rex/types.h>

#include <cstdint>
#include <string>

namespace renut::online::coop_challenge {

// From net_api::Declare: renut.net's co-op challenge functions and events (renut.coop_challenge_*).
void Declare();

// Game thread, once a frame from net_api::Tick: this machine's challenge objects sent, the host's rewards
// applied, the events the hooks queued fired.
void NetTick(PPCContext &ctx, uint8_t *base);

// A message this owns (renut.coop_ch_objs, renut.coop_progress): taken, true.
bool TakeMessage(PPCContext &ctx, uint8_t *base, uint32_t from, const std::string &name, const std::string &data);

// While a co-op world is played, challengeMan code runs with byte_82E19934 cleared, as in single player.
class SinglePlayerScope {
 public:
  SinglePlayerScope(uint8_t *base, bool on);
  ~SinglePlayerScope();
  SinglePlayerScope(const SinglePlayerScope &) = delete;
  SinglePlayerScope &operator=(const SinglePlayerScope &) = delete;

 private:
  uint8_t *base_;
  bool on_;
  uint8_t saved_ = 0;
};

// Whether challenges run as single player here (a co-op world in multiplayer).
bool Active(const uint8_t *base);

// challengeManTick (frameHooks): in co-op it runs the original itself and returns true, else false.
bool RunTick(PPCContext &ctx, uint8_t *base, uint32_t man);

// BeginChallenge (part_api's hook): before the original (whether to scope it), and after it.
bool BeginStarting(const uint8_t *base, uint32_t man);
void BeginDone(const uint8_t *base, uint32_t man, uint32_t flag);

// The settle of state 14 (part_api's hook): true while this machine's group holds the end.
bool HoldSettle(PPCContext &ctx, uint8_t *base, uint32_t man);

// CreateObject (part_api's hook): true when a joiner's own copy of a networked challenge object isn't made
// (the starter's is used); and the objects made and removed, for the starter's list.
bool SkipObject(PPCContext &ctx, uint8_t *base, uint32_t copy, uint32_t net);
void ObjectMade(const uint8_t *base, uint32_t object);
void ObjectRemoved(uint32_t object);

// counterAdd (custom_npcs' hook): a guest's reward goes to the host too. True when the host drops a repeat.
bool CounterAdded(const uint8_t *base, uint32_t profile, uint32_t counter, uint32_t amount);

// For net_api's proxy gate: -1 when the net object isn't a challenge's, else whether it's made in `world`.
int ProxyVerdict(uint64_t id, uint32_t world);
// Whether another machine's challenge group plays in `world` now.
bool GroupInWorld(uint32_t world);

}  // namespace renut::online::coop_challenge
