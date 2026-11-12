#include "node_library.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <rexfabric/game_api.h>

#include "lua_compiler.h"

namespace rexfabric::graph {
namespace {

// Number pins resolve through the nodes feeding them; a graph deeper than this is treated as floats.
constexpr int kMaxResolveDepth = 64;

PinType ResolveOutput(const Graph& graph, const PinRef& output, int depth) {
  const Node* node = graph.FindNode(output.node);
  if (!node) return {PinKind::kAny};
  const NodePins pins = PinsOf(*node, graph);
  const Pin* pin = FindPin(pins.outputs, output.pin);
  if (!pin) return {PinKind::kAny};
  if (pin->type.kind != PinKind::kNumber) return pin->type;
  if (depth > kMaxResolveDepth) return {PinKind::kFloat};

  // An int only when every number going in is one.
  for (const Pin& input : pins.inputs) {
    if (input.type.kind != PinKind::kNumber) continue;
    if (const Link* link = graph.LinkInto({node->id, input.name})) {
      if (ResolveOutput(graph, link->from, depth + 1).kind != PinKind::kInt) return {PinKind::kFloat};
      continue;
    }
    const auto typed = node->values.find(input.name);
    const nlohmann::json& value = typed != node->values.end() ? typed->second : input.initial;
    if (!value.is_string() || IsFloatText(value.get<std::string>())) return {PinKind::kFloat};
  }
  return {PinKind::kInt};
}

// What comes into a reroute, following reroutes back; Any when nothing does.
PinType RerouteUpstream(const Graph& graph, const Node& reroute, int depth) {
  const Link* link = depth <= kMaxResolveDepth ? graph.LinkInto({reroute.id, std::string(kRerouteIn)}) : nullptr;
  const Node* source = link ? graph.FindNode(link->from.node) : nullptr;
  if (!source) return {PinKind::kAny};
  if (source->type == kReroute) return RerouteUpstream(graph, *source, depth + 1);
  return ResolveOutput(graph, link->from, depth + 1);
}

// What a reroute goes to, following reroutes on; Any when it goes nowhere.
PinType RerouteDownstream(const Graph& graph, const Node& reroute, int depth) {
  if (depth > kMaxResolveDepth) return {PinKind::kAny};
  for (const Link* link : graph.LinksOutOf({reroute.id, std::string(kRerouteOut)})) {
    const Node* target = graph.FindNode(link->to.node);
    if (!target) continue;
    if (target->type == kReroute) {
      const PinType type = RerouteDownstream(graph, *target, depth + 1);
      if (type.kind != PinKind::kAny) return type;
      continue;
    }
    const NodePins pins = PinsOf(*target, graph);
    if (const Pin* pin = FindPin(pins.inputs, link->to.pin)) return pin->type;
  }
  return {PinKind::kAny};
}

// Whether `node`'s value depends, through data links, on `on`.
bool DependsOn(const Graph& graph, int node, int on, int depth = 0) {
  if (node == on) return true;
  if (depth > kMaxResolveDepth) return true;
  for (const Link& link : graph.links()) {
    if (link.to.node == node && DependsOn(graph, link.from.node, on, depth + 1)) return true;
  }
  return false;
}

}  // namespace

const Library& Library::Get() {
  static const Library library = [] {
    Library made;
    AddBuiltinNodes(made);
    AddGameNodes(made);
    AddApiNodes(made);
    return made;
  }();
  return library;
}

void Library::Add(NodeDesc desc) {
  desc.title = TitleCase(desc.title);
  auto owned = std::make_unique<NodeDesc>(std::move(desc));
  by_id_[owned->id] = owned.get();
  nodes_.push_back(std::move(owned));
}

const NodeDesc* Library::Find(std::string_view id) const {
  const auto found = by_id_.find(std::string(id));
  return found != by_id_.end() ? found->second : nullptr;
}

Pin ExecPin(std::string_view name) { return {std::string(name), {PinKind::kExec}}; }

Pin DataPin(std::string_view name, PinKind kind, nlohmann::json initial) {
  Pin pin{std::string(name), {kind}, initial.is_null() ? DefaultValue(kind) : std::move(initial)};
  pin.required = kind == PinKind::kObject || kind == PinKind::kRegisters;
  return pin;
}

Pin DelegatePin(std::string_view name) {
  Pin pin{std::string(name), {PinKind::kDelegate}};
  pin.required = true;
  return pin;
}

Pin ObjectPin(std::string_view name, std::string_view type) {
  Pin pin{std::string(name), {PinKind::kObject, std::string(type)}};
  pin.required = true;
  return pin;
}

nlohmann::json DefaultValue(PinKind kind) {
  switch (kind) {
    case PinKind::kBool: return false;
    case PinKind::kInt:
    case PinKind::kNumber: return "0";
    case PinKind::kFloat: return 0.0;
    case PinKind::kString:
    case PinKind::kAny: return "";
    default: return nullptr;
  }
}

const NodeDesc* FindDesc(const Node& node) { return Library::Get().Find(node.type); }

NodeRole RoleOf(const Node& node, const Graph& graph) {
  const NodeDesc* desc = FindDesc(node);
  if (!desc) return NodeRole::kAction;
  return desc->role_of ? desc->role_of(node, graph) : desc->role;
}

const Routine* RoutineOf(const Node& node, const Graph& graph) {
  if (node.type == kRoutineCall) {
    const std::string id = SettingOf(node, kRoutineSetting);
    return id.empty() ? nullptr : graph.FindRoutine(std::atoi(id.c_str()));
  }
  return node.owner != 0 ? graph.FindRoutine(node.owner) : nullptr;
}

const Node* EntryOf(const Graph& graph, int routine) {
  const auto found = std::find_if(graph.nodes().begin(), graph.nodes().end(), [&](const Node& node) { return node.owner == routine && node.type == kRoutineEntry; });
  return found != graph.nodes().end() ? &*found : nullptr;
}

const Node* ExitOf(const Graph& graph, int routine) {
  const auto found = std::find_if(graph.nodes().begin(), graph.nodes().end(), [&](const Node& node) { return node.owner == routine && node.type == kRoutineExit; });
  return found != graph.nodes().end() ? &*found : nullptr;
}

Routine& CreateRoutine(Graph& graph, RoutineKind kind, std::string name) {
  const int id = graph.AddRoutine(kind, name).id;
  Node& entry = graph.AddNode(std::string(kRoutineEntry), 0.0f, 0.0f);
  entry.owner = id;
  entry.settings[std::string(kRoutineNameSetting)] = name;
  graph.AddNode(std::string(kRoutineExit), 360.0f, 0.0f).owner = id;
  return *graph.FindRoutine(id);
}

void RenameRoutine(Graph& graph, int id, const std::string& name) {
  Routine* routine = graph.FindRoutine(id);
  if (!routine) return;
  routine->name = name;
  for (Node& node : graph.nodes()) {
    const bool call = node.type == kRoutineCall && SettingOf(node, kRoutineSetting) == std::to_string(id);
    if (call || (node.type == kRoutineEntry && node.owner == id)) node.settings[std::string(kRoutineNameSetting)] = name;
  }
}

void DeleteRoutine(Graph& graph, int id) {
  std::vector<int> calls;
  for (const Node& node : graph.nodes()) {
    if (node.type == kRoutineCall && SettingOf(node, kRoutineSetting) == std::to_string(id)) calls.push_back(node.id);
  }
  for (int call : calls) graph.RemoveNode(call);
  graph.RemoveRoutine(id);
}

void RenameRoutineParam(Graph& graph, int routine, bool output, const std::string& from, const std::string& to) {
  const std::string id = std::to_string(routine);
  std::vector<std::pair<int, bool>> pins;  // node, and whether the pin is one of its outputs
  for (const Node& node : graph.nodes()) {
    // An input of the routine is an output of its entry and an input of its calls; an output the other way round.
    if (node.type == kRoutineCall && SettingOf(node, kRoutineSetting) == id) pins.emplace_back(node.id, output);
    else if (node.owner == routine && node.type == (output ? kRoutineExit : kRoutineEntry)) pins.emplace_back(node.id, !output);
  }
  for (const auto& [node, is_output] : pins) graph.RenamePin({node, from}, is_output, to);
}

std::vector<std::string> ListOf(const Node& node, std::string_view setting) {
  std::vector<std::string> entries;
  const std::string text = SettingOf(node, setting);
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find('\n', start);
    const std::string entry = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!entry.empty()) entries.push_back(entry);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return entries;
}

