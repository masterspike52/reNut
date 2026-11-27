#include "localisation.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <set>

#include <rexfabric/content.h>

#include "assets.h"

namespace rexfabric::content {
namespace {

// How deep texts are looked for in structs inside structs.
constexpr int kMaxDepth = 16;

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

struct Registry {
  std::mutex mutex;
  std::vector<Language> languages;
  std::function<std::string()> current;
  std::map<std::string, ModInfo> mods;                                  // the loaded ones, by lower case folder
  std::map<std::string, std::map<std::string, StringTable>> tables;     // language, then lower case folder
};

Registry& Get() {
  static auto* registry = new Registry();
  return *registry;
}

std::filesystem::path TableFile(const ModInfo& mod, std::string_view language) {
  std::filesystem::path file = mod.dir / kLocalisationFolder / std::filesystem::path(language);
  file += ".json";
  return file;
}

// The asset a key is of: what comes before its colon.
std::string_view AssetOf(std::string_view key) { return key.substr(0, key.find(':')); }

bool Within(std::string_view path, std::string_view base) {
  const std::string lower = Lower(path);
  const std::string prefix = Lower(base);
  return lower == prefix || (lower.size() > prefix.size() && lower.starts_with(prefix) && lower[prefix.size()] == '/');
}

void Walk(const TypeLookup& types, const ValueType& type, const nlohmann::json& value, const std::string& key, const std::function<void(const std::string&, const std::string&)>& visit, int depth);

void WalkSingle(const TypeLookup& types, const ValueType& type, const nlohmann::json& value, const std::string& key, const std::function<void(const std::string&, const std::string&)>& visit, int depth) {
  if (type.type == PropertyType::kText && value.is_string()) {
    visit(key, value.get<std::string>());
    return;
  }
  if (type.type != PropertyType::kStruct || !value.is_object() || depth >= kMaxDepth || !types.find_struct) return;
  const std::optional<Struct> found = types.find_struct(type.of);
  if (!found) return;
  for (const Property& field : found->fields) {
    const auto held = value.find(field.name);
    // A field the value lacks, or has as the struct has it, is the struct's default, which is the struct's own text.
    if (held != value.end() && *held != field.value) Walk(types, field.type, *held, FieldKey(key, field.name), visit, depth + 1);
    else Walk(types, field.type, field.value, TextKey(found->path, field.name), visit, depth + 1);
  }
}

void Walk(const TypeLookup& types, const ValueType& type, const nlohmann::json& value, const std::string& key, const std::function<void(const std::string&, const std::string&)>& visit, int depth) {
  switch (type.container) {
    case Container::kSingle: WalkSingle(types, type, value, key, visit, depth); break;
    case Container::kArray:
      if (!value.is_array()) return;
      for (size_t i = 0; i < value.size(); ++i) WalkSingle(types, ElementType(type), value[i], ElementKey(key, i), visit, depth);
      break;
    case Container::kMap:
      if (!value.is_array()) return;
      for (const nlohmann::json& pair : value) {
        if (pair.is_array() && pair.size() == 2) WalkSingle(types, ElementType(type), pair[1], EntryKey(key, pair[0]), visit, depth);
      }
      break;
  }
}

}  // namespace

void DeclareLanguages(std::vector<LanguageDecl> languages, std::function<std::string()> current) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  registry.languages.clear();
  for (LanguageDecl& language : languages) registry.languages.push_back({std::move(language.code), std::move(language.name)});
  registry.current = std::move(current);
}

std::vector<Language> Languages() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  return registry.languages;
}

std::string CurrentLanguage() {
  std::function<std::string()> current;
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    current = registry.current;
  }
  return current ? current() : std::string();
}

std::string LanguageName(std::string_view code) {
  for (const Language& language : Languages()) {
    if (language.code == code) return language.name;
  }
  return std::string(code);
}

std::string OwnLanguage(const ModInfo& mod) {
  if (!mod.language.empty()) return mod.language;
  const std::vector<Language> languages = Languages();
  return languages.empty() ? std::string() : languages.front().code;
}

std::string TextKey(std::string_view asset, std::string_view where) { return std::string(asset) + ":" + std::string(where); }

std::string FieldKey(const std::string& key, std::string_view field) { return key + "." + std::string(field); }

std::string ElementKey(const std::string& key, size_t index) { return key + "[" + std::to_string(index + 1) + "]"; }

std::string EntryKey(const std::string& key, const nlohmann::json& map_key) { return key + "[" + (map_key.is_string() ? map_key.get<std::string>() : map_key.dump()) + "]"; }

void WalkTexts(const TypeLookup& types, const ValueType& type, const nlohmann::json& value, const std::string& key, const std::function<void(const std::string& key, const std::string& text)>& visit) { Walk(types, type, value, key, visit, 0); }

