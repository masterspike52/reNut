#include "events.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.h"

namespace rexfabric {

struct Event::Cell {
  std::string name;
  std::atomic<const std::vector<std::pair<uint64_t, std::shared_ptr<events::Listener>>>*> listeners{nullptr};
};

namespace events {
namespace {

using List = std::vector<std::pair<uint64_t, std::shared_ptr<Listener>>>;

struct Placement {
  Event::Cell* cell;
  const RexFabricMod* owner;
};

// Cells and lists are never freed: an Event holds its cell for the life of the process, and a thread firing
// may still be walking a list that was just replaced.
struct Registry {
  std::mutex mutex;
  std::map<std::string, std::unique_ptr<Event::Cell>, std::less<>> cells;
  std::vector<std::unique_ptr<List>> lists;
  std::unordered_map<uint64_t, Placement> placed;
  uint64_t next_id = 1;
};

Registry& Get() {
  static Registry registry;
  return registry;
}

Event::Cell* Intern(Registry& registry, std::string_view name) {
  auto it = registry.cells.find(name);
  if (it == registry.cells.end()) {
    auto cell = std::make_unique<Event::Cell>();
    cell->name = std::string(name);
    it = registry.cells.emplace(cell->name, std::move(cell)).first;
  }
  return it->second.get();
}

void Publish(Registry& registry, Event::Cell& cell, List list) {
  if (list.empty()) {
    cell.listeners.store(nullptr, std::memory_order_release);
    return;
  }
  auto published = std::make_unique<List>(std::move(list));
  cell.listeners.store(published.get(), std::memory_order_release);
  registry.lists.push_back(std::move(published));
}

List Current(const Event::Cell& cell) {
  const List* list = cell.listeners.load(std::memory_order_acquire);
  return list ? *list : List{};
}

}  // namespace

uint64_t Listen(std::string_view name, const RexFabricMod* owner, Listener fn) {
  if (!fn) return 0;
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  Event::Cell* cell = Intern(registry, name);
  List list = Current(*cell);
  const uint64_t id = registry.next_id++;
  list.emplace_back(id, std::make_shared<Listener>(std::move(fn)));
  Publish(registry, *cell, std::move(list));
  registry.placed[id] = {cell, owner};
  return id;
}

bool Unlisten(uint64_t id) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  const auto it = registry.placed.find(id);
  if (it == registry.placed.end()) return false;
  Event::Cell& cell = *it->second.cell;
  registry.placed.erase(it);
  List list = Current(cell);
  std::erase_if(list, [id](const auto& entry) { return entry.first == id; });
  Publish(registry, cell, std::move(list));
  return true;
}

void UnlistenAll(const RexFabricMod* owner) {
  std::vector<uint64_t> ids;
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    for (const auto& [id, placement] : registry.placed) {
      if (placement.owner == owner) ids.push_back(id);
    }
  }
  for (uint64_t id : ids) Unlisten(id);
}

std::vector<std::string> Names() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  std::vector<std::string> names;
  for (const auto& [name, cell] : registry.cells) names.push_back(name);
  return names;
}

}  // namespace events

Event::Event(std::string_view name) {
  events::Registry& registry = events::Get();
  std::lock_guard lock(registry.mutex);
  cell_ = events::Intern(registry, name);
}

void Event::Fire(std::span<const Value> args) const {
  const events::List* list = cell_->listeners.load(std::memory_order_acquire);
  if (!list) return;
  for (const auto& [id, listener] : *list) (*listener)(args);
}

bool Event::HasListeners() const { return cell_->listeners.load(std::memory_order_relaxed) != nullptr; }
const std::string& Event::name() const { return cell_->name; }

}  // namespace rexfabric
