#include "renut_engine/app/android_content.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>

#include <rex/cvar.h>

#include "renut_engine/app/android_driver.h"
#include "renut_engine/app/app_paths.h"
#include "renut_engine/mods/json.h"

REXCVAR_DECLARE(std::string, renut_mods_folder);

namespace renut::content {

namespace {

constexpr const char *kRaw = "https://raw.githubusercontent.com/etonedemid/nutstaller-workshop/main/";
constexpr uint32_t kTitleId = 0x4D5307ED;
constexpr size_t kContentHeaderSize = 0x148;  // XCONTENT_AGGREGATE_DATA
constexpr size_t kBlueprintHeader = 0x84, kPartSize = 36;
constexpr uint8_t kBlueprintMagic[8] = {0x3F, 0x9A, 0xE1, 0x48, 0x40, 0x54, 0x7A, 0xE1};

std::optional<std::vector<uint8_t>> ReadFile(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

bool WriteFile(const fs::path &path, const void *data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char *>(data), std::streamsize(size));
    return bool(out);
}

bool WriteText(const fs::path &path, const std::string &text) { return WriteFile(path, text.data(), text.size()); }

uint32_t Be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void PutBe32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

fs::path HeadersDir(const fs::path &save) { return save / "Headers" / "00000001"; }

// The header's display name; non-ASCII as '?', which is all the checks need.
std::string DisplayName(const std::vector<uint8_t> &header) {
    std::string name;
    for (size_t i = 8; i + 1 < 8 + 256 && i + 1 < header.size(); i += 2) {
        const uint16_t c = uint16_t(header[i] << 8 | header[i + 1]);
        if (!c)
            break;
        name += c < 0x80 ? char(c) : '?';
    }
    return name;
}

// UTF-8 to UTF-16 code units.
std::u16string Utf16(const std::string &s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        uint32_t cp = 0xFFFD;
        size_t n = 1;
        if (c < 0x80)
            cp = c;
        else if ((c >> 5) == 6 && i + 1 < s.size())
            cp = (c & 0x1F) << 6 | (s[i + 1] & 0x3F), n = 2;
        else if ((c >> 4) == 14 && i + 2 < s.size())
            cp = (c & 0x0F) << 12 | (s[i + 1] & 0x3F) << 6 | (s[i + 2] & 0x3F), n = 3;
        else if ((c >> 3) == 30 && i + 3 < s.size())
            cp = (c & 0x07) << 18 | (s[i + 1] & 0x3F) << 12 | (s[i + 2] & 0x3F) << 6 | (s[i + 3] & 0x3F), n = 4;
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += char16_t(0xD800 | (cp >> 10));
            out += char16_t(0xDC00 | (cp & 0x3FF));
        } else {
            out += char16_t(cp);
        }
    }
    return out;
}

std::string Json(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\')
            out += '\\', out += char(c);
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else
            out += char(c);
    }
    return out + "\"";
}

std::optional<mods::JsonValue> ReadJson(const fs::path &path) {
    auto data = ReadFile(path);
    if (!data)
        return std::nullopt;
    std::string error;
    return mods::ParseJson(std::string_view(reinterpret_cast<const char *>(data->data()), data->size()), &error);
}

std::string Now(const char *format) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, format, &tm);
    return buf;
}

// "save-20261022-112055" -> "2026-10-22 11:20:55", or "".
std::string StampTime(const std::string &stem) {
    if (stem.size() < 20 || stem.rfind("save-", 0) != 0 || stem[13] != '-')
        return {};
    for (size_t i = 5; i < 20; ++i)
        if (i != 13 && !std::isdigit(static_cast<unsigned char>(stem[i])))
            return {};
    return stem.substr(5, 4) + "-" + stem.substr(9, 2) + "-" + stem.substr(11, 2) + " " + stem.substr(14, 2) + ":" +
           stem.substr(16, 2) + ":" + stem.substr(18, 2);
}

bool HasSave(const fs::path &dir) {
    std::error_code ec;
    return fs::is_directory(HeadersDir(dir), ec) && fs::is_directory(dir / "00000001", ec);
}

std::optional<fs::path> FindSaveRoot(const fs::path &dir) {
    if (HasSave(dir))
        return dir;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_directory(ec) && HasSave(it->path()))
            return it->path();
    return std::nullopt;
}

