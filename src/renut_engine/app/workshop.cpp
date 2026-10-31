#include "renut_engine/app/workshop.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <rex/filesystem.h>

#include "renut_engine/app/renut_logging.h"

// Static, so it can't clash with the stb_image rexglue and the Guide build in.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace renut::workshop {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr const char kTitleFolder[] = "4D5307ED";
constexpr const char kSdkProfile[] = "B13EBABEBABEBABE";  // the SDK's built-in profile
constexpr const char kSaveType[] = "00000001";            // saved games, vehicles among them
constexpr const char kManifest[] = "renut-workshop.json";

constexpr uint8_t kBlueprintMagic[8] = {0x3F, 0x9A, 0xE1, 0x48, 0x40, 0x54, 0x7A, 0xE1};
constexpr size_t kBlueprintHeader = 0x84;
constexpr size_t kBlueprintPart = 36;

// XCONTENT_AGGREGATE_DATA (save_headers.cpp has the layout): device, content
// type, display name (UTF-16BE, 128 characters), file name at 0x108, title ID
// at 0x140.
constexpr size_t kAggregateBytes = 0x148;
constexpr size_t kDisplayNameChars = 127;
constexpr uint32_t kDeviceHdd = 1;
constexpr uint32_t kContentTypeSave = 1;
constexpr uint32_t kTitleId = 0x4D5307ED;

void StoreBe32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

// UTF-8 to UTF-16 code units (anything malformed becomes '?').
std::u16string Utf16(const std::string &s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        const int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (extra < 0 || i + extra >= s.size() + (extra ? 0 : 1)) {
            out.push_back(u'?');
            ++i;
            continue;
        }
        uint32_t cp = extra == 0 ? c : c & (0x3F >> extra);
        for (int k = 1; k <= extra; ++k)
            cp = cp << 6 | (uint8_t(s[i + k]) & 0x3F);
        i += size_t(extra) + 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(char16_t(0xD800 + (cp >> 10)));
            out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(char16_t(cp));
        }
    }
    return out;
}

fs::path HeadersFolder(const fs::path &save) {
    return save / "Headers" / kSaveType;
}

fs::path ContentFile(const fs::path &save, const std::string &content_id) {
    return save / kSaveType / content_id / content_id.substr(2);
}

fs::path ManifestPath(const fs::path &save) {
    return save.parent_path().parent_path() / kManifest;
}

std::map<std::string, std::string> ReadManifest(const fs::path &save) {
    std::map<std::string, std::string> installed;
    std::ifstream file(ManifestPath(save));
    if (!file)
        return installed;
    try {
        const json j = json::parse(file);
        for (const auto &[id, content] : j.items())
            if (content.is_string())
                installed[id] = content.get<std::string>();
    } catch (...) {
    }
    return installed;
}

void WriteManifest(const fs::path &save, const std::map<std::string, std::string> &installed) {
    json j = json::object();
    for (const auto &[id, content] : installed)
        j[id] = content;
    std::ofstream file(ManifestPath(save), std::ios::trunc);
    file << j.dump(2);
    if (!file)
        RNUT_WARN("Workshop: can't write {}", rex::path_to_utf8(ManifestPath(save)));
}

std::string Text(const json &j, const char *key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

uint32_t LoadBe32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

std::string ReadFile(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string Base64(const void *data, size_t size) {
    static constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto *p = static_cast<const uint8_t *>(data);
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t n = uint32_t(p[i]) << 16 | (i + 1 < size ? uint32_t(p[i + 1]) << 8 : 0) |
                           (i + 2 < size ? uint32_t(p[i + 2]) : 0);
        out += kDigits[n >> 18];
        out += kDigits[(n >> 12) & 63];
        out += i + 1 < size ? kDigits[(n >> 6) & 63] : '=';
        out += i + 2 < size ? kDigits[n & 63] : '=';
    }
    return out;
}

// RGBA scaled down to 'width' (area average), its height kept in proportion.
std::vector<uint8_t> Shrink(const std::vector<uint8_t> &rgba, int w, int h, int width, int *height) {
    *height = std::max(1, int(int64_t(h) * width / w));
    std::vector<uint8_t> out(size_t(width) * *height * 4);
    for (int y = 0; y < *height; ++y) {
        const int y0 = int(int64_t(y) * h / *height), y1 = std::max(y0 + 1, int(int64_t(y + 1) * h / *height));
        for (int x = 0; x < width; ++x) {
            const int x0 = int(int64_t(x) * w / width), x1 = std::max(x0 + 1, int(int64_t(x + 1) * w / width));
            uint32_t sum[4] = {};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx)
                    for (int c = 0; c < 4; ++c)
                        sum[c] += rgba[(size_t(sy) * w + sx) * 4 + c];
            const uint32_t n = uint32_t((y1 - y0) * (x1 - x0));
            for (int c = 0; c < 4; ++c)
                out[(size_t(y) * width + x) * 4 + c] = uint8_t(sum[c] / n);
        }
    }
    return out;
}

