// Named flags a mod sets and the game's compiled-in code reads.
//
// Some patches can only be compiled into the game: a midasm stub, a branch in the game's own C++. A switch
// lets a mod decide whether such a patch applies. The game reads it where the patch is, for one relaxed
// load; a mod sets it by name.
//
//   bool Space1_hook() {
//     static const rexfabric::Switch unlocked("UnlimitedGardenSpace");
//     return unlocked.on();
//   }
//
//   rex.switch.set("UnlimitedGardenSpace", true)   -- a Lua mod
//
// Switches start at 0, which the game should read as "behave like the unmodded game". The switches Lua set go
// back to 0 when scripts reload, so a mod that was turned off stops applying.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rexfabric {

// One switch. Construction interns the name and may allocate, so make one once, as a static where it's read.
class Switch {
 public:
  explicit Switch(std::string_view name);

  int32_t value() const { return cell_->load(std::memory_order_relaxed); }
  bool on() const { return value() != 0; }
  explicit operator bool() const { return on(); }
  void set(int32_t value) const { cell_->store(value, std::memory_order_relaxed); }

 private:
  std::atomic<int32_t>* cell_;
};

void SetSwitch(std::string_view name, int32_t value);
// 0 for a switch nothing has set.
int32_t GetSwitch(std::string_view name);
// Every switch, by name.
std::vector<std::pair<std::string, int32_t>> Switches();
// Sets every switch to 0. Storage stays, so Switch handles stay valid.
void ResetSwitches();

}  // namespace rexfabric
