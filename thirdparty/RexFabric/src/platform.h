// The few things the menus ask of the desktop.

#pragma once

#include <filesystem>
#include <string>

namespace rexfabric {

// In the system's browser and file manager.
void OpenUrl(const std::string& url);
void OpenFolder(const std::filesystem::path& folder);

}  // namespace rexfabric
