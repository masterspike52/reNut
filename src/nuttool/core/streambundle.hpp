// Stream bundle (asset type 0x50): an archive of standalone CAFF assets that the game
// streams in on demand (texture top mips, cutscenes, animations, ...).
//
// Layout (big-endian):
//   f32 version (281.41)   u32 entrySize (12)   u32 entryCount   u32 timestamp
//   u32 dependencyCount    u32 dependencies[dependencyCount]   (other stream bundle ids)
//   entries[entryCount] { u32 assetId, u32 offset, u32 size }
//   entry data, contiguous, in entry order
#pragma once

#include "common.hpp"

namespace nut::streambundle {

constexpr float kVersion = 281.41f;

struct Entry {
    uint32_t id = 0;
    Bytes data;
};

struct StreamBundle {
    float version = kVersion;
    uint32_t timestamp = 0;
    std::vector<uint32_t> dependencies;
    std::vector<Entry> entries;

    static StreamBundle parse(const Bytes& file);
    Bytes serialize() const;
};

bool looksLikeStreamBundle(const Bytes& data);

// Random access to a stream bundle file without loading it: the entry table only, then single
// entries on demand. readIndex returns false for files that are not plain stream bundles.
struct IndexEntry {
    uint32_t id = 0, offset = 0, size = 0;
};
bool readIndex(const fs::path& file, std::vector<IndexEntry>& out);
Bytes readEntry(const fs::path& file, const IndexEntry& e);

} // namespace nut::streambundle
