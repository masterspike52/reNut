#include "material_graph.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

namespace rexfabric::content::material {
namespace {

constexpr const char* kTypeNames[] = {"float", "float", "float2", "float3", "float4"};
constexpr const char* kSwizzles[] = {"", ".x", ".xy", ".xyz", ".xyzw"};

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

bool Same(std::string_view a, std::string_view b) { return Lower(a) == Lower(b); }

std::string Constant(const std::array<float, 4>& value, uint8_t width) { return ConstantCode(value, width); }

// A value as `width` floats: a single float spread over all of them, a wider one cut, a narrower one padded with 0.
std::string Cast(const Value& value, uint8_t width) {
  if (value.width == width) return value.code;
  if (value.width == 1) return width == 1 ? value.code : "((" + std::string(kTypeNames[width]) + ")(" + value.code + "))";
  if (value.width > width) return "(" + value.code + ")" + kSwizzles[width];
  std::string code = std::string(kTypeNames[width]) + "(" + value.code;
  for (uint8_t i = value.width; i < width; ++i) code += ", 0.0";
  return code + ")";
}

// FNV-1a, stable across runs, for the game keeping its compiled shaders by it.
uint64_t Hash(std::string_view text) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 0x100000001B3ull;
  }
  return hash;
}

std::array<float, 4> FloatsOf(const nlohmann::json& json) {
  std::array<float, 4> floats{};
  if (json.is_number()) floats[0] = json.get<float>();
  for (size_t i = 0; json.is_array() && i < json.size() && i < 4; ++i) {
    if (json[i].is_number()) floats[i] = json[i].get<float>();
  }
  return floats;
}

// The Material node and the game's nodes of each graph material type, made once: the types never change once declared.
struct TypeNodes {
  NodeType result;
  std::vector<NodeType> game;
};

const TypeNodes& NodesOf(const MaterialType& type) {
  static std::mutex mutex;
  static std::unordered_map<std::string, std::unique_ptr<TypeNodes>> made;
  std::lock_guard lock(mutex);
  std::unique_ptr<TypeNodes>& nodes = made[Lower(type.path)];
  if (nodes) return *nodes;
  nodes = std::make_unique<TypeNodes>();
  nodes->result = {.id = std::string(kResult), .title = "Material", .category = "", .help = "What the material is: each input one of what its shading model lights. The inputs it doesn't read are greyed out"};
  for (const MaterialPinDecl& input : type.graph_inputs) nodes->result.inputs.push_back({.name = input.name, .width = input.width, .initial = input.initial, .help = input.help});
  for (const MaterialNodeDecl& decl : type.graph_nodes) {
    NodeType node{.id = decl.id, .title = decl.title, .category = decl.category, .help = decl.help};
    for (const MaterialPinDecl& output : decl.outputs) node.outputs.push_back({.name = output.name, .width = output.width, .help = output.help});
    node.emit = [code = decl.code](Compiler&, const NodeType&, const Node&, const std::vector<Value>&) { return code; };
    nodes->game.push_back(std::move(node));
  }
  return *nodes;
}

// Works a graph out from its Material node: each node reached once, its outputs in locals.
class Writer final : public Compiler {
 public:
  Writer(const MaterialType& type, const Graph& graph) : type_(type), graph_(graph) {}

  int Texture(const MaterialTextureUse& use) override {
    const auto found = std::find(program_.textures.begin(), program_.textures.end(), use);
    if (found != program_.textures.end()) return static_cast<int>(found - program_.textures.begin());
    program_.textures.push_back(use);
    return static_cast<int>(program_.textures.size() - 1);
  }

  int Parameter(const std::string& name, uint8_t width, const std::array<float, 4>& value) override {
    for (size_t i = 0; i < program_.parameters.size(); ++i) {
      const MaterialParameterUse& known = program_.parameters[i];
      if (!Same(known.name, name)) continue;
      if (known.width != width) {
        Problem("the parameter " + name + " is already one of " + std::to_string(known.width) + " floats");
        return -1;
      }
      return static_cast<int>(i);
    }
    program_.parameters.push_back({.name = name, .width = width, .value = value});
    return static_cast<int>(program_.parameters.size() - 1);
  }

