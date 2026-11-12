#pragma once
// Applies .nbpatch mods at startup: each changed or added file (game or update
// folder) is built into <mods>/.cache/<target SHA-256>.bin and served in place of the original.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rex::kernel {
class TitleRuntime;
}

namespace renut::mods {

// The mods folder (renut_mods_folder, relative to reNut's folder), which is
// also RexFabric's.
std::filesystem::path ModsFolder();

// Call once the title's file system is mounted and before the guest starts
// (RenutApp::OnTitleLaunch, before TitleRuntime::Launch).
void LoadMods(rex::kernel::TitleRuntime &title);

// After rexfabric::Start, whose Lua mods add parts too (renut.parts.add): remembers
// this run's custom parts, and no more are added.
void FinishCustomParts();

// The game's save folder (its vehicles' blueprints are in it), as LoadMods found it; empty before it or without one.
std::filesystem::path SaveFolder();

// The .nbpatch mods LoadMods applied, by name (for crash reports).
std::vector<std::string> LoadedMods();

// The size of one of the game's own files (relative to its data folder: "Bundle/4f/01ede7"), or 0 when it has none.
std::uintmax_t GameFileSize(const std::string &rel);
// ...as the game reads it: a compressed (XCTD) file's unpacked size from its block table, else its size.
std::uintmax_t GameFileUnpackedSize(const std::string &rel);

// Whether stream bundle `id` is a character pack served from <mods>/.characters (NutTool's renut-characters).
bool HasCharacterPack(uint32_t id);

} // namespace renut::mods