bool NotEmpty(const fs::path &dir) {
    std::error_code ec;
    return fs::is_directory(dir, ec) && fs::directory_iterator(dir, ec) != fs::directory_iterator();
}

bool IsBlueprint(const std::vector<uint8_t> &d, int *parts) {
    if (d.size() < kBlueprintHeader || !std::equal(std::begin(kBlueprintMagic), std::end(kBlueprintMagic), d.begin()))
        return false;
    const int n = d[8] << 8 | d[9];
    if (parts)
        *parts = n;
    return n >= 1 && d.size() == kBlueprintHeader + size_t(n) * kPartSize;
}

fs::path ManifestPath(const fs::path &save) { return save.parent_path().parent_path() / "nutstaller-workshop.json"; }
fs::path ModManifestPath(const fs::path &save) {
    return save.parent_path().parent_path() / "nutstaller-workshop-mods.json";
}

fs::path ContentFile(const fs::path &save, const std::string &content_id) {
    return save / "00000001" / content_id / content_id.substr(2);
}

void SaveInstalled(const fs::path &save, const std::vector<Installed> &list) {
    std::string s = "[";
    for (const auto &i : list) {
        if (s.size() > 1)
            s += ",";
        s += "{\"Id\":" + Json(i.id) + ",\"ContentId\":" + Json(i.content_id) + ",\"Name\":" + Json(i.name) +
             ",\"Author\":" + Json(i.author) + ",\"Description\":" + Json(i.description) +
             ",\"Parts\":" + std::to_string(i.parts) + ",\"PreviewPath\":" + Json(i.preview) + "}";
    }
    WriteText(ManifestPath(save), s + "]");
}

void SaveInstalledMods(const fs::path &save, const std::vector<InstalledMod> &list) {
    std::string s = "[";
    for (const auto &m : list) {
        if (s.size() > 1)
            s += ",";
        s += "{\"Id\":" + Json(m.id) + ",\"ModId\":" + Json(m.mod_id) + ",\"Name\":" + Json(m.name) +
             ",\"Author\":" + Json(m.author) + ",\"Description\":" + Json(m.description) +
             ",\"Parts\":" + std::to_string(m.parts) + ",\"PreviewPath\":" + Json(m.preview) +
             ",\"File\":" + Json(m.file) + "}";
    }
    WriteText(ModManifestPath(save), s + "]");
}

// Adds the blueprint as a new vehicle: the next id after the game's own, its
// content file and header. The id, or empty.
std::string AddVehicle(const fs::path &save, const std::vector<uint8_t> &blueprint, const std::string &name) {
    std::error_code ec;
    fs::create_directories(HeadersDir(save), ec);
    uint32_t next = 1;
    for (const auto &e : fs::directory_iterator(HeadersDir(save), ec)) {
        const std::string stem = e.path().stem().string();
        if (e.path().extension() == ".header" && stem.rfind("0x", 0) == 0) {
            const uint32_t used = uint32_t(std::strtoul(stem.c_str() + 2, nullptr, 16));
            if (used < 0x01000000)
                next = std::max(next, used + 1);
        }
    }
    char id[16];
    std::snprintf(id, sizeof id, "0x%08x", next);
    const fs::path folder = save / "00000001" / id;
    fs::create_directories(folder, ec);
    if (!WriteFile(folder / (id + 2), blueprint.data(), blueprint.size()))
        return {};

    std::vector<uint8_t> header(kContentHeaderSize, 0);
    PutBe32(&header[0x00], 1);  // device: hard drive
    PutBe32(&header[0x04], 1);  // content type: save game
    std::u16string display = Utf16("VEHICLE: " + name);
    if (display.size() > 127)
        display.resize(127);
    for (size_t i = 0; i < display.size(); ++i) {
        header[8 + i * 2] = uint8_t(display[i] >> 8);
        header[9 + i * 2] = uint8_t(display[i]);
    }
    std::memcpy(&header[0x108], id, 10);  // file name
    PutBe32(&header[0x140], kTitleId);
    if (!WriteFile(HeadersDir(save) / (std::string(id) + ".header"), header.data(), header.size()))
        return {};
    return id;
}

}  // namespace

fs::path shared_root = "/sdcard/reNut";

