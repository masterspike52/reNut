#include "renut_engine/mods/mod_loader.h"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <rex/filesystem/file_system.h>
#include <rex/kernel/title_paths.h>
#include <rex/kernel/title_runtime.h>
#include <rex/path_settings.h>
#include <rex/setting.h>
#include <rex/string/text.h>
#include <rexfabric/rexfabric.h>

#include "renut_engine/app/app_paths.h"
#include "renut_engine/app/renut_settings.h"
#include "renut_engine/app/workshop.h"
#include "renut_engine/mods/custom_parts.h"
#include "renut_engine/mods/mod_device.h"
#include "renut_engine/mods/nbpatch.h"
#include "renut_engine/mods/sha256.h"
#include "renut_engine/mods/xcompress.h"
#include "renut_engine/mods/zip_reader.h"

namespace {
struct ModSettings final : rex::SettingOwner {
    ModSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
    REX_SETTING(bool, renut_mods, true, "Applies the .nbpatch mods in the mods folder when the game starts.");
    REX_SETTING(std::string, renut_mods_folder, "mods",
        "Mods folder, relative to reNut's folder: loose .nbpatch mods (NB Studio patches), which load in file-name "
        "order, and a folder per mod (the mods menu), whose .nbpatch files load after them in the menu's order.");
};
ModSettings &g_mod_settings = *new ModSettings();  // never freed, registered by RenutApp::OnConfigure
}  // namespace

