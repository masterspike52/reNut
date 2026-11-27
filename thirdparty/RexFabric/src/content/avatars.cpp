#include "avatars.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>

#include "../log.h"
#include "classes.h"
#include "components.h"
#include "types.h"

namespace rexfabric::content {
namespace {

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

// A component of a spawned avatar: its type, where it hangs, its values and its own state.
struct Part {
  std::shared_ptr<const ComponentType> type;
  std::string name;
  int parent = -1;  // the part it's on, or -1 for the avatar
  Transform local;
  std::map<std::string, PropertyValue> values;
  std::any state;
  bool ticking = true;
};

struct Avatar {
  uint32_t id = 0;
  std::string path;
  std::shared_ptr<const BackingDecl> backing;
  uint64_t handle = 0;
  // Under the registry's lock: where it is, its property values, whether it's saved with its world, and whether it was
  // removed.
  Transform at;
  std::vector<Property> properties;
  bool saved = false;
  bool removed = false;
  // Only on the game's thread.
  std::vector<Part> parts;
};

struct Registry {
  std::mutex mutex;
  std::vector<std::shared_ptr<Avatar>> avatars;
  // Removed by RemoveAllAvatars, to dispose of on the next tick.
  std::vector<std::shared_ptr<Avatar>> leaving;
  // The avatars forgotten because their backing went with its world. Ids are never used twice.
  std::unordered_set<uint32_t> went_with_world;
  uint32_t next_id = 1;

  std::shared_ptr<Avatar> Find(uint32_t id) const {
    const auto found = std::find_if(avatars.begin(), avatars.end(), [id](const auto& avatar) { return avatar->id == id; });
    return found != avatars.end() ? *found : nullptr;
  }

  std::shared_ptr<Avatar> Take(uint32_t id) {
    const auto found = std::find_if(avatars.begin(), avatars.end(), [id](const auto& avatar) { return avatar->id == id; });
    if (found == avatars.end()) return nullptr;
    std::shared_ptr<Avatar> taken = *found;
    taken->removed = true;
    avatars.erase(found);
    return taken;
  }
};

Registry& Get() {
  static auto* registry = new Registry();
  return *registry;
}

AvatarValues ValuesOf(const std::vector<Property>& properties) {
  AvatarValues values;
  for (const Property& property : properties) values[property.name] = ToPropertyValue(property.value);
  return values;
}

// Runs one of the game's functions, logging a throw; false when it threw.
bool Guarded(const Avatar& avatar, std::string_view what, const std::function<void()>& run) {
  try {
    run();
    return true;
  } catch (const std::exception& error) {
    RF_WARN("content: avatar {} of {}: {}: {}", avatar.id, avatar.path, what, error.what());
    return false;
  }
}

// The class's components as the avatar's parts, each after the one it's on.
void AddParts(const std::vector<Component>& components, int parent, std::vector<Part>& parts) {
  for (const Component& component : components) {
    std::shared_ptr<const ComponentType> type = FindComponentType(component.type);
    if (!type) continue;  // checked as the class loaded; gone only if the game took it away
    Part part{.type = type, .name = component.name, .parent = parent, .local = component.transform};
    for (const auto& [name, value] : ComponentValues(*type, component)) part.values[name] = ToPropertyValue(value);
    parts.push_back(std::move(part));
    AddParts(component.children, static_cast<int>(parts.size()) - 1, parts);
  }
}

// Calls `function` (attach, tick or detach) of one part, where the avatar and the parts above it put it now.
void RunPart(Avatar& avatar, size_t index, const std::vector<Transform>& world, double seconds, const std::function<void(ComponentContext&)>& function, std::string_view what) {
  Part& part = avatar.parts[index];
  if (!function) return;
  ComponentContext context{.avatar = avatar.id, .avatar_class = avatar.path, .name = part.name, .world = world[index], .properties = part.values, .seconds = seconds, .state = std::move(part.state)};
  const bool ran = Guarded(avatar, std::string(what) + " of " + part.name, [&] { function(context); });
  part.state = std::move(context.state);
  if (!ran && what == "tick") part.ticking = false;
}

// Where each part is in the world, the avatar being at `at`.
std::vector<Transform> PartPlaces(const Avatar& avatar, const Transform& at) {
  std::vector<Transform> world;
  world.reserve(avatar.parts.size());
  for (const Part& part : avatar.parts) world.push_back(Compose(part.parent < 0 ? at : world[static_cast<size_t>(part.parent)], part.local));
  return world;
}

Transform PlaceOf(const Avatar& avatar) {
  std::lock_guard lock(Get().mutex);
  return avatar.at;
}

void Detach(Avatar& avatar) {
  const std::vector<Transform> world = PartPlaces(avatar, PlaceOf(avatar));
  for (size_t i = avatar.parts.size(); i-- > 0;) RunPart(avatar, i, world, 0.0, avatar.parts[i].type->detach, "detach");
}

// Detaches a taken avatar's parts and removes its backing.
void Dispose(Avatar& avatar) {
  Detach(avatar);
  Guarded(avatar, "removing its backing", [&] { avatar.backing->remove(avatar.handle); });
}

}  // namespace

uint32_t SpawnAvatar(std::string_view path, const Transform& at, std::string* why) {
  const std::optional<Class> found = FindClass(path);
  const std::string backing_path = found ? BackingOf(found->path) : std::string();
  if (!found || backing_path.empty()) {
    if (why) *why = found ? found->path + " isn't an avatar class: nothing gives it a backing" : "there's no class " + std::string(path);
    return 0;
  }
  auto avatar = std::make_shared<Avatar>();
  avatar->path = found->path;
  avatar->backing = FindBacking(backing_path);
  avatar->at = at;
  avatar->properties = Properties(found->path);
  if (!avatar->backing) {
    if (why) *why = "the game has no backing " + backing_path;
    return 0;
  }
  AddParts(ComponentsOf(found->path), -1, avatar->parts);
  try {
    avatar->handle = avatar->backing->make(at, ValuesOf(avatar->properties));
  } catch (const std::exception& error) {
    if (why) *why = error.what();
    return 0;
  }
  if (avatar->handle == 0) {
    if (why) *why = "its backing " + backing_path + " couldn't be made";
    return 0;
  }
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    avatar->id = registry.next_id++;
    registry.avatars.push_back(avatar);
  }
  const std::vector<Transform> world = PartPlaces(*avatar, at);
  for (size_t i = 0; i < avatar->parts.size() && !avatar->removed; ++i) RunPart(*avatar, i, world, 0.0, avatar->parts[i].type->attach, "attach");
  return avatar->id;
}

