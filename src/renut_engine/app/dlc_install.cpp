#include "renut_engine/app/dlc_install.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>
#include <rex/kernel/title_runtime.h>
#include <rex/string/text.h>
#include <rex/xam/content_manager.h>
#include "renut_engine/app/app_paths.h"

namespace renut::dlc {

namespace {

namespace fs = std::filesystem;


constexpr const char kPackageFolder[] = "dlc";
constexpr size_t kHeaderBytes = 0x500;
constexpr size_t kContentTypeOffset = 0x344;
constexpr size_t kMediaIdOffset = 0x354;
constexpr size_t kTitleIdOffset = 0x360;
constexpr size_t kDisplayNameOffset = 0x411;
constexpr size_t kDisplayNameChars = 64;
constexpr uint32_t kContentTypeMarketplace = 2;
constexpr const char kProfileFolder[] = "0000000000000000";
constexpr const char kContentTypeFolder[] = "00000002";
constexpr const char kInstalledList[] = "installed.txt";

fs::path g_user_data_root;

uint32_t LoadBe32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

std::string HexTitle(uint32_t title_id) {
    char s[9];
    std::snprintf(s, sizeof(s), "%08X", title_id);
    return s;
}

fs::path InstalledHeader(uint32_t title_id, const std::string &file_name) {
    if (g_user_data_root.empty())
        return {};
    return g_user_data_root / kProfileFolder / HexTitle(title_id) / "Headers" / kContentTypeFolder /
           (file_name + ".header");
}

std::vector<std::string> ReadInstalledList(const fs::path &list) {
    std::vector<std::string> lines;
    std::ifstream f(list);
    for (std::string line; std::getline(f, line);)
        if (!line.empty())
            lines.push_back(line);
    return lines;
}

std::string ListEntry(const std::string &file_name, uintmax_t size) {
    return file_name + " " + std::to_string(size);
}

} 

bool ReadPackageInfo(const fs::path &path, PackageInfo &out) {
    std::ifstream f(path, std::ios::binary);
    std::array<uint8_t, kHeaderBytes> h{};
    if (!f.read(reinterpret_cast<char *>(h.data()), h.size()))
        return false;
    const std::string magic(reinterpret_cast<const char *>(h.data()), 4);
    if (magic != "LIVE" && magic != "PIRS" && magic != "CON ")
        return false;
    out.content_type = LoadBe32(h.data() + kContentTypeOffset);
    out.media_id = LoadBe32(h.data() + kMediaIdOffset);
    out.title_id = LoadBe32(h.data() + kTitleIdOffset);
    out.name.clear();
    for (size_t i = 0; i < kDisplayNameChars; ++i) {
        const uint8_t *c = h.data() + kDisplayNameOffset + 2 * i;
        const uint16_t ch = uint16_t(c[0] << 8 | c[1]);
        if (!ch)
            break;
        out.name.push_back(ch < 0x80 ? char(ch) : '?');
    }
    return true;
}

fs::path PackageFolder() {
    return renut::paths::WritableFolder() / kPackageFolder;
}

void SetUserDataRoot(const fs::path &root) {
    g_user_data_root = root;
}

void InstallPackages(rex::kernel::TitleRuntime &title) {
    const fs::path folder = PackageFolder();
    std::error_code ec;
    fs::create_directories(folder, ec);

    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(folder, ec))
        if (entry.is_regular_file(ec) && entry.path().filename() != kInstalledList)
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    if (files.empty())
        return;

    const uint32_t title_id = title.GetTitleID();
    const fs::path list_path = folder / kInstalledList;
    std::vector<std::string> installed = ReadInstalledList(list_path);
    bool list_changed = false;

    for (const fs::path &path : files) {
        const std::string file_name = rex::string::PathToUTF8(path.filename());
        PackageInfo package;
        // Not an Xbox 360 package, another game's, or not DLC: left alone.
        if (!ReadPackageInfo(path, package) || package.title_id != title_id ||
            package.content_type != kContentTypeMarketplace)
            continue;
        const uintmax_t size = fs::file_size(path, ec);
        const std::string entry = ListEntry(file_name, size);
        const fs::path header = InstalledHeader(title_id, file_name);
        const bool listed = std::find(installed.begin(), installed.end(), entry) != installed.end();
        const bool header_there = !header.empty() && fs::exists(header, ec);
        if (header_there || (header.empty() && listed))
            continue;

        if (!title.GetContentManager().InstallContent(path))
            continue;
        if (!listed) {
            installed.push_back(entry);
            list_changed = true;
        }
    }

    if (list_changed) {
        std::ofstream f(list_path, std::ios::trunc);
        for (const std::string &line : installed)
            f << line << "\n";
    }
}

} 