// --- saves ---------------------------------------------------------------------

fs::path SaveDir(const fs::path &user_data_root) { return user_data_root / "B13EBABEBABEBABE" / "4D5307ED"; }

SaveSummary Summarize(const fs::path &save) {
    SaveSummary s;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(HeadersDir(save), ec)) {
        if (e.path().extension() != ".header")
            continue;
        auto header = ReadFile(e.path());
        if (!header || header->size() < 0x108)
            continue;
        const std::string name = DisplayName(*header);
        if (name.rfind("VEHICLE: ", 0) == 0)
            s.vehicles++;
        else if (name.find(" - REPLAY - ") != std::string::npos)
            s.replays++;
        else
            s.slots++;
    }
    for (auto it = fs::recursive_directory_iterator(save, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_regular_file(ec))
            s.bytes += it->file_size(ec);
    return s;
}

fs::path BackupDir() { return shared_root / "backups"; }

std::vector<Backup> Backups() {
    std::vector<Backup> list;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(BackupDir(), ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".zip")
            continue;
        Backup b;
        b.file = e.path();
        b.bytes = e.file_size(ec);
        const std::string stem = e.path().stem().string();
        b.made = StampTime(stem);
        b.note = b.made.empty() ? stem : stem.size() > 21 ? stem.substr(21) : "";
        std::replace(b.note.begin(), b.note.end(), '-', ' ');
        list.push_back(std::move(b));
    }
    // Stamped names sort by time; the rest after them.
    std::sort(list.begin(), list.end(), [](const Backup &a, const Backup &b) {
        if (a.made.empty() != b.made.empty())
            return b.made.empty();
        return a.made != b.made ? a.made > b.made : a.file.filename() < b.file.filename();
    });
    return list;
}

fs::path BackUp(const fs::path &save, const std::string &note, std::string *error) {
    if (!NotEmpty(save)) {
        *error = "there is no save to back up yet";
        return {};
    }
    std::error_code ec;
    fs::create_directories(BackupDir(), ec);
    const fs::path zip = BackupDir() / ("save-" + Now("%Y%m%d-%H%M%S") + (note.empty() ? "" : "-" + note) + ".zip");
    *error = renut::android::ZipFolder(save, zip);
    return error->empty() ? zip : fs::path();
}

fs::path RenameBackup(const fs::path &file, const std::string &name, std::string *error) {
    std::string clean;
    for (char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
            clean += c;
        else if (c == ' ')
            clean += '-';
    while (!clean.empty() && clean.front() == '-')
        clean.erase(0, 1);
    while (!clean.empty() && clean.back() == '-')
        clean.pop_back();
    if (clean.size() > 60)
        clean.resize(60);
    const std::string stem = file.stem().string();
    std::string target;
    if (!StampTime(stem).empty())
        target = stem.substr(0, 20) + (clean.empty() ? "" : "-" + clean);
    else if (!clean.empty())
        target = clean;
    else
        target = stem;
    const fs::path to = file.parent_path() / (target + ".zip");
    std::error_code ec;
    if (to != file) {
        if (fs::exists(to, ec)) {
            *error = "a backup with that name already exists";
            return {};
        }
        fs::rename(file, to, ec);
        if (ec) {
            *error = ec.message();
            return {};
        }
    }
    return to;
}

std::vector<fs::path> ImportCandidates() {
    std::vector<fs::path> list;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(shared_root, ec)) {
        const std::string name = e.path().filename().string();
        if (!e.is_directory(ec) || name == "game" || name == "backups" || name == "drivers")
            continue;
        if (FindSaveRoot(e.path()))
            list.push_back(e.path());
    }
    std::sort(list.begin(), list.end());
    return list;
}

