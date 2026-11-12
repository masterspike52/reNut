// What the mods menus show and change, shared by the standalone windows and the Guide blade: the mod list,
// Thunderstore when the game has a community, the icons, and whether a restart is due.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include <imgui.h>
#include <rexfabric/rexfabric.h>

#include "../mods.h"
#include "../net/thunderstore.h"

namespace rexfabric::ui {

class MenuModel {
 public:
  MenuModel(std::filesystem::path mods_dir, const std::string& community, Images images);
  ~MenuModel();

  // Once per frame, before anything is drawn: picks up finished installs, which rescans the list and so
  // frees textures a frame's draw list may still hold.
  void Poll();

  ModList& mods() { return mods_; }
  // Null when the game has no Thunderstore community.
  net::Thunderstore* thunderstore() { return thunderstore_.get(); }
  net::Query& query() { return query_; }
  bool IsInstalled(const std::string& package_name) const { return mods_.Find(package_name) != nullptr; }

  void SetEnabled(size_t index, bool enabled);
  void Move(size_t index, int delta);
  void Install(const net::Package& package);

  // Installs that failed, kept until the player dismisses them, wherever they were when it happened.
  const std::vector<net::Thunderstore::Failure>& failures() const { return failures_; }
  void DismissFailures() { failures_.clear(); }

  // Mods load at startup, so a change takes a restart. Only Lua mods can reload in place.
  bool restart_needed() const { return changed_; }
  bool Restart();
  void ReloadScripts();

  // ImTextureID_Invalid while loading (PackageIcon then sets *loading), or when there's no image.
  ImTextureID ModIcon(const ModInfo& mod);
  ImTextureID PackageIcon(const net::Package& package, bool* loading = nullptr);
  void OpenModsFolder() const;

 private:
  // An uploaded icon, freed through Images::destroy when it goes.
  class Texture {
   public:
    Texture(const Images& images, ImTextureID id) : images_(images), id_(id) {}
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    ImTextureID id() const { return id_; }

   private:
    const Images& images_;
    ImTextureID id_;
  };

  std::unique_ptr<Texture> MakeTexture(const std::vector<uint8_t>& encoded);
  void PrunePackageIcons();

  struct CachedIcon {
    std::unique_ptr<Texture> texture;
    uint64_t used = 0;
  };

  ModList mods_;
  std::unique_ptr<net::Thunderstore> thunderstore_;
  net::Query query_;
  Images images_;
  std::unordered_map<std::string, CachedIcon> mod_icons_;
  std::unordered_map<std::string, CachedIcon> package_icons_;
  std::vector<net::Thunderstore::Failure> failures_;
  uint64_t frame_ = 0;
  uint64_t seen_revision_ = 0;  // ModsRevision as of the last scan
  bool changed_ = false;
};

}  // namespace rexfabric::ui
