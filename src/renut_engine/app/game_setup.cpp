#include "renut_engine/app/game_setup.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <rex/filesystem/file_system_factory.h>
#include <rex/filesystem/stfs.h>
#include <rex/module.h>
#include <rex/string/text.h>
#include "renut_engine/app/app_paths.h"
#include "renut_engine/app/dlc_install.h"
#include "renut_engine/app/path_config_store.h"

namespace renut::setup {

namespace {

namespace fs = std::filesystem;
using rex::filesystem::ContainerNode;
using rex::filesystem::IContainerImage;

constexpr uint32_t kTitleId = 0x4D5307ED; 
constexpr uint32_t kUsMediaId = 0x3E567DFF;
constexpr uint32_t kContentTypeTitleUpdate = 0x000B0000;
constexpr uint32_t kContentTypeDlc = 2;
constexpr const char kDlcHeaders[] = "0000000000000000/4D5307ED/Headers/00000002";
constexpr const char kSystemUpdate[] = "$SystemUpdate";
constexpr const char kUnfinished[] = ".renut_unfinished";
constexpr size_t kChunkBytes = size_t(8) << 20;

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Cancelled {};

struct State {
    std::mutex mutex;
    std::thread thread;
    Progress progress;
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> cancel{false};

    ~State() {
        cancel = true;
        if (thread.joinable())
            thread.join();
    }
} g;

std::string Utf8(const fs::path &path) {
    return rex::string::PathToUTF8(path);
}

uint32_t LoadBe32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void ShowFile(std::string file) {
    std::lock_guard lock(g.mutex);
    g.progress.file = std::move(file);
}

void MarkUnfinished(const fs::path &folder) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    std::ofstream marker(folder / kUnfinished, std::ios::trunc);
    marker << "reNut was filling this folder and did not finish.\n";
    marker.close();
    if (!marker)
        throw Failure("Can't write to " + Utf8(folder) + ".");
}

void MarkFinished(const fs::path &folder) {
    std::error_code ec;
    fs::remove(folder / kUnfinished, ec);
}

void WriteChunk(std::ofstream &out, const uint8_t *data, size_t size, const fs::path &dest) {
    out.write(reinterpret_cast<const char *>(data), std::streamsize(size));
    if (!out)
        throw Failure("Can't write " + Utf8(dest) + " (is the disk full?).");
    g.bytes += size;
}

void CopyEntry(const IContainerImage &image, const ContainerNode &entry, const fs::path &dest,
               std::vector<uint8_t> &buffer) {
    const std::string path = image.GetNodePath(entry);
    ShowFile(path);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out)
        throw Failure("Can't write " + Utf8(dest) + ".");
    for (uint64_t offset = 0, size = entry.info.size; offset < size;) {
        if (g.cancel)
            throw Cancelled{};
        const size_t want = size_t(std::min<uint64_t>(buffer.size(), size - offset));
        const auto read = image.Read(entry, offset, std::span<uint8_t>(buffer.data(), want));
        if (!read || !*read)
            throw Failure("Can't read " + path + " (is the file damaged?).");
        WriteChunk(out, buffer.data(), *read, dest);
        offset += *read;
    }
    out.close();
    if (!out)
        throw Failure("Can't write " + Utf8(dest) + " (is the disk full?).");
}

bool IsFolder(const ContainerNode &entry) {
    return rex::HasFlags(entry.info.attributes, rex::filesystem::FileAttributes::kDirectory);
}

uint64_t TreeBytes(const IContainerImage &image, const ContainerNode &dir, const char *skip) {
    uint64_t bytes = 0;
    for (const uint32_t index : dir.children) {
        const ContainerNode &child = image.GetNodes()[index];
        if (skip && child.info.name == skip)
            continue;
        bytes += IsFolder(child) ? TreeBytes(image, child, nullptr) : child.info.size;
    }
    return bytes;
}

