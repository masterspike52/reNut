// One graph open in the node editor: where it lives, its undo history, and the Lua it compiles to.
//
//   mods/<Mod>/graphs/<Name>.json         the graph
//   mods/<Mod>/scripts/<Name>.graph.lua   its script, run like any other of the mod's scripts
//
// The script is plain Lua, so a mod made in the editor ships and runs as an ordinary Lua mod; the graph is only
// needed to keep editing it.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "graph.h"
#include "lua_compiler.h"

namespace rexfabric::graph {

// The game's namespaces, which the scripts reach as globals and variables can't hide.
std::vector<std::string> GameGlobals();

// The graphs a mod has, by name.
std::vector<std::string> ListGraphs(const std::filesystem::path& mod_dir);
std::filesystem::path GraphFile(const std::filesystem::path& mod_dir, std::string_view name);
std::filesystem::path ScriptFile(const std::filesystem::path& mod_dir, std::string_view name);
// A name a graph, or a mod made in the editor, can have: letters, digits, spaces, _ and -.
bool IsFileName(std::string_view name);
// Removes the graph and its script.
bool DeleteGraph(const std::filesystem::path& mod_dir, std::string_view name, std::string* error);
// Renames the graph and its script.
bool RenameGraph(const std::filesystem::path& mod_dir, std::string_view from, std::string_view to, std::string* error);

class Document {
 public:
  Document(std::filesystem::path mod_dir, std::string name);

  // Reads the graph file. A graph without one starts empty.
  bool Load(std::string* error);
  // Writes the graph and its script. A graph with errors gets a script that says so in the log, rather than
  // running half a mod.
  bool Save(std::string* error);

  Graph& graph() { return graph_; }
  const Graph& graph() const { return graph_; }
  const std::string& name() const { return name_; }
  const std::filesystem::path& mod_dir() const { return mod_dir_; }
  const Compiled& compiled() const { return compiled_; }
  // Differs from the file.
  bool dirty() const { return history_[at_] != saved_; }

  // After an edit: drops links to pins that went away and recompiles. A settled edit is an undo step; text
  // still being typed isn't, so it's one step once typing stops.
  void Edited(bool settled = true);
  bool CanUndo() const { return at_ > 0; }
  bool CanRedo() const { return at_ + 1 < history_.size(); }
  bool Undo();
  bool Redo();

 private:
  void Recompile();
  void Restore(const std::string& snapshot);

  std::filesystem::path mod_dir_;
  std::string name_;
  Graph graph_;
  Compiled compiled_;
  // The graph as written after each settled edit, oldest first; at_ is where the graph is now.
  std::vector<std::string> history_;
  size_t at_ = 0;
  std::string saved_;
};

}  // namespace rexfabric::graph
