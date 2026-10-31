/**
 * @file    game/cheats.cpp
 * @brief   Whether anything in the Cheats menu is in use.
 *
 * Every on/off switch in Nuts&Bolts/Cheats counts, so a cheat added there
 * later does too. A switch counts from the moment it's turned on until the
 * game is restarted, even turned off again (a record set with a cheat and the
 * cheat turned off before the score posts mustn't go up): WatchCheats has the
 * cvars report each change. Set Notes and a change made in the flag editor
 * count until the restart too.
 */
#include "renut_engine/game/cheats.h"

#include <rex/cvar.h>

#include <atomic>
#include <string>
#include <string_view>

namespace renut::game {
namespace {

constexpr std::string_view kCheats = "Nuts&Bolts/Cheats";
// Lets the players in an online session use cheats; not a cheat itself.
constexpr std::string_view kHostCheats = "renut_host_cheats";

std::atomic<bool> g_notes_set{false};
std::atomic<bool> g_save_edited{false};
// A cheat switch has been on since the game started.
std::atomic<bool> g_switch_used{false};

bool IsCheatSwitch(const std::string &name) {
  if (name == kHostCheats)
    return false;
  const rex::cvar::FlagEntry *flag = rex::cvar::GetFlagInfo(name);
  return flag && flag->type == rex::cvar::FlagType::Boolean;
}

// A bool cvar's value as the setter takes it ("true", "1").
bool IsOn(std::string_view value) {
  return value == "true" || value == "1" || value == "True" || value == "TRUE";
}

} // namespace

bool AnyCheatOn() {
  if (g_notes_set.load(std::memory_order_relaxed) || g_save_edited.load(std::memory_order_relaxed) ||
      g_switch_used.load(std::memory_order_relaxed))
    return true;
  for (const std::string &name : rex::cvar::ListFlagsByCategory(kCheats)) {
    if (IsCheatSwitch(name) && rex::cvar::Query<bool>(name)) {
      g_switch_used.store(true, std::memory_order_relaxed);
      return true;
    }
  }
  return false;
}

void WatchCheats() {
  for (const std::string &name : rex::cvar::ListFlagsByCategory(kCheats)) {
    if (!IsCheatSwitch(name))
      continue;
    // Called under the cvar registry's lock: only the latch is set here.
    rex::cvar::RegisterChangeCallback(name, [](std::string_view, std::string_view value) {
      if (IsOn(value))
        g_switch_used.store(true, std::memory_order_relaxed);
    });
    if (rex::cvar::Query<bool>(name))
      g_switch_used.store(true, std::memory_order_relaxed);
  }
}

void NotesSet() {
  g_notes_set.store(true, std::memory_order_relaxed);
}

void SaveEdited() {
  g_save_edited.store(true, std::memory_order_relaxed);
}

} // namespace renut::game
