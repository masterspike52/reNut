#include <rexfabric/switches.h>

#include <map>
#include <memory>
#include <mutex>

namespace rexfabric {
namespace {

// Cells are never erased: a Switch holds its cell's address for the life of the process. The lock guards
// lookup and insertion only; reading a switch never takes it.
struct Registry {
  std::mutex mutex;
  std::map<std::string, std::unique_ptr<std::atomic<int32_t>>, std::less<>> cells;
};

Registry& Get() {
  static Registry registry;
  return registry;
}

std::atomic<int32_t>* Intern(std::string_view name) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  auto it = registry.cells.find(name);
  if (it == registry.cells.end()) it = registry.cells.emplace(std::string(name), std::make_unique<std::atomic<int32_t>>(0)).first;
  return it->second.get();
}

}  // namespace

Switch::Switch(std::string_view name) : cell_(Intern(name)) {}

void SetSwitch(std::string_view name, int32_t value) { Intern(name)->store(value, std::memory_order_relaxed); }

int32_t GetSwitch(std::string_view name) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  const auto it = registry.cells.find(name);
  return it != registry.cells.end() ? it->second->load(std::memory_order_relaxed) : 0;
}

std::vector<std::pair<std::string, int32_t>> Switches() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  std::vector<std::pair<std::string, int32_t>> switches;
  for (const auto& [name, cell] : registry.cells) switches.emplace_back(name, cell->load(std::memory_order_relaxed));
  return switches;
}

void ResetSwitches() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  for (auto& [name, cell] : registry.cells) cell->store(0, std::memory_order_relaxed);
}

}  // namespace rexfabric
