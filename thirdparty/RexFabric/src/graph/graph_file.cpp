#include "graph_file.h"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace rexfabric::graph {
namespace {

using nlohmann::json;

// Marks clipboard text as nodes, so pasting other text does nothing.
constexpr const char* kClipboardKind = "rexfabric.nodes";

json NodeJson(const Node& node) {
  json out = {{"id", node.id}, {"type", node.type}, {"x", node.x}, {"y", node.y}};
  if (node.owner != 0) out["owner"] = node.owner;
  if (!node.settings.empty()) out["settings"] = node.settings;
  if (!node.values.empty()) out["values"] = node.values;
  return out;
}

json ParamsJson(const std::vector<Param>& params) {
  json out = json::array();
  for (const Param& param : params) out.push_back({{"name", param.name}, {"type", PinKindName(param.type)}});
  return out;
}

std::vector<Param> ReadParams(const json& in) {
  std::vector<Param> params;
  if (!in.is_array()) return params;
  for (const json& entry : in) {
    Param param;
    param.name = entry.value("name", std::string());
    ParsePinKind(entry.value("type", std::string("float")), param.type);
    params.push_back(std::move(param));
  }
  return params;
}

json LinkJson(const Link& link) { return {{"from", {{"node", link.from.node}, {"pin", link.from.pin}}}, {"to", {{"node", link.to.node}, {"pin", link.to.pin}}}}; }

bool ReadNode(const json& in, Node& node) {
  if (!in.is_object() || !in.contains("id") || !in.contains("type")) return false;
  node.id = in.at("id").get<int>();
  node.owner = in.value("owner", 0);
  node.type = in.at("type").get<std::string>();
  node.x = in.value("x", 0.0f);
  node.y = in.value("y", 0.0f);
  if (const auto settings = in.find("settings"); settings != in.end() && settings->is_object()) {
    for (const auto& [name, value] : settings->items()) {
      if (value.is_string()) node.settings[name] = value.get<std::string>();
    }
  }
  if (const auto values = in.find("values"); values != in.end() && values->is_object()) {
    for (const auto& [name, value] : values->items()) node.values[name] = value;
  }
  return true;
}

bool ReadPinRef(const json& in, PinRef& ref) {
  if (!in.is_object() || !in.contains("node") || !in.contains("pin")) return false;
  ref.node = in.at("node").get<int>();
  ref.pin = in.at("pin").get<std::string>();
  return true;
}

void ReadLinks(const json& in, Graph& graph) {
  const auto links = in.find("links");
  if (links == in.end() || !links->is_array()) return;
  for (const json& link : *links) {
    PinRef from;
    PinRef to;
    if (link.is_object() && link.contains("from") && link.contains("to") && ReadPinRef(link.at("from"), from) && ReadPinRef(link.at("to"), to)) graph.InsertLink(from, to);
  }
}

}  // namespace

std::string Write(const Graph& graph) {
  json out = {{"version", kFileVersion}};
  json variables = json::array();
  for (const Variable& variable : graph.variables()) variables.push_back({{"name", variable.name}, {"type", PinKindName(variable.type)}, {"initial", variable.initial}, {"comment", variable.comment}});
  out["variables"] = std::move(variables);
  json routines = json::array();
  for (const Routine& routine : graph.routines()) {
    routines.push_back({{"id", routine.id}, {"kind", routine.kind == RoutineKind::kMacro ? "macro" : "function"}, {"name", routine.name}, {"pure", routine.pure}, {"comment", routine.comment}, {"inputs", ParamsJson(routine.inputs)}, {"outputs", ParamsJson(routine.outputs)}});
  }
  out["routines"] = std::move(routines);
  json dispatchers = json::array();
  for (const Dispatcher& dispatcher : graph.dispatchers()) dispatchers.push_back({{"name", dispatcher.name}, {"comment", dispatcher.comment}, {"params", ParamsJson(dispatcher.params)}});
  out["dispatchers"] = std::move(dispatchers);
  json nodes = json::array();
  for (const Node& node : graph.nodes()) nodes.push_back(NodeJson(node));
  out["nodes"] = std::move(nodes);
  json links = json::array();
  for (const Link& link : graph.links()) links.push_back(LinkJson(link));
  out["links"] = std::move(links);
  return out.dump(2);
}

