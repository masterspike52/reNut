// Midasm sites mods can claim.
//
// A hook reaches a function at its entry. A midasm hook sits at one instruction partway through a function:
// a branch to take, a register to force. Codegen emits a call to a named stub there, from the game's
// [[midasm_hook]] config, so the stub has to be compiled into the game. What doesn't have to be compiled in
// is the decision: a stub built on a MidasmSite asks whichever mods claimed its address.
//
//   # the game's codegen config
//   [[midasm_hook]]
//   address = 0x824DC3AC
//   name = "Space1_hook"
//   jump_address_on_true = 0x824DC830
//
//   [[midasm_hook]]
//   address = 0x823DA320
//   name = "Space3_hook"
//   registers = ["r3"]
//
//   // the game's C++
//   bool Space1_hook() { return rexfabric::Midasm<0x824DC3AC>(); }
//   void Space3_hook(PPCRegister& r3) { rexfabric::Midasm<0x823DA320>({{"r3", &r3}}); }
//
//   -- a Lua mod
//   rex.midasm(0x824DC3AC, function() return true end)
//   rex.midasm(0x823DA320, function(m) m.r3 = 1 end)
//
// The stub names the registers codegen gave it, and mods address them by those names. An unclaimed site
// answers false and writes nothing, so the guest's own instruction runs as compiled; that costs one relaxed
// load. A claimed one costs a call into each claiming mod.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include <rex/ppc/context.h>

struct RexFabricMod;

namespace rexfabric {

// A register a stub lends to the mods for one call. A name starting with 'f' is a float register, anything
// else an integer one.
struct MidasmReg {
  const char* name;
  PPCRegister* reg;
};

// One midasm address. Construction interns it and may allocate, so make one once, as a static in the stub.
class MidasmSite {
 public:
  explicit MidasmSite(uint32_t address);

  bool claimed() const { return claimed_->load(std::memory_order_relaxed); }
  // True when any claiming mod asked for the patch. Every claim runs, in claim order, so a register one
  // writes is what the next one reads.
  bool Call(std::initializer_list<MidasmReg> regs = {}) const;
  uint32_t address() const { return address_; }

 private:
  uint32_t address_;
  const std::atomic<bool>* claimed_;
};

// The stub body for a site at `Address`.
template <uint32_t Address>
bool Midasm(std::initializer_list<MidasmReg> regs = {}) {
  static const MidasmSite site(Address);
  return site.Call(regs);
}

// A claim's handler. `regs` is what the stub lent, valid for this call only. True asks for the patch.
using MidasmFn = bool (*)(void* user, uint32_t address, const MidasmReg* regs, size_t count);

// Never zero for a claim that was made.
uint64_t ClaimMidasm(const RexFabricMod* owner, uint32_t address, MidasmFn fn, void* user);
bool ReleaseMidasm(uint64_t id);
size_t ReleaseAllMidasm(const RexFabricMod* owner);

struct MidasmClaim {
  uint64_t id;
  uint32_t address;
  const RexFabricMod* owner;
};

// Every claim, by address then claim order.
std::vector<MidasmClaim> MidasmClaims();

}  // namespace rexfabric
