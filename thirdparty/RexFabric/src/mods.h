// Mods on disk: one folder each under the mods directory, with their load order.
//
// The order lives in mods.json in the mods directory: the enabled mods in load order, and the ones turned
// off. A folder the file doesn't mention is new, and joins the end of the load order enabled, so a mod
// dropped into the folder or installed from Thunderstore just works.
//
//   { "load_order": ["UnlimitedGardenSpace", "FreeCam"], "disabled": ["Test"] }

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rexfabric {

// The load order, in the mods directory.
inline constexpr const char* kOrderFile = "mods.json";

// A mod another needs: its folder, and the lowest version it works with (empty for any).
struct ModDependency {
  std::string mod;
  std::string version;
};

struct ModInfo {
  std::string folder;
  std::filesystem::path dir;
  // From mod.toml, else Thunderstore's manifest.json. The name falls back to the folder.
  std::string name;
  std::string description;
  std::string version;
  std::string author;
  // From mod.toml only (docs/content_and_classes.md, The mod's metadata).
  std::vector<ModDependency> dependencies;
  std::string min_game_version;
  // The language its texts are written in, as the game names it ("englishus"); empty for the game's first.
  std::string language;
  bool enabled = true;
};

// Writes the mod's metadata into its mod.toml: name, description, version, author, min_game_version and
// dependencies, each left out when empty, keeping whatever else the file has (its comments don't survive). A mod
// without one gets one. False with `error` for a mod.toml that doesn't parse, which is left as it is.
bool WriteMetadata(const ModInfo& mod, std::string* error);

// Why a mod can't load, for the log and the menu.
struct RefusedMod {
  std::string folder;
  std::string reason;
};

// The enabled mods that can load, each after the mods it depends on and otherwise in load order. A mod can't load
// when the game is older than its min_game_version, when a dependency isn't enabled or is older than it asks, when a
// dependency can't load itself, or when dependencies go round in a circle. `game_version` empty skips the game check.
std::vector<ModInfo> LoadableMods(const std::vector<ModInfo>& enabled, std::string_view game_version, std::vector<RefusedMod>* refused);

// Compares two versions part by part ("1.10" is above "1.9"): numbers as numbers, anything else as text, a missing
// part as 0. Below zero when a is older.
int CompareVersions(std::string_view a, std::string_view b);

class ModList {
 public:
  explicit ModList(std::filesystem::path root) : root_(std::move(root)) {}

  // Rereads the folders and the order file.
  void Scan();
  bool Save() const;

  // The enabled mods in load order, then the disabled ones by name.
  const std::vector<ModInfo>& mods() const { return mods_; }
  size_t EnabledCount() const;
  const std::filesystem::path& root() const { return root_; }
  const ModInfo* Find(std::string_view folder) const;
  // A folder the order file doesn't name yet, which the last Scan placed by the rules for new folders.
  bool New(std::string_view folder) const;

  // Enabling puts a mod last in the load order; disabling takes it out. Both save.
  void SetEnabled(size_t index, bool enabled);
  // Moves an enabled mod `delta` places in the load order, and saves.
  void Move(size_t index, int delta);
  // Enables these folders in this order and turns every other one off, and saves. Names that match no folder are
  // skipped.
  void SetOrder(const std::vector<std::string>& enabled);
  // Renames a mod's folder on disk, keeping its place in the load order, and saves.
  bool Rename(size_t index, const std::string& folder, std::string* error);

 private:
  void SortDisabled();

  std::filesystem::path root_;
  std::vector<ModInfo> mods_;
  std::vector<std::string> new_;
};

bool SameModName(std::string_view a, std::string_view b);

// The mods folder changed here: the menu, the game or an install turned a mod on or off, moved or renamed one.
// Bumps ModsRevision. Defined in rexfabric.cpp.
void NoteModsChanged();

// The native plugins a mod ships: .asi and the platform's library files, in the mod folder itself or in its
// plugins folder, sorted by name.
std::vector<std::filesystem::path> NativePlugins(const std::filesystem::path& dir);

// The Lua files a mod runs: every .lua directly in its scripts folder, by name. Subfolders are for require.
std::vector<std::filesystem::path> LuaScripts(const std::filesystem::path& dir);

}  // namespace rexfabric
