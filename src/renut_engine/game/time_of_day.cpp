// renut_time_of_day: picks Showdown Town's scene entry (dword_82E10214, 4 of 80
// bytes) in place of sub_823EA9F0's random pick; the intro's visits stay as is.
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/setting.h>
#include <rex/types.h>

#include <cstdint>
#include <string>

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/game/guest_call.h"

namespace {

struct TimeOfDaySettings final : rex::SettingOwner {
  TimeOfDaySettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
  REX_SETTING(std::string, renut_time_of_day, "Game",
              "Showdown Town's time of day: Game (as the game picks: at random once the "
              "intro is done), or always Morning, Afternoon, Evening or Night. Takes effect "
              "the next time Showdown Town loads.",
              rex::Allowed("Game", "Morning", "Afternoon", "Evening", "Night"));
};
TimeOfDaySettings &g_time_of_day = *new TimeOfDaySettings();  // never freed, registered by RenutApp::OnConfigure

}  // namespace

REX_IMPORT(__imp__sub_823EA9F0, PickTownScene, u32(u32));           // town entries → the game's pick
REX_IMPORT(__imp__sub_822237D0, FindGameFlag, u32(u32, u32, u32));  // (last index, names, name) → index, or -1

namespace {

using renut::guest::Load32;

constexpr const char *kTimes[] = {"Morning", "Afternoon", "Evening", "Night"};  // the entries, in order
constexpr uint32_t kEntries = 4;
constexpr uint32_t kEntrySize = 80;
constexpr uint32_t kEntryFlagName = 16;

constexpr uint32_t kFlagCount = 0x82DF1CF0;   // the game flags' names: their count
constexpr uint32_t kFlagNames = 0x82DF1CF4;   // and the array (sorted, from + 4)
constexpr uint32_t kPlayer1 = 0x82E1B280;
constexpr uint32_t kPlayerProgress = 0x114;   // + 8: its flags' bits
constexpr uint32_t kNotFound = 0xFFFFFFFFu;

// The entry player 1's progress insists on (sub_823EA9F0's first rule), or
// kEntries for none.
uint32_t StoryEntry(const uint8_t *base, uint32_t entries) {
  const uint32_t player = Load32(base, kPlayer1);
  const uint32_t progress = player ? Load32(base, player + kPlayerProgress) : 0;
  const uint32_t bits = progress ? Load32(base, progress + 8) : 0;
  const uint32_t count = Load32(base, kFlagCount);
  if (!bits || !count)
    return kEntries;
  const uint32_t names = Load32(base, kFlagNames) + 4;
  for (uint32_t i = 0; i < kEntries; ++i) {
    const uint32_t index = FindGameFlag(count - 1, names, entries + kEntrySize * i + kEntryFlagName);
    if (index == kNotFound)
      continue;
    const uint32_t bit = index + 1;
    if (!(base[bits + (bit >> 3)] & (1u << (bit & 7))))
      return i;
  }
  return kEntries;
}

// The entry renut_time_of_day picks, or 0 for the game's own pick: the
// setting is Game, or the intro insists on an entry.
uint32_t TownEntry(const uint8_t *base, uint32_t entries) {
  const std::string setting = g_time_of_day.renut_time_of_day.Get();
  if (!entries || setting.empty() || setting == "Game")
    return 0;
  uint32_t wanted = kEntries;
  for (uint32_t i = 0; i < kEntries; ++i)
    if (setting == kTimes[i])
      wanted = i;
  if (wanted == kEntries || !Load32(base, entries + kEntrySize * wanted))
    return 0;
  if (StoryEntry(base, entries) != kEntries)
    return 0;
  return entries + kEntrySize * wanted;
}

// entry *sub_823EA9F0(entries): the entry LoadShowdownTown_82548080 loads.
uint32_t PickTownScene_hook(uint32_t entries) {
  const uint8_t *base = REX_RUNTIME()->GetMembase().GetBase();
  if (const uint32_t entry = TownEntry(base, entries))
    return entry;
  return PickTownScene(entries);
}

} // namespace

REX_HOOK(sub_823EA9F0, PickTownScene_hook);