// The screenshot as a JPEG of at most 'room' bytes: 640 wide or narrower,
// lower quality first.
std::string PreviewJpeg(const fs::path &screenshot, size_t room, std::string *error) {
    const std::string file = ReadFile(screenshot);
    int w = 0, h = 0;
    const std::vector<uint8_t> rgba = DecodeImage(file.data(), file.size(), &w, &h);
    if (rgba.empty()) {
        *error = "The screenshot could not be read (it has to be a PNG or JPEG).";
        return {};
    }
    std::string jpeg;
    for (int width = std::min(w, 640); width >= 160; width -= 160) {
        int height = 0;
        const std::vector<uint8_t> small = width == w ? rgba : Shrink(rgba, w, h, width, &height);
        if (width == w)
            height = h;
        for (int quality = 85; quality >= 40; quality -= 15) {
            jpeg.clear();
            stbi_write_jpg_to_func(
                [](void *context, void *data, int size) {
                    static_cast<std::string *>(context)->append(static_cast<const char *>(data), size_t(size));
                },
                &jpeg, width, height, 4, small.data(), quality);
            if (!jpeg.empty() && jpeg.size() <= room)
                return jpeg;
        }
    }
    *error = "The screenshot could not be made small enough for GitHub.";
    return {};
}

} // namespace

std::vector<Entry> ParseIndex(const std::string &text, std::string *error) {
    std::vector<Entry> entries;
    try {
        const json j = json::parse(text);
        for (const json &b : j.value("blueprints", json::array())) {
            Entry e;
            e.id = Text(b, "id");
            e.name = Text(b, "name");
            e.description = Text(b, "description");
            e.author = Text(b, "author");
            e.created = Text(b, "created");
            e.blueprint = Text(b, "blueprint");
            e.preview = Text(b, "preview");
            e.parts = b.value("parts", 0);
            for (const json &r : b.value("requires", json::array()))
                if (r.is_string())
                    e.needs.push_back(r.get<std::string>());
            if (!e.id.empty() && !e.blueprint.empty())
                entries.push_back(std::move(e));
        }
    } catch (const std::exception &) {
        if (error)
            *error = "The workshop's list could not be read.";
        return {};
    }
    return entries;
}

fs::path SaveFolder(const fs::path &user_root) {
    std::error_code ec;
    const fs::path sdk = user_root / kSdkProfile / kTitleFolder;
    if (fs::is_directory(sdk / kSaveType, ec))
        return sdk;
    for (const auto &profile : fs::directory_iterator(user_root, ec)) {
        const fs::path save = profile.path() / kTitleFolder;
        if (profile.is_directory(ec) && fs::is_directory(save / kSaveType, ec))
            return save;
    }
    return sdk;
}

bool IsBlueprint(const std::string &data) {
    if (data.size() < kBlueprintHeader || std::memcmp(data.data(), kBlueprintMagic, sizeof(kBlueprintMagic)) != 0)
        return false;
    const size_t parts = size_t(uint8_t(data[8])) << 8 | uint8_t(data[9]);
    return parts >= 1 && data.size() == kBlueprintHeader + parts * kBlueprintPart;
}

