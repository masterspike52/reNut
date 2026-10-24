#pragma once
// Android: the manager's save management and workshop, without the UI. The
// same formats as NutStaller (etonedemid/NutStaller, Core/Saves.cs and
// Core/Workshop.cs), so a save, a backup or a workshop install moves between
// the PC and the phone as is.
//
// Saves: <user_data_root>/B13EBABEBABEBABE/4D5307ED, backed up as zips in
// /sdcard/reNut/backups (where they can be copied off the phone).
// Workshop: github.com/etonedemid/nutstaller-workshop's index.json; vehicles
// go into the save, part mods into reNut's mods folder.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace renut::content {

namespace fs = std::filesystem;

// /sdcard/reNut: backups, saves to import (a variable for the host test).
extern fs::path shared_root;

// --- saves ---------------------------------------------------------------------

fs::path SaveDir(const fs::path &user_data_root);

struct SaveSummary {
    int slots = 0, vehicles = 0, replays = 0;
    uint64_t bytes = 0;
};
SaveSummary Summarize(const fs::path &save);

fs::path BackupDir();

struct Backup {
    fs::path file;
    std::string made;  // "2026-10-22 11:20:55", from the name; "" for other zips
    std::string note;  // the name after the time, or the file name of other zips
    uint64_t bytes = 0;
};
// Every zip in BackupDir(), newest first.
std::vector<Backup> Backups();

// save-<time>[-note].zip; the file, or empty with *error.
fs::path BackUp(const fs::path &save, const std::string &note, std::string *error);
// Names a backup (letters, digits, spaces, - and _; empty clears it).
fs::path RenameBackup(const fs::path &file, const std::string &name, std::string *error);

// Saves to import: folders in /sdcard/reNut holding one (the 4D5307ED folder,
// or one with it inside).
std::vector<fs::path> ImportCandidates();

// Replaces the save with a zip (a backup) or a folder, after backing up the
// current one (*kept, empty when there was none). False with *error; the save
// is then unchanged.
bool Replace(const fs::path &save, const fs::path &source, const std::string &note, fs::path *kept,
             std::string *error);

// --- workshop ------------------------------------------------------------------

struct Entry {
    std::string id, name, description, author, blueprint, preview;
    int parts = 0;
    std::vector<std::string> requires_mods;  // workshop mod ids the vehicle needs
    std::vector<std::string> missing;        // "part (mod)" not in the workshop
};

struct ModEntry {
    std::string id, mod_id, name, description, author, version, sha256, download, preview;
    std::vector<std::string> part_names;
    uint64_t size = 0;
};

struct Index {
    std::vector<Entry> blueprints;
    std::vector<ModEntry> mods;
};

std::optional<Index> FetchIndex(std::string *error);
// A file of the workshop repo (preview, blueprint).
std::optional<std::vector<uint8_t>> Download(const std::string &repo_path);

struct Installed {
    std::string id, content_id, name, author, description, preview;
    int parts = 0;
};
struct InstalledMod {
    std::string id, mod_id, name, author, description, preview, file;
    int parts = 0;
};

// What came from the workshop and is still there.
std::vector<Installed> LoadInstalled(const fs::path &save);
std::vector<InstalledMod> LoadInstalledMods(const fs::path &save);

fs::path ModsDir();

// Downloads (and checks) the mod into the mods folder.
bool InstallMod(const fs::path &save, const ModEntry &mod, std::string *error);
void RemoveMod(const fs::path &save, const std::string &mod_id);
// Installs the mods the vehicle needs first, then puts it in the save.
bool InstallVehicle(const fs::path &save, const Entry &entry, const Index &index, std::string *error);
void RemoveVehicle(const fs::path &save, const std::string &id);

}  // namespace renut::content
