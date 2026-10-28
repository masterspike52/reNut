#include "renut_engine/app/save_headers.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "renut_engine/app/renut_logging.h"

namespace renut::saves {

namespace {

namespace fs = std::filesystem;

// An Xbox 360 package's header (big-endian, as dlc_install.cpp reads one):
// the magic, then at 0x344 the content type, 0x360 the title ID and 0x411 the
// first display name (UTF-16, 128 characters).
constexpr size_t kContentTypeOffset = 0x344;
constexpr size_t kTitleIdOffset = 0x360;
constexpr size_t kDisplayNameOffset = 0x411;
constexpr size_t kDisplayNameBytes = 256;
constexpr size_t kPackageHeaderBytes = kDisplayNameOffset + kDisplayNameBytes;

// XCONTENT_AGGREGATE_DATA (rex/system/xam/content_manager.h), big-endian:
// device ID, content type, display name (UTF-16, 128 characters), file name
// (42 characters), padding, XUID (8, at 0x138), title ID (0x140).
constexpr size_t kAggregateBytes = 0x148;
constexpr size_t kDeviceIdOffset = 0x000;
constexpr size_t kAggContentTypeOffset = 0x004;
constexpr size_t kAggDisplayNameOffset = 0x008;
constexpr size_t kFileNameOffset = 0x108;
constexpr size_t kFileNameChars = 42;
constexpr size_t kAggTitleIdOffset = 0x140;
constexpr uint32_t kDeviceHdd = 1;  // as reNut's own headers have

void StoreBe32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

uint32_t LoadBe32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

bool IsPackageHeader(const std::vector<uint8_t> &data) {
    if (data.size() < kPackageHeaderBytes)
        return false;
    const std::string magic(reinterpret_cast<const char *>(data.data()), 4);
    return magic == "CON " || magic == "LIVE" || magic == "PIRS";
}

// Rewrites one header if it's a package header. True when it did.
bool FixHeader(const fs::path &path) {
    std::vector<uint8_t> data;
    {
        std::ifstream in(path, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(in), {});
    }
    if (!IsPackageHeader(data))
        return false;
    const std::string file_name = path.stem().string();
    if (file_name.empty() || file_name.size() > kFileNameChars)
        return false;

    std::array<uint8_t, kAggregateBytes> out{};
    StoreBe32(out.data() + kDeviceIdOffset, kDeviceHdd);
    StoreBe32(out.data() + kAggContentTypeOffset, LoadBe32(data.data() + kContentTypeOffset));
    std::memcpy(out.data() + kAggDisplayNameOffset, data.data() + kDisplayNameOffset, kDisplayNameBytes);
    out[kAggDisplayNameOffset + kDisplayNameBytes - 2] = out[kAggDisplayNameOffset + kDisplayNameBytes - 1] = 0;
    std::memcpy(out.data() + kFileNameOffset, file_name.data(), file_name.size());
    StoreBe32(out.data() + kAggTitleIdOffset, LoadBe32(data.data() + kTitleIdOffset));

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.write(reinterpret_cast<const char *>(out.data()), out.size())) {
        RNUT_WARN("Saves: can't rewrite {}", path.string());
        return false;
    }
    return true;
}

} // namespace

// The user data folder: <xuid>\<title>\Headers\<content type>\<name>.header.
void FixPackageHeaders(const fs::path &user_data_root) {
    std::error_code ec;
    if (user_data_root.empty() || !fs::is_directory(user_data_root, ec))
        return;
    int fixed = 0;
    for (const auto &profile : fs::directory_iterator(user_data_root, ec)) {
        if (!profile.is_directory(ec))
            continue;
        for (const auto &title : fs::directory_iterator(profile.path(), ec)) {
            const fs::path headers = title.path() / "Headers";
            if (!fs::is_directory(headers, ec))
                continue;
            for (const auto &type : fs::directory_iterator(headers, ec)) {
                if (!type.is_directory(ec))
                    continue;
                for (const auto &entry : fs::directory_iterator(type.path(), ec)) {
                    if (entry.is_regular_file(ec) && entry.path().extension() == ".header" &&
                        FixHeader(entry.path())) {
                        RNUT_INFO("Saves: converted Xenia header {}", entry.path().string());
                        ++fixed;
                    }
                }
            }
        }
    }
    if (fixed)
        RNUT_INFO("Saves: {} Xenia save header(s) converted for reNut", fixed);
}

} // namespace renut::saves