bool Replace(const fs::path &save, const fs::path &source, const std::string &note, fs::path *kept,
             std::string *error) {
    std::error_code ec;
    kept->clear();
    const fs::path tmp = save.string() + ".renut-tmp", unpacked = save.string() + ".renut-tmp-zip",
                   old = save.string() + ".renut-old";
    fs::remove_all(tmp, ec);
    fs::remove_all(unpacked, ec);
    auto cleanup = [&] {
        fs::remove_all(tmp, ec);
        fs::remove_all(unpacked, ec);
    };
    fs::create_directories(save.parent_path(), ec);

    fs::path from = source;
    if (fs::is_regular_file(source, ec)) {
        fs::create_directories(unpacked, ec);
        *error = renut::android::Unzip(source, unpacked);
        if (!error->empty()) {
            cleanup();
            return false;
        }
        from = unpacked;
    }
    auto root = FindSaveRoot(from);
    if (!root) {
        *error = "that is not a Nuts & Bolts save: no Headers/00000001 and 00000001 folders in it";
        cleanup();
        return false;
    }
    fs::copy(*root, tmp, fs::copy_options::recursive, ec);
    if (ec) {
        *error = "copying the save failed: " + ec.message();
        cleanup();
        return false;
    }
    if (NotEmpty(save)) {
        *kept = BackUp(save, "before-" + note, error);
        if (kept->empty()) {
            *error = "backing up the current save failed: " + *error;
            cleanup();
            return false;
        }
    }
    fs::remove_all(old, ec);
    if (fs::exists(save, ec)) {
        fs::rename(save, old, ec);
        if (ec) {
            *error = ec.message();
            cleanup();
            return false;
        }
    }
    fs::rename(tmp, save, ec);
    if (ec) {
        *error = ec.message();
        std::error_code ignore;
        if (!fs::exists(save, ignore) && fs::exists(old, ignore))
            fs::rename(old, save, ignore);
        cleanup();
        return false;
    }
    fs::remove_all(old, ec);
    cleanup();
    error->clear();
    return true;
}

// --- workshop ------------------------------------------------------------------

std::optional<Index> FetchIndex(std::string *error) {
    // raw.githubusercontent.com caches; the query string gets a fresh copy
    auto body = renut::android::HttpGet(std::string(kRaw) + "index.json?t=" + std::to_string(std::time(nullptr)));
    if (!body) {
        *error = "couldn't reach the workshop (no internet?)";
        return std::nullopt;
    }
    auto doc = mods::ParseJson(std::string_view(reinterpret_cast<const char *>(body->data()), body->size()), error);
    if (!doc)
        return std::nullopt;
    Index index;
    for (const auto &b : doc->Array("blueprints")) {
        Entry e;
        e.id = b.String("id");
        e.name = b.String("name");
        e.description = b.String("description");
        e.author = b.String("author");
        e.parts = int(b.Int("parts"));
        e.blueprint = b.String("blueprint");
        e.preview = b.String("preview");
        for (const auto &r : b.Array("requires"))
            if (r.type == mods::JsonValue::Type::String)
                e.requires_mods.push_back(r.string);
        for (const auto &m : b.Array("missing"))
            e.missing.push_back(m.String("id") + " (" + m.String("mod") + ")");
        index.blueprints.push_back(std::move(e));
    }
    for (const auto &m : doc->Array("mods")) {
        ModEntry e;
        e.id = m.String("id");
        e.mod_id = m.String("mod_id");
        e.name = m.String("name");
        e.description = m.String("description");
        e.author = m.String("author");
        e.version = m.String("version");
        e.sha256 = m.String("sha256");
        e.download = m.String("download");
        e.preview = m.String("preview");
        e.size = uint64_t(m.Int("size"));
        for (const auto &p : m.Array("parts"))
            e.part_names.push_back(p.String("name"));
        index.mods.push_back(std::move(e));
    }
    return index;
}

std::optional<std::vector<uint8_t>> Download(const std::string &repo_path) {
    return renut::android::HttpGet(kRaw + repo_path);
}

std::vector<Installed> LoadInstalled(const fs::path &save) {
    std::vector<Installed> list;
    auto doc = ReadJson(ManifestPath(save));
    if (!doc || doc->type != mods::JsonValue::Type::Array)
        return list;
    std::error_code ec;
    for (const auto &v : doc->array) {
        Installed i{v.String("Id"),     v.String("ContentId"),   v.String("Name"),
                    v.String("Author"), v.String("Description"), v.String("PreviewPath"),
                    int(v.Int("Parts"))};
        if (i.content_id.size() > 2 && fs::exists(ContentFile(save, i.content_id), ec))
            list.push_back(std::move(i));
    }
    return list;
}

