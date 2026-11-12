// The hook slot every recompiled function checks on entry. Generated code includes this through
// RexFabric's codegen templates (templates/pch_hooks.inja), so it pulls in nothing but rex/ppc/recompiled_code.h.

#pragma once

#include <atomic>
#include <cstdint>

#include <rex/ppc/recompiled_code.h>

namespace rexfabric {

struct HookChain;

// One per recompiled function, defined in the generated init file. `chain` stays null until something
// hooks the function, so an unhooked call costs one relaxed load and a predicted branch.
struct HookSlot {
  uint32_t address;
  std::atomic<const HookChain*> chain{nullptr};
};

struct SlotEntry {
  uint32_t address;
  HookSlot* slot;
};

// Runs the hooks attached to `slot`, which decide whether `original` runs.
void Dispatch(HookSlot& slot, PPCFunc* original, PPCContext& ctx, uint8_t* base);

}  // namespace rexfabric

// Every hookable function, in codegen order. Defined in the generated init file.
extern const rexfabric::SlotEntry rexfabric_slots[];
extern const uint32_t rexfabric_slot_count;