void CopyTree(const IContainerImage &image, const ContainerNode &dir, const fs::path &dest, const char *skip,
              std::vector<uint8_t> &buffer) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec)
        throw Failure("Can't make " + Utf8(dest) + ": " + ec.message() + ".");
    for (const uint32_t index : dir.children) {
        const ContainerNode &child = image.GetNodes()[index];
        if (skip && child.info.name == skip)
            continue;
        const fs::path target = dest / rex::string::to_path(child.info.name);
        if (IsFolder(child))
            CopyTree(image, child, target, nullptr, buffer);
        else
            CopyEntry(image, child, target, buffer);
    }
}

// The disc image or package opened for reading, or null when it isn't one.
std::unique_ptr<IContainerImage> OpenContainer(const fs::path &path) {
    auto *factory = REX_INTERFACE(rex::filesystem::IFileSystemFactory);
    if (!factory)
        return nullptr;
    auto opened = factory->OpenContainer(path);
    if (!opened)
        return nullptr;
    return std::move(*opened);
}

struct XexIds {
    uint32_t media_id = 0;
    uint32_t title_id = 0;
};

XexIds ParseXexIds(const uint8_t *h, size_t size) {
    if (size < 0x18 || std::memcmp(h, "XEX2", 4) != 0)
        return {};
    const uint32_t count = LoadBe32(h + 0x14);
    for (uint32_t i = 0; i < count && 0x18 + (i + 1) * 8 <= size; ++i) {
        const uint8_t *header = h + 0x18 + i * 8;
        const uint32_t offset = LoadBe32(header + 4);
        if (LoadBe32(header) == 0x00040006 && offset + 16 <= size)
            return {LoadBe32(h + offset), LoadBe32(h + offset + 12)};
    }
    return {};
}

constexpr size_t kXexHeadBytes = 64 * 1024;

XexIds DiscXexIds(const IContainerImage &image, const ContainerNode &xex) {
    std::vector<uint8_t> h(size_t(std::min<uint64_t>(xex.info.size, kXexHeadBytes)));
    const auto read = image.Read(xex, 0, std::span<uint8_t>(h));
    return read ? ParseXexIds(h.data(), *read) : XexIds{};
}

XexIds FileXexIds(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> h(kXexHeadBytes);
    in.read(reinterpret_cast<char *>(h.data()), std::streamsize(h.size()));
    return ParseXexIds(h.data(), size_t(in.gcount()));
}

bool SameFolder(const fs::path &a, const fs::path &b) {
    std::error_code ec;
    return fs::weakly_canonical(a, ec) == fs::weakly_canonical(b, ec);
}

std::string ExtractDisc(const fs::path &image, const fs::path &game) {
    const std::unique_ptr<IContainerImage> disc = OpenContainer(image);
    if (!disc || disc->GetType() != rex::filesystem::ContainerType::kGDFX)
        throw Failure("That is not an Xbox 360 game disc image.");
    const ContainerNode &root = disc->GetRoot();
    const ContainerNode *xex = disc->Find("default.xex");
    if (!xex)
        throw Failure("That disc image has no default.xex.");
    const XexIds ids = DiscXexIds(*disc, *xex);
    if (ids.title_id != kTitleId)
        throw Failure(std::format("That is another game's disc (title {:08X}), not Nuts & Bolts.", ids.title_id));
    if (ids.media_id != kUsMediaId)
        throw Failure("That is another region's Nuts & Bolts disc: reNut runs the US (NTSC) version.");
    g.total = TreeBytes(*disc, root, kSystemUpdate);
    std::vector<uint8_t> buffer(kChunkBytes);
    MarkUnfinished(game);
    CopyTree(*disc, root, game, kSystemUpdate, buffer);
    MarkFinished(game);
    return "The game's files are in " + Utf8(game) + ".";
}

