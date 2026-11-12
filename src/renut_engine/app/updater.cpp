#include "renut_engine/app/updater.h"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <span>
#include <system_error>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include <rex/platform/file.h>
#include <rex/string/text.h>
#include "renut_engine/app/app_paths.h"
#include "renut_engine/mods/sha256.h"
#include "renut_engine/mods/zip_reader.h"
#include "thirdparty/rexglue_guide/src/http.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
extern char **environ;
#endif

namespace renut::update {

namespace {

namespace fs = std::filesystem;
namespace http = rexglue_guide::http;
using json = nlohmann::json;

constexpr const char kApiHost[] = "https://api.github.com";
constexpr const char kLatestPath[] = "/repos/masterspike52/reNut/releases/latest";
constexpr const char kInstalledFile[] = "installed_release.txt";
constexpr int kApiTimeoutMs = 15000;
constexpr int kDownloadTimeoutMs = 15 * 60 * 1000;

std::mutex g_mutex;
Status g_status;

void Set(State state, std::string release = {}, std::string error = {}) {
    std::lock_guard lock(g_mutex);
    g_status.state = state;
    if (!release.empty())
        g_status.release = std::move(release);
    g_status.error = std::move(error);
}

fs::path InstalledPath() {
    return renut::paths::WritableFolder() / kInstalledFile;
}

std::string ReadInstalled() {
    std::ifstream in(InstalledPath());
    std::string tag;
    std::getline(in, tag);
    while (!tag.empty() && (tag.back() == '\r' || tag.back() == ' '))
        tag.pop_back();
    return tag;
}

void WriteInstalled(const std::string &tag) {
    std::ofstream(InstalledPath(), std::ios::trunc) << tag << "\n";
}

bool SplitUrl(const std::string &url, http::Endpoint *endpoint, std::string *path) {
    if (!http::ParseUrl(url, endpoint))
        return false;
    const size_t start = url.find("://");
    const size_t slash = start == std::string::npos ? std::string::npos : url.find('/', start + 3);
    *path = slash == std::string::npos ? "/" : url.substr(slash);
    return true;
}

http::Response Fetch(const std::string &url, int timeout_ms) {
    http::Endpoint endpoint;
    std::string path;
    if (!SplitUrl(url, &endpoint, &path)) {
        http::Response r;
        r.error = "bad address " + url;
        return r;
    }
    return http::Request(endpoint, "GET", path, "", "", "application/json", timeout_ms);
}

struct Asset {
    std::string name;
    std::string url;
    std::string sha256; 
};

std::vector<Asset> PlatformAssets(const json &release) {
    std::vector<Asset> assets;
    for (const json &a : release.value("assets", json::array())) {
        Asset asset{a.value("name", ""), a.value("browser_download_url", ""), a.value("digest", "")};
        if (asset.sha256.rfind("sha256:", 0) == 0)
            asset.sha256 = asset.sha256.substr(7);
        else
            asset.sha256.clear();
#if defined(_WIN32)
        const bool ours = asset.name.size() > 4 && asset.name.substr(asset.name.size() - 4) == ".zip";
#else
        const bool ours = asset.name.size() > 9 && asset.name.substr(asset.name.size() - 9) == ".AppImage";
#endif
        if (ours && !asset.url.empty())
            assets.push_back(std::move(asset));
    }
    return assets;
}

struct Failure {
    std::string what;
};

std::string Download(const Asset &asset) {
    const http::Response r = Fetch(asset.url, kDownloadTimeoutMs);
    if (r.status != 200)
        throw Failure{r.status ? "GitHub answered " + std::to_string(r.status) + " for " + asset.name
                               : "the download of " + asset.name + " failed (" + r.error + ")"};
    if (!asset.sha256.empty()) {
        const auto *bytes = reinterpret_cast<const uint8_t *>(r.body.data());
        if (renut::mods::Sha256Hex(std::span<const uint8_t>(bytes, r.body.size())) != asset.sha256)
            throw Failure{asset.name + " did not download whole"};
    }
    return r.body;
}

void SaveFile(const fs::path &path, const void *data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char *>(data), std::streamsize(size));
    out.close();
    if (!out)
        throw Failure{"can't write " + rex::string::PathToUTF8(path)};
}

#if defined(_WIN32)

void InstallZip(const std::string &zip_bytes, const fs::path &folder) {
    const fs::path zip_path = renut::paths::WritableFolder() / "update.zip";
    SaveFile(zip_path, zip_bytes.data(), zip_bytes.size());
    renut::mods::ZipReader zip;
    std::string error;
    if (!zip.Open(zip_path, &error))
        throw Failure{"the release's zip can't be read (" + error + ")"};
    std::vector<fs::path> written;
    for (const auto &entry : zip.Entries()) {
        if (entry.name.empty() || entry.name.back() == '/')
            continue;
        const fs::path relative = rex::string::to_path(entry.name).lexically_normal();
        if (relative.is_absolute() || relative.empty() || *relative.begin() == "..")
            throw Failure{"the release's zip has a file outside its folder (" + entry.name + ")"};
        const auto data = zip.Read(entry.name, &error);
        if (!data)
            throw Failure{"the release's zip is damaged (" + error + ")"};
        const fs::path target = folder / relative;
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        fs::path fresh = target;
        fresh += ".new";
        SaveFile(fresh, data->data(), data->size());
        written.push_back(target);
    }
    std::error_code ec;
    for (const fs::path &target : written) {
        fs::path fresh = target, old = target;
        fresh += ".new";
        old += ".old";
        fs::remove(old, ec);
        if (fs::exists(target, ec)) {
            fs::rename(target, old, ec);
            if (ec)
                throw Failure{"can't move " + rex::string::PathToUTF8(target) + " aside (" + ec.message() + ")"};
        }
        fs::rename(fresh, target, ec);
        if (ec)
            throw Failure{"can't put " + rex::string::PathToUTF8(target) + " in place (" + ec.message() + ")"};
    }
    fs::remove(zip_path, ec);
}

#else

void InstallAppImage(const std::string &bytes, const fs::path &image) {
    fs::path fresh = image;
    fresh += ".new";
    SaveFile(fresh, bytes.data(), bytes.size());
    std::error_code ec;
    fs::permissions(fresh, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec, ec);
    fs::rename(fresh, image, ec);
    if (ec)
        throw Failure{"can't replace " + rex::string::PathToUTF8(image) + " (" + ec.message() + ")"};
}

#endif

fs::path InstallTarget() {
#if defined(_WIN32)
    return rex::platform::GetExecutableFolder();
#else
    const char *image = std::getenv("APPIMAGE");
    return image && *image ? fs::path(image) : fs::path();
#endif
}

void Run() {
    const fs::path target = InstallTarget();
    if (target.empty()) {
        Set(State::kOff);
        return;
    }
    Set(State::kChecking);
    const http::Response r = Fetch(std::string(kApiHost) + kLatestPath, kApiTimeoutMs);
    if (r.status != 200) {
        Set(State::kFailed, {}, r.status ? "GitHub answered " + std::to_string(r.status) : "no connection to GitHub");
        return;
    }
    json release;
    try {
        release = json::parse(r.body);
    } catch (...) {
        Set(State::kFailed, {}, "GitHub's answer could not be read");
        return;
    }
    const std::string tag = release.value("tag_name", "");
    const std::string name = release.value("name", tag);
    if (tag.empty()) {
        Set(State::kFailed, {}, "GitHub named no release");
        return;
    }
    const std::string installed = ReadInstalled();
    if (installed.empty()) {
        WriteInstalled(tag);
        Set(State::kUpToDate, name);
        return;
    }

    if (installed == tag) {
        Set(State::kUpToDate, name);
        return;
    }

    try {
        const std::vector<Asset> assets = PlatformAssets(release);
        if (assets.empty())
            throw Failure{"the release has no download for this system"};
        Set(State::kDownloading, name);
        std::vector<std::string> files;
        for (const Asset &asset : assets)
            files.push_back(Download(asset));
        Set(State::kInstalling, name);
#if defined(_WIN32)
        for (const std::string &zip : files) InstallZip(zip, target);
#else
        InstallAppImage(files.front(), target);
#endif
    } catch (const Failure &f) {
        Set(State::kFailed, name, f.what);
        return;
    } catch (const std::exception &e) {
        Set(State::kFailed, name, e.what());
        return;
    }
    WriteInstalled(tag);
    Set(State::kReady, name);
}

void DeleteOldFiles() {
#if defined(_WIN32)
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(rex::platform::GetExecutableFolder(), ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".old")
            fs::remove(entry.path(), ec);
#endif
}

} 

void Start() {
#if defined(RENUT_RELEASE_BUILD)
    static std::once_flag once;
    std::call_once(once, [] {
        DeleteOldFiles();
        std::thread(Run).detach();
    });
#endif
}

Status Get() {
    std::lock_guard lock(g_mutex);
    return g_status;
}

bool Restart() {
#if defined(_WIN32)
    const std::wstring exe = rex::platform::GetExecutablePath().wstring();
    std::wstring command_line = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, rex::platform::GetExecutableFolder().wstring().c_str(), &startup, &process))
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    const char *image = std::getenv("APPIMAGE");
    if (!image || !*image)
        return false;
    char *argv[] = {const_cast<char *>(image), nullptr};
    pid_t pid = 0;
    return posix_spawn(&pid, image, nullptr, nullptr, argv, environ) == 0;
#endif
}

} 
