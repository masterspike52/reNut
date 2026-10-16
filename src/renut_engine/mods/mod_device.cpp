#include "renut_engine/mods/mod_device.h"

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/file.h>
#include <rex/string.h>
#include <rex/string/utf8.h>

#include "renut_engine/app/renut_logging.h"

namespace renut::mods {

using rex::X_STATUS;
using rex::filesystem::Entry;
using rex::filesystem::File;
using rex::filesystem::FileInfo;
using rex::filesystem::HostPathEntry;

// A directory handle: the guest lists a directory through its entry's
// children, so the handle itself carries nothing.
class ModDirFile final : public File {
public:
    ModDirFile(uint32_t file_access, Entry *entry) : File(file_access, entry) {}

    void Destroy() override { delete this; }
    X_STATUS ReadSync(std::span<uint8_t>, size_t, size_t *) override { return X_STATUS_FILE_IS_A_DIRECTORY; }
    X_STATUS WriteSync(std::span<const uint8_t>, size_t, size_t *) override { return X_STATUS_ACCESS_DENIED; }
};

class ModDirEntry final : public Entry {
public:
    ModDirEntry(rex::filesystem::Device *device, Entry *parent, std::string_view path, const FileInfo *info)
        : Entry(device, parent, path) {
        attributes_ = rex::filesystem::kFileAttributeDirectory;
        if (info) {
            create_timestamp_ = info->create_timestamp;
            access_timestamp_ = info->access_timestamp;
            write_timestamp_ = info->write_timestamp;
        }
    }

    X_STATUS Open(uint32_t desired_access, File **out_file) override {
        *out_file = new ModDirFile(desired_access, this);
        return X_STATUS_SUCCESS;
    }

    void Add(std::unique_ptr<Entry> child) { children_.push_back(std::move(child)); }
};

std::string PathKey(std::string_view path) {
    std::string key;
    key.reserve(path.size());
    for (char c : path) {
        if (c == '\\')
            c = '/';
        else if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
        if (c == '/' && (key.empty() || key.back() == '/'))
            continue;
        key += c;
    }
    while (!key.empty() && key.back() == '/')
        key.pop_back();
    return key;
}

ModGameDevice::ModGameDevice(std::string_view mount_path, const std::filesystem::path &game_root,
                             std::map<std::string, Redirect> redirects)
    : HostPathDevice(mount_path, game_root, true), game_root_(game_root), redirects_(std::move(redirects)) {}

ModGameDevice::~ModGameDevice() = default;

bool ModGameDevice::Initialize() {
    if (!std::filesystem::is_directory(game_root_))
        return false;
    root_ = std::make_unique<ModDirEntry>(this, nullptr, "", nullptr);
    Populate(root_.get(), game_root_, "");
    for (const auto &[key, redirect] : redirects_)
        if (!placed_.contains(key))
            AddNewFile(key, redirect);
    return true;
}

void ModGameDevice::Populate(ModDirEntry *dir, const std::filesystem::path &host_dir, const std::string &key) {
    for (const FileInfo &info : rex::filesystem::ListFiles(host_dir)) {
        const std::string name = rex::path_to_utf8(info.name);
        const std::string child_key = key.empty() ? PathKey(name) : key + "/" + PathKey(name);
        const std::string guest_path = rex::string::utf8_join_guest_paths(dir->path(), name);
        if (info.type == FileInfo::Type::kDirectory) {
            auto child = std::make_unique<ModDirEntry>(this, dir, guest_path, &info);
            Populate(child.get(), host_dir / info.name, child_key);
            dir->Add(std::move(child));
            continue;
        }
        auto it = redirects_.find(child_key);
        if (it != redirects_.end()) {
            FileInfo modded;
            if (rex::filesystem::GetInfo(it->second.host, &modded)) {
                modded.name = info.name;  // the guest still sees the original name
                dir->Add(std::unique_ptr<Entry>(HostPathEntry::Create(this, dir, it->second.host, modded)));
                placed_.insert(child_key);
                continue;
            }
            RNUT_ERROR("mods: {} is missing; serving the original {}", rex::path_to_utf8(it->second.host), it->second.path);
        }
        dir->Add(std::unique_ptr<Entry>(HostPathEntry::Create(this, dir, host_dir / info.name, info)));
    }
}

// A file a mod adds: its folders are created in the tree as needed.
void ModGameDevice::AddNewFile(const std::string &key, const Redirect &redirect) {
    auto parts = rex::string::utf8_split_path(redirect.path);
    if (parts.empty())
        return;
    ModDirEntry *dir = root_.get();
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        Entry *child = dir->GetChild(parts[i]);
        if (!child) {
            auto made = std::make_unique<ModDirEntry>(this, dir, rex::string::utf8_join_guest_paths(dir->path(), parts[i]), nullptr);
            child = made.get();
            dir->Add(std::move(made));
        }
        dir = dynamic_cast<ModDirEntry *>(child);
        if (!dir) {
            RNUT_ERROR("mods: cannot add {}: {} is a file in the game folder", redirect.path, parts[i]);
            return;
        }
    }
    FileInfo info;
    if (!rex::filesystem::GetInfo(redirect.host, &info)) {
        RNUT_ERROR("mods: {} is missing; {} is not added", rex::path_to_utf8(redirect.host), redirect.path);
        return;
    }
    info.name = rex::to_path(parts.back());
    dir->Add(std::unique_ptr<Entry>(HostPathEntry::Create(this, dir, redirect.host, info)));
    placed_.insert(key);
}

void ModGameDevice::Dump(rex::string::StringBuffer *string_buffer) {
    auto global_lock = global_critical_region_.Acquire();
    root_->Dump(string_buffer, 0);
}

Entry *ModGameDevice::ResolvePath(std::string_view path) {
    return root_ ? root_->ResolvePath(path) : nullptr;
}

} // namespace renut::mods
