/**
 * @file    game/cheats.h
 * @brief   Whether anything in the Cheats menu is in use (cheats.cpp).
 */
#pragma once

namespace renut::game {

// Any Nuts&Bolts/Cheats switch on since the game started (on now or turned
// off since), or Set Notes or the flag editor used. Leaderboard scores and
// replays aren't posted then (renut_app.h). Any thread.
bool AnyCheatOn();

// Starts noting the cheat switches being turned on (and any already on, from
// the config). Once at startup, after the config is loaded and before the game
// runs (RenutApp::OnPostSetup).
void WatchCheats();

// Set Notes gave player 1 notes (notes.cpp).
void NotesSet();

// The flag editor changed player 1's flags or counters (flag_editor.cpp).
void SaveEdited();

}  // namespace renut::game
