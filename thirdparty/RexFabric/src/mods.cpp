#include "mods.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include "log.h"

namespace rexfabric {
namespace {

#if defined(_WIN32)
constexpr std::array<std::string_view, 2> kNativeExtensions = {".asi", ".dll"};
#elif defined(__APPLE__)
constexpr std::array<std::string_view, 2> kNativeExtensions = {".asi", ".dylib"};
#else
constexpr std::array<std::string_view, 2> kNativeExtensions = {".asi", ".so"};
#endif

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

std::vector<std::filesystem::path> FilesWith(const std::filesystem::path& dir, auto&& accept) {
  std::vector<std::filesystem::path> files;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
    if (entry.is_regular_file(error) && accept(Lower(entry.path().extension().string()))) files.push_back(entry.path());
  }
  return files;
}

nlohmann::json ReadJson(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return {};
  return nlohmann::json::parse(in, nullptr, false);
}

std::string JsonString(const nlohmann::json& json, const char* key) {
  const auto it = json.find(key);
  return it != json.end() && it->is_string() ? it->get<std::string>() : std::string();
}

void ReadMetadata(ModInfo& mod) {
  const nlohmann::json manifest = ReadJson(mod.dir / "manifest.json");
  if (manifest.is_object()) {
    mod.name = JsonString(manifest, "name");
    mod.description = JsonString(manifest, "description");
    mod.version = JsonString(manifest, "version_number");
  }
  std::error_code error;
  const std::filesystem::path toml_file = mod.dir / "mod.toml";
  if (std::filesystem::is_regular_file(toml_file, error)) {
    try {
      const toml::table table = toml::parse_file(toml_file.string());
      // A blank entry leaves what the manifest said.
      const auto take = [&](const char* key, std::string& field) {
        if (auto value = table[key].value<std::string>(); value && !value->empty()) field = *value;
      };
      take("name", mod.name);
      take("description", mod.description);
      take("version", mod.version);
      take("author", mod.author);
      take("min_game_version", mod.min_game_version);
      take("language", mod.language);
      // [[dependencies]] tables of mod and version, or plain folder names.
      if (const toml::array* dependencies = table["dependencies"].as_array()) {
        for (const toml::node& entry : *dependencies) {
          if (auto folder = entry.value<std::string>()) {
            mod.dependencies.push_back({*folder, {}});
          } else if (const toml::table* dependency = entry.as_table()) {
            if (auto folder = (*dependency)["mod"].value<std::string>(); folder && !folder->empty()) mod.dependencies.push_back({*folder, (*dependency)["version"].value_or(std::string())});
          }
        }
      }
    } catch (const toml::parse_error& parse_error) {
      // The folder is read often (the menu, a game's content loaders), so a broken file is reported once.
      static std::mutex warned_mutex;
      static std::set<std::filesystem::path> warned;
      std::lock_guard lock(warned_mutex);
      if (warned.insert(toml_file).second) RF_WARN("{}: unreadable mod.toml: {}", mod.folder, parse_error.description());
    }
  }
  if (mod.name.empty()) mod.name = mod.folder;
}

std::vector<std::string> StringList(const nlohmann::json& json, const char* key) {
  std::vector<std::string> names;
  const auto it = json.is_object() ? json.find(key) : json.end();
  if (it == json.end() || !it->is_array()) return names;
  for (const auto& name : *it) {
    if (name.is_string()) names.push_back(name.get<std::string>());
  }
  return names;
}

bool Listed(const std::vector<std::string>& names, std::string_view folder) {
  return std::any_of(names.begin(), names.end(), [&](const std::string& name) { return SameModName(name, folder); });
}

}  // namespace