  std::string Local(const std::string& code, uint8_t width) override {
    const std::string name = "l" + std::to_string(locals_++);
    body_ += "  " + std::string(kTypeNames[width]) + " " + name + " = " + code + ";\n";
    return name;
  }

  void Helper(const std::string& name, const std::string& code) override {
    if (helper_names_.insert(name).second) helpers_ += code + "\n";
  }

  void Problem(std::string text) override {
    if (!node_) return;
    const std::string title = TitleOf(type_, *node_);
    compiled_.node_problems[node_->id].push_back(text);
    program_.problems.push_back(title + ": " + text);
  }

  Compiled Finish(std::string_view path) {
    const Node* result = graph_.Result();
    if (!result) {
      program_.problems.push_back("It has no Material node");
      return Take();
    }
    const ShadingModelDecl* model = type_.FindShadingModel(graph_.shading_model);
    if (!model) {
      program_.problems.push_back(graph_.shading_model.empty() ? "It has no shading model: pick one on the Material node" : "The game has no shading model " + graph_.shading_model);
      return Take();
    }
    program_.shading_model = model->name;

    std::string fields;
    std::string results;
    for (const Pin& input : NodesOf(type_).result.inputs) {
      const std::string field = FieldName(input.name);
      fields += "  " + std::string(kTypeNames[input.width]) + " " + field + ";\n";
      const auto typed = result->values.find(input.name);
      Value value{Constant(typed != result->values.end() ? typed->second : input.initial, input.width), input.width};
      // Only what the shading model reads is worked out; the rest stay as they start.
      if (Reads(type_, model->name, input.name)) {
        if (const Link* link = graph_.LinkInto(result->id, input.name)) {
          value = Output(link->from, link->output);
          program_.linked.push_back(input.name);
        }
      }
      results += "  r." + field + " = " + Cast(value, input.width) + ";\n";
    }
    if (!program_.problems.empty()) return Take();

    std::string& code = program_.code;
    code += "// " + std::string(path) + ", " + model->name + "\n";
    code += "struct MaterialResult {\n" + fields + "};\n\n";
    code += helpers_;
    if (!helpers_.empty()) code += "\n";
    code += "MaterialResult EvaluateMaterial(MaterialContext c) {\n" + body_ + "  MaterialResult r;\n" + results + "  return r;\n}\n";
    program_.hash = Hash(code);
    return Take();
  }

 private:
  Compiled Take() {
    if (!program_.problems.empty()) {
      program_.code.clear();
      program_.hash = 0;
    }
    compiled_.program = std::move(program_);
    return std::move(compiled_);
  }

  // An output of a node, writing the node first if it isn't yet.
  Value Output(int id, const std::string& output) {
    const Node* node = graph_.Find(id);
    if (!node) return {"0.0", 1};
    if (!written_.contains(id)) Write(*node);
    const auto found = outputs_.find({id, Lower(output)});
    if (found == outputs_.end()) return {"0.0", 1};
    return found->second;
  }

