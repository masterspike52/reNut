#include "tables.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include <rexfabric/content.h>

#include "../log.h"
#include "assets.h"
#include "classes.h"
#include "types.h"

namespace rexfabric::content {
namespace {

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

bool Same(std::string_view a, std::string_view b) { return Lower(a) == Lower(b); }

ValueType RowType(const std::string& row) { return {.type = PropertyType::kStruct, .of = row}; }

struct Entry {
  Table table;
  // A game table's: what makes its rows, how many times it was asked to (TableChanged), and for which of those its
  // rows were last made. Its rows are out of date while the two differ.
  std::function<bool(TableRows&)> fill;
  uint64_t changes = 1;
  uint64_t made_for = 0;
  // Being made by some thread now, so a read from inside the making doesn't make it again.
  std::shared_ptr<std::atomic<bool>> filling = std::make_shared<std::atomic<bool>>(false);
};

struct Registry {
  std::shared_mutex mutex;
  std::vector<Entry> entries;  // the game's, then the mods'
  size_t declared = 0;

  Entry* Find(std::string_view path) {
    const auto found = std::find_if(entries.begin(), entries.end(), [&path](const Entry& entry) { return Same(entry.table.path, path); });
    return found != entries.end() ? &*found : nullptr;
  }
};

Registry& Get() {
  static auto* registry = new Registry();
  return *registry;
}

// A game table's rows, as its `rows` gives them: each field set by a dotted path into the row's object.
class RowBuilder final : public TableRows {
 public:
  void Row(std::string name) override { rows_.emplace_back(std::move(name), nlohmann::json::object()); }

  void Set(std::string_view field, PropertyValue value) override {
    if (nlohmann::json* at = Field(field)) *at = ToJson(value);
  }

  void Add(std::string_view field, PropertyValue value) override {
    nlohmann::json* at = Field(field);
    if (!at) return;
    if (!at->is_array()) *at = nlohmann::json::array();
    at->push_back(ToJson(value));
  }

  void Put(std::string_view field, PropertyValue key, PropertyValue value, std::string_view inner) override {
    nlohmann::json* at = Field(field);
    if (!at) return;
    if (!at->is_array()) *at = nlohmann::json::array();
    const nlohmann::json held_key = ToJson(key);
    auto entry = std::find_if(at->begin(), at->end(), [&held_key](const nlohmann::json& pair) { return pair.is_array() && pair.size() == 2 && pair[0] == held_key; });
    if (entry == at->end()) {
      at->push_back(nlohmann::json::array({held_key, inner.empty() ? nlohmann::json() : nlohmann::json::object()}));
      entry = at->end() - 1;
    }
    nlohmann::json& held = (*entry)[1];
    if (inner.empty()) {
      held = ToJson(value);
      return;
    }
    if (!held.is_object()) held = nlohmann::json::object();
    held[std::string(inner)] = ToJson(value);
  }

  std::vector<std::pair<std::string, nlohmann::json>> Take() { return std::move(rows_); }

 private:
  // The value a dotted path names in the row being made, made on the way; null before any row.
  nlohmann::json* Field(std::string_view path) {
    if (rows_.empty()) return nullptr;
    nlohmann::json* at = &rows_.back().second;
    size_t start = 0;
    for (size_t dot = 0; dot != std::string_view::npos; start = dot + 1) {
      dot = path.find('.', start);
      if (!at->is_object()) *at = nlohmann::json::object();
      at = &(*at)[std::string(path.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start))];
    }
    return at;
  }

