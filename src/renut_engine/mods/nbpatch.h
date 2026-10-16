#pragma once
// .nbpatch packages from NB Studio (NB.Core/Project/PatchPackage.cs): a zip
// with patch.json and one entry per changed game file, either an NBDL binary
// delta against the expanded original ("delta") or the whole file ("new").
// "xexmods" entries and ExeMods are word patches to default.xex; world edits
// (Ops, format 3) are NB Studio commands replayed on the game's bundles.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace renut::mods {

class ZipReader;

struct PatchFile {
    std::string path;  // relative, '/' separated, as in patch.json
    std::string kind;  // "delta", "new" or "xexmods"
    std::string source_sha256;
    int64_t source_size = 0;
    std::string target_sha256;
    int64_t target_size = 0;
    std::string entry; // zip entry of the delta or file
};

struct PatchExeMod {
    std::string id;
    std::string name;
    size_t words = 0;
};

struct PatchManifest {
    int format = 1;
    std::string id;    // PatchPackage.IdOf: the Id, or a slug of the name
    std::string name;
    std::string version;
    std::string author;
    std::string category;
    std::vector<PatchFile> files;
    std::vector<PatchExeMod> exe_mods;
    std::vector<std::string> requires_ids;
    std::vector<std::string> conflicts;
    size_t ops = 0;
};

// Newest patch.json format NB Studio writes (3 adds Ops).
constexpr int kNewestPatchFormat = 3;

std::optional<PatchManifest> ReadManifest(ZipReader &zip, std::string *error);

// Applies an NBDL delta (COPY / ADD ops) to its expanded source.
std::optional<std::vector<uint8_t>> ApplyDelta(std::span<const uint8_t> source, std::span<const uint8_t> delta, std::string *error);

} // namespace renut::mods