  void Write(const Node& node) {
    written_.insert(node.id);
    const NodeType* desc = FindNodeType(type_, node.type);
    const Node* outer = node_;
    if (!desc || !desc->emit || desc->id == kResult) {
      node_ = &node;
      Problem(desc ? "it can't feed another node" : "the game has no node " + node.type);
      node_ = outer;
      return;
    }
    if (visiting_.contains(node.id)) {
      node_ = &node;
      Problem("its inputs go round in a circle back to it");
      node_ = outer;
      return;
    }
    visiting_.insert(node.id);
    const NodePins pins = PinsOf(type_, node);

    // Each input's value: what links into it, or what's typed into it.
    std::vector<Value> inputs;
    for (const Pin& pin : pins.inputs) {
      if (const Link* link = graph_.LinkInto(node.id, pin.name)) {
        inputs.push_back(Output(link->from, link->output));
        continue;
      }
      const uint8_t width = pin.width ? pin.width : 1;
      if (!pin.unlinked.empty()) {
        inputs.push_back({pin.unlinked, width});
        continue;
      }
      const auto typed = node.values.find(pin.name);
      inputs.push_back({Constant(typed != node.values.end() ? typed->second : pin.initial, width), width});
    }
    node_ = &node;

    // Inputs of any width are as wide as the widest of them, unless the node keeps each its own.
    uint8_t widest = 1;
    for (size_t i = 0; i < pins.inputs.size(); ++i) {
      if (pins.inputs[i].width == 0) widest = std::max(widest, inputs[i].width);
    }
    std::vector<uint8_t> widths;
    for (size_t i = 0; i < pins.inputs.size(); ++i) {
      const Pin& pin = pins.inputs[i];
      Value& value = inputs[i];
      if (pin.width != 0) {
        value = {Cast(value, pin.width), pin.width};
      } else if (!desc->mixed) {
        if (value.width != 1 && value.width != widest) Problem(pin.name + " is " + kTypeNames[value.width] + " where the node's other inputs are " + kTypeNames[widest] + ": they need the same width, or a single float");
        value = {Cast(value, widest), widest};
      }
      widths.push_back(value.width);
    }

    std::vector<uint8_t> output_widths;
    if (desc->widths) output_widths = desc->widths(node, widths);
    for (size_t i = output_widths.size(); i < pins.outputs.size(); ++i) output_widths.push_back(pins.outputs[i].width ? pins.outputs[i].width : widest);

    const std::vector<std::string> codes = desc->emit(*this, *desc, node, inputs);
    for (size_t i = 0; i < pins.outputs.size() && i < codes.size(); ++i) {
      const uint8_t width = std::clamp<uint8_t>(output_widths[i], 1, 4);
      // Only what's read gets a local.
      if (graph_.HasLinkFrom(node.id, pins.outputs[i].name)) outputs_[{node.id, Lower(pins.outputs[i].name)}] = {Local(codes[i], width), width};
    }
    node_ = outer;
    visiting_.erase(node.id);
  }

  const MaterialType& type_;
  const Graph& graph_;
  Compiled compiled_;
  MaterialProgram program_;
  std::string body_;
  std::string helpers_;
  std::set<std::string> helper_names_;
  int locals_ = 0;
  std::set<int> written_;
  std::set<int> visiting_;
  std::map<std::pair<int, std::string>, Value> outputs_;
  const Node* node_ = nullptr;
};

// The widths of a graph's nodes, worked out as a compile does, for the canvas.
class WidthResolver {
 public:
  WidthResolver(const MaterialType& type, const Graph& graph) : type_(type), graph_(graph) {}

  Widths Of(const Node& node) {
    if (const auto found = made_.find(node.id); found != made_.end()) return found->second;
    Widths widths;
    if (!visiting_.insert(node.id).second) return widths;
    const NodeType* desc = FindNodeType(type_, node.type);
    const NodePins pins = PinsOf(type_, node);
    uint8_t widest = 1;
    for (const Pin& pin : pins.inputs) {
      uint8_t width = pin.width ? pin.width : 1;
      if (const Link* link = graph_.LinkInto(node.id, pin.name)) {
        if (pin.width == 0) width = OutputWidth(link->from, link->output);
      }
      if (pin.width == 0) widest = std::max(widest, width);
      widths.inputs.push_back(width);
    }
    if (desc && !desc->mixed) {
      for (size_t i = 0; i < pins.inputs.size(); ++i) {
        if (pins.inputs[i].width == 0 && widths.inputs[i] == 1) widths.inputs[i] = widest;
      }
    }
    if (desc && desc->widths) widths.outputs = desc->widths(node, widths.inputs);
    for (size_t i = widths.outputs.size(); i < pins.outputs.size(); ++i) widths.outputs.push_back(pins.outputs[i].width ? pins.outputs[i].width : widest);
    for (uint8_t& width : widths.outputs) width = std::clamp<uint8_t>(width, 1, 4);
    visiting_.erase(node.id);
    made_[node.id] = widths;
    return widths;
  }

