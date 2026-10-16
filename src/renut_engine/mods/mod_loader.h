#pragma once
// Runtime .nbpatch mods (NB Studio's distributable patches).
//
// At startup every *.nbpatch in the mods folder (renut_mods_folder, next to
// renut.exe by default) is checked against the player's game files, in
// file-name order. Each changed or added file is built once into
// <mods>/.cache/<target SHA-256>.bin and served to the guest in place of the
// original; the game folder itself is never written. A mod applies whole or
// not at all: a missing or different original file, a conflict with an
// earlier mod or a damaged package skips it with a log message.
//
// Not applied (logged per mod):
// - executable mods (ExeMods / default.xex word patches): the game code is
//   recompiled ahead of time, so patching default.xex changes nothing;
// - world edits (patch.json format 3 Ops), which NB Studio replays with its
//   own bundle editor;
// - two mods changing the same file (NB Studio merges them asset by asset);
//   the later one in file-name order is skipped.

namespace rex {
class Runtime;
}

namespace renut::mods {

// Call once the runtime's file system is set up and before the game module
// starts (ReXApp::OnPostSetup).
void LoadMods(rex::Runtime *runtime);

} // namespace renut::mods
