#pragma once

#include <string>
#include <string_view>

namespace renut::mods {

// Lower-case ASCII, '/' separators: the key a game-relative path is matched
// by (the VFS folds names the same way).
std::string PathKey(std::string_view path);

} // namespace renut::mods
