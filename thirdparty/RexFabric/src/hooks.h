// The hook registry: which hooks sit on which guest function, and the dispatch that runs them.
//
// A slot's chain is immutable once published. Adding or removing a hook builds a new chain and swaps it in,
// so a guest thread already walking the old chain finishes on it undisturbed. Replaced chains are never freed,
// since nothing tracks when the last thread leaves one; hooks change rarely, so they cost little.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rexfabric/api.h>
#include <rexfabric/hook_slot.h>

namespace rexfabric {

struct Hook {
  RexFabricHookId id;
  RexFabricHookFn fn;
  void* user;
  RexFabricMod* owner;
};

struct HookChain {
  std::vector<Hook> hooks;
};

class Hooks {
 public:
  static Hooks& Get();

  // Indexes the generated slot table. Safe to call more than once.
  void Init();

  bool IsHookable(uint32_t address) const { return Find(address) != nullptr; }
  RexFabricHookId Add(RexFabricMod* owner, uint32_t address, RexFabricHookFn fn, void* user);
  bool Remove(RexFabricHookId id);
  void RemoveAll(const RexFabricMod* owner);

 private:
  HookSlot* Find(uint32_t address) const;
  const HookChain* Current(const HookSlot& slot) const { return slot.chain.load(std::memory_order_acquire); }
  void Publish(HookSlot& slot, std::vector<Hook> hooks);

  struct Placement {
    HookSlot* slot;
    const RexFabricMod* owner;
  };

  std::vector<SlotEntry> slots_;  // sorted by address
  mutable std::mutex mutex_;
  std::unordered_map<RexFabricHookId, Placement> placed_;
  std::vector<std::unique_ptr<HookChain>> chains_;  // every chain ever published
  RexFabricHookId next_id_ = 1;
};

}  // namespace rexfabric

// A hooked call in progress. Hooks see it as an opaque RexFabricCall.
struct RexFabricCall {
  PPCContext* ctx;
  uint8_t* base;
  PPCFunc* original;
  const rexfabric::HookChain* chain;
  size_t next;
  uint32_t address;
};

namespace rexfabric {

// Runs the next hook in the call's chain, or the original function once the hooks are used up.
void CallNext(RexFabricCall& call);

}  // namespace rexfabric
