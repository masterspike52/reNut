#include "renut_engine/game/flag_editor.h"

#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>

#include <array>
#include <mutex>

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
  return rex::byte_swap(*REX_RUNTIME()->GetMembase().TranslateVirtual<uint32_t *>(va));
}

uint8_t *Bytes(uint32_t va) {
  return REX_RUNTIME()->GetMembase().TranslateVirtual<uint8_t *>(va);
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

// Game thread. Whether the value changed.
bool WriteValue(uint32_t profile, const Edit &edit) {
  const uint32_t va = ValueAddress(profile, edit.table, edit.index);
  if (!va)
    return false;
  if (ReadValue(profile, edit.table, edit.index) == edit.value)
    return false;
  if (edit.table == Table::kCounters) {
    *REX_RUNTIME()->GetMembase().TranslateVirtual<uint32_t *>(va) =rex::byte_swap(uint32_t(edit.value));
  } else {
    const uint8_t bit = uint8_t(1u << (edit.index & 7));
    uint8_t *byte = Bytes(va);
    *byte = edit.value ? uint8_t(*byte | bit) : uint8_t(*byte & ~bit);
  }
  return true;
}

void ApplyPending(PPCContext &ctx, uint8_t *base) {
  std::vector<Edit> edits;
  {
    std::lock_guard lock(g_mutex);
    if (g_pending.empty())
      return;
    edits.swap(g_pending);
  }
  // Player 1's profile went before the changes were made: they're dropped.
  const uint32_t profile = Profile();
  if (!profile)
    return;
  size_t changed = 0;
  for (const Edit &edit : edits)
    changed += WriteValue(profile, edit);
  if (!changed)
    return;
  renut::game::SaveEdited();
  renut::guest::Guest g(ctx, base);
  ProfileChanged(g.ctx(), base, profile);
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
        names.emplace_back(name ? REX_RUNTIME()->GetMembase().TranslateVirtual<const char *>(name) : "");
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

// The game's main loop pass (sub_82283BB0): the editor's changes go in first.
REX_HOOK_RAW(sub_82283BB0) {
  renut::game::flags::ApplyPending(ctx, base);
  __imp__sub_82283BB0(ctx, base);
}
