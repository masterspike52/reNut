/**
 * @file    game/flag_editor.cpp
 * @brief   Player 1's game flags and counters, by the game's own names.
 *
 * N&B (TU3, IDA): sub_8223AD58 fills the table set at dword_82DF1CDC at
 * startup: + 0 the counter count (247) and + 4 their names; + 20 / + 28 /
 * + 36 the normal (1817), global (274) and volatile (2) flag counts, each
 * followed by its names. A name's index is the game's id (gameFlagsSetFlag,
 * sub_824AC030's name lookup), 0 being the unused "..._Null".
 *
 * The player's profile (dword_82E1B280[player], what sub_82212D18 returns;
 * sub_82286FD0 builds it) holds a bit set per flag table at + 276 / + 280 /
 * + 284 ({bit count, byte count, data}; id i is bit i & 7 of byte i >> 3,
 * gameFlagsSetFlag and sub_82332930 setting and clearing them) and the
 * counters at + 308 (int32 by id; notes are id 90, notes.cpp). Saving
 * (sub_824E8838 → sub_824E9148) writes all of them; sub_824E8FA0 marks the
 * profile changed for it, as the game's own setters do.
 *
 * Changes are made between the game's frames on its main thread (sub_82283BB0,
 * a pass of its main loop, menus included), straight into the profile: what
 * the game's setters do on top (gameFlagsSetFlag awarding achievements for
 * finished sets) isn't done.
 */
#include "renut_engine/game/flag_editor.h"

#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include <array>
#include <mutex>

#include "renut_engine/app/renut_logging.h"
#include "renut_engine/game/cheats.h"
#include "renut_engine/game/guest_call.h"
#include "renut_engine/online/online.h"

REX_EXTERN(__imp__sub_82283BB0);
REX_IMPORT(__imp__sub_824E8FA0, ProfileChanged, void(u32));