int CompareVersions(std::string_view a, std::string_view b) {
  const auto split = [](std::string_view version) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (size_t i = 0; i <= version.size(); ++i) {
      if (i == version.size() || version[i] == '.' || version[i] == '-' || version[i] == '_') {
        parts.push_back(version.substr(start, i - start));
        start = i + 1;
      }
    }
    return parts;
  };
  const auto number = [](std::string_view part, uint64_t* value) {
    if (part.empty() || !std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c); })) return false;
    *value = 0;
    for (const char c : part) *value = *value * 10 + static_cast<uint64_t>(c - '0');
    return true;
  };
  const std::vector<std::string_view> left = split(a);
  const std::vector<std::string_view> right = split(b);
  for (size_t i = 0; i < std::max(left.size(), right.size()); ++i) {
    const std::string_view x = i < left.size() ? left[i] : std::string_view("0");
    const std::string_view y = i < right.size() ? right[i] : std::string_view("0");
    uint64_t nx = 0;
    uint64_t ny = 0;
    if (number(x, &nx) && number(y, &ny)) {
      if (nx != ny) return nx < ny ? -1 : 1;
    } else if (const int text = Lower(std::string(x)).compare(Lower(std::string(y))); text != 0) {
      return text < 0 ? -1 : 1;
    }
  }
  return 0;
}

std::vector<ModInfo> LoadableMods(const std::vector<ModInfo>& enabled, std::string_view game_version, std::vector<RefusedMod>* refused) {
  const auto find = [&enabled](std::string_view folder) {
    return std::find_if(enabled.begin(), enabled.end(), [folder](const ModInfo& mod) { return SameModName(mod.folder, folder); });
  };
  std::vector<std::string> reason(enabled.size());
  for (size_t i = 0; i < enabled.size(); ++i) {
    const ModInfo& mod = enabled[i];
    if (!game_version.empty() && !mod.min_game_version.empty() && CompareVersions(game_version, mod.min_game_version) < 0) {
      reason[i] = "needs game version " + mod.min_game_version + " or later, and this is " + std::string(game_version);
      continue;
    }
    for (const ModDependency& dependency : mod.dependencies) {
      const auto found = find(dependency.mod);
      if (found == enabled.end()) {
        reason[i] = "needs the mod " + dependency.mod + ", which isn't installed and enabled";
        break;
      }
      if (!dependency.version.empty() && CompareVersions(found->version, dependency.version) < 0) {
        reason[i] = "needs " + dependency.mod + " " + dependency.version + " or later, and it is " + (found->version.empty() ? std::string("unversioned") : found->version);
        break;
      }
    }
  }
  // A mod whose dependency can't load can't either, however deep.
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 0; i < enabled.size(); ++i) {
      if (!reason[i].empty()) continue;
      for (const ModDependency& dependency : enabled[i].dependencies) {
        const size_t index = static_cast<size_t>(find(dependency.mod) - enabled.begin());
        if (!reason[index].empty()) {
          reason[i] = "needs " + dependency.mod + ", which can't load";
          changed = true;
          break;
        }
      }
    }
  }
  // Each mod after its dependencies, otherwise in load order; what can never be placed is in a circle.
  std::vector<ModInfo> order;
  std::vector<bool> placed(enabled.size());
  for (bool progress = true; progress;) {
    progress = false;
    for (size_t i = 0; i < enabled.size(); ++i) {
      if (placed[i] || !reason[i].empty()) continue;
      const bool ready = std::all_of(enabled[i].dependencies.begin(), enabled[i].dependencies.end(), [&](const ModDependency& dependency) { return placed[static_cast<size_t>(find(dependency.mod) - enabled.begin())]; });
      if (!ready) continue;
      order.push_back(enabled[i]);
      placed[i] = true;
      progress = true;
      break;
    }
  }
  for (size_t i = 0; i < enabled.size(); ++i) {
    if (!placed[i] && reason[i].empty()) reason[i] = "its dependencies go round in a circle";
    if (!reason[i].empty() && refused) refused->push_back({enabled[i].folder, reason[i]});
  }
  return order;
}