 private:
  uint8_t OutputWidth(int id, const std::string& output) {
    const Node* node = graph_.Find(id);
    if (!node) return 1;
    const NodePins pins = PinsOf(type_, *node);
    const Widths widths = Of(*node);
    for (size_t i = 0; i < pins.outputs.size() && i < widths.outputs.size(); ++i) {
      if (Same(pins.outputs[i].name, output)) return widths.outputs[i];
    }
    return 1;
  }

  const MaterialType& type_;
  const Graph& graph_;
  std::map<int, Widths> made_;
  std::set<int> visiting_;
};

bool ValidField(std::string_view name) {
  if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

}  // namespace

Graph Graph::Read(const nlohmann::json& json) {
  Graph graph;
  if (!json.is_object()) return graph;
  graph.shading_model = json.value("shading_model", std::string());
  for (const nlohmann::json& stored : json.value("nodes", nlohmann::json::array())) {
    if (!stored.is_object() || !stored.value("id", nlohmann::json()).is_number_integer() || !stored.value("type", nlohmann::json()).is_string()) continue;
    Node node{.id = stored["id"].get<int>(), .type = stored["type"].get<std::string>(), .x = stored.value("x", 0.0f), .y = stored.value("y", 0.0f)};
    if (node.id <= 0 || graph.Find(node.id)) continue;
    if (const auto settings = stored.find("settings"); settings != stored.end() && settings->is_object()) node.settings = *settings;
    if (const auto values = stored.find("values"); values != stored.end() && values->is_object()) {
      for (const auto& [pin, value] : values->items()) node.values[pin] = FloatsOf(value);
    }
    graph.next_id_ = std::max(graph.next_id_, node.id + 1);
    graph.nodes.push_back(std::move(node));
  }
  for (const nlohmann::json& stored : json.value("links", nlohmann::json::array())) {
    const nlohmann::json from = stored.is_object() ? stored.value("from", nlohmann::json()) : nlohmann::json();
    const nlohmann::json to = stored.is_object() ? stored.value("to", nlohmann::json()) : nlohmann::json();
    if (!from.is_array() || !to.is_array() || from.size() != 2 || to.size() != 2 || !from[0].is_number_integer() || !from[1].is_string() || !to[0].is_number_integer() || !to[1].is_string()) continue;
    const int source = from[0].get<int>();
    const int target = to[0].get<int>();
    if (!graph.Find(source) || !graph.Find(target) || graph.LinkInto(target, to[1].get<std::string>())) continue;
    graph.links.push_back({.from = source, .output = from[1].get<std::string>(), .to = target, .input = to[1].get<std::string>()});
  }
  return graph;
}

nlohmann::json Graph::Write() const {
  nlohmann::json json = nlohmann::json::object();
  json["shading_model"] = shading_model;
  nlohmann::json& stored_nodes = json["nodes"] = nlohmann::json::array();
  for (const Node& node : nodes) {
    nlohmann::json stored = {{"id", node.id}, {"type", node.type}, {"x", node.x}, {"y", node.y}};
    if (!node.settings.empty()) stored["settings"] = node.settings;
    if (!node.values.empty()) {
      nlohmann::json& values = stored["values"] = nlohmann::json::object();
      for (const auto& [pin, value] : node.values) values[pin] = {value[0], value[1], value[2], value[3]};
    }
    stored_nodes.push_back(std::move(stored));
  }
  nlohmann::json& stored_links = json["links"] = nlohmann::json::array();
  for (const Link& link : links) stored_links.push_back({{"from", {link.from, link.output}}, {"to", {link.to, link.input}}});
  return json;
}

Node* Graph::Find(int id) {
  const auto found = std::find_if(nodes.begin(), nodes.end(), [id](const Node& node) { return node.id == id; });
  return found != nodes.end() ? &*found : nullptr;
}

