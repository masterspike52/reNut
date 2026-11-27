// Mod packages in NB Studio's .nbpatch format, which reNut (F:\renut2, renut_engine/mods) loads at
// startup without touching the game folder.
//
// A package is a zip with patch.json and one entry per changed game file:
//   patch.json   {"Format": 1, "Id", "Name", "Version", "Author", "Category", "Description",
//                 "Files": [{"Path", "Kind", "SourceSha256", "SourceSize", "TargetSha256",
//                            "TargetSize", "Entry"}]}
//   "delta"      replaces an existing game file: an NBDL delta against the file's expanded form;
//                SourceSha256/SourceSize are those of the file as it is on disc (before expanding)
//   "new"        adds a file the game folder does not have (the whole file is stored)
// Paths are relative to the game folder with '/' separators ("Bundle/4f/685374"). Hashes are
// lower-case hex SHA-256. The loader checks the original against SourceSha256, rebuilds the file
// and checks it against TargetSha256, so a package only applies to the exact files it was made from.
//
// The expanded form (NB Studio's PatchPackage.Expand): an XCTD file decompressed; a Bundle/50
// streaming archive (magic 0x438CB47C) followed by every XCTD entry inside it, decompressed, in
// entry order (stopping at the first entry that does not decompress); any other file as it is.
//
// NBDL: "NBDL", u32 version 1, u64 target length (little-endian), then ops to the end:
//   0x01 COPY varint source offset, varint length   (from the expanded original)
//   0x02 ADD  varint length, bytes
#pragma once

#include "common.hpp"
#include "ops.hpp"

#include <optional>

namespace nut::nbpatch {

std::string sha256Hex(const uint8_t* data, size_t size);
inline std::string sha256Hex(const Bytes& b) { return sha256Hex(b.data(), b.size()); }

Bytes expand(const Bytes& raw);
Bytes makeDelta(const Bytes& source, const Bytes& target);
// Throws when the delta does not fit the source.
Bytes applyDelta(const Bytes& source, const Bytes& delta);

// PatchPackage.Slug: lower-case letters and digits, other runs become '-'.
std::string slug(const std::string& name);

struct Info {
    std::string id; // empty: slug of the name
    std::string name;
    std::string version = "1";
    std::string author;
    std::string category;
    std::string description;
};

// One changed game file. `original` is empty for a file the game does not have.
struct Change {
    std::string path; // relative to the game folder, '/' separators
    fs::path original;
    fs::path modified;
};

// Finds the files of `gameRoot` that differ from their originals: with `originalRoot` empty, every
// file that has a "<file>.bak" next to it (NutTool's backups); otherwise every file whose copy in
// `originalRoot` differs or is missing.
std::vector<Change> findChanges(const fs::path& gameRoot, const fs::path& originalRoot, const ops::Progress& pr = {});

// Adds the title update's changed files (findChanges over `updateDir`, against `originalUpdateDir`
// or the .bak backups) with paths under "update/", which reNut serves as UPDATE: files. Adds nothing
// when `updateDir` is empty or inside `gameRoot`, whose findChanges already lists it.
void addUpdateChanges(std::vector<Change>& out, const fs::path& gameRoot, const fs::path& updateDir, const fs::path& originalUpdateDir,
                      const ops::Progress& pr = {});

// Writes the package and checks every entry by rebuilding it from the original. Returns false (with
// the reason logged) when a file could not be packaged.
bool write(const fs::path& out, const Info& info, const std::vector<Change>& changes, const ops::Progress& pr = {});

} // namespace nut::nbpatch
