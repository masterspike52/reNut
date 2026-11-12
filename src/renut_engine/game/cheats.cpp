#include "renut_engine/game/cheats.h"

#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/utility.h>

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/game/graphics_menu.h"

namespace renut::game {
namespace {

constexpr std::string_view kCheats = "Nuts&Bolts/Cheats";
constexpr std::string_view kHostCheats = "renut_host_cheats";

std::atomic<bool> g_notes_set{false};
std::atomic<bool> g_save_edited{false};
std::atomic<bool> g_switch_used{false};

// A cheat's on/off switch, or null for any other setting.
rex::Setting<bool> *CheatSwitch(const renut::graphics_menu::CategorizedSetting &entry) {
  if (entry.category != kCheats || entry.name == kHostCheats)
    return nullptr;
  auto found = REX_SETTINGS().Find(std::string(renut::settings::kScope) + "." + entry.name);
  if (!found || (*found)->GetType() != rex::SettingType::kBool)
    return nullptr;
  return static_cast<rex::Setting<bool> *>(*found);
}

}

bool AnyCheatOn() {
  if (g_notes_set.load(std::memory_order_relaxed) || g_save_edited.load(std::memory_order_relaxed) ||
      g_switch_used.load(std::memory_order_relaxed))
    return true;
  for (const renut::graphics_menu::CategorizedSetting &entry : renut::graphics_menu::CategorizedSettings()) {
    rex::Setting<bool> *cheat = CheatSwitch(entry);
    if (cheat && cheat->Get()) {
      g_switch_used.store(true, std::memory_order_relaxed);
      return true;
    }
  }
  return false;
}

void WatchCheats() {
  // Never freed: the watches last the session.
  static auto *watches = new std::vector<rex::ScopeGuard>();
  for (const renut::graphics_menu::CategorizedSetting &entry : renut::graphics_menu::CategorizedSettings()) {
    rex::Setting<bool> *cheat = CheatSwitch(entry);
    if (!cheat)
      continue;
    watches->push_back(cheat->OnChange([](const bool &on) {
      if (on)
        g_switch_used.store(true, std::memory_order_relaxed);
    }));

    if (cheat->Get())
      g_switch_used.store(true, std::memory_order_relaxed);
  }
}

void NotesSet() {
  g_notes_set.store(true, std::memory_order_relaxed);
}

void SaveEdited() {
  g_save_edited.store(true, std::memory_order_relaxed);
}

}
