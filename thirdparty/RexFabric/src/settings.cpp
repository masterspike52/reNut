#include "settings.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/settings_registry.h>
#include <rexfabric/rexfabric.h>

namespace rexfabric {
namespace {

// One per mod setting, as an owner's settings are fixed once it registers.
class ModSettingOwner final : public rex::SettingOwner {
 public:
  template <typename T>
  ModSettingOwner(std::string_view name, const T& value, std::string_view help)
      : rex::SettingOwner(kModSettingScope), setting_(std::make_unique<rex::Setting<T>>(this, name, value, help)) {}

  rex::SettingBase& setting() { return *setting_; }

 private:
  std::unique_ptr<rex::SettingBase> setting_;
};

struct ModEntry {
  ModSettingOwner* owner;
  std::string category;
  std::string mod;
};

// Mod settings stay registered for the life of the process, so a reload keeps their values. Never freed.
struct Registered {
  std::mutex mutex;
  std::vector<ModEntry> mods;
  std::unordered_map<std::string, std::string> old_names;  // an old flat name -> the full name it found
};

Registered& Get() {
  static auto* registered = new Registered();
  return *registered;
}

std::string Join(std::string_view scope, std::string_view name) { return std::string(scope) + "." + std::string(name); }

}  // namespace

rex::SettingBase* FindSetting(std::string_view name) {
  rex::SettingsRegistry& registry = REX_SETTINGS();
  if (auto found = registry.Find(name)) return *found;
  if (auto found = registry.Find(Join(kModSettingScope, name))) return *found;
  if (!registry.IsAlias(name)) return nullptr;
  Registered& registered = Get();
  {
    std::lock_guard lock(registered.mutex);
    if (auto it = registered.old_names.find(std::string(name)); it != registered.old_names.end()) {
      if (auto found = registry.Find(it->second)) return *found;
    }
  }
  // Find doesn't follow aliases; an old flat name is the last part of the setting that took it over.
  const std::string suffix = "." + std::string(name);
  for (rex::SettingBase* setting : registry.Enumerate()) {
    if (!setting->GetName().ends_with(suffix)) continue;
    std::lock_guard lock(registered.mutex);
    registered.old_names[std::string(name)] = setting->GetName();
    return setting;
  }
  return nullptr;
}

template <typename T>
rex::Setting<T>* AddModSetting(std::string_view name, const T& value, std::string_view help, std::string category, std::string mod) {
  auto owner = std::make_unique<ModSettingOwner>(name, value, help);
  rex::SettingsRegistry& registry = REX_SETTINGS();
  if (!registry.Register(*owner)) return nullptr;
  registry.AddAlias(name, owner->setting().GetName());
  auto* setting = static_cast<rex::Setting<T>*>(&owner->setting());
  Registered& registered = Get();
  std::lock_guard lock(registered.mutex);
  registered.mods.push_back({owner.release(), std::move(category), std::move(mod)});
  return setting;
}

template rex::Setting<bool>* AddModSetting(std::string_view, const bool&, std::string_view, std::string, std::string);
template rex::Setting<rex::i32>* AddModSetting(std::string_view, const rex::i32&, std::string_view, std::string, std::string);
template rex::Setting<rex::i64>* AddModSetting(std::string_view, const rex::i64&, std::string_view, std::string, std::string);
template rex::Setting<rex::u32>* AddModSetting(std::string_view, const rex::u32&, std::string_view, std::string, std::string);
template rex::Setting<rex::u64>* AddModSetting(std::string_view, const rex::u64&, std::string_view, std::string, std::string);
template rex::Setting<rex::f64>* AddModSetting(std::string_view, const rex::f64&, std::string_view, std::string, std::string);
template rex::Setting<std::string>* AddModSetting(std::string_view, const std::string&, std::string_view, std::string, std::string);

std::vector<ModSetting> ModSettings() {
  Registered& registered = Get();
  std::lock_guard lock(registered.mutex);
  std::vector<ModSetting> settings;
  settings.reserve(registered.mods.size());
  for (const ModEntry& entry : registered.mods) settings.push_back({entry.owner->setting().GetName(), entry.category, entry.mod});
  return settings;
}

}  // namespace rexfabric
