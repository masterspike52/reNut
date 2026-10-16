#pragma once
// The game folder as the guest sees it with mods applied: the same tree as
// the SDK's game device, except that files a mod changes or adds are served
// from the mod cache. The game opens everything through "GAME:" (bundles are
// GAME:bundle\<type>\<id>, assetDbBundleLoadBundle 0x822BDBD0), so the
// loader points that link here.
//
// Files are the SDK's own HostPathEntry (reads, mapping and the read-only
// checks behave as on the stock device); directories are our entries, so
// the tree can hold files the original folder does not have.

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>

#include <rex/filesystem/devices/host_path_device.h>

namespace renut::mods {

class ModDirEntry;

class ModGameDevice final : public rex::filesystem::HostPathDevice {
public:
    struct Redirect {
        std::string path;           // relative to the game root, '/' separated, as the mod names it
        std::filesystem::path host; // the file served instead
    };

    // redirects: keyed by the lower-case path.
    ModGameDevice(std::string_view mount_path, const std::filesystem::path &game_root,
                  std::map<std::string, Redirect> redirects);
    ~ModGameDevice() override;

    bool Initialize() override;
    void Dump(rex::string::StringBuffer *string_buffer) override;
    rex::filesystem::Entry *ResolvePath(std::string_view path) override;

private:
    void Populate(ModDirEntry *dir, const std::filesystem::path &host_dir, const std::string &key);
    void AddNewFile(const std::string &key, const Redirect &redirect);

    std::filesystem::path game_root_;
    std::map<std::string, Redirect> redirects_;
    std::set<std::string> placed_;
    std::unique_ptr<ModDirEntry> root_;
};

// Lower-case ASCII, '/' separators: the key a game-relative path is matched
// by (the VFS folds names the same way).
std::string PathKey(std::string_view path);

} // namespace renut::mods
