/**
 * @file    game/notes.cpp
 * @brief   Setting player 1's note count (notes_amount, set_notes).
 *
 * N&B (TU3, IDA): the notes are a 32-bit count at + 360 of the object at
 * + 308 of the player's profile (dword_82E1B280[player], what sub_82212D18
 * returns). Spending them (sub_824AC140, the parts shop and the like; the
 * no_notes_spent hook skips its store) takes the price from there and saves
 * with the profile.
 */
#include <rex/cvar.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include <charconv>
#include <cstdint>
#include <string>

#include "renut_engine/app/renut_logging.h"
#include "renut_engine/game/cheats.h"
#include "renut_engine/online/online.h"

REXCVAR_DEFINE_STRING(notes_amount, "", "Nuts&Bolts/Cheats",
                      "How many notes Set Notes gives player 1 (0 to 2147483647). Off in online "
                      "multiplayer unless the host allows cheats.");

namespace {

// Player 1's profile.
constexpr uint32_t kPlayerProfiles = 0x82E1B280;
constexpr uint32_t kProfileCounters = 308;
constexpr uint32_t kNotes = 360;

void SetNotes() {
  if (renut::online::InSession() && !renut::online::SessionAllowsCheats()) {
    RNUT_WARN("Set Notes: cheats are off in this session");
    return;
  }
  const std::string &text = REXCVAR_GET(notes_amount);
  int32_t notes = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), notes);
  if (error != std::errc() || end != text.data() + text.size() || notes < 0) {
    RNUT_WARN("Set Notes: '{}' is no note count (0 to 2147483647)", text);
    return;
  }
  auto *memory = REX_KERNEL_MEMORY();
  const uint32_t profile = rex::byte_swap(*memory->TranslateVirtual<uint32_t *>(kPlayerProfiles));
  const uint32_t counters =
      profile ? rex::byte_swap(*memory->TranslateVirtual<uint32_t *>(profile + kProfileCounters)) : 0;
  if (!counters) {
    RNUT_WARN("Set Notes: no player profile yet (load a save first)");
    return;
  }
  auto *count = memory->TranslateVirtual<uint32_t *>(counters + kNotes);
  RNUT_INFO("Set Notes: {} -> {}", int32_t(rex::byte_swap(*count)), notes);
  *count = rex::byte_swap(uint32_t(notes));
  renut::game::NotesSet();
}

}  // namespace

REXCVAR_DEFINE_COMMAND(set_notes, SetNotes, "Nuts&Bolts/Cheats",
                       "Sets player 1's notes to notes_amount (load a save first).");