std::vector<InstalledMod> LoadInstalledMods(const fs::path &save) {
    std::vector<InstalledMod> list;
    auto doc = ReadJson(ModManifestPath(save));
    if (!doc || doc->type != mods::JsonValue::Type::Array)
        return list;
    std::error_code ec;
    for (const auto &v : doc->array) {
        InstalledMod m{v.String("Id"),          v.String("ModId"),       v.String("Name"), v.String("Author"),
                       v.String("Description"), v.String("PreviewPath"), v.String("File"), int(v.Int("Parts"))};
        if (fs::exists(m.file, ec))
            list.push_back(std::move(m));
    }
    return list;
}

fs::path ModsDir() {
    fs::path folder = REXCVAR_GET(renut_mods_folder);
    if (folder.empty())
        folder = "mods";
    return folder.is_relative() ? renut::paths::WritableFolder() / folder : folder;
}

bool InstallMod(const fs::path &save, const ModEntry &mod, std::string *error) {
    // only a plain id becomes a file name
    if (mod.mod_id.empty() || mod.mod_id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos) {
        *error = "the workshop gave a bad mod id";
        return false;
    }
    auto data = renut::android::HttpGet(mod.download);
    if (!data) {
        *error = "couldn't download " + mod.name;
        return false;
    }
    std::string hash = renut::android::Sha256(*data), want = mod.sha256;
    std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (hash.empty() || hash != want) {
        *error = mod.name + " did not download correctly; try again";
        return false;
    }
    std::error_code ec;
    fs::create_directories(ModsDir(), ec);
    const fs::path file = ModsDir() / (mod.mod_id + ".nbpatch");
    if (!WriteFile(file, data->data(), data->size())) {
        *error = "couldn't write " + file.string();
        return false;
    }
    auto list = LoadInstalledMods(save);
    std::erase_if(list, [&](const InstalledMod &m) { return m.mod_id == mod.mod_id; });
    list.push_back({mod.id, mod.mod_id, mod.name, mod.author, mod.description, mod.preview, file.string(),
                    int(mod.part_names.size())});
    SaveInstalledMods(save, list);
    return true;
}

void RemoveMod(const fs::path &save, const std::string &mod_id) {
    auto list = LoadInstalledMods(save);
    std::error_code ec;
    for (const auto &m : list)
        if (m.mod_id == mod_id)
            fs::remove(m.file, ec);
    std::erase_if(list, [&](const InstalledMod &m) { return m.mod_id == mod_id; });
    SaveInstalledMods(save, list);
}

bool InstallVehicle(const fs::path &save, const Entry &entry, const Index &index, std::string *error) {
    const auto have = LoadInstalledMods(save);
    for (const auto &need : entry.requires_mods) {
        if (std::any_of(have.begin(), have.end(), [&](const InstalledMod &m) { return m.id == need; }))
            continue;
        auto mod = std::find_if(index.mods.begin(), index.mods.end(), [&](const ModEntry &m) { return m.id == need; });
        if (mod == index.mods.end()) {
            *error = "a part mod it needs is gone from the workshop";
            return false;
        }
        if (!InstallMod(save, *mod, error))
            return false;
    }
    auto blueprint = Download(entry.blueprint);
    int parts = 0;
    if (!blueprint || !IsBlueprint(*blueprint, &parts)) {
        *error = "couldn't download the vehicle";
        return false;
    }
    const std::string content_id = AddVehicle(save, *blueprint, entry.name);
    if (content_id.empty()) {
        *error = "couldn't write to the save";
        return false;
    }
    auto list = LoadInstalled(save);
    std::erase_if(list, [&](const Installed &i) { return i.id == entry.id; });
    list.push_back({entry.id, content_id, entry.name, entry.author, entry.description, entry.preview, entry.parts});
    SaveInstalled(save, list);
    return true;
}

void RemoveVehicle(const fs::path &save, const std::string &id) {
    auto list = LoadInstalled(save);
    std::error_code ec;
    for (const auto &i : list) {
        if (i.id != id)
            continue;
        fs::remove_all(save / "00000001" / i.content_id, ec);
        fs::remove(HeadersDir(save) / (i.content_id + ".header"), ec);
    }
    std::erase_if(list, [&](const Installed &i) { return i.id == id; });
    SaveInstalled(save, list);
}

}  // namespace renut::content
