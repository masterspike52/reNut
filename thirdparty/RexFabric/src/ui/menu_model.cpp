#include "menu_model.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

#include <rexfabric/rexfabric.h>

#include "../content/classes.h"
#include "../log.h"
#include "../platform.h"
#include "../lua/lua_scripts.h"
#include "image_decode.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace rexfabric::ui {
namespace {

// Thunderstore icons kept around for paging back and forth.
constexpr size_t kMaxPackageIcons = 96;

std::vector<uint8_t> ReadFile(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  return in ? std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {}) : std::vector<uint8_t>{};
}

// Starts this program again with the same arguments.
bool Relaunch() {
#if defined(_WIN32)
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  std::wstring command = GetCommandLineW();
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return false;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
#elif defined(__linux__)
  std::ifstream in("/proc/self/cmdline", std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(in)), {});
  std::vector<char*> args;
  for (size_t start = 0; start < text.size(); start = text.find('\0', start) + 1) args.push_back(text.data() + start);
  args.push_back(nullptr);
  if (fork() == 0) {
    execv("/proc/self/exe", args.data());
    _exit(1);
  }
  return true;
#else
  return false;
#endif
}

}  // namespace

MenuModel::MenuModel(std::filesystem::path mods_dir, const std::string& community, Images images) : mods_(mods_dir), images_(std::move(images)) {
  if (!community.empty()) thunderstore_ = std::make_unique<net::Thunderstore>(community, mods_dir);
  mods_.Scan();
  seen_revision_ = ModsRevision();
}

MenuModel::~MenuModel() = default;

void MenuModel::Poll() {
  ++frame_;
  if (thunderstore_) {
    thunderstore_->Poll();
    for (net::Thunderstore::Failure& failure : thunderstore_->TakeFailed()) failures_.push_back(std::move(failure));
    if (!thunderstore_->TakeInstalled().empty()) {
      mods_.Save();
      NoteModsChanged();
      changed_ = true;
    }
  }
  retired_.clear();
  // Something changed the mods since the last scan: an install, or the game.
  if (ModsRevision() != seen_revision_) {
    seen_revision_ = ModsRevision();
    mods_.Scan();
    mod_icons_.clear();
  }
}

void MenuModel::SetEnabled(size_t index, bool enabled) {
  mods_.SetEnabled(index, enabled);
  NoteModsChanged();
  seen_revision_ = ModsRevision();
  changed_ = true;
}

void MenuModel::Move(size_t index, int delta) {
  mods_.Move(index, delta);
  NoteModsChanged();
  seen_revision_ = ModsRevision();
  changed_ = true;
}

void MenuModel::Install(const net::Package& package) {
  if (thunderstore_) thunderstore_->RequestInstall(package);
}

bool MenuModel::Restart() {
  rexfabric::Stop();
  if (!Relaunch()) {
    RF_ERROR("couldn't restart; restart the game to apply mod changes");
    return false;
  }
  rex::log::Flush();
  std::_Exit(0);
}

std::vector<ModInfo> MenuModel::Loadable() const {
  const std::vector<ModInfo> enabled(mods_.mods().begin(), mods_.mods().begin() + static_cast<ptrdiff_t>(mods_.EnabledCount()));
  return LoadableMods(enabled, GetConfig().game_version, nullptr);
}

void MenuModel::ReloadScripts() {
  const std::vector<ModInfo> loadable = Loadable();
  content::LoadModContent(loadable);
  lua::Reload(loadable);
}

void MenuModel::ReloadContent() { content::LoadModContent(Loadable()); }

MenuModel::Texture::~Texture() {
  if (id_ != ImTextureID_Invalid && images_.destroy) images_.destroy(id_);
}

std::unique_ptr<MenuModel::Texture> MenuModel::MakeTexture(const std::vector<uint8_t>& encoded, uint32_t* width_out, uint32_t* height_out) {
  if (!images_.create || encoded.empty()) return nullptr;
  int width = 0;
  int height = 0;
  const std::vector<uint8_t> rgba = DecodeImageRGBA(encoded.data(), encoded.size(), width, height);
  if (rgba.empty() || width <= 0 || height <= 0) return nullptr;
  if (width_out) *width_out = static_cast<uint32_t>(width);
  if (height_out) *height_out = static_cast<uint32_t>(height);
  const ImTextureID id = images_.create(static_cast<uint32_t>(width), static_cast<uint32_t>(height), rgba.data());
  if (id == ImTextureID_Invalid) return nullptr;
  return std::make_unique<Texture>(images_, id);
}

MenuModel::Picture MenuModel::AssetPicture(const std::filesystem::path& file) {
  std::error_code code;
  const std::filesystem::file_time_type written = std::filesystem::last_write_time(file, code);
  if (code) return {};
  CachedPicture& picture = pictures_[file.string()];
  // Read once per change, failed or not, so a picture that can't be read isn't read every frame.
  if (!picture.texture || picture.written != written) {
    if (picture.texture) retired_.push_back(std::move(picture.texture));
    picture.written = written;
    picture.texture = MakeTexture(ReadFile(file), &picture.width, &picture.height);
  }
  return picture.texture ? Picture{picture.texture->id(), picture.width, picture.height} : Picture{};
}

ImTextureID MenuModel::ModIcon(const ModInfo& mod) {
  auto [it, added] = mod_icons_.try_emplace(mod.folder);
  // Made once per scan, failed or not, so a missing icon isn't looked for every frame.
  if (added) it->second.texture = MakeTexture(ReadFile(mod.dir / "icon.png"));
  return it->second.texture ? it->second.texture->id() : ImTextureID_Invalid;
}

ImTextureID MenuModel::PackageIcon(const net::Package& package, bool* loading) {
  if (!thunderstore_ || package.icon_url.empty()) return ImTextureID_Invalid;
  if (auto it = package_icons_.find(package.icon_url); it != package_icons_.end()) {
    it->second.used = frame_;
    return it->second.texture ? it->second.texture->id() : ImTextureID_Invalid;
  }
  thunderstore_->RequestIcon(package.icon_url);
  std::vector<uint8_t> encoded;
  if (!thunderstore_->TakeIcon(package.icon_url, encoded)) {
    if (loading) *loading = thunderstore_->IconLoading(package.icon_url);
    return ImTextureID_Invalid;
  }
  if (package_icons_.size() >= kMaxPackageIcons) PrunePackageIcons();
  CachedIcon& icon = package_icons_[package.icon_url];
  icon = {MakeTexture(encoded), frame_};
  return icon.texture ? icon.texture->id() : ImTextureID_Invalid;
}

void MenuModel::PrunePackageIcons() {
  // The least recently drawn go first. This frame's are kept, as its draw list may hold them.
  std::vector<std::pair<uint64_t, std::string>> by_use;
  for (const auto& [url, icon] : package_icons_) {
    if (icon.used < frame_) by_use.emplace_back(icon.used, url);
  }
  std::sort(by_use.begin(), by_use.end());
  const size_t drop = std::min(by_use.size(), package_icons_.size() - kMaxPackageIcons * 3 / 4);
  for (size_t i = 0; i < drop; ++i) {
    package_icons_.erase(by_use[i].second);
    thunderstore_->ForgetIcon(by_use[i].second);
  }
}

void MenuModel::OpenModsFolder() const {
  std::error_code error;
  std::filesystem::create_directories(mods_.root(), error);
  OpenFolder(mods_.root());
}

}  // namespace rexfabric::ui
