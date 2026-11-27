// The data tables RexFabric knows (docs/content_and_classes.md, Data tables): the game's, made from its data when
// they're read (rexfabric/content.h), and the mods' table assets. Mods' tables load at Start and on a reload, after the
// structs their rows are; everything here may be called from any thread.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../mods.h"

namespace rexfabric::content {

struct Table {
  std::string path;
  std::string mod;  // empty for the game's
  std::string help;
  std::string row;  // the struct of its rows
  bool game = false;
  // Each row's value fitted to the struct, every field there.
  std::vector<std::pair<std::string, nlohmann::json>> rows;
  // Goes up each time the rows change, for an editor showing them.
  uint64_t version = 0;
};

// Loads every table asset of these mods (the ones that can load, in load order), after dropping the ones loaded
// before. A row that doesn't fit the struct, or has a name that isn't valid or is there twice, is logged and left out.
void LoadModTables(const std::vector<ModInfo>& mods);

// A table, a game table's rows made first when they're out of date and the game can make them now; null for none.
std::optional<Table> FindTable(std::string_view path);
// Every table the mod `from` may use (MayReference): the game's, then the mods' in load order.
std::vector<std::string> TablePaths(std::string_view from);
// The game's tables, without making their rows.
std::vector<std::string> GameTablePaths();
// One row, as the table's struct, or null for a row it doesn't have; every row's name, in order; whether it has one.
std::optional<nlohmann::json> TableRow(std::string_view path, std::string_view name);
std::vector<std::string> RowNames(std::string_view path);
bool HasRow(std::string_view path, std::string_view name);
// The struct of a table's rows, empty for no table; and its version (Table::version), 0 for none.
std::string RowStruct(std::string_view path);
uint64_t TableVersion(std::string_view path);

}  // namespace rexfabric::content