bool RemoveAvatar(uint32_t id) {
  std::shared_ptr<Avatar> avatar;
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    avatar = registry.Take(id);
  }
  if (!avatar) return false;
  Dispose(*avatar);
  return true;
}

void RemoveAllAvatars() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  const auto kept = std::stable_partition(registry.avatars.begin(), registry.avatars.end(), [](const auto& avatar) { return avatar->saved; });
  for (auto it = kept; it != registry.avatars.end(); ++it) (*it)->removed = true;
  registry.leaving.insert(registry.leaving.end(), kept, registry.avatars.end());
  registry.avatars.erase(kept, registry.avatars.end());
}

bool MoveAvatar(uint32_t id, const Transform& to, std::string* why) {
  std::shared_ptr<Avatar> avatar;
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    avatar = registry.Find(id);
  }
  if (!avatar) {
    if (why) *why = "the avatar is gone";
    return false;
  }
  try {
    avatar->backing->move(avatar->handle, to);
  } catch (const std::exception& error) {
    if (why) *why = error.what();
    return false;
  }
  std::lock_guard lock(Get().mutex);
  avatar->at = to;
  return true;
}

std::optional<AvatarView> FindAvatar(uint32_t id) {
  std::shared_ptr<Avatar> avatar;
  AvatarView view;
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    avatar = registry.Find(id);
    if (!avatar) return std::nullopt;
    view = {.id = avatar->id, .path = avatar->path, .at = avatar->at, .saved = avatar->saved};
  }
  if (avatar->backing->object) Guarded(*avatar, "its object", [&] { view.object = avatar->backing->object(avatar->handle); });
  return view;
}

std::vector<uint32_t> AvatarsOf(std::string_view path) {
  std::vector<std::pair<uint32_t, std::string>> all;
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    for (const auto& avatar : registry.avatars) all.emplace_back(avatar->id, avatar->path);
  }
  std::vector<uint32_t> ids;
  for (const auto& [id, of] : all) {
    if (path.empty() || IsA(of, path)) ids.push_back(id);
  }
  return ids;
}

