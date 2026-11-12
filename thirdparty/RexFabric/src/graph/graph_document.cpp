#include "graph_document.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <rexfabric/game_api.h>

#include "../hooks.h"
#include "graph_file.h"
#include "node_library.h"

namespace rexfabric::graph {
namespace {

namespace fs = std::filesystem;

constexpr const char* kGraphsFolder = "graphs";
constexpr const char* kScriptExtension = ".graph.lua";
// Undo steps kept; older ones drop off.
constexpr size_t kHistoryLimit = 200;

bool WriteText(const fs::path& file, const std::string& text, std::string* error) {
  std::error_code code;
  fs::create_directories(file.parent_path(), code);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (out) out << text;
  if (out) return true;
  if (error) *error = "Couldn't write " + file.string();
  return false;
}

std::string GraphSource(std::string_view name) { return std::string(kGraphsFolder) + "/" + std::string(name) + ".json"; }

CompileOptions Options(std::string_view name) {
  CompileOptions options;
  options.source = GraphSource(name);
  options.hookable = [](uint32_t address) { return Hooks::Get().IsHookable(address); };
  options.globals = GameGlobals();
  return options;
}

}  // namespace

std::vector<std::string> GameGlobals() {
  std::vector<std::string> globals;
  for (const std::unique_ptr<NamespaceDef>& space : Namespaces()) globals.push_back(space->name);
  return globals;
}

std::vector<std::string> ListGraphs(const fs::path& mod_dir) {
  std::vector<std::string> names;
  std::error_code error;
  for (const fs::directory_entry& entry : fs::directory_iterator(mod_dir / kGraphsFolder, error)) {
    if (entry.is_regular_file(error) && entry.path().extension() == ".json") names.push_back(entry.path().stem().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

fs::path GraphFile(const fs::path& mod_dir, std::string_view name) { return mod_dir / kGraphsFolder / (std::string(name) + ".json"); }

fs::path ScriptFile(const fs::path& mod_dir, std::string_view name) { return mod_dir / "scripts" / (std::string(name) + kScriptExtension); }

bool IsFileName(std::string_view name) {
  if (name.empty() || name.front() == ' ' || name.back() == ' ') return false;
  return std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-'; });
}

bool DeleteGraph(const fs::path& mod_dir, std::string_view name, std::string* error) {
  std::error_code code;
  fs::remove(GraphFile(mod_dir, name), code);
  if (code) {
    if (error) *error = code.message();
    return false;
  }
  fs::remove(ScriptFile(mod_dir, name), code);
  return true;
}

bool RenameGraph(const fs::path& mod_dir, std::string_view from, std::string_view to, std::string* error) {
  std::error_code code;
  if (fs::exists(GraphFile(mod_dir, to), code)) {
    if (error) *error = "There's a graph named " + std::string(to) + " already";
    return false;
  }
  fs::rename(GraphFile(mod_dir, from), GraphFile(mod_dir, to), code);
  if (code) {
    if (error) *error = code.message();
    return false;
  }
  if (fs::exists(ScriptFile(mod_dir, from), code)) fs::rename(ScriptFile(mod_dir, from), ScriptFile(mod_dir, to), code);
  return true;
}

Document::Document(fs::path mod_dir, std::string name) : mod_dir_(std::move(mod_dir)), name_(std::move(name)) {
  history_.push_back(Write(graph_));
  saved_ = history_.front();
  Recompile();
}

bool Document::Load(std::string* error) {
  std::ifstream in(GraphFile(mod_dir_, name_), std::ios::binary);
  if (in) {
    std::ostringstream text;
    text << in.rdbuf();
    if (!Read(text.str(), graph_, error)) return false;
  }
  history_ = {Write(graph_)};
  at_ = 0;
  saved_ = in ? history_.front() : std::string();
  Recompile();
  return true;
}

bool Document::Save(std::string* error) {
  const std::string snapshot = Write(graph_);
  if (!WriteText(GraphFile(mod_dir_, name_), snapshot, error)) return false;

  std::string script = compiled_.lua;
  if (!compiled_.ok()) {
    script = "-- Made with the RexFabric node editor from " + GraphSource(name_) + ".\n";
    script += "-- The graph has problems, so it doesn't run until they're fixed in the node editor.\n\n";
    script += "rex.error(" + LuaQuote(GraphSource(name_) + " has problems, so it doesn't run; open it in the node editor to see them") + ")\n";
  }
  if (!WriteText(ScriptFile(mod_dir_, name_), script, error)) return false;
  saved_ = snapshot;
  return true;
}

void Document::Edited(bool settled) {
  RemoveDanglingLinks(graph_);
  Recompile();
  if (!settled) return;
  std::string snapshot = Write(graph_);
  if (snapshot == history_[at_]) return;
  history_.resize(at_ + 1);
  history_.push_back(std::move(snapshot));
  if (history_.size() > kHistoryLimit) history_.erase(history_.begin());
  at_ = history_.size() - 1;
}

bool Document::Undo() {
  if (!CanUndo()) return false;
  Restore(history_[--at_]);
  return true;
}

bool Document::Redo() {
  if (!CanRedo()) return false;
  Restore(history_[++at_]);
  return true;
}

void Document::Recompile() { compiled_ = Compile(graph_, Options(name_)); }

void Document::Restore(const std::string& snapshot) {
  Read(snapshot, graph_);
  Recompile();
}

}  // namespace rexfabric::graph
