#include "renut_engine/mods/mod_loader.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/runtime.h>

#include "renut_engine/app/app_paths.h"
#include "renut_engine/app/renut_logging.h"
#include "renut_engine/mods/custom_parts.h"
#include "renut_engine/mods/mod_device.h"
#include "renut_engine/mods/nbpatch.h"
#include "renut_engine/mods/sha256.h"
#include "renut_engine/mods/xcompress.h"
#include "renut_engine/mods/zip_reader.h"

REXCVAR_DEFINE_BOOL(renut_mods, true, "Nuts&Bolts/Mods",
    "Applies the .nbpatch mods in the mods folder when the game starts.");
REXCVAR_DEFINE_STRING(renut_mods_folder, "mods", "Nuts&Bolts/Mods",
    "Folder of .nbpatch mods (NB Studio patches), relative to reNut's folder. Mods load in file-name order.");

namespace renut::mods {

namespace {

namespace fs = std::filesystem;

// Where game: and d: point once mods are applied (see mod_device.h). No
// other device's mount path may be a prefix of it.
constexpr std::string_view kMountPath = "\\Device\\RenutModdedGame";

std::string Lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
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
    fs::path direct = game_root / rex::to_path(rel);
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
            if (PathKey(rex::path_to_utf8(child.path().filename())) == want) {
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

// Makes sure the file's patched version is in the cache: built from the
// player's original and the package's delta (or taken whole for a new file),
// and checked against the SHA-256 the mod was made with.
Built BuildFile(ZipReader &zip, const PatchFile &f, const fs::path &game_root, const fs::path &cached, std::string *error) {
    std::error_code ec;
    if (fs::is_regular_file(cached, ec) && int64_t(fs::file_size(cached, ec)) == f.target_size && !ec)
        return Built::kServe;

    const std::string target = Lower(f.target_sha256);
    const fs::path original = FindGameFile(game_root, f.path);
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
        RNUT_INFO("mods: building {}", f.path);
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
        *error = "cannot write " + rex::path_to_utf8(cached);
        return Built::kFailed;
    }
    return Built::kServe;
}

struct LoadState {
    fs::path game_root;
    fs::path cache_dir;
    std::map<std::string, ModGameDevice::Redirect> redirects;  // by PathKey
    std::map<std::string, std::string> owner;                  // PathKey -> mod name
    std::set<std::string> loaded_ids;                          // lower-case
    std::vector<std::pair<std::string, std::vector<std::string>>> loaded_conflicts;
    std::set<fs::path> used_cache;
    int mods = 0;
};

void LoadMod(const fs::path &package, LoadState &state) {
    const std::string file_name = rex::path_to_utf8(package.filename());
    std::string error;
    ZipReader zip;
    if (!zip.Open(package, &error)) {
        RNUT_ERROR("mods: {}: {}", file_name, error);
        return;
    }
    auto manifest = ReadManifest(zip, &error);
    if (!manifest) {
        RNUT_ERROR("mods: {}: {}", file_name, error);
        return;
    }
    const PatchManifest &m = *manifest;
    const std::string label = "'" + m.name + "' " + m.version + " (" + file_name + ")";
    const std::string id = Lower(m.id);

    if (m.format > kNewestPatchFormat) {
        RNUT_ERROR("mods: {} skipped: made with a newer NB Studio (format {})", label, m.format);
        return;
    }
    if (state.loaded_ids.contains(id)) {
        RNUT_ERROR("mods: {} skipped: mod '{}' is already loaded", label, m.id);
        return;
    }
    for (const std::string &r : m.requires_ids) {
        if (!state.loaded_ids.contains(Lower(r))) {
            RNUT_ERROR("mods: {} skipped: it needs the mod '{}' loaded before it (mods load in file-name order)", label, r);
            return;
        }
    }
    for (const auto &[other, other_conflicts] : state.loaded_conflicts) {
        bool clash = std::any_of(m.conflicts.begin(), m.conflicts.end(), [&](const std::string &c) { return Lower(c) == other; }) ||
                     std::any_of(other_conflicts.begin(), other_conflicts.end(), [&](const std::string &c) { return Lower(c) == id; });
        if (clash) {
            RNUT_ERROR("mods: {} skipped: it cannot be combined with '{}' (their authors say so)", label, other);
            return;
        }
    }

    // Everything is checked before anything is built.
    for (const PatchFile &f : m.files) {
        if (f.kind == "xexmods")
            continue;
        if (f.kind != "delta" && f.kind != "new") {
            RNUT_ERROR("mods: {} skipped: {} has unknown kind '{}'", label, f.path, f.kind);
            return;
        }
        if (!IsSafeRelative(f.path) || !IsSha256(f.target_sha256) || (f.kind == "delta" && !IsSha256(f.source_sha256))) {
            RNUT_ERROR("mods: {} skipped: bad file record '{}'", label, f.path);
            return;
        }
        auto it = state.owner.find(PathKey(f.path));
        if (it != state.owner.end()) {
            RNUT_ERROR("mods: {} skipped: '{}' also changes {}, and combining two mods' changes to one file is not supported",
                       label, it->second, f.path);
            return;
        }
    }
    auto parts = ReadCustomParts(zip, &error);
    if (!parts) {
        RNUT_ERROR("mods: {} skipped: {}", label, error);
        return;
    }

    std::vector<std::pair<std::string, ModGameDevice::Redirect>> serve;
    int already = 0;
    for (const PatchFile &f : m.files) {
        if (f.kind == "xexmods")
            continue;
        const fs::path cached = state.cache_dir / rex::to_path(Lower(f.target_sha256) + ".bin");
        switch (BuildFile(zip, f, state.game_root, cached, &error)) {
        case Built::kServe:
            serve.push_back({PathKey(f.path), {f.path, cached}});
            break;
        case Built::kAlreadyInGame:
            ++already;
            break;
        case Built::kFailed:
            RNUT_ERROR("mods: {} skipped: {}", label, error);
            return;
        }
    }

    for (auto &[key, redirect] : serve) {
        state.used_cache.insert(redirect.host);
        state.owner[key] = m.name;
        state.redirects[key] = std::move(redirect);
    }
    state.loaded_ids.insert(id);
    state.loaded_conflicts.push_back({id, m.conflicts});
    ++state.mods;

    RNUT_INFO("mods: loaded {}{}: {} file(s){}{}", label, m.author.empty() ? "" : " by " + m.author, serve.size(),
              already ? " (" + std::to_string(already) + " already in the game folder)" : "",
              parts->empty() ? "" : ", " + std::to_string(parts->size()) + " part(s)");
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
    AddCustomParts(std::move(*parts));
    if (!m.exe_mods.empty()) {
        std::string names;
        for (const PatchExeMod &e : m.exe_mods)
            names += (names.empty() ? "" : ", ") + (e.name.empty() ? e.id : e.name);
        RNUT_WARN("mods: {}: executable changes not applied ({}): reNut runs recompiled code, so default.xex patches have no effect",
                  label, names);
    }
    if (m.ops)
        RNUT_WARN("mods: {}: {} world edit(s) not applied (not supported yet)", label, m.ops);
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

} // namespace

void LoadMods(rex::Runtime *runtime) {
    fs::path folder = rex::to_path(REXCVAR_GET(renut_mods_folder));
    if (folder.is_relative())
        folder = renut::paths::WritableFolder() / folder;
    if (!REXCVAR_GET(renut_mods)) {
        RNUT_INFO("mods: disabled (renut_mods)");
        RememberCustomParts(folder);
        return;
    }
    auto *vfs = runtime ? runtime->file_system() : nullptr;
    if (!vfs || runtime->game_data_root().empty())
        return;

    std::error_code ec;
    LoadState state;
    state.game_root = fs::absolute(runtime->game_data_root(), ec);
    fs::create_directories(folder, ec);
    state.cache_dir = folder / ".cache";

    std::vector<fs::path> packages;
    for (const auto &entry : fs::directory_iterator(folder, ec))
        if (entry.is_regular_file(ec) && Lower(rex::path_to_utf8(entry.path().extension())) == ".nbpatch")
            packages.push_back(entry.path());
    std::sort(packages.begin(), packages.end(), [](const fs::path &a, const fs::path &b) {
        return Lower(rex::path_to_utf8(a.filename())) < Lower(rex::path_to_utf8(b.filename()));
    });

    for (const fs::path &package : packages)
        LoadMod(package, state);
    PruneCache(state);
    RememberCustomParts(folder);

    if (state.redirects.empty()) {
        if (packages.empty())
            RNUT_INFO("mods: no .nbpatch files in {}", rex::path_to_utf8(folder));
        else
            RNUT_INFO("mods: {} mod(s) loaded, no game files changed", state.mods);
        return;
    }

    const size_t files = state.redirects.size();
    auto device = std::make_unique<ModGameDevice>(kMountPath, state.game_root, std::move(state.redirects));
    if (!device->Initialize() || !vfs->RegisterDevice(std::move(device))) {
        RNUT_ERROR("mods: cannot mount the modded game folder; the game runs unmodded");
        return;
    }
    for (std::string_view link : {"game:", "d:"}) {
        std::string target;
        if (!vfs->FindSymbolicLink(link, target))
            continue;
        vfs->UnregisterSymbolicLink(link);
        vfs->RegisterSymbolicLink(link, kMountPath);
    }
    RNUT_INFO("mods: {} mod(s) loaded, {} game file(s) replaced or added", state.mods, files);
}

} // namespace renut::mods