bool Install(const fs::path &save, const Entry &entry, const std::string &blueprint, std::string *error) {
    const auto fail = [&](std::string message) {
        if (error)
            *error = std::move(message);
        return false;
    };
    if (!IsBlueprint(blueprint))
        return fail(entry.name + " did not download whole; try again.");
    std::error_code ec;
    fs::create_directories(HeadersFolder(save), ec);
    if (ec)
        return fail("Can't write to the save folder: " + ec.message() + ".");

    // The game numbers its vehicles 0x00000001 up: the next number after
    // every one in use, by header or by content folder.
    uint32_t next = 1;
    const auto take = [&](const std::string &name) {
        if (name.size() != 10 || name.rfind("0x", 0) != 0)
            return;
        const uint32_t used = uint32_t(std::strtoul(name.c_str() + 2, nullptr, 16));
        if (used < 0x01000000)
            next = std::max(next, used + 1);
    };
    for (const auto &header : fs::directory_iterator(HeadersFolder(save), ec))
        take(header.path().stem().string());
    for (const auto &folder : fs::directory_iterator(save / kSaveType, ec))
        take(folder.path().filename().string());
    const std::string content_id = fmt::format("0x{:08x}", next);

    const fs::path file = ContentFile(save, content_id);
    fs::create_directories(file.parent_path(), ec);
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(blueprint.data(), std::streamsize(blueprint.size()));
        if (!out)
            return fail("Can't write the vehicle into the save folder.");
    }

    std::array<uint8_t, kAggregateBytes> header{};
    StoreBe32(header.data() + 0x000, kDeviceHdd);
    StoreBe32(header.data() + 0x004, kContentTypeSave);
    std::u16string display = Utf16("VEHICLE: " + entry.name);
    display.resize(std::min(display.size(), kDisplayNameChars));
    for (size_t i = 0; i < display.size(); ++i) {
        header[0x008 + i * 2] = uint8_t(display[i] >> 8);
        header[0x009 + i * 2] = uint8_t(display[i]);
    }
    std::memcpy(header.data() + 0x108, content_id.data(), content_id.size());
    StoreBe32(header.data() + 0x140, kTitleId);
    {
        std::ofstream out(HeadersFolder(save) / (content_id + ".header"), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(header.data()), std::streamsize(header.size()));
        if (!out) {
            fs::remove_all(file.parent_path(), ec);
            return fail("Can't write the vehicle's header into the save folder.");
        }
    }

    auto installed = ReadManifest(save);
    installed[entry.id] = content_id;
    WriteManifest(save, installed);
    RNUT_INFO("Workshop: {} ({}) added to the save as {}", entry.name, entry.id, content_id);
    return true;
}

void Remove(const fs::path &save, const std::string &id) {
    auto installed = ReadManifest(save);
    const auto it = installed.find(id);
    if (it == installed.end())
        return;
    const std::string content_id = it->second;
    std::error_code ec;
    if (content_id.size() == 10 && content_id.rfind("0x", 0) == 0) {
        fs::remove_all(save / kSaveType / content_id, ec);
        fs::remove(HeadersFolder(save) / (content_id + ".header"), ec);
    }
    installed.erase(it);
    WriteManifest(save, installed);
    RNUT_INFO("Workshop: {} removed from the save ({})", id, content_id);
}

std::map<std::string, std::string> Installed(const fs::path &save) {
    auto installed = ReadManifest(save);
    std::error_code ec;
    for (auto it = installed.begin(); it != installed.end();) {
        if (it->second.size() == 10 && fs::is_regular_file(ContentFile(save, it->second), ec))
            ++it;
        else
            it = installed.erase(it);
    }
    return installed;
}

std::vector<uint8_t> DecodeImage(const void *data, size_t size, int *width, int *height, int max_width) {
    *width = *height = 0;
    if (!data || !size || size > size_t(INT_MAX))
        return {};
    int w = 0, h = 0, channels = 0;
    stbi_uc *pixels = stbi_load_from_memory(static_cast<const stbi_uc *>(data), int(size), &w, &h, &channels, 4);
    if (!pixels)
        return {};
    std::vector<uint8_t> rgba(pixels, pixels + size_t(w) * h * 4);
    stbi_image_free(pixels);
    if (max_width > 0 && w > max_width) {
        rgba = Shrink(rgba, w, h, max_width, &h);
        w = max_width;
    }
    *width = w;
    *height = h;
    return rgba;
}