std::string ExtractTitleUpdate(const fs::path &package, const fs::path &game, const fs::path &update) {
    dlc::PackageInfo info;
    if (!dlc::ReadPackageInfo(package, info))
        throw Failure("That is not an Xbox 360 package (the title update is the file named TU_...).");
    if (info.title_id != kTitleId)
        throw Failure("That package is for another game.");
    if (info.content_type != kContentTypeTitleUpdate)
        throw Failure("That package is not a title update.");
    if (info.media_id != kUsMediaId)
        throw Failure("That title update is for another region's Nuts & Bolts: reNut needs the US (NTSC) one.");
    if (info.name.find("Title Update #") != std::string::npos &&
        info.name.find("Title Update #3") == std::string::npos)
        throw Failure("That is " + info.name + "; reNut runs on Title Update #3.");
    if (SameFolder(game, update))
        throw Failure("The update folder has to be a folder of its own, not the game folder.");
    const std::unique_ptr<IContainerImage> tu = OpenContainer(package);
    if (!tu || tu->GetType() == rex::filesystem::ContainerType::kGDFX)
        throw Failure("That title update could not be read (is the file damaged?).");
    const ContainerNode &root = tu->GetRoot();
    const ContainerNode *xexp = tu->Find("default.xexp");
    if (!xexp)
        throw Failure("That title update has no default.xexp.");
    g.total = TreeBytes(*tu, root, nullptr) + xexp->info.size;
    std::vector<uint8_t> buffer(kChunkBytes);
    MarkUnfinished(update);
    CopyTree(*tu, root, update, nullptr, buffer);
    std::error_code ec;
    fs::create_directories(game, ec);
    CopyEntry(*tu, *xexp, game / "default.xexp", buffer);
    MarkFinished(update);
    return (info.name.empty() ? std::string("The title update") : info.name) + " is in place.";
}

void CopyHostFile(const fs::path &from, const fs::path &to, std::vector<uint8_t> &buffer) {
    std::ifstream in(from, std::ios::binary);
    if (!in)
        throw Failure("Can't read " + Utf8(from) + ".");
    ShowFile(Utf8(from.filename()));
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out)
        throw Failure("Can't write " + Utf8(to) + ".");
    while (in) {
        if (g.cancel)
            throw Cancelled{};
        in.read(reinterpret_cast<char *>(buffer.data()), std::streamsize(buffer.size()));
        if (const auto read = in.gcount(); read > 0)
            WriteChunk(out, buffer.data(), size_t(read), to);
    }
    out.close();
    if (!out)
        throw Failure("Can't write " + Utf8(to) + " (is the disk full?).");
}

std::string AddDlc(const std::vector<fs::path> &packages) {
    // Nuts & Bolts has one DLC: one package, and none in the folder already.
    if (packages.size() != 1)
        throw Failure("Choose the one DLC package.");
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dlc::PackageFolder(), ec)) {
        dlc::PackageInfo info;
        if (entry.is_regular_file(ec) && dlc::ReadPackageInfo(entry.path(), info) && info.title_id == kTitleId &&
            info.content_type == kContentTypeDlc)
            throw Failure("The DLC is already added.");
    }
    std::vector<dlc::PackageInfo> infos;
    uint64_t total = 0;
    for (const fs::path &path : packages) {
        dlc::PackageInfo info;
        if (!dlc::ReadPackageInfo(path, info))
            throw Failure(Utf8(path.filename()) + " is not an Xbox 360 package.");
        if (info.title_id != kTitleId)
            throw Failure(Utf8(path.filename()) + " is for another game.");
        if (info.content_type != kContentTypeDlc)
            throw Failure(Utf8(path.filename()) + " is not DLC.");
        infos.push_back(std::move(info));
        total += fs::file_size(path, ec);
    }
    const fs::path folder = dlc::PackageFolder();
    fs::create_directories(folder, ec);
    g.total = total;
    std::vector<uint8_t> buffer(kChunkBytes);
    for (const fs::path &path : packages) {
        const fs::path target = folder / path.filename();
        if (!SameFolder(path, target))
            CopyHostFile(path, target, buffer);
    }
    if (infos.size() == 1)
        return (infos[0].name.empty() ? std::string("The DLC") : infos[0].name) + " installs when the game starts.";
    return std::format("{} DLC packages install when the game starts.", infos.size());
}

