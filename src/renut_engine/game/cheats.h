#pragma once

namespace renut::game {

bool AnyCheatOn();


void WatchCheats();


void NotesSet();


void SaveEdited();

// Whether a mod (RexFabric's or an .nbpatch) was on this session; it stays true until the game restarts.
bool AnyModUsed();
// At title launch, after the mods load: notes the mods it started with.
void NoteModsLoaded();

}