std::vector<LocalVehicle> LocalVehicles(const fs::path &save) {
    std::vector<LocalVehicle> vehicles;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(HeadersFolder(save), ec)) {
        const std::string content_id = entry.path().stem().string();
        if (content_id.size() != 10 || content_id.rfind("0x", 0) != 0)
            continue;
        const std::string header = ReadFile(entry.path());
        if (header.size() < kAggregateBytes)
            continue;
        // The display name: UTF-16BE at 0x08, "VEHICLE: <name>" for a vehicle.
        std::string name;
        for (size_t i = 0x08; i + 1 < 0x108; i += 2) {
            const uint16_t c = uint16_t(uint8_t(header[i]) << 8 | uint8_t(header[i + 1]));
            if (!c)
                break;
            name += c < 0x80 ? char(c) : '?';
        }
        if (name.rfind("VEHICLE: ", 0) != 0)
            continue;
        const fs::path file = ContentFile(save, content_id);
        if (fs::is_regular_file(file, ec))
            vehicles.push_back({name.substr(9), file});
    }
    std::sort(vehicles.begin(), vehicles.end(), [](const LocalVehicle &a, const LocalVehicle &b) {
        return a.name < b.name;
    });
    return vehicles;
}

std::string BuildSubmission(const fs::path &blueprint_file, const std::string &name, const std::string &description,
                            const fs::path &screenshot, const fs::path &mods_folder, std::string *error) {
    const auto fail = [&](std::string message) {
        *error = std::move(message);
        return std::string();
    };
    const std::string blueprint = ReadFile(blueprint_file);
    if (!IsBlueprint(blueprint))
        return fail("That vehicle's file is damaged.");
    if (name.empty())
        return fail("Give the vehicle a name.");
    if (screenshot.empty())
        return fail("Choose a screenshot of the vehicle.");

    // The custom parts in it: each part record (36 bytes after the header)
    // holds its part's asset id at offset 8; reNut remembers every mod
    // part's in the mods folder.
    const size_t parts = size_t(uint8_t(blueprint[8])) << 8 | uint8_t(blueprint[9]);
    std::vector<uint32_t> aids;
    for (size_t i = 0; i < parts; ++i)
        aids.push_back(LoadBe32(reinterpret_cast<const uint8_t *>(blueprint.data()) + kBlueprintHeader +
                                i * kBlueprintPart + 8));
    nlohmann::ordered_json custom = nlohmann::ordered_json::array();
    try {
        const json known = json::parse(ReadFile(mods_folder / ".renut_custom_parts.json"));
        for (const json &p : known.value("parts", json::array())) {
            const uint32_t aid = uint32_t(std::strtoul(Text(p, "aid").c_str(), nullptr, 16));
            if (aid && std::find(aids.begin(), aids.end(), aid) != aids.end())
                custom.push_back({{"id", Text(p, "id")}, {"mod", Text(p, "mod")}});
        }
    } catch (...) {
        // No mods, or none remembered: no custom parts.
    }

    // GitHub takes an issue of 65536 characters: the blueprint, the text and
    // the custom parts first, the picture in what is left.
    const std::string blueprint64 = Base64(blueprint.data(), blueprint.size());
    const long long left = 60000 - (long long)blueprint64.size() - (long long)description.size() * 6 -
                           (long long)custom.dump().size() * 2;
    const long long room = left * 3 / 4;
    if (room < 8000)
        return fail("That vehicle is too big to share through GitHub.");
    std::string preview_error;
    const std::string jpeg = PreviewJpeg(screenshot, size_t(room), &preview_error);
    if (jpeg.empty())
        return fail(preview_error);

    nlohmann::ordered_json submission;
    submission["name"] = name;
    submission["description"] = description;
    submission["blueprint"] = blueprint64;
    submission["preview"] = Base64(jpeg.data(), jpeg.size());
    submission["custom_parts"] = custom;
    // Typed text that isn't valid UTF-8 is mended, not thrown on.
    return submission.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

} // namespace renut::workshop