const Node* Graph::Find(int id) const { return const_cast<Graph*>(this)->Find(id); }

const Node* Graph::Result() const {
  const auto found = std::find_if(nodes.begin(), nodes.end(), [](const Node& node) { return node.type == kResult; });
  return found != nodes.end() ? &*found : nullptr;
}

Node& Graph::Add(std::string type, float x, float y) {
  nodes.push_back({.id = next_id_++, .type = std::move(type), .x = x, .y = y});
  return nodes.back();
}

void Graph::Remove(int id) {
  std::erase_if(nodes, [id](const Node& node) { return node.id == id; });
  std::erase_if(links, [id](const Link& link) { return link.from == id || link.to == id; });
}

const Link* Graph::LinkInto(int node, std::string_view input) const {
  const auto found = std::find_if(links.begin(), links.end(), [&](const Link& link) { return link.to == node && Same(link.input, input); });
  return found != links.end() ? &*found : nullptr;
}

bool Graph::HasLinkFrom(int node, std::string_view output) const {
  return std::any_of(links.begin(), links.end(), [&](const Link& link) { return link.from == node && Same(link.output, output); });
}

void Graph::Connect(int from, std::string output, int to, std::string input) {
  Disconnect(to, input);
  links.push_back({.from = from, .output = std::move(output), .to = to, .input = std::move(input)});
}

void Graph::Disconnect(int to, std::string_view input) {
  std::erase_if(links, [&](const Link& link) { return link.to == to && Same(link.input, input); });
}

bool Graph::Feeds(int to, int from) const {
  // Back from `from` along the links into each node, looking for `to`.
  std::set<int> seen;
  std::vector<int> open = {from};
  while (!open.empty()) {
    const int at = open.back();
    open.pop_back();
    if (at == to) return true;
    if (!seen.insert(at).second) continue;
    for (const Link& link : links) {
      if (link.to == at) open.push_back(link.from);
    }
  }
  return false;
}

Graph DefaultGraph(const MaterialType& type) {
  Graph graph;
  graph.shading_model = type.shading_models.empty() ? std::string() : type.shading_models.front().name;
  graph.Add(std::string(kResult), 0.0f, 0.0f);
  return graph;
}

std::vector<const NodeType*> NodeTypes(const MaterialType& type) {
  std::vector<const NodeType*> types;
  const TypeNodes& own = NodesOf(type);
  types.push_back(&own.result);
  for (const NodeType& node : BuiltinNodes()) types.push_back(&node);
  for (const NodeType& node : own.game) types.push_back(&node);
  return types;
}

const NodeType* FindNodeType(const MaterialType& type, std::string_view id) {
  const TypeNodes& own = NodesOf(type);
  if (id == kResult) return &own.result;
  for (const NodeType& node : own.game) {
    if (node.id == id) return &node;
  }
  for (const NodeType& node : BuiltinNodes()) {
    if (node.id == id) return &node;
  }
  return nullptr;
}

NodePins PinsOf(const MaterialType& type, const Node& node) {
  const NodeType* desc = FindNodeType(type, node.type);
  if (!desc) return {};
  NodePins pins{desc->inputs, desc->outputs};
  if (desc->shape) desc->shape(node, pins.inputs, pins.outputs);
  return pins;
}

const Pin* FindPin(const std::vector<Pin>& pins, std::string_view name) {
  const auto found = std::find_if(pins.begin(), pins.end(), [name](const Pin& pin) { return Same(pin.name, name); });
  return found != pins.end() ? &*found : nullptr;
}

std::string TitleOf(const MaterialType& type, const Node& node) {
  const NodeType* desc = FindNodeType(type, node.type);
  if (!desc) return node.type;
  return desc->title_of ? desc->title_of(node) : desc->title;
}

nlohmann::json SettingOf(const NodeType& type, const Node& node, std::string_view name) {
  if (const auto found = node.settings.find(std::string(name)); found != node.settings.end()) return *found;
  for (const Setting& setting : type.settings) {
    if (setting.name == name) return setting.initial;
  }
  return nullptr;
}