bool Start(Job job, std::function<std::string()> work) {
    std::lock_guard lock(g.mutex);
    if (g.progress.running)
        return false;
    if (g.thread.joinable())
        g.thread.join();
    g.progress = {};
    g.progress.job = job;
    g.progress.running = true;
    g.bytes = 0;
    g.total = 0;
    g.cancel = false;
    g.thread = std::thread([work = std::move(work)] {
        std::string result, error;
        try {
            result = work();
        } catch (const Cancelled &) {
            error = "Stopped before it finished.";
        } catch (const std::exception &e) {
            error = e.what();
        }
        std::lock_guard lock(g.mutex);
        g.progress.file.clear();
        g.progress.result = std::move(result);
        g.progress.error = std::move(error);
        g.progress.running = false;
    });
    return true;
}

}

Paths LoadPaths(const std::string &app_name, const Paths &defaults) {
    Paths p;
    if (auto saved = PathConfigStore::TryLoad(app_name)) {
        p.game = saved->game_data_root;
        p.user = saved->user_data_root;
        p.update = saved->update_data_root;
    }
    std::error_code ec;
    if (p.game.empty())
        p.game = fs::is_regular_file(defaults.game / "default.xex", ec) ? defaults.game : renut::paths::WritableFolder() / "game";
    if (p.user.empty())
        p.user = defaults.user;
    if (p.update.empty())
        p.update = defaults.update.empty() ? p.game / "update" : defaults.update;
    return p;
}

void SavePaths(const std::string &app_name, const Paths &paths) {
    PathConfigStore::SavedPaths saved;
    saved.game_data_root = paths.game;
    saved.user_data_root = paths.user;
    saved.update_data_root = paths.update;
    PathConfigStore::Save(app_name, saved);
}

Status Check(const Paths &paths) {
    Status s;
    std::error_code ec;
    if (fs::is_regular_file(paths.game / "default.xex", ec) && !fs::exists(paths.game / kUnfinished, ec)) {
        const XexIds ids = FileXexIds(paths.game / "default.xex");
        s.game = ids.title_id == kTitleId && ids.media_id == kUsMediaId;
        s.other_region = !s.game;
    }

    if (fs::is_regular_file(paths.game / "default.xexp", ec) && fs::is_directory(paths.update, ec) &&
        !fs::exists(paths.update / kUnfinished, ec)) {
        for (const auto &entry : fs::directory_iterator(paths.update, ec)) {
            if (entry.is_regular_file(ec)) {
                s.update = true;
                break;
            }
        }
    }

    for (const auto &entry : fs::directory_iterator(dlc::PackageFolder(), ec)) {
        dlc::PackageInfo info;
        if (entry.is_regular_file(ec) && dlc::ReadPackageInfo(entry.path(), info) && info.title_id == kTitleId &&
            info.content_type == kContentTypeDlc)
            s.dlc = true;
    }

    for (const auto &entry : fs::directory_iterator(paths.user / kDlcHeaders, ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".header")
            s.dlc = true;
    return s;
}

bool StartExtractDisc(const fs::path &image, const fs::path &game) {
    return Start(Job::kDisc, [image, game] { return ExtractDisc(image, game); });
}

bool StartExtractTitleUpdate(const fs::path &package, const fs::path &game, const fs::path &update) {
    return Start(Job::kTitleUpdate, [package, game, update] { return ExtractTitleUpdate(package, game, update); });
}

bool StartAddDlc(std::vector<fs::path> packages) {
    return Start(Job::kDlc, [packages = std::move(packages)] { return AddDlc(packages); });
}

Progress GetProgress() {
    std::lock_guard lock(g.mutex);
    Progress p = g.progress;
    p.bytes = g.bytes;
    p.total = g.total;
    return p;
}

void Cancel() {
    g.cancel = true;
    if (g.thread.joinable())
        g.thread.join();
    g.cancel = false;
}

}