bool WriteMetadata(const ModInfo& mod, std::string* error) {
  const std::filesystem::path file = mod.dir / "mod.toml";
  toml::table table;
  std::error_code code;
  if (std::filesystem::is_regular_file(file, code)) {
    try {
      table = toml::parse_file(file.string());
    } catch (const toml::parse_error& parse_error) {
      if (error) *error = "mod.toml can't be read (" + std::string(parse_error.description()) + "), so it's left as it is";
      return false;
    }
  }
  const auto put = [&table](const char* key, const std::string& value) {
    if (value.empty()) table.erase(key);
    else table.insert_or_assign(key, value);
  };
  put("name", mod.name);
  put("description", mod.description);
  put("version", mod.version);
  put("author", mod.author);
  put("min_game_version", mod.min_game_version);
  put("language", mod.language);
  toml::array dependencies;
  for (const ModDependency& dependency : mod.dependencies) {
    toml::table entry{{"mod", dependency.mod}};
    if (!dependency.version.empty()) entry.insert_or_assign("version", dependency.version);
    dependencies.push_back(std::move(entry));
  }
  if (dependencies.empty()) table.erase("dependencies");
  else table.insert_or_assign("dependencies", std::move(dependencies));

  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << table << '\n';
  if (!out && error) *error = "couldn't write " + file.string();
  return static_cast<bool>(out);
}

bool SameModName(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

void ModList::Scan() {
  std::vector<ModInfo> found;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(root_, error)) {
    const std::string folder = entry.path().filename().string();
    // Dot folders are tool state, and a half finished install is not a mod yet.
    if (!entry.is_directory(error) || folder.starts_with(".") || folder.ends_with(".installing")) continue;
    ModInfo mod{folder, entry.path()};
    ReadMetadata(mod);
    found.push_back(std::move(mod));
  }

  const nlohmann::json order = ReadJson(root_ / kOrderFile);
  const std::vector<std::string> load_order = StringList(order, "load_order");
  const std::vector<std::string> disabled = StringList(order, "disabled");

  mods_.clear();
  for (const std::string& name : load_order) {
    auto it = std::find_if(found.begin(), found.end(), [&](const ModInfo& mod) { return SameModName(mod.folder, name); });
    if (it == found.end() || Listed(disabled, it->folder)) continue;
    mods_.push_back(std::move(*it));
    found.erase(it);
  }
  // What the file doesn't place: new folders join the load order by name, the rest stay off.
  std::sort(found.begin(), found.end(), [](const ModInfo& a, const ModInfo& b) { return Lower(a.folder) < Lower(b.folder); });
  new_.clear();
  std::vector<ModInfo> off;
  for (ModInfo& mod : found) {
    if (!Listed(disabled, mod.folder)) new_.push_back(mod.folder);
    mod.enabled = !Listed(disabled, mod.folder);
    (mod.enabled ? mods_ : off).push_back(std::move(mod));
  }
  for (ModInfo& mod : off) mods_.push_back(std::move(mod));
  SortDisabled();
}

bool ModList::Save() const {
  nlohmann::json order = {{"load_order", nlohmann::json::array()}, {"disabled", nlohmann::json::array()}};
  for (const ModInfo& mod : mods_) order[mod.enabled ? "load_order" : "disabled"].push_back(mod.folder);
  std::error_code error;
  std::filesystem::create_directories(root_, error);
  std::ofstream out(root_ / kOrderFile, std::ios::binary | std::ios::trunc);
  out << order.dump(2) << '\n';
  if (!out) RF_ERROR("can't write {}", (root_ / kOrderFile).string());
  return static_cast<bool>(out);
}

bool ModList::New(std::string_view folder) const {
  return std::any_of(new_.begin(), new_.end(), [&](const std::string& name) { return SameModName(name, folder); });
}

size_t ModList::EnabledCount() const { return static_cast<size_t>(std::count_if(mods_.begin(), mods_.end(), [](const ModInfo& mod) { return mod.enabled; })); }