  std::vector<std::pair<std::string, nlohmann::json>> rows_;
};

// Rows checked against the struct: a valid name, not there before, a value that fits (filled to every field) and
// names only what `may_use` takes. The rest are logged with `who` and left out.
std::vector<std::pair<std::string, nlohmann::json>> CheckRows(std::vector<std::pair<std::string, nlohmann::json>> given, const std::string& path, const std::string& row, const std::string& who, const std::function<bool(std::string_view, std::string*)>& may_use) {
  std::vector<std::pair<std::string, nlohmann::json>> rows;
  std::string why;
  for (auto& [name, value] : given) {
    if (!ValidName(name, &why)) {
      RF_WARN("content: [{}] {}: row {}: {}", who, path, name, why);
      continue;
    }
    if (std::any_of(rows.begin(), rows.end(), [&name](const auto& kept) { return Same(kept.first, name); })) {
      RF_WARN("content: [{}] {}: row {} is there twice", who, path, name);
      continue;
    }
    nlohmann::json fitted;
    if (!FitValue(LoadedTypes(), RowType(row), value, &fitted, &why)) {
      RF_WARN("content: [{}] {}: row {}: {}", who, path, name, why);
      continue;
    }
    bool allowed = true;
    WalkValues(LoadedTypes(), RowType(row), fitted, [&](const ValueType& single, nlohmann::json& held) {
      if (allowed && single.type == PropertyType::kAsset && held.is_string()) allowed = may_use(held.get<std::string>(), &why);
    });
    if (!allowed) {
      RF_WARN("content: [{}] {}: row {}: {}", who, path, name, why);
      continue;
    }
    rows.emplace_back(std::move(name), std::move(fitted));
  }
  return rows;
}

// Makes a game table's rows when they're out of date (or always, with `force`), outside the lock, as `fill` reads the
// game. False when it's out of date and couldn't be made now.
bool Ensure(std::string_view path, bool force) {
  Registry& registry = Get();
  std::function<bool(TableRows&)> fill;
  std::shared_ptr<std::atomic<bool>> filling;
  uint64_t changes = 0;
  std::string table_path;
  std::string row;
  {
    std::shared_lock lock(registry.mutex);
    Entry* entry = registry.Find(path);
    if (!entry || !entry->fill) return entry != nullptr;
    if (!force && entry->made_for == entry->changes) return true;
    fill = entry->fill;
    filling = entry->filling;
    changes = entry->changes;
    table_path = entry->table.path;
    row = entry->table.row;
  }
  if (filling->exchange(true)) return false;
  RowBuilder builder;
  const bool made = fill(builder);
  filling->store(false);
  if (!made) return false;
  const auto anything = [](std::string_view, std::string*) { return true; };
  std::vector<std::pair<std::string, nlohmann::json>> rows = CheckRows(builder.Take(), table_path, row, "the game", anything);
  std::unique_lock lock(registry.mutex);
  Entry* entry = registry.Find(path);
  if (!entry) return false;
  entry->table.rows = std::move(rows);
  ++entry->table.version;
  entry->made_for = changes;
  return true;
}

bool ValidPath(std::string_view path, std::string* why) {
  size_t start = 0;
  for (size_t slash = 0; slash != std::string_view::npos; start = slash + 1) {
    slash = path.find('/', start);
    if (!ValidName(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start), why)) return false;
  }
  return true;
}

}  // namespace

bool DeclareTable(TableDecl decl) {
  std::string why;
  if (!ValidPath(decl.path, &why)) {
    RF_ERROR("content: the game's table {}: {}", decl.path, why);
    return false;
  }
  if (!FindStruct(decl.row)) {
    RF_ERROR("content: the game's table {} has rows of {}, which isn't a struct", decl.path, decl.row);
    return false;
  }
  if (!decl.rows) {
    RF_ERROR("content: the game's table {} has nothing making its rows", decl.path);
    return false;
  }
  if (FindClass(decl.path) || FindStruct(decl.path)) {
    RF_ERROR("content: the game's table {} takes a path that's taken", decl.path);
    return false;
  }
  Registry& registry = Get();
  std::unique_lock lock(registry.mutex);
  if (registry.Find(decl.path)) {
    RF_ERROR("content: the game's table {} is declared twice", decl.path);
    return false;
  }
  Entry entry{.table = {.path = std::move(decl.path), .help = std::move(decl.help), .row = std::move(decl.row), .game = true}, .fill = std::move(decl.rows)};
  registry.entries.insert(registry.entries.begin() + static_cast<ptrdiff_t>(registry.declared), std::move(entry));
  ++registry.declared;
  return true;
}

