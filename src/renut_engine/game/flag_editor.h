/**
 * @file    game/flag_editor.h
 * @brief   Player 1's game flags and counters, by the game's own names
 *          (flag_editor.cpp; the window is ui/flag_editor_dialog.h, F7).
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace renut::game::flags {

// N&B's named tables: gameFlag_Normal_*, gameFlag_Global_*,
// gameFlag_Volatile_* (on/off) and gameCounter_BanjoX_* (32-bit counts).
enum class Table { kNormal, kGlobal, kVolatile, kCounters };
constexpr int kTableCount = 4;

// UI thread: the table's names, by the game's id (0 is "..._Null", unused);
// empty until the game has set its tables up.
const std::vector<std::string> &Names(Table table);

// Whether player 1 has a profile (a save loaded).
bool HasProfile();

// Player 1's value: a flag's 0 or 1, a counter's count. A change not yet made
// shows already.
int32_t Get(Table table, uint32_t index);

// Changes player 1's value on the game's next frame, and marks the profile
// changed so the game's next save keeps it. False (and nothing changes) in an
// online session whose host doesn't allow cheats.
bool Set(Table table, uint32_t index, int32_t value);

// An online session whose host doesn't allow cheats: Set changes nothing.
bool Blocked();

}  // namespace renut::game::flags
