#include <rex/console_command.h>
#include <rex/kernel/memory_view.h>
#include <rex/kernel/title_runtime.h>
#include <rex/result.h>
#include <rex/setting.h>
#include <rex/types.h>

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/game/cheats.h"
#include "renut_engine/online/online.h"

namespace {

void SetNotes();

struct NotesSettings final : rex::SettingOwner {
  NotesSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
  REX_SETTING(std::string, notes_amount, "",
              "How many notes Set Notes gives player 1 (0 to 2147483647). Off in online "
              "multiplayer unless the host allows cheats.");
  REX_COMMAND(set_notes, "Sets player 1's notes to notes_amount (load a save first).",
              [](std::string_view) {
                SetNotes();
                return rex::Ok();
              });
};
NotesSettings &g_notes = *new NotesSettings();  // never freed, registered by RenutApp::OnConfigure

// Player 1's profile.
constexpr uint32_t kPlayerProfiles = 0x82E1B280;
constexpr uint32_t kProfileCounters = 308;
constexpr uint32_t kNotes = 360;

// Nothing when cheats are off in this session, notes_amount is no note count
// (0 to 2147483647) or there's no player profile yet (no save loaded).
void SetNotes() {
  if (renut::online::InSession() && !renut::online::SessionAllowsCheats())
    return;
  const std::string text = g_notes.notes_amount.Get();
  int32_t notes = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), notes);
  if (error != std::errc() || end != text.data() + text.size() || notes < 0)
    return;
  rex::kernel::GuestMemory *guest = REX_GUEST_MEMORY();
  if (!guest)
    return;
  const rex::kernel::MemoryView memory = guest->GetView();
  const uint32_t profile = rex::byte_swap(*memory.TranslateVirtual<uint32_t *>(kPlayerProfiles));
  const uint32_t counters =
      profile ? rex::byte_swap(*memory.TranslateVirtual<uint32_t *>(profile + kProfileCounters)) : 0;
  if (!counters)
    return;
  auto *count = memory.TranslateVirtual<uint32_t *>(counters + kNotes);
  *count = rex::byte_swap(uint32_t(notes));
  renut::game::NotesSet();
}

}  // namespace
