#pragma once
// The game's xcompress (0x0FF512ED, LZX) bundle container, and the "expanded"
// form of a game file that .nbpatch deltas are made against (NB Studio's
// PatchPackage.Expand): an xcompress file decompressed, or a Bundle/50
// streaming archive followed by every xcompress entry inside it,
// decompressed, in entry order.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace renut::mods {

bool IsXCompressed(std::span<const uint8_t> data);
// nullopt (with *error set) when the data is damaged.
std::optional<std::vector<uint8_t>> XDecompress(std::span<const uint8_t> data, std::string *error);

std::optional<std::vector<uint8_t>> ExpandGameFile(std::vector<uint8_t> raw, std::string *error);

} // namespace renut::mods