Widths WidthsOf(const MaterialType& type, const Graph& graph, const Node& node) { return WidthResolver(type, graph).Of(node); }

bool CanConnect(const MaterialType& type, const Graph& graph, int from, std::string_view output, int to, std::string_view input, std::string* reason) {
  const auto refuse = [reason](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  const Node* source = graph.Find(from);
  const Node* target = graph.Find(to);
  if (!source || !target) return false;
  if (from == to) return refuse("A node can't feed itself");
  if (!FindPin(PinsOf(type, *source).outputs, output) || !FindPin(PinsOf(type, *target).inputs, input)) return false;
  if (graph.Feeds(to, from)) return refuse("That would go round in a circle");
  return true;
}

bool Reads(const MaterialType& type, std::string_view shading_model, std::string_view input) {
  const ShadingModelDecl* model = type.FindShadingModel(shading_model);
  return model && std::any_of(model->inputs.begin(), model->inputs.end(), [input](const std::string& name) { return Same(name, input); });
}

Compiled Compile(const MaterialType& type, const Graph& graph, std::string_view path) { return Writer(type, graph).Finish(path); }

std::string NumberCode(float value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.9g", value);
  std::string number = text;
  if (number.find_first_of(".eEn") == std::string::npos) number += ".0";
  return number;
}

std::string ConstantCode(const std::array<float, 4>& value, uint8_t width) {
  if (width <= 1) return NumberCode(value[0]);
  std::string code = std::string(kTypeNames[width]) + "(";
  for (uint8_t i = 0; i < width; ++i) code += (i ? ", " : "") + NumberCode(value[i]);
  return code + ")";
}

std::string FieldName(std::string_view pin) {
  std::string field;
  for (const unsigned char c : pin) field += std::isalnum(c) ? static_cast<char>(std::tolower(c)) : '_';
  return field;
}

bool CheckDeclared(const MaterialType& type, std::string* why) {
  std::set<std::string> fields;
  for (const MaterialPinDecl& input : type.graph_inputs) {
    if (input.width < 1 || input.width > 4) {
      *why = "its graph input " + input.name + " isn't 1 to 4 floats";
      return false;
    }
    if (!ValidField(FieldName(input.name)) || !fields.insert(FieldName(input.name)).second) {
      *why = "its graph input " + input.name + " has no name of its own";
      return false;
    }
  }
  if (type.HasGraph() && type.shading_models.empty()) {
    *why = "it has graph inputs but no shading model";
    return false;
  }
  for (const ShadingModelDecl& model : type.shading_models) {
    for (const std::string& input : model.inputs) {
      if (!fields.contains(FieldName(input))) {
        *why = "its shading model " + model.name + " reads " + input + ", which isn't one of its graph inputs";
        return false;
      }
    }
  }
  std::set<std::string> ids;
  for (const NodeType& node : BuiltinNodes()) ids.insert(node.id);
  for (const MaterialNodeDecl& node : type.graph_nodes) {
    if (node.id.empty() || !ids.insert(node.id).second) {
      *why = "its graph node " + node.id + " has no id of its own";
      return false;
    }
    if (node.code.size() != node.outputs.size()) {
      *why = "its graph node " + node.id + " doesn't have code for each of its outputs";
      return false;
    }
  }
  return true;
}

void ForEachTexture(nlohmann::json& graph, const std::function<void(std::string& path, const std::string& where)>& visit) {
  if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()) return;
  for (nlohmann::json& node : graph["nodes"]) {
    if (!node.is_object() || !node.contains("settings") || !node["settings"].is_object()) continue;
    for (auto& [name, value] : node["settings"].items()) {
      // A texture setting: {"asset": path} for a mod's, {"game": name} for one of the game's.
      if (!value.is_object() || !value.contains("asset") || !value["asset"].is_string()) continue;
      std::string path = value["asset"].get<std::string>();
      if (path.empty()) continue;
      visit(path, "a texture node of its graph");
      value["asset"] = path;
    }
  }
}

}  // namespace rexfabric::content::material
