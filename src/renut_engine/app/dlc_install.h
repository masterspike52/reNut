#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace rex::kernel {
class TitleRuntime;
}

namespace renut::dlc {

struct PackageInfo {
    uint32_t content_type = 0;
    uint32_t media_id = 0;
    uint32_t title_id = 0;
    std::string name;
};

bool ReadPackageInfo(const std::filesystem::path &path, PackageInfo &out);

std::filesystem::path PackageFolder();

void SetUserDataRoot(const std::filesystem::path &root);

void InstallPackages(rex::kernel::TitleRuntime &title);

} 
