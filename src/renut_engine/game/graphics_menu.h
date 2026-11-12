#pragma once

#include <filesystem>
#include <span>

namespace renut::graphics_menu {

// The config file the GRAPHICS menu saves its changes to (RenutApp::OnConfigurePaths).
void SetConfigPath(const std::filesystem::path& path);

// A reNut setting or command (its old cvar name, "renut.<name>" now) and the old category it
// shows under ("Nuts&Bolts/Cheats").
struct CategorizedSetting {
  const char* name;
  const char* category;
};

// Every reNut setting and command with its category, each category's in its menu order.
std::span<const CategorizedSetting> CategorizedSettings();

}  // namespace renut::graphics_menu