const ModInfo* ModList::Find(std::string_view folder) const {
  auto it = std::find_if(mods_.begin(), mods_.end(), [&](const ModInfo& mod) { return SameModName(mod.folder, folder); });
  return it != mods_.end() ? &*it : nullptr;
}

void ModList::SetEnabled(size_t index, bool enabled) {
  if (index >= mods_.size() || mods_[index].enabled == enabled) return;
  ModInfo mod = std::move(mods_[index]);
  mods_.erase(mods_.begin() + static_cast<ptrdiff_t>(index));
  mod.enabled = enabled;
  mods_.insert(mods_.begin() + static_cast<ptrdiff_t>(EnabledCount()), std::move(mod));
  SortDisabled();
  Save();
}

void ModList::Move(size_t index, int delta) {
  const size_t enabled = EnabledCount();
  const ptrdiff_t target = static_cast<ptrdiff_t>(index) + delta;
  if (index >= enabled || target < 0 || target >= static_cast<ptrdiff_t>(enabled) || delta == 0) return;
  // Shifts the ones between along, so they keep their order.
  const auto from = mods_.begin() + static_cast<ptrdiff_t>(index);
  const auto to = mods_.begin() + target;
  if (delta > 0) std::rotate(from, from + 1, to + 1);
  else std::rotate(to, from, from + 1);
  Save();
}

void ModList::SetOrder(const std::vector<std::string>& enabled) {
  std::vector<ModInfo> ordered;
  for (const std::string& name : enabled) {
    const auto it = std::find_if(mods_.begin(), mods_.end(), [&](const ModInfo& mod) { return SameModName(mod.folder, name); });
    if (it == mods_.end()) continue;
    it->enabled = true;
    ordered.push_back(std::move(*it));
    mods_.erase(it);
  }
  for (ModInfo& mod : mods_) mod.enabled = false;
  ordered.insert(ordered.end(), std::make_move_iterator(mods_.begin()), std::make_move_iterator(mods_.end()));
  mods_ = std::move(ordered);
  SortDisabled();
  Save();
}

bool ModList::Rename(size_t index, const std::string& folder, std::string* error) {
  if (index >= mods_.size()) return false;
  ModInfo& mod = mods_[index];
  const std::filesystem::path to = root_ / folder;
  std::error_code fs_error;
  if (std::filesystem::exists(to, fs_error) && !std::filesystem::equivalent(to, mod.dir, fs_error)) {
    if (error) *error = "A mod named " + folder + " exists already";
    return false;
  }
  std::filesystem::rename(mod.dir, to, fs_error);
  if (fs_error) {
    if (error) *error = "Could not rename the folder: " + fs_error.message();
    return false;
  }
  if (mod.name == mod.folder) mod.name = folder;
  mod.folder = folder;
  mod.dir = to;
  return Save();
}

void ModList::SortDisabled() {
  const auto first_disabled = mods_.begin() + static_cast<ptrdiff_t>(EnabledCount());
  std::sort(first_disabled, mods_.end(), [](const ModInfo& a, const ModInfo& b) { return Lower(a.name) < Lower(b.name); });
}

std::vector<std::filesystem::path> NativePlugins(const std::filesystem::path& dir) {
  const auto native = [](const std::string& extension) { return std::find(kNativeExtensions.begin(), kNativeExtensions.end(), extension) != kNativeExtensions.end(); };
  std::vector<std::filesystem::path> plugins = FilesWith(dir, native);
  std::vector<std::filesystem::path> nested = FilesWith(dir / "plugins", native);
  plugins.insert(plugins.end(), nested.begin(), nested.end());
  std::sort(plugins.begin(), plugins.end());
  return plugins;
}

std::vector<std::filesystem::path> LuaScripts(const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> scripts = FilesWith(dir / "scripts", [](const std::string& extension) { return extension == ".lua"; });
  std::sort(scripts.begin(), scripts.end());
  return scripts;
}

}  // namespace rexfabric