namespace renut::mods {

namespace {

namespace fs = std::filesystem;

// The guest partitions game: and d:, and update:, resolve to; each mod file gets a link from its path there.
constexpr std::string_view kGamePartition = "\\Device\\Harddisk0\\Partition1";
constexpr std::string_view kUpdatePartition = "\\Device\\Harddisk0\\PartitionUpdate";
// Each host folder mod files are served from is mounted read-only as this plus a number.
constexpr std::string_view kModFilesMount = "\\Device\\RenutModFiles";
// A game folder a mod adds files to is served whole from a merged copy of hard links, mounted as this plus a number.
constexpr std::string_view kMergedMount = "\\Device\\RenutModMerged";

// A file a mod changes or adds.
struct Redirect {
    std::string path;           // relative to the game or update folder, '/' separated, as the mod names it
    std::filesystem::path host; // the file served instead
};

// Patch paths are relative to the game folder, whose "update" folder is the title
// update's by default: paths under it name UPDATE: files.
constexpr std::string_view kUpdatePrefix = "update/";

// The mods applied (LoadedMods; read by crash reports from any thread).
std::mutex g_loaded_mutex;
std::vector<std::string> g_loaded;

std::string Lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

// The .nbpatch files directly in 'dir', in file-name order.
std::vector<fs::path> PackagesIn(const fs::path &dir) {
    std::error_code ec;
    std::vector<fs::path> packages;
    for (const auto &entry : fs::directory_iterator(dir, ec))
        if (entry.is_regular_file(ec) && Lower(rex::string::PathToUTF8(entry.path().extension())) == ".nbpatch")
            packages.push_back(entry.path());
    std::sort(packages.begin(), packages.end(), [](const fs::path &a, const fs::path &b) {
        return Lower(rex::string::PathToUTF8(a.filename())) < Lower(rex::string::PathToUTF8(b.filename()));
    });
    return packages;
}

bool IsSha256(const std::string &s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

// A bundle file's id: "Bundle/4f/685374" -> 0x4F685374 (the game opens
// bundles as GAME:bundle\<type>\<id>); 0 for any other file.
uint32_t BundleId(const std::string &path) {
    const std::string key = PathKey(path);
    uint32_t type = 0;
    if (key.starts_with("bundle/4f/"))
        type = 0x4F;
    else if (key.starts_with("bundle/50/"))
        type = 0x50;
    else
        return 0;
    const std::string_view rest = std::string_view(key).substr(10);
    if (rest.size() != 6)
        return 0;
    uint32_t id = 0;
    for (char c : rest) {
        if (c >= '0' && c <= '9')
            id = id << 4 | uint32_t(c - '0');
        else if (c >= 'a' && c <= 'f')
            id = id << 4 | uint32_t(c - 'a' + 10);
        else
            return 0;
    }
    return type << 24 | id;
}

// A path inside the game folder: relative, no "." or ".." parts.
bool IsSafeRelative(const std::string &path) {
    if (path.empty() || path[0] == '/' || path[0] == '\\' || path.find(':') != std::string::npos)
        return false;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of("/\\", start);
        if (end == std::string::npos)
            end = path.size();
        std::string_view part(path.data() + start, end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        start = end + 1;
    }
    return true;
}

// The game file a patch path names, matching names without regard to case
// (the guest does; a Linux game folder may differ in case). Empty if missing.
fs::path FindGameFile(const fs::path &game_root, const std::string &rel) {
    std::error_code ec;
    fs::path direct = game_root / rex::string::to_path(rel);
    if (fs::is_regular_file(direct, ec))
        return direct;
    fs::path at = game_root;
    size_t start = 0;
    while (start < rel.size()) {
        size_t end = rel.find_first_of("/\\", start);
        if (end == std::string::npos)
            end = rel.size();
        const std::string want = PathKey(rel.substr(start, end - start));
        fs::path found;
        for (const auto &child : fs::directory_iterator(at, ec)) {
            if (PathKey(rex::string::PathToUTF8(child.path().filename())) == want) {
                found = child.path();
                break;
            }
        }
        if (found.empty())
            return {};
        at = found;
        start = end + 1;
    }
    return fs::is_regular_file(at, ec) ? at : fs::path{};
}

std::optional<std::vector<uint8_t>> ReadWholeFile(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    file.seekg(0, std::ios::end);
    std::vector<uint8_t> data(size_t(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(data.data()), std::streamsize(data.size()));
    if (!file)
        return std::nullopt;
    return data;
}

bool WriteCacheFile(const fs::path &path, const std::vector<uint8_t> &data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
        if (!file)
            return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

enum class Built { kServe, kAlreadyInGame, kFailed };

// Builds the file's patched version into the cache (original + delta, or whole for a
// new file), checked against the mod's SHA-256. 'rel' is its path within 'root'.
Built BuildFile(ZipReader &zip, const PatchFile &f, const fs::path &root, const std::string &rel, const fs::path &cached,
                std::string *error) {
    std::error_code ec;
    if (fs::is_regular_file(cached, ec) && int64_t(fs::file_size(cached, ec)) == f.target_size && !ec)
        return Built::kServe;

    const std::string target = Lower(f.target_sha256);
    const fs::path original = FindGameFile(root, rel);
    std::optional<std::vector<uint8_t>> result;
    if (f.kind == "new") {
        if (!original.empty()) {
            if (Sha256File(original) == target)
                return Built::kAlreadyInGame;
            *error = f.path + " already exists in the game folder";
            return Built::kFailed;
        }
        result = zip.Read(f.entry, error);
    } else {
        if (original.empty()) {
            *error = f.path + " is not in the game folder";
            return Built::kFailed;
        }
        auto raw = ReadWholeFile(original);
        if (!raw) {
            *error = "cannot read " + f.path;
            return Built::kFailed;
        }
        const std::string sha = Sha256Hex(*raw);
        if (sha == target)
            return Built::kAlreadyInGame;
        if (sha != Lower(f.source_sha256)) {
            *error = f.path + " is not the original file this mod was made for (SHA-256 " + sha.substr(0, 12) + "...)";
            return Built::kFailed;
        }
        auto source = ExpandGameFile(std::move(*raw), error);
        if (!source)
            return Built::kFailed;
        auto delta = zip.Read(f.entry, error);
        if (!delta)
            return Built::kFailed;
        result = ApplyDelta(*source, *delta, error);
        if (!result)
            *error = f.path + ": " + *error;
    }
    if (!result)
        return Built::kFailed;
    if (int64_t(result->size()) != f.target_size || Sha256Hex(*result) != target) {
        *error = f.path + ": the result does not match the mod (SHA-256)";
        return Built::kFailed;
    }
    if (!WriteCacheFile(cached, *result)) {
        *error = "cannot write " + rex::string::PathToUTF8(cached);
        return Built::kFailed;
    }
    return Built::kServe;
}

struct LoadState {
    fs::path game_root;
    fs::path update_root;  // empty without a mounted update folder
    fs::path cache_dir;
    std::map<std::string, Redirect> redirects;         // by PathKey
    std::map<std::string, Redirect> update_redirects;  // by PathKey within update_root
    std::map<std::string, std::string> owner;                         // PathKey -> mod name
    std::set<std::string> loaded_ids;                          // lower-case
    std::vector<std::pair<std::string, std::vector<std::string>>> loaded_conflicts;
    std::set<fs::path> used_cache;
};

bool IsUpdateFile(const LoadState &state, const std::string &path) {
    return !state.update_root.empty() && PathKey(path).starts_with(kUpdatePrefix);
}

// Applies one mod whole or skips it (newer format, already loaded, missing requirement,
// conflict, bad file record, file another mod changes, or a build failure).
void LoadMod(const fs::path &package, LoadState &state) {
    std::string error;
    ZipReader zip;
    if (!zip.Open(package, &error))
        return;
    auto manifest = ReadManifest(zip, &error);
    if (!manifest)
        return;
    const PatchManifest &m = *manifest;
    const std::string id = Lower(m.id);

    if (m.format > kNewestPatchFormat || state.loaded_ids.contains(id))
        return;
    for (const std::string &r : m.requires_ids) {
        if (!state.loaded_ids.contains(Lower(r)))
            return;
    }
    for (const auto &[other, other_conflicts] : state.loaded_conflicts) {
        bool clash = std::any_of(m.conflicts.begin(), m.conflicts.end(), [&](const std::string &c) { return Lower(c) == other; }) ||
                     std::any_of(other_conflicts.begin(), other_conflicts.end(), [&](const std::string &c) { return Lower(c) == id; });
        if (clash)
            return;
    }

    // Everything is checked before anything is built.
    for (const PatchFile &f : m.files) {
        if (f.kind == "xexmods")
            continue;
        if (f.kind != "delta" && f.kind != "new")
            return;
        if (!IsSafeRelative(f.path) || !IsSha256(f.target_sha256) || (f.kind == "delta" && !IsSha256(f.source_sha256)))
            return;
        if (state.owner.contains(PathKey(f.path)))
            return;
    }
    auto parts = ReadCustomParts(zip, &error);
    if (!parts)
        return;

    struct Serve {
        bool update;
        std::string key;  // PathKey within its folder
        Redirect redirect;
    };
    std::vector<Serve> serve;
    for (const PatchFile &f : m.files) {
        if (f.kind == "xexmods")
            continue;
        const bool update = IsUpdateFile(state, f.path);
        const std::string rel = update ? f.path.substr(kUpdatePrefix.size()) : f.path;
        const fs::path cached = state.cache_dir / rex::string::to_path(Lower(f.target_sha256) + ".bin");
        switch (BuildFile(zip, f, update ? state.update_root : state.game_root, rel, cached, &error)) {
        case Built::kServe:
            serve.push_back({update, PathKey(rel), {rel, cached}});
            break;
        case Built::kAlreadyInGame:
            break;
        case Built::kFailed:
            return;
        }
    }

    for (Serve &s : serve) {
        state.used_cache.insert(s.redirect.host);
        state.owner[s.update ? std::string(kUpdatePrefix) + s.key : s.key] = m.name;
        (s.update ? state.update_redirects : state.redirects)[s.key] = std::move(s.redirect);
    }
    state.loaded_ids.insert(id);
    state.loaded_conflicts.push_back({id, m.conflicts});
    {
        std::lock_guard lock(g_loaded_mutex);
        g_loaded.push_back(m.version.empty() ? m.name : m.name + " " + m.version);
    }

    if (!parts->empty()) {
        // Its own bundles (models, collision, textures of its parts) load with
        // the common bundle.
        for (const PatchFile &f : m.files)
            if (f.kind == "new")
                if (const uint32_t bundle = BundleId(f.path))
                    AddModBundle({bundle, m.name});
    }
    for (CustomPart &part : *parts)
        part.mod = m.name;
    // Its executable changes (no effect on recompiled code) and world edits
    // aren't applied.
    AddCustomParts(std::move(*parts));
}

// A mod folder's own bundles, built by renut-modtool models (bundles/Bundle/4f|50/<id>):
// served as new game files and loaded with the common bundle, for its Lua parts' models.
// Its worlds' bundles (worlds/Bundle/4f|50/<id>) are only served: their scenes load them.
// The bundle files under `root` (Bundle/4f|50/<id>) served as new game files for `owner`; the ids of those served.
std::vector<uint32_t> ServeBundles(const fs::path &root, const std::string &owner, bool resident, LoadState &state) {
    std::vector<uint32_t> served;
    std::error_code ec;
    for (const char *type : {"4f", "50"}) {
        for (const auto &entry : fs::directory_iterator(root / "Bundle" / type, ec)) {
            if (!entry.is_regular_file(ec))
                continue;
            const std::string rel = std::string("Bundle/") + type + "/" + rex::string::PathToUTF8(entry.path().filename());
            const uint32_t id = BundleId(rel);
            const std::string key = PathKey(rel);
            // Not a bundle file, one another mod serves, or the id of a bundle of the game.
            if (!id || state.owner.contains(key) || !FindGameFile(state.game_root, rel).empty())
                continue;
            state.owner[key] = owner;
            state.redirects[key] = {rel, entry.path()};
            if (resident)
                AddModBundle({id, owner});
            served.push_back(id);
        }
    }
    return served;
}

void LoadFolderBundles(const fs::path &dir, LoadState &state) {
    const std::string mod = rex::string::PathToUTF8(dir.filename());
    ServeBundles(dir / "bundles", mod, true, state);
    ServeBundles(dir / "worlds", mod, false, state);
}

// Removes cache files no loaded mod uses (mods removed or updated).
void PruneCache(const LoadState &state) {
    std::error_code ec;
    if (!fs::is_directory(state.cache_dir, ec))
        return;
    std::vector<fs::path> stale;
    for (const auto &entry : fs::directory_iterator(state.cache_dir, ec))
        if (entry.is_regular_file(ec) && !state.used_cache.contains(entry.path()))
            stale.push_back(entry.path());
    for (const fs::path &p : stale)
        fs::remove(p, ec);
}

// Whether a folder of 'rel' is a file in 'root', where the mod's file can't be added.
bool FolderIsGameFile(const fs::path &root, const std::string &rel) {
    for (size_t end = rel.find_first_of("/\\"); end != std::string::npos; end = rel.find_first_of("/\\", end + 1))
        if (!FindGameFile(root, rel.substr(0, end)).empty())
            return true;
    return false;
}

// The game folder of 'rel' ('/' separated, any case), or empty when there's none.
fs::path FindGameFolder(const fs::path &root, const std::string &rel) {
    std::error_code ec;
    fs::path at = root;
    size_t start = 0;
    while (start < rel.size()) {
        size_t end = rel.find('/', start);
        if (end == std::string::npos)
            end = rel.size();
        const std::string want = PathKey(rel.substr(start, end - start));
        fs::path found;
        for (const auto &child : fs::directory_iterator(at, ec)) {
            if (child.is_directory(ec) && PathKey(rex::string::PathToUTF8(child.path().filename())) == want) {
                found = child.path();
                break;
            }
        }
        if (found.empty())
            return {};
        at = found;
        start = end + 1;
    }
    return at;
}

// Puts 'source' at 'dest': a hard link, else a symbolic link, else a copy.
bool PlaceFile(const fs::path &source, const fs::path &dest) {
    std::error_code ec;
    fs::create_hard_link(source, dest, ec);
    if (!ec)
        return true;
    ec.clear();
    fs::create_symlink(source, dest, ec);
    if (!ec)
        return true;
    ec.clear();
    return fs::copy_file(source, dest, fs::copy_options::overwrite_existing, ec);
}

std::string FolderOf(const std::string &rel) {
    const size_t slash = rel.find_last_of('/');
    return slash == std::string::npos ? std::string() : rel.substr(0, slash);
}

std::string NameOf(const std::string &rel) {
    const size_t slash = rel.find_last_of('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

// Serves the mods' files in 'root' (at 'partition'): a game file is linked to the mod's on its own. A folder a mod
// adds files to is served whole from a merged copy, as the SDK drops a linked file its parent folder lacks.
void Mount(rex::filesystem::IFileSystem &vfs, std::string_view partition, const fs::path &root,
           const std::map<std::string, Redirect> &redirects, std::map<fs::path, std::string> &mounts,
           const fs::path &merged_root, size_t &merged_count) {
    std::error_code ec;
    if (redirects.empty() || !fs::is_directory(root, ec))
        return;
    std::vector<const Redirect *> served;
    std::map<std::string, std::string> added;  // PathKey of a folder a mod adds a file to -> the folder
    for (const auto &[key, redirect] : redirects) {
        if (!fs::is_regular_file(redirect.host, ec) || FolderIsGameFile(root, redirect.path))
            continue;
        served.push_back(&redirect);
        const std::string folder = FolderOf(redirect.path);
        if (!folder.empty() && FindGameFile(root, redirect.path).empty())
            added.emplace(PathKey(folder), folder);
    }

    std::map<std::string, std::vector<const Redirect *>> merged;  // by folder PathKey
    for (const auto &[folder_key, folder] : added) {
        const fs::path game_folder = FindGameFolder(root, folder);
        bool flat = true;
        for (const auto &entry : fs::directory_iterator(game_folder, ec))
            flat = flat && !entry.is_directory(ec);
        if (flat)  // a folder with folders of its own keeps the per-file links
            merged[folder_key];
    }
    std::vector<const Redirect *> single;
    for (const Redirect *redirect : served) {
        auto folder = merged.find(PathKey(FolderOf(redirect->path)));
        if (folder != merged.end())
            folder->second.push_back(redirect);
        else
            single.push_back(redirect);
    }

    for (const auto &[folder_key, files] : merged) {
        const std::string &folder = added.at(folder_key);
        const fs::path staging = merged_root / rex::string::to_path(folder);
        fs::create_directories(staging, ec);
        std::map<std::string, std::string> names;  // PathKey of a name in the copy -> the name
        const fs::path game_folder = FindGameFolder(root, folder);
        for (const auto &entry : fs::directory_iterator(game_folder, ec)) {
            const std::string name = rex::string::PathToUTF8(entry.path().filename());
            if (entry.is_regular_file(ec) && PlaceFile(entry.path(), staging / entry.path().filename()))
                names[PathKey(name)] = name;
        }
        for (const Redirect *redirect : files) {
            std::string name = NameOf(redirect->path);
            if (auto existing = names.find(PathKey(name)); existing != names.end()) {
                name = existing->second;
                fs::remove(staging / rex::string::to_path(name), ec);
            }
            PlaceFile(redirect->host, staging / rex::string::to_path(name));
        }
        const std::string device = std::string(kMergedMount) + std::to_string(merged_count++);
        if (!vfs.Mount({device, rex::filesystem::ProviderType::kHostDirectory, staging}))
            continue;
        std::string link = std::string(partition) + "\\" + folder;
        std::replace(link.begin(), link.end(), '/', '\\');
        (void)vfs.Link(link, device);
    }

    for (const Redirect *redirect_ptr : single) {
        const Redirect &redirect = *redirect_ptr;
        const fs::path folder = redirect.host.parent_path();
        auto mount = mounts.find(folder);
        if (mount == mounts.end()) {
            std::string device = std::string(kModFilesMount) + std::to_string(mounts.size());
            if (!vfs.Mount({device, rex::filesystem::ProviderType::kHostDirectory, folder}))
                continue;
            mount = mounts.emplace(folder, std::move(device)).first;
        }
        std::string link = std::string(partition) + "\\" + redirect.path;
        std::replace(link.begin(), link.end(), '/', '\\');
        (void)vfs.Link(link, mount->second + "\\" + rex::string::PathToUTF8(redirect.host.filename()));
    }
}

} // namespace

fs::path ModsFolder() {
    fs::path folder = rex::string::to_path(g_mod_settings.renut_mods_folder.Get());
    if (folder.is_relative())
        folder = renut::paths::WritableFolder() / folder;
    return folder;
}

void FinishCustomParts() {
    RememberCustomParts(ModsFolder());
    FixCustomParts();
}

std::vector<std::string> LoadedMods() {
    std::lock_guard lock(g_loaded_mutex);
    return g_loaded;
}

namespace {

// A blueprint's name (UTF-16BE at 0x28, 32 characters), ASCII kept.
std::string BlueprintName(const std::string &blueprint) {
    std::string name;
    for (size_t i = 0; i < 32 && 0x29 + 2 * i < blueprint.size(); ++i) {
        const uint16_t c = uint16_t(uint8_t(blueprint[0x28 + 2 * i]) << 8 | uint8_t(blueprint[0x29 + 2 * i]));
        if (!c)
            break;
        name += c < 0x80 ? char(c) : '?';
    }
    return name;
}

// The game's save folder, as LoadMods found it (SaveFolder).
fs::path g_save_folder;

// The enabled mod folders' vehicles (vehicles/*.blueprint) go into the save once, as the workshop's
// do; <mods>/.renut_vehicles remembers them, so one the player deletes stays deleted.
void InstallVehicles(const fs::path &user_root) {
    if (user_root.empty())
        return;
    std::error_code ec;
    const fs::path record = ModsFolder() / ".renut_vehicles";
    std::set<std::string> done;
    {
        std::ifstream in(record);
        for (std::string line; std::getline(in, line);)
            if (!line.empty())
                done.insert(line);
    }
    const fs::path save = renut::workshop::SaveFolder(user_root);
    bool changed = false;
    for (const fs::path &dir : rexfabric::EnabledModDirs()) {
        for (const auto &entry : fs::directory_iterator(dir / "vehicles", ec)) {
            if (!entry.is_regular_file(ec) || Lower(rex::string::PathToUTF8(entry.path().extension())) != ".blueprint")
                continue;
            const std::string id =
                "mod:" + rex::string::PathToUTF8(dir.filename()) + "/" + rex::string::PathToUTF8(entry.path().stem());
            if (done.contains(id))
                continue;
            const auto data = ReadWholeFile(entry.path());
            if (!data)
                continue;
            const std::string blueprint(data->begin(), data->end());
            renut::workshop::Entry vehicle;
            vehicle.id = id;
            vehicle.name = BlueprintName(blueprint);
            if (renut::workshop::Install(save, vehicle, blueprint, nullptr)) {
                done.insert(id);
                changed = true;
            }
        }
    }
    if (changed) {
        std::ofstream out(record, std::ios::trunc);
        for (const std::string &id : done)
            out << id << '\n';
    }
}

} // namespace

// The game's data folder, for GameFileSize (set by LoadMods, mods on or off).
static fs::path g_game_root;

std::uintmax_t GameFileSize(const std::string &rel) {
    if (g_game_root.empty())
        return 0;
    const fs::path file = FindGameFile(g_game_root, rel);
    std::error_code ec;
    const std::uintmax_t size = file.empty() ? 0 : fs::file_size(file, ec);
    return ec ? 0 : size;
}

// An XCTD file: magic, version, id, flags (bits 6-21 its block count, bits 22-23 its table's entry width: 0 for 20
// bits, 1 for 32), then each block's unpacked size, bit-packed from the top. Without blocks it's stored as is.
std::uintmax_t GameFileUnpackedSize(const std::string &rel) {
    if (g_game_root.empty())
        return 0;
    const fs::path file = FindGameFile(g_game_root, rel);
    std::ifstream in(file, std::ios::binary);
    uint8_t head[16];
    if (file.empty() || !in.read(reinterpret_cast<char *>(head), sizeof(head)))
        return GameFileSize(rel);
    auto be32 = [](const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
    const uint32_t flags = be32(head + 12);
    const uint32_t count = (flags >> 6) & 0xFFFF, width = ((flags >> 22) & 3) ? 32 : 20;
    if (be32(head) != 0x0FF512EDu || count == 0)
        return GameFileSize(rel);
    std::vector<uint8_t> table((size_t(width) * count + 31) / 32 * 4 + 4, 0);
    in.read(reinterpret_cast<char *>(table.data()), std::streamsize(table.size() - 4));
    std::uintmax_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t bit = uint64_t(width) * i;
        const uint64_t word = uint64_t(be32(&table[(bit >> 5) * 4])) << 32 | be32(&table[(bit >> 5) * 4 + 4]);
        total += uint32_t((word << (bit & 31)) >> (64 - width));
    }
    return total;
}

// The character packs LoadMods serves (set before the game starts).
static std::set<uint32_t> g_character_packs;

bool HasCharacterPack(uint32_t id) { return g_character_packs.contains(id); }

void LoadMods(rex::kernel::TitleRuntime &title) {
    const fs::path folder = ModsFolder();
    std::error_code ec;
    const fs::path game_data_root = title.GetTitlePaths().game_root.Get();
    const fs::path update_data_root = title.GetTitlePaths().update_root.Get();
    const fs::path user_data_root = REX_PATHS().user_root.Get();
    if (!game_data_root.empty())
        g_game_root = fs::absolute(game_data_root, ec);
    if (!g_mod_settings.renut_mods.Get())
        return;
    if (game_data_root.empty())
        return;
    rex::filesystem::IFileSystem &vfs = title.GetFileSystem();

    LoadState state;
    state.game_root = fs::absolute(game_data_root, ec);
    if (!update_data_root.empty()) {
        const fs::path update_root = fs::absolute(update_data_root, ec);
        if (fs::is_directory(update_root, ec))
            state.update_root = update_root;
    }
    fs::create_directories(folder, ec);
    state.cache_dir = folder / ".cache";

    // The loose files, then the enabled mod folders' in the load order.
    std::vector<fs::path> packages = PackagesIn(folder);
    for (const fs::path &dir : rexfabric::EnabledModDirs()) {
        std::vector<fs::path> in_dir = PackagesIn(dir);
        packages.insert(packages.end(), in_dir.begin(), in_dir.end());
    }

    for (const fs::path &package : packages)
        LoadMod(package, state);
    for (const fs::path &dir : rexfabric::EnabledModDirs())
        LoadFolderBundles(dir, state);
    for (const uint32_t id : ServeBundles(folder / ".characters", "character packs", false, state))
        g_character_packs.insert(id);
    PruneCache(state);
    InstallVehicles(user_data_root);
    if (!user_data_root.empty())
        g_save_folder = renut::workshop::SaveFolder(user_data_root);

    // The merged folders are rebuilt each start; they're links, so removing them leaves the files alone.
    const fs::path merged_root = folder / ".merged";
    fs::remove_all(merged_root, ec);
    std::map<fs::path, std::string> mounts;
    size_t merged_count = 0;
    Mount(vfs, kGamePartition, state.game_root, state.redirects, mounts, merged_root / "game", merged_count);
    Mount(vfs, kUpdatePartition, state.update_root, state.update_redirects, mounts, merged_root / "update",
          merged_count);
}

fs::path SaveFolder() {
    return g_save_folder;
}

} // namespace renut::mods
