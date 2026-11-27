#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace renut::nuttool {

// A mod NutTool can open: a mod folder (its .nbpatch, made when it has none) or a loose .nbpatch in the mods folder.
struct ModEntry {
  std::string name;               // the folder's or file's name
  std::filesystem::path location; // the folder, or the .nbpatch
  bool loose = false;
};

// The mods in the mods folder, by name.
std::vector<ModEntry> ListMods(const std::filesystem::path &mods);

// Opens a mod for editing (a new folder when `location` doesn't exist): NutTool's saves to game files go to its
// workspace from now on, which its package's changes are expanded into the first time. False with `error` set when
// its package can't be read.
bool OpenMod(const std::filesystem::path &game, const std::filesystem::path &mods, const std::filesystem::path &location,
             std::string *error);
// A mod's package unpacked into `out` in the game's layout (its files as the game would get them), for looking at it
// without opening it; done again only when the package changed. False with `error` set when it has none or can't be read.
bool UnpackMod(const std::filesystem::path &game, const std::filesystem::path &location, const std::filesystem::path &out,
               std::string *error);
// Stops sending saves to a mod: they go to a scratch workspace (NutTool's folder, scratch) until one is opened, which
// takes them. The game's files are never written.
void CloseMod(const std::filesystem::path &game, const std::filesystem::path &mods);

// The open mod's folder or loose package, and its name; empty when none is open.
std::filesystem::path OpenModLocation();
std::string OpenModName();

// Writes the open mod's package from its workspace (every file there that differs from the game's), and its mod.toml
// for a folder. Lines about what it did go to `log`; false when it couldn't.
bool SaveMod(const std::filesystem::path &game, const std::string &author, std::vector<std::string> *log);

} // namespace renut::nuttool
