#include "renut_engine/app/renut_settings.h"

#include <string>
#include <vector>

namespace renut::settings {

namespace {

// Never freed: the classes outlive the registry's session.
std::vector<rex::SettingOwner *> &Owners() {
  static auto *owners = new std::vector<rex::SettingOwner *>();
  return *owners;
}

}  // namespace

void Add(rex::SettingOwner &owner) {
  Owners().push_back(&owner);
}

rex::Result<void> RegisterAll(rex::SettingsRegistry &registry) {
  for (rex::SettingOwner *owner : Owners()) {
    REX_TRY_VOID(registry.Register(*owner));
    const std::string prefix = owner->GetScope() + ".";
    for (rex::SettingBase *setting : owner->GetOwnedSettings()) {
      const std::string &name = setting->GetName();
      if (name.starts_with(prefix))
        registry.AddAlias(name.substr(prefix.size()), name);
    }
  }
  return rex::Ok();
}

}  // namespace renut::settings