std::string JoinList(const std::vector<std::string>& entries) {
  std::string text;
  for (const std::string& entry : entries) text += (text.empty() ? "" : "\n") + entry;
  return text;
}

namespace {

bool IsIdentifier(std::string_view text) {
  if (text.empty() || std::isdigit(static_cast<unsigned char>(text[0]))) return false;
  return std::all_of(text.begin(), text.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

}  // namespace

std::string LuaPath(std::string_view name) {
  std::string path;
  size_t start = 0;
  while (start <= name.size()) {
    const size_t dot = name.find('.', start);
    const std::string_view part = name.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
    path += path.empty() ? std::string(part) : LuaMember(part);
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return path;
}

std::string LuaMember(std::string_view name) { return IsIdentifier(name) ? "." + std::string(name) : "[" + LuaQuote(name) + "]"; }

const EnumDef* FindEnum(std::string_view name) {
  for (const std::unique_ptr<NamespaceDef>& space : Namespaces()) {
    for (const EnumDef& enumeration : space->enums) {
      if (enumeration.name == name) return &enumeration;
    }
  }
  return nullptr;
}

std::vector<std::string> EnumNames() {
  std::vector<std::string> names;
  for (const std::unique_ptr<NamespaceDef>& space : Namespaces()) {
    for (const EnumDef& enumeration : space->enums) names.push_back(enumeration.name);
  }
  return names;
}

bool AcceptsVariable(const SettingDesc& setting, PinKind kind) { return setting.kinds.empty() || std::find(setting.kinds.begin(), setting.kinds.end(), kind) != setting.kinds.end(); }

bool UsesMember(const Node& node, SettingKind kind, std::string_view name) {
  const NodeDesc* desc = FindDesc(node);
  if (!desc) return false;
  return std::any_of(desc->settings.begin(), desc->settings.end(), [&](const SettingDesc& setting) { return setting.kind == kind && SettingOf(node, setting.name) == name; });
}

bool UsesVariable(const Node& node, std::string_view name) { return UsesMember(node, SettingKind::kVariable, name); }

namespace {

// Points every setting of `kind` naming `from` at `to` instead.
void RenameMember(Graph& graph, SettingKind kind, const std::string& from, const std::string& to) {
  for (Node& node : graph.nodes()) {
    if (!UsesMember(node, kind, from)) continue;
    for (const SettingDesc& setting : FindDesc(node)->settings) {
      if (setting.kind == kind && SettingOf(node, setting.name) == from) node.settings[setting.name] = to;
    }
  }
}

}  // namespace

void RenameDispatcher(Graph& graph, const std::string& from, const std::string& to) {
  Dispatcher* dispatcher = graph.FindDispatcher(from);
  if (!dispatcher) return;
  RenameMember(graph, SettingKind::kDispatcher, from, to);
  dispatcher->name = to;
}

void DeleteDispatcher(Graph& graph, const std::string& name) {
  std::vector<int> users;
  for (Node& node : graph.nodes()) {
    if (!UsesMember(node, SettingKind::kDispatcher, name)) continue;
    // A setting that may be empty lets go of it; a node that needs it goes.
    bool needed = false;
    for (const SettingDesc& setting : FindDesc(node)->settings) {
      if (setting.kind != SettingKind::kDispatcher || SettingOf(node, setting.name) != name) continue;
      if (setting.optional) node.settings[setting.name].clear();
      else needed = true;
    }
    if (needed) users.push_back(node.id);
  }
  for (int id : users) graph.RemoveNode(id);
  std::erase_if(graph.dispatchers(), [&name](const Dispatcher& dispatcher) { return dispatcher.name == name; });
  RemoveDanglingLinks(graph);
}

void RenameDispatcherParam(Graph& graph, const std::string& dispatcher, const std::string& from, const std::string& to) {
  // A param is an input of Call and an output of the events taking the dispatcher's params.
  std::vector<std::pair<int, bool>> pins;
  for (const Node& node : graph.nodes()) {
    if (!UsesMember(node, SettingKind::kDispatcher, dispatcher)) continue;
    const NodePins shape = PinsOf(node, graph);
    if (FindPin(shape.inputs, from)) pins.emplace_back(node.id, false);
    if (FindPin(shape.outputs, from)) pins.emplace_back(node.id, true);
  }
  for (const auto& [node, output] : pins) graph.RenamePin({node, from}, output, to);
}

void RenameVariable(Graph& graph, const std::string& from, const std::string& to) {
  Variable* variable = graph.FindVariable(from);
  if (!variable) return;
  RenameMember(graph, SettingKind::kVariable, from, to);
  variable->name = to;
}

void DeleteVariable(Graph& graph, const std::string& name) {
  std::vector<int> users;
  for (const Node& node : graph.nodes()) {
    if (UsesVariable(node, name)) users.push_back(node.id);
  }
  for (int id : users) graph.RemoveNode(id);
  std::erase_if(graph.variables(), [&name](const Variable& variable) { return variable.name == name; });
}

std::vector<PinKind> ParamKinds(RoutineKind kind) {
  std::vector<PinKind> kinds = {PinKind::kBool, PinKind::kInt, PinKind::kFloat, PinKind::kString, PinKind::kAny};
  if (kind == RoutineKind::kMacro) kinds.insert(kinds.begin(), PinKind::kExec);
  return kinds;
}

NodePins PinsOf(const Node& node, const Graph& graph) {
  const NodeDesc* desc = FindDesc(node);
  if (!desc) return {};
  NodePins pins = desc->pins;
  if (desc->shape) desc->shape(node, graph, pins);
  if (desc->delegate) pins.outputs.push_back({.name = std::string(kDelegatePin), .type = {PinKind::kDelegate}});
  return pins;
}

const Pin* FindPin(const std::vector<Pin>& pins, std::string_view name) {
  const auto found = std::find_if(pins.begin(), pins.end(), [name](const Pin& pin) { return pin.name == name; });
  return found != pins.end() ? &*found : nullptr;
}

std::string SettingOf(const Node& node, std::string_view name) {
  if (const auto found = node.settings.find(name); found != node.settings.end()) return found->second;
  if (const NodeDesc* desc = FindDesc(node)) {
    for (const SettingDesc& setting : desc->settings) {
      if (setting.name == name) return setting.initial;
    }
  }
  return {};
}

std::string TitleOf(const Node& node) {
  const NodeDesc* desc = FindDesc(node);
  if (!desc) return node.type;
  return desc->title_of ? desc->title_of(node) : desc->title;
}

std::string TitleCase(std::string_view name) {
  const auto lower = [](char c) { return std::islower(static_cast<unsigned char>(c)) != 0; };
  const auto upper = [](char c) { return std::isupper(static_cast<unsigned char>(c)) != 0; };
  const auto digit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
  std::string title;
  bool word_start = true;
  for (size_t i = 0; i < name.size(); ++i) {
    char c = name[i];
    if (c == '_' || c == ' ') {
      // One space for a run of them, none at either end.
      if (!title.empty() && title.back() != ' ' && i + 1 < name.size()) title += ' ';
      word_start = true;
      continue;
    }
    // A new word where camel case starts one: "tM" in "SetMaterial", "PS" before "Server" in "HTTPServer".
    if (upper(c) && i > 0 && !word_start) {
      const char before = name[i - 1];
      const bool after_lower = lower(before) || digit(before);
      const bool acronym_end = upper(before) && i + 1 < name.size() && lower(name[i + 1]);
      if (after_lower || acronym_end) title += ' ';
    }
    if (word_start && lower(c)) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    title += c;
    word_start = c == '.' || c == ':' || c == '/';
  }
  while (!title.empty() && title.back() == ' ') title.pop_back();
  return title;
}

std::string TypeName(const PinType& type) {
  switch (type.kind) {
    case PinKind::kExec: return "Exec";
    case PinKind::kBool: return "Bool";
    case PinKind::kInt: return "Int";
    case PinKind::kFloat: return "Float";
    case PinKind::kNumber: return "Number";
    case PinKind::kString: return "String";
    case PinKind::kObject: return type.object.empty() ? "Object" : type.object;
    case PinKind::kRegisters: return "Registers";
    case PinKind::kDelegate: return "Event";
    case PinKind::kAny: return "Any";
  }
  return "Any";
}

bool IsFloatText(std::string_view text) {
  const bool hex = text.find("0x") != std::string_view::npos || text.find("0X") != std::string_view::npos;
  if (hex) return text.find('.') != std::string_view::npos || text.find('p') != std::string_view::npos || text.find('P') != std::string_view::npos;
  return text.find_first_of(".eE") != std::string_view::npos || text.find("inf") != std::string_view::npos || text.find("nan") != std::string_view::npos;
}

PinType OutputType(const Graph& graph, const PinRef& output) { return ResolveOutput(graph, output, 0); }

bool Fits(const PinType& from, const PinType& to) {
  if (from.kind == PinKind::kExec || to.kind == PinKind::kExec) return from.kind == to.kind;
  if (from.kind == PinKind::kAny || to.kind == PinKind::kAny) return true;
  switch (to.kind) {
    case PinKind::kFloat:
    case PinKind::kNumber: return from.kind == PinKind::kInt || from.kind == PinKind::kFloat || from.kind == PinKind::kNumber;
    case PinKind::kObject: return from.kind == PinKind::kObject && (to.object.empty() || from.object.empty() || to.object == from.object);
    default: return from.kind == to.kind;
  }
}

bool CanConnect(const Graph& graph, const PinRef& from, const PinRef& to, std::string* reason) {
  const auto no = [reason](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  if (from.node == to.node) return no("A node can't link to itself");
  const Node* source = graph.FindNode(from.node);
  const Node* target = graph.FindNode(to.node);
  if (!source || !target) return no({});
  if (source->owner != target->owner) return no("Those nodes are in different graphs");
  const NodePins source_pins = PinsOf(*source, graph);
  const NodePins target_pins = PinsOf(*target, graph);
  const Pin* output = FindPin(source_pins.outputs, from.pin);
  const Pin* input = FindPin(target_pins.inputs, to.pin);
  if (!output || !input) return no({});

  const bool exec = output->type.kind == PinKind::kExec || input->type.kind == PinKind::kExec;
  // A wildcard becomes whatever it's linked to.
  if (!output->wildcard && !input->wildcard) {
    if (exec != (input->type.kind == PinKind::kExec) || exec != (output->type.kind == PinKind::kExec)) return no("Exec pins only link to exec pins");
    const PinType carried = OutputType(graph, from);
    if (!Fits(carried, input->type)) return no(TypeName(carried) + " doesn't fit " + TypeName(input->type));
  }
  if (!exec && DependsOn(graph, from.node, to.node)) return no("That would make a loop");
  return true;
}

bool Connect(Graph& graph, const PinRef& from, const PinRef& to) {
  if (!CanConnect(graph, from, to)) return false;
  const NodePins source = PinsOf(*graph.FindNode(from.node), graph);
  const NodePins target = PinsOf(*graph.FindNode(to.node), graph);
  // Either end can be a wildcard, so the other says whether it's exec.
  const bool exec = FindPin(source.outputs, from.pin)->type.kind == PinKind::kExec || FindPin(target.inputs, to.pin)->type.kind == PinKind::kExec;
  graph.AddLink(from, to, exec);
  return true;
}

PinType RerouteType(const Graph& graph, const Node& reroute) {
  const PinType upstream = RerouteUpstream(graph, reroute, 0);
  return upstream.kind != PinKind::kAny ? upstream : RerouteDownstream(graph, reroute, 0);
}

int InsertReroute(Graph& graph, int link, float x, float y) {
  const auto found = std::find_if(graph.links().begin(), graph.links().end(), [link](const Link& entry) { return entry.id == link; });
  if (found == graph.links().end()) return 0;
  const Link split = *found;
  const Node* source = graph.FindNode(split.from.node);
  if (!source) return 0;
  const int owner = source->owner;
  Node& reroute = graph.AddNode(std::string(kReroute), x, y);
  reroute.owner = owner;
  const int id = reroute.id;
  graph.RemoveLink(link);
  const PinRef in{id, std::string(kRerouteIn)};
  const PinRef out{id, std::string(kRerouteOut)};
  // Into the reroute first, so it has the link's type when it links on.
  if (!Connect(graph, split.from, in) || !Connect(graph, out, split.to)) {
    graph.RemoveNode(id);
    Connect(graph, split.from, split.to);
    return 0;
  }
  return id;
}

void DissolveReroute(Graph& graph, int reroute) {
  std::vector<PinRef> sources;
  std::vector<PinRef> targets;
  for (const Link& link : graph.links()) {
    if (link.to.node == reroute) sources.push_back(link.from);
    if (link.from.node == reroute) targets.push_back(link.to);
  }
  graph.RemoveNode(reroute);
  for (const PinRef& source : sources) {
    for (const PinRef& target : targets) Connect(graph, source, target);
  }
}

void RemoveDanglingLinks(Graph& graph) {
  std::vector<int> dangling;
  for (const Link& link : graph.links()) {
    const Node* source = graph.FindNode(link.from.node);
    const Node* target = graph.FindNode(link.to.node);
    // Links of a node the library doesn't know are kept, in case its type comes back.
    if (!source || !target || !FindDesc(*source) || !FindDesc(*target)) continue;
    const NodePins source_pins = PinsOf(*source, graph);
    const NodePins target_pins = PinsOf(*target, graph);
    if (!FindPin(source_pins.outputs, link.from.pin) || !FindPin(target_pins.inputs, link.to.pin)) dangling.push_back(link.id);
  }
  for (int id : dangling) graph.RemoveLink(id);
}

}  // namespace rexfabric::graph
