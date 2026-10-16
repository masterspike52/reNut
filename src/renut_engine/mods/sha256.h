#pragma once
// SHA-256, for checking .nbpatch sources and results against the hashes in
// their patch.json (lower-case hex, as NB Studio writes them).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace renut::mods {

class Sha256 {
public:
    Sha256();
    void Update(std::span<const uint8_t> data);
    // Lower-case hex digest; the object is spent afterwards.
    std::string FinalHex();

private:
    void Block(const uint8_t* p);

    uint32_t state_[8];
    uint8_t buffer_[64];
    size_t buffered_ = 0;
    uint64_t total_ = 0;
};

std::string Sha256Hex(std::span<const uint8_t> data);
// nullopt when the file cannot be read.
std::optional<std::string> Sha256File(const std::filesystem::path &path);

} // namespace renut::mods