void TableChanged(std::string_view path) {
  Registry& registry = Get();
  std::unique_lock lock(registry.mutex);
  if (Entry* entry = registry.Find(path)) ++entry->changes;
}

bool FillTable(std::string_view path) { return Ensure(path, true); }

void LoadModTables(const std::vector<ModInfo>& mods) {
  Registry& registry = Get();
  std::unique_lock lock(registry.mutex);
  registry.entries.resize(registry.declared);
  std::string why;
  for (const ModInfo& mod : mods) {
    if (!ValidName(mod.folder)) continue;  // the classes loader says why
    const auto may_use = [&mod](std::string_view path, std::string* reason) { return MayReference(mod.folder, path, reason); };
    for (const AssetInfo& asset : ScanContent(mod).assets) {
      if (asset.kind != AssetKind::kTable) continue;
      std::optional<TableAsset> read = ReadTableAsset(asset.file, &why);
      if (!read) {
        RF_WARN("content: [{}] {}: {}", mod.folder, asset.path, why);
        continue;
      }
      if (!FindStruct(read->row)) {
        RF_WARN("content: [{}] {}: its rows are of {}, which isn't a struct", mod.folder, asset.path, read->row);
        continue;
      }
      if (!may_use(read->row, &why)) {
        RF_WARN("content: [{}] {}: {}", mod.folder, asset.path, why);
        continue;
      }
      Table table{.path = asset.path, .mod = mod.folder, .help = read->help, .row = read->row, .rows = CheckRows(std::move(read->rows), asset.path, read->row, mod.folder, may_use), .version = 1};
      registry.entries.push_back({.table = std::move(table)});
    }
  }
  RF_INFO("content: {} tables from mods", registry.entries.size() - registry.declared);
}

std::optional<Table> FindTable(std::string_view path) {
  Ensure(path, false);
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  const Entry* entry = registry.Find(path);
  return entry ? std::optional<Table>(entry->table) : std::nullopt;
}

std::vector<std::string> TablePaths(std::string_view from) {
  std::vector<std::string> paths;
  {
    Registry& registry = Get();
    std::shared_lock lock(registry.mutex);
    for (const Entry& entry : registry.entries) paths.push_back(entry.table.path);
  }
  std::erase_if(paths, [&from](const std::string& path) { return !MayReference(from, path, nullptr); });
  return paths;
}

std::vector<std::string> GameTablePaths() {
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  std::vector<std::string> paths;
  for (size_t i = 0; i < registry.declared; ++i) paths.push_back(registry.entries[i].table.path);
  return paths;
}

std::optional<nlohmann::json> TableRow(std::string_view path, std::string_view name) {
  Ensure(path, false);
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  const Entry* entry = registry.Find(path);
  if (!entry) return std::nullopt;
  const auto row = std::find_if(entry->table.rows.begin(), entry->table.rows.end(), [&name](const auto& held) { return held.first == name; });
  return row != entry->table.rows.end() ? std::optional<nlohmann::json>(row->second) : std::nullopt;
}

std::vector<std::string> RowNames(std::string_view path) {
  Ensure(path, false);
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  std::vector<std::string> names;
  if (const Entry* entry = registry.Find(path)) {
    for (const auto& [name, value] : entry->table.rows) names.push_back(name);
  }
  return names;
}

bool HasRow(std::string_view path, std::string_view name) { return TableRow(path, name).has_value(); }

std::string RowStruct(std::string_view path) {
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  const Entry* entry = registry.Find(path);
  return entry ? entry->table.row : std::string();
}

uint64_t TableVersion(std::string_view path) {
  Ensure(path, false);
  Registry& registry = Get();
  std::shared_lock lock(registry.mutex);
  const Entry* entry = registry.Find(path);
  return entry ? entry->table.version : 0;
}

}  // namespace rexfabric::content
