#pragma once

#include <filesystem>

namespace renut::graphics_menu {

// The config file the GRAPHICS menu saves its changes to (RenutApp::OnConfigurePaths).
void SetConfigPath(const std::filesystem::path& path);

}  // namespace renut::graphics_menu
