#include "hooks.h"

#include <algorithm>

#include "log.h"

namespace rexfabric {

Hooks& Hooks::Get() {
  static Hooks hooks;
  return hooks;
}

void Hooks::Init() {
  std::lock_guard lock(mutex_);
  if (!slots_.empty()) return;
  slots_.assign(rexfabric_slots, rexfabric_slots + rexfabric_slot_count);
  std::sort(slots_.begin(), slots_.end(), [](const SlotEntry& a, const SlotEntry& b) { return a.address < b.address; });
  RF_INFO("{} hookable functions", slots_.size());
}

HookSlot* Hooks::Find(uint32_t address) const {
  auto it = std::lower_bound(slots_.begin(), slots_.end(), address, [](const SlotEntry& entry, uint32_t value) { return entry.address < value; });
  return it != slots_.end() && it->address == address ? it->slot : nullptr;
}

RexFabricHookId Hooks::Add(RexFabricMod* owner, uint32_t address, RexFabricHookFn fn, void* user) {
  if (!fn) return 0;
  std::lock_guard lock(mutex_);
  HookSlot* slot = Find(address);
  if (!slot) return 0;
  const HookChain* current = Current(*slot);
  std::vector<Hook> hooks = current ? current->hooks : std::vector<Hook>{};
  const RexFabricHookId id = next_id_++;
  hooks.push_back({id, fn, user, owner});
  Publish(*slot, std::move(hooks));
  placed_[id] = {slot, owner};
  return id;
}

bool Hooks::Remove(RexFabricHookId id) {
  std::lock_guard lock(mutex_);
  auto it = placed_.find(id);
  if (it == placed_.end()) return false;
  HookSlot& slot = *it->second.slot;
  placed_.erase(it);
  std::vector<Hook> hooks = Current(slot)->hooks;
  std::erase_if(hooks, [id](const Hook& hook) { return hook.id == id; });
  Publish(slot, std::move(hooks));
  return true;
}

void Hooks::RemoveAll(const RexFabricMod* owner) {
  std::lock_guard lock(mutex_);
  std::vector<HookSlot*> touched;
  std::erase_if(placed_, [&](const auto& entry) {
    if (entry.second.owner != owner) return false;
    touched.push_back(entry.second.slot);
    return true;
  });
  std::sort(touched.begin(), touched.end());
  touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
  for (HookSlot* slot : touched) {
    std::vector<Hook> hooks = Current(*slot)->hooks;
    std::erase_if(hooks, [owner](const Hook& hook) { return hook.owner == owner; });
    Publish(*slot, std::move(hooks));
  }
}

void Hooks::Publish(HookSlot& slot, std::vector<Hook> hooks) {
  if (hooks.empty()) {
    slot.chain.store(nullptr, std::memory_order_release);
    return;
  }
  auto chain = std::make_unique<HookChain>(HookChain{std::move(hooks)});
  slot.chain.store(chain.get(), std::memory_order_release);
  chains_.push_back(std::move(chain));
}

void Dispatch(HookSlot& slot, PPCFunc* original, PPCContext& ctx, uint8_t* base) {
  const HookChain* chain = slot.chain.load(std::memory_order_acquire);
  // The last hook may have gone between the caller's check and this load.
  if (!chain) return original(ctx, base);
  RexFabricCall call{&ctx, base, original, chain, 0, slot.address};
  CallNext(call);
}

void CallNext(RexFabricCall& call) {
  if (call.next < call.chain->hooks.size()) {
    const Hook& hook = call.chain->hooks[call.next++];
    hook.fn(&call, hook.user);
    return;
  }
  call.original(*call.ctx, call.base);
}

}  // namespace rexfabric