std::vector<TextEntry> ModTexts(const ModList& mods, const ModInfo& mod) {
  const TypeLookup types = FileTypes(mods);
  std::vector<TextEntry> texts;
  std::set<std::string> seen;
  // Only the asset's own texts: a struct's default met in a value is the struct's, listed with it.
  const auto collect = [&](const std::string& asset) {
    return [&texts, &seen, asset](const std::string& key, const std::string& text) {
      if (AssetOf(key) != asset || !seen.insert(key).second) return;
      texts.push_back({key, asset, key.substr(asset.size() + 1), text});
    };
  };
  for (const AssetInfo& info : ScanContent(mod).assets) {
    switch (info.kind) {
      case AssetKind::kClass:
        if (std::optional<ClassAsset> asset = ReadClassAsset(info.file, nullptr)) {
          for (const Property& property : asset->properties) WalkTexts(types, property.type, property.value, TextKey(info.path, property.name), collect(info.path));
          const std::vector<Property> inherited = InheritedProperties(mods, asset->parent);
          for (const auto& [name, value] : asset->defaults.items()) {
            const auto found = std::find_if(inherited.begin(), inherited.end(), [&name](const Property& property) { return Lower(property.name) == Lower(name); });
            if (found != inherited.end()) WalkTexts(types, found->type, value, TextKey(info.path, found->name), collect(info.path));
          }
        }
        break;
      case AssetKind::kStruct:
        if (std::optional<StructAsset> asset = ReadStructAsset(info.file, nullptr)) {
          for (const Property& field : asset->fields) WalkTexts(types, field.type, field.value, TextKey(info.path, field.name), collect(info.path));
        }
        break;
      case AssetKind::kTable:
        if (std::optional<TableAsset> asset = ReadTableAsset(info.file, nullptr)) {
          for (const auto& [name, value] : asset->rows) WalkTexts(types, {.type = PropertyType::kStruct, .of = asset->row}, value, TextKey(info.path, name), collect(info.path));
        }
        break;
      case AssetKind::kEnum:
        if (std::optional<EnumAsset> asset = ReadEnumAsset(info.file, nullptr)) {
          for (const EnumValue& value : asset->values) collect(info.path)(TextKey(info.path, value.name), value.display.empty() ? value.name : value.display);
        }
        break;
      case AssetKind::kPacket:
        if (std::optional<PacketAsset> asset = ReadPacketAsset(info.file, nullptr)) {
          collect(info.path)(TextKey(info.path, "name"), asset->name);
          if (!asset->help.empty()) collect(info.path)(TextKey(info.path, "help"), asset->help);
        }
        break;
      case AssetKind::kLanguage:
      case AssetKind::kTexture:
      case AssetKind::kMapEdit:
      case AssetKind::kMaterial:
      case AssetKind::kMesh:
      case AssetKind::kWorld:
      case AssetKind::kOther: break;
    }
  }
  return texts;
}

StringTable ReadStringTable(const ModInfo& mod, std::string_view language) {
  StringTable table;
  std::ifstream in(TableFile(mod, language), std::ios::binary);
  if (!in) return table;
  const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
  const nlohmann::json texts = json.is_object() ? json.value("texts", nlohmann::json::object()) : nlohmann::json::object();
  if (!texts.is_object()) return table;
  for (const auto& [key, entry] : texts.items()) {
    if (entry.is_object()) table[key] = {entry.value("source", std::string()), entry.value("text", std::string())};
  }
  return table;
}

bool WriteStringTable(const ModInfo& mod, std::string_view language, const StringTable& table, std::string* error) {
  nlohmann::ordered_json json;
  json["language"] = language;
  nlohmann::ordered_json texts = nlohmann::ordered_json::object();
  for (const auto& [key, translation] : table) {
    if (!translation.text.empty()) texts[key] = {{"source", translation.source}, {"text", translation.text}};
  }
  json["texts"] = std::move(texts);
  const std::filesystem::path file = TableFile(mod, language);
  std::error_code code;
  std::filesystem::create_directories(file.parent_path(), code);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << json.dump(2) << "\n";
  if (!out && error) *error = "couldn't write " + file.string();
  return static_cast<bool>(out);
}

void RenameTextKeys(const ModInfo& mod, std::string_view from, std::string_view to) {
  std::error_code code;
  for (const auto& entry : std::filesystem::directory_iterator(mod.dir / kLocalisationFolder, code)) {
    if (entry.path().extension() != ".json") continue;
    const std::string language = entry.path().stem().string();
    StringTable table = ReadStringTable(mod, language);
    StringTable renamed;
    bool changed = false;
    for (auto& [key, translation] : table) {
      if (Within(AssetOf(key), from)) {
        renamed[std::string(to) + key.substr(from.size())] = std::move(translation);
        changed = true;
      } else {
        renamed[key] = std::move(translation);
      }
    }
    if (changed) WriteStringTable(mod, language, renamed, nullptr);
  }
}

void LoadTranslations(const std::vector<ModInfo>& mods) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  registry.mods.clear();
  registry.tables.clear();
  for (const ModInfo& mod : mods) registry.mods[Lower(mod.folder)] = mod;
}

std::string Translate(std::string_view key, std::string_view source) {
  const std::string language = CurrentLanguage();
  if (language.empty()) return std::string(source);
  Registry& registry = Get();
  const std::string mod_key = Lower(ModOf(key));
  std::lock_guard lock(registry.mutex);
  const auto mod = registry.mods.find(mod_key);
  if (mod == registry.mods.end()) return std::string(source);
  // OwnLanguage reads the languages, which this lock guards: worked out here instead.
  const std::string own = !mod->second.language.empty() ? mod->second.language : registry.languages.empty() ? std::string() : registry.languages.front().code;
  if (own == language) return std::string(source);
  std::map<std::string, StringTable>& tables = registry.tables[language];
  auto table = tables.find(mod_key);
  if (table == tables.end()) table = tables.emplace(mod_key, ReadStringTable(mod->second, language)).first;
  const auto found = table->second.find(std::string(key));
  return found != table->second.end() && !found->second.text.empty() ? found->second.text : std::string(source);
}

}  // namespace rexfabric::content
