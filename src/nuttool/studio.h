#pragma once

#include <cstdint>
#include <filesystem>

namespace renut::nuttool {

// The studio: a world of reNut's own (a lit stage) where the in-game NutTool shows models.
inline constexpr const char kStudioId[] = "nuttool_studio";

// NutTool's own folder in reNut's (its studio, settings and scratch work), and the studio's files in it.
std::filesystem::path NutToolFolder();
std::filesystem::path StudioFolder();

// Before the game's files are served: makes the studio's files from the game's assets when they're missing or older
// than this reNut's stage. False when they couldn't be made.
bool PrepareStudio(const std::filesystem::path &assets);

// Whether PrepareStudio made or found the studio this session.
bool StudioReady();

// The studio's scene script (aid_script_renutworld_nuttool_studio).
uint32_t StudioScene();

// The main menu's NUTTOOL icon, a texture asset of the studio's bundle (aid_texture_renut_nuttool_icon).
uint32_t IconId();

} // namespace renut::nuttool
