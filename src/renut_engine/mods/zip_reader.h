#pragma once
// Reads the entries of a .nbpatch, which is a plain zip written by .NET's
// ZipArchive: stored or deflated entries, sizes taken from the central
// directory, CRC-32 checked. No zip64 (an .nbpatch is far below 4 GB).

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace renut::mods {

class ZipReader {
public:
    struct EntryInfo {
        std::string name;
        uint16_t method = 0;
        uint32_t crc32 = 0;
        uint64_t compressed_size = 0;
        uint64_t size = 0;
        uint64_t local_header_offset = 0;
    };

    // Reads the central directory; false (with *error set) when the file is
    // not a zip this reader understands.
    bool Open(const std::filesystem::path &path, std::string *error);

    const EntryInfo *Find(std::string_view name) const;
    const std::vector<EntryInfo> &Entries() const { return entries_; }
    // The entry's uncompressed bytes; nullopt (with *error set) when it is
    // missing or damaged.
    std::optional<std::vector<uint8_t>> Read(std::string_view name, std::string *error);

private:
    std::ifstream file_;
    std::vector<EntryInfo> entries_;
};

// Raw DEFLATE (RFC 1951) into a buffer of exactly expected_size bytes.
bool Inflate(const uint8_t *in, size_t in_size, std::vector<uint8_t> &out, size_t expected_size);

uint32_t Crc32(const uint8_t *data, size_t size);

} // namespace renut::mods