bool Read(std::string_view text, Graph& graph, std::string* error) {
  try {
    const json in = json::parse(text);
    if (in.value("version", 0) > kFileVersion) {
      if (error) *error = "The graph was saved by a newer RexFabric";
      return false;
    }
    Graph loaded;
    if (const auto variables = in.find("variables"); variables != in.end() && variables->is_array()) {
      for (const json& entry : *variables) {
        Variable variable;
        variable.name = entry.value("name", std::string());
        ParsePinKind(entry.value("type", std::string("float")), variable.type);
        variable.initial = entry.value("initial", json());
        variable.comment = entry.value("comment", std::string());
        loaded.variables().push_back(std::move(variable));
      }
    }
    if (const auto routines = in.find("routines"); routines != in.end() && routines->is_array()) {
      for (const json& entry : *routines) {
        Routine routine;
        routine.id = entry.value("id", 0);
        routine.kind = entry.value("kind", std::string("function")) == "macro" ? RoutineKind::kMacro : RoutineKind::kFunction;
        routine.name = entry.value("name", std::string());
        routine.pure = entry.value("pure", false);
        routine.comment = entry.value("comment", std::string());
        routine.inputs = ReadParams(entry.value("inputs", json::array()));
        routine.outputs = ReadParams(entry.value("outputs", json::array()));
        if (routine.id > 0 && !loaded.FindRoutine(routine.id)) loaded.InsertRoutine(std::move(routine));
      }
    }
    if (const auto dispatchers = in.find("dispatchers"); dispatchers != in.end() && dispatchers->is_array()) {
      for (const json& entry : *dispatchers) {
        Dispatcher dispatcher;
        dispatcher.name = entry.value("name", std::string());
        dispatcher.comment = entry.value("comment", std::string());
        dispatcher.params = ReadParams(entry.value("params", json::array()));
        loaded.dispatchers().push_back(std::move(dispatcher));
      }
    }
    if (const auto nodes = in.find("nodes"); nodes != in.end() && nodes->is_array()) {
      for (const json& entry : *nodes) {
        Node node;
        if (ReadNode(entry, node) && !loaded.FindNode(node.id)) loaded.InsertNode(std::move(node));
      }
    }
    ReadLinks(in, loaded);
    graph = std::move(loaded);
    return true;
  } catch (const json::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
}

std::string WriteNodes(const Graph& graph, std::span<const int> nodes) {
  const auto chosen = [&nodes](int id) { return std::find(nodes.begin(), nodes.end(), id) != nodes.end(); };
  json out = {{"kind", kClipboardKind}};
  json written = json::array();
  for (const Node& node : graph.nodes()) {
    if (chosen(node.id)) written.push_back(NodeJson(node));
  }
  out["nodes"] = std::move(written);
  json links = json::array();
  for (const Link& link : graph.links()) {
    if (chosen(link.from.node) && chosen(link.to.node)) links.push_back(LinkJson(link));
  }
  out["links"] = std::move(links);
  return out.dump();
}

bool ReadNodes(std::string_view text, Graph& graph) {
  const json in = json::parse(text, nullptr, false);
  if (!in.is_object() || in.value("kind", std::string()) != kClipboardKind) return false;
  try {
    if (const auto nodes = in.find("nodes"); nodes != in.end() && nodes->is_array()) {
      for (const json& entry : *nodes) {
        Node node;
        if (ReadNode(entry, node) && !graph.FindNode(node.id)) graph.InsertNode(std::move(node));
      }
    }
    ReadLinks(in, graph);
    return !graph.nodes().empty();
  } catch (const json::exception&) {
    return false;
  }
}

}  // namespace rexfabric::graph