namespace renut::game::flags {
namespace {

// The game's table set (sub_8223AD58): per table, its count's offset (the
// names follow it).
constexpr uint32_t kTableSet = 0x82DF1CDC;
constexpr std::array<uint32_t, kTableCount> kTableCountOffset = {20, 28, 36, 0};

// Player 1's profile.
constexpr uint32_t kPlayerProfiles = 0x82E1B280;
constexpr uint32_t kProfileFlags = 276;     // + 4 x flag table: its bit set
constexpr uint32_t kProfileCounters = 308;  // int32 by id
// A bit set (sub_822ED480): its bit count, byte count, then its data.
constexpr uint32_t kBitSetCount = 0;
constexpr uint32_t kBitSetData = 8;

struct Edit {
  Table table;
  uint32_t index;
  int32_t value;
};

std::mutex g_mutex;
std::array<std::vector<std::string>, kTableCount> g_names;
std::vector<Edit> g_pending;

uint32_t Read32(uint32_t va) {
  return rex::byte_swap(*REX_KERNEL_MEMORY()->TranslateVirtual<uint32_t *>(va));
}

uint8_t *Bytes(uint32_t va) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t *>(va);
}

uint32_t Count(Table table) {
  return Read32(kTableSet + kTableCountOffset[int(table)]);
}

uint32_t Profile() {
  return Read32(kPlayerProfiles);
}

// Where the value lives in the profile: the flag's byte or the counter; 0 if
// the profile doesn't have the table as the game set it up.
uint32_t ValueAddress(uint32_t profile, Table table, uint32_t index) {
  if (!profile || !index || index >= Count(table))
    return 0;
  if (table == Table::kCounters) {
    const uint32_t counters = Read32(profile + kProfileCounters);
    return counters ? counters + 4 * index : 0;
  }
  const uint32_t bits = Read32(profile + kProfileFlags + 4 * uint32_t(table));
  if (!bits || Read32(bits + kBitSetCount) != Count(table))
    return 0;
  const uint32_t data = Read32(bits + kBitSetData);
  return data ? data + (index >> 3) : 0;
}

int32_t ReadValue(uint32_t profile, Table table, uint32_t index) {
  const uint32_t va = ValueAddress(profile, table, index);
  if (!va)
    return 0;
  if (table == Table::kCounters)
    return int32_t(Read32(va));
  return (*Bytes(va) >> (index & 7)) & 1;
}

// Game thread. Whether the value changed; 'log' logs the change.
bool WriteValue(uint32_t profile, const Edit &edit, bool log) {
  const uint32_t va = ValueAddress(profile, edit.table, edit.index);
  if (!va)
    return false;
  const int32_t before = ReadValue(profile, edit.table, edit.index);
  if (before == edit.value)
    return false;
  if (edit.table == Table::kCounters) {
    *REX_KERNEL_MEMORY()->TranslateVirtual<uint32_t *>(va) = rex::byte_swap(uint32_t(edit.value));
  } else {
    const uint8_t bit = uint8_t(1u << (edit.index & 7));
    uint8_t *byte = Bytes(va);
    *byte = edit.value ? uint8_t(*byte | bit) : uint8_t(*byte & ~bit);
  }
  if (log) {
    // The names were read before anything could be changed.
    const auto &names = g_names[int(edit.table)];
    RNUT_INFO("Flag editor: {} {} -> {}",
              edit.index < names.size() ? names[edit.index] : std::to_string(edit.index), before,
              edit.value);
  }
  return true;
}

// More changes at once than this (all shown on or off) are logged as a count.
constexpr size_t kLoggedEdits = 8;

void ApplyPending(PPCContext &ctx, uint8_t *base) {
  std::vector<Edit> edits;
  {
    std::lock_guard lock(g_mutex);
    if (g_pending.empty())
      return;
    edits.swap(g_pending);
  }
  const uint32_t profile = Profile();
  if (!profile) {
    RNUT_WARN("Flag editor: player 1's profile went before {} change(s) were made", edits.size());
    return;
  }
  const bool log = edits.size() <= kLoggedEdits;
  size_t changed = 0;
  for (const Edit &edit : edits)
    changed += WriteValue(profile, edit, log);
  if (!changed)
    return;
  if (!log)
    RNUT_INFO("Flag editor: {} value(s) changed", changed);
  renut::game::SaveEdited();
  renut::guest::Guest g(ctx, base);
  ProfileChanged(g.frame(), base, profile);
}

}  // namespace

const std::vector<std::string> &Names(Table table) {
  std::lock_guard lock(g_mutex);
  auto &names = g_names[int(table)];
  if (names.empty()) {
    const uint32_t count = Count(table);
    const uint32_t list = Read32(kTableSet + kTableCountOffset[int(table)] + 4);
    if (count && list) {
      names.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t name = Read32(list + 4 * i);
        names.emplace_back(name ? REX_KERNEL_MEMORY()->TranslateVirtual<const char *>(name) : "");
      }
    }
  }
  return names;
}

bool HasProfile() {
  return Profile() != 0;
}

int32_t Get(Table table, uint32_t index) {
  {
    std::lock_guard lock(g_mutex);
    for (auto it = g_pending.rbegin(); it != g_pending.rend(); ++it) {
      if (it->table == table && it->index == index)
        return it->value;
    }
  }
  return ReadValue(Profile(), table, index);
}

bool Set(Table table, uint32_t index, int32_t value) {
  if (Blocked())
    return false;
  if (table != Table::kCounters)
    value = value ? 1 : 0;
  std::lock_guard lock(g_mutex);
  g_pending.push_back({table, index, value});
  return true;
}

bool Blocked() {
  return renut::online::InSession() && !renut::online::SessionAllowsCheats();
}

}  // namespace renut::game::flags

// The game's main loop pass (its frame: input, the game tick, the menus), on
// its main thread: the editor's changes go in first.
REX_HOOK_RAW(sub_82283BB0) {
  renut::game::flags::ApplyPending(ctx, base);
  __imp__sub_82283BB0(ctx, base);
}
