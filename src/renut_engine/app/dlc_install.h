#pragma once
// DLC packages installed by reNut itself.
//
// At startup every file in the DLC folder (renut_dlc_folder, "dlc" next to
// renut.exe by default) is looked at: an Xbox Live package (LIVE, PIRS or
// CON) of marketplace content (type 2) for this game is installed into the
// user data folder, as the console's own install would, the first time it is
// seen. The package file stays where it is; deleting it later does not
// uninstall it. Installing copies the package's files, so the first start
// after adding one takes a moment longer.

#include <cstdint>
#include <filesystem>
#include <string>

namespace rex {
class Runtime;
}

namespace renut::dlc {

// What an Xbox 360 package's header says (big-endian there): its content type
// (2 DLC, 0x000B0000 a title update), the game's media ID (which disc, so
// which region) and title ID, and its display name (the ASCII part).
struct PackageInfo {
    uint32_t content_type = 0;
    uint32_t media_id = 0;
    uint32_t title_id = 0;
    std::string name;
};

// False if 'path' is not an Xbox 360 package (LIVE, PIRS or CON).
bool ReadPackageInfo(const std::filesystem::path &path, PackageInfo &out);

// The DLC folder: renut_dlc_folder, relative to reNut's writable folder.
std::filesystem::path PackageFolder();

// The user data folder the runtime uses (from the resolved paths), where an
// installed package's header shows it is installed.
void SetUserDataRoot(const std::filesystem::path &root);

// Call once the runtime is set up and before the game module starts
// (ReXApp::OnPostSetup).
void InstallPackages(rex::Runtime *runtime);

} // namespace renut::dlc