uint32_t AvatarOfObject(const Object& object) {
  if (!object) return 0;
  std::vector<std::shared_ptr<Avatar>> all;
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    all = registry.avatars;
  }
  for (const auto& avatar : all) {
    if (!avatar->backing->object) continue;
    Object backing;
    if (Guarded(*avatar, "its object", [&] { backing = avatar->backing->object(avatar->handle); }) && backing.address == object.address && backing.type == object.type) return avatar->id;
  }
  return 0;
}

std::optional<nlohmann::json> AvatarValue(uint32_t id, std::string_view property) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  const std::shared_ptr<Avatar> avatar = registry.Find(id);
  if (!avatar) return std::nullopt;
  for (const Property& known : avatar->properties) {
    if (Lower(known.name) == Lower(property)) return known.value;
  }
  return std::nullopt;
}

bool SetAvatarValue(uint32_t id, std::string_view property, const nlohmann::json& value, std::string* why) {
  std::shared_ptr<Avatar> avatar;
  AvatarValues values;
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    avatar = registry.Find(id);
    if (!avatar) {
      if (why) *why = "the avatar is gone";
      return false;
    }
    const auto known = std::find_if(avatar->properties.begin(), avatar->properties.end(), [property](const Property& at) { return Lower(at.name) == Lower(property); });
    if (known == avatar->properties.end()) {
      if (why) *why = avatar->path + " has no property " + std::string(property);
      return false;
    }
    nlohmann::json fitted;
    if (!FitValue(LoadedTypes(), known->type, value, &fitted, why)) return false;
    known->value = std::move(fitted);
    values = ValuesOf(avatar->properties);
  }
  // Outside the lock: the backing calls into the game.
  if (avatar->backing->apply) Guarded(*avatar, "showing its values", [&] { avatar->backing->apply(avatar->handle, values); });
  return true;
}

bool WentWithItsWorld(uint32_t id) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  return registry.went_with_world.contains(id);
}

bool SetAvatarSaved(uint32_t id, bool saved) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  const std::shared_ptr<Avatar> avatar = registry.Find(id);
  if (!avatar) return false;
  avatar->saved = saved;
  return true;
}

std::vector<SavedAvatar> SavedAvatars() {
  struct Held {
    std::string path;
    Transform at;
    std::vector<Property> properties;
  };
  std::vector<Held> held;
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    for (const auto& avatar : registry.avatars) {
      if (avatar->saved) held.push_back({avatar->path, avatar->at, avatar->properties});
    }
  }
  // Outside the lock: the class's defaults come from the class registry.
  std::vector<SavedAvatar> saved;
  saved.reserve(held.size());
  for (const Held& avatar : held) {
    SavedAvatar record{.path = avatar.path, .at = avatar.at};
    const std::vector<Property> defaults = Properties(avatar.path);
    for (const Property& property : avatar.properties) {
      const auto by_class = std::find_if(defaults.begin(), defaults.end(), [&property](const Property& known) { return known.name == property.name; });
      if (by_class == defaults.end() || by_class->value != property.value) record.values[property.name] = property.value;
    }
    saved.push_back(std::move(record));
  }
  return saved;
}

void TickAvatars(double seconds) {
  std::vector<std::shared_ptr<Avatar>> all;
  std::vector<std::shared_ptr<Avatar>> leaving;
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    all = registry.avatars;
    leaving.swap(registry.leaving);
  }
  for (auto it = leaving.rbegin(); it != leaving.rend(); ++it) Dispose(**it);
  for (const auto& avatar : all) {
    Transform now;
    bool there = false;
    if (!Guarded(*avatar, "where it is", [&] { there = avatar->backing->where(avatar->handle, now); })) there = false;
    if (!there) {
      // Gone with its world: its parts go, and the backing is the game's to clean up.
      {
        Registry& registry = Get();
        std::lock_guard lock(registry.mutex);
        if (!registry.Take(avatar->id)) continue;
        registry.went_with_world.insert(avatar->id);
      }
      Detach(*avatar);
      continue;
    }
    {
      std::lock_guard lock(Get().mutex);
      if (avatar->removed) continue;
      avatar->at = now;
    }
    const std::vector<Transform> world = PartPlaces(*avatar, now);
    for (size_t i = 0; i < avatar->parts.size() && !avatar->removed; ++i) {
      if (avatar->parts[i].ticking) RunPart(*avatar, i, world, seconds, avatar->parts[i].type->tick, "tick");
    }
  }
}

}  // namespace rexfabric::content
