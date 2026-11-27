// Material graphs (docs/materials.md): what a graph material is made of, the kinds of node it can have, and the HLSL
// it compiles to.
//
// A graph is nodes and links, as the node editor's are, but every value is 1 to 4 floats and nothing runs in order:
// the graph is worked out backwards from its Material node, the one node every graph has, whose inputs are its
// material type's graph inputs. Each node reached becomes a line or two of HLSL with its outputs in locals, and the
// Material node's inputs the shading model reads become the fields of MaterialResult.
//
//   {"shading_model": "Basic Lit",
//    "nodes": [{"id": 1, "type": "material.result", "x": 400, "y": 0},
//              {"id": 2, "type": "texture.sample", "x": 0, "y": 0, "settings": {"texture": "MyMod/Brick"}}],
//    "links": [{"from": [2, "RGB"], "to": [1, "Base Colour"]}]}

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <rexfabric/content.h>

#include "materials.h"

namespace rexfabric::content::material {

// The Material node's type.
inline constexpr std::string_view kResult = "material.result";

// A pin: its width in floats, 1 to 4. On an input, 0 is any width; on an output, 0 is as wide as the widest of the
// node's inputs of any width.
struct Pin {
  std::string name;
  uint8_t width = 1;
  std::array<float, 4> initial{};
  // An input's code while nothing links to it, in place of its initial value (a texture sample's UV set 0). It isn't
  // typed in on the node.
  std::string unlinked;
  std::string help;
};

enum class SettingKind : uint8_t {
  kFloat,
  kInt,
  kBool,
  kColour,   // RGBA
  kVector,   // 2 to 4 floats, as many as `width`
  kName,     // a parameter's name
  kTexture,  // {"asset": a mod's Texture asset} or {"game": one of the game's textures by name}
  kChoice,   // one of `choices`
};

struct Setting {
  std::string name;
  SettingKind kind = SettingKind::kFloat;
  nlohmann::json initial;
  uint8_t width = 4;
  std::vector<std::string> choices;
  std::string help;
};

struct Node {
  int id = 0;
  std::string type;
  float x = 0.0f;
  float y = 0.0f;
  nlohmann::json settings = nlohmann::json::object();
  // Values typed into unlinked inputs, by pin name.
  std::map<std::string, std::array<float, 4>> values;
};

// From an output to an input; an input takes one link.
struct Link {
  int from = 0;
  std::string output;
  int to = 0;
  std::string input;
  bool operator==(const Link&) const = default;
};

class Graph {
 public:
  // A graph as a material file keeps it; anything it can't read is left out.
  static Graph Read(const nlohmann::json& json);
  nlohmann::json Write() const;

  Node* Find(int id);
  const Node* Find(int id) const;
  // The Material node, or null.
  const Node* Result() const;
  Node& Add(std::string type, float x, float y);
  // Removes the node and its links.
  void Remove(int id);
  const Link* LinkInto(int node, std::string_view input) const;
  bool HasLinkFrom(int node, std::string_view output) const;
  // Replaces whatever linked into the input.
  void Connect(int from, std::string output, int to, std::string input);
  void Disconnect(int to, std::string_view input);
  // Whether `to` feeds `from` through links, so a link from `from` into `to` would go round in a circle.
  bool Feeds(int to, int from) const;

  std::string shading_model;
  std::vector<Node> nodes;
  std::vector<Link> links;

 private:
  int next_id_ = 1;
};

// A graph with only its Material node, lit by the type's first shading model.
Graph DefaultGraph(const MaterialType& type);

// A value in the code: an HLSL expression and its width.
struct Value {
  std::string code;
  uint8_t width = 1;
};

// What a node's code is written with: the textures and parameters it needs, locals for what it uses more than once,
// and helper functions.
class Compiler {
 public:
  virtual ~Compiler() = default;
  // The index of a texture in the program's list, for MaterialSample2D(c, n, uv) and MaterialSampleCube(c, n, dir).
  virtual int Texture(const MaterialTextureUse& use) = 0;
  // The index of a named parameter, for MaterialParameter(c, n); -1 with a problem when the name is taken by one of
  // another width.
  virtual int Parameter(const std::string& name, uint8_t width, const std::array<float, 4>& value) = 0;
  // A new local holding `code`, for a value read more than once: its name.
  virtual std::string Local(const std::string& code, uint8_t width) = 0;
  // A function the code calls, written once before EvaluateMaterial.
  virtual void Helper(const std::string& name, const std::string& code) = 0;
  // A problem with the node being written.
  virtual void Problem(std::string text) = 0;
};

struct NodeType {
  std::string id;
  std::string title;
  // "Math", "Texture": slashes nest the add menu.
  std::string category;
  std::string help;
  // More to find it by in the add menu: its operator.
  std::string keywords;
  std::vector<Pin> inputs;
  std::vector<Pin> outputs;
  std::vector<Setting> settings;
  // Inputs of any width keep their own (an Append's), rather than all being as wide as the widest.
  bool mixed = false;
  // A row of its own on the canvas showing its main setting (a Constant's value): the setting's name.
  std::string shown;
  // The pins as the node's settings shape them; after `inputs` and `outputs` are copied in.
  std::function<void(const Node& node, std::vector<Pin>& inputs, std::vector<Pin>& outputs)> shape;
  // The output widths from the inputs' (an Append's); unset: each output's own, 0 the widest input of any width.
  std::function<std::vector<uint8_t>(const Node& node, const std::vector<uint8_t>& inputs)> widths;
  // The title with the node's settings in it ("Texture Sample Brick"); unset: `title`.
  std::function<std::string(const Node& node)> title_of;
  // The code of each output, from the inputs' values, each as wide as its pin (an input of any width: the widest of
  // them, or its own for a `mixed` node).
  std::function<std::vector<std::string>(Compiler& compiler, const NodeType& self, const Node& node, const std::vector<Value>& inputs)> emit;
};

// The nodes every graph material may have: math, textures, coordinates, parameters (material_nodes.cpp).
const std::vector<NodeType>& BuiltinNodes();
// Every node a graph of `type` may have: the Material node with its inputs, the built-in nodes and the game's.
std::vector<const NodeType*> NodeTypes(const MaterialType& type);
const NodeType* FindNodeType(const MaterialType& type, std::string_view id);

struct NodePins {
  std::vector<Pin> inputs;
  std::vector<Pin> outputs;
};

// A node's pins, shaped by its settings; none for a node type the type doesn't have.
NodePins PinsOf(const MaterialType& type, const Node& node);
const Pin* FindPin(const std::vector<Pin>& pins, std::string_view name);
std::string TitleOf(const MaterialType& type, const Node& node);
// A setting's value, or its initial value when the node doesn't have it.
nlohmann::json SettingOf(const NodeType& type, const Node& node, std::string_view name);

// The width each of a node's inputs and outputs carries as the graph is now: an input what links into it (or its own,
// unlinked), an output resolved from them.
struct Widths {
  std::vector<uint8_t> inputs;
  std::vector<uint8_t> outputs;
};
Widths WidthsOf(const MaterialType& type, const Graph& graph, const Node& node);

// A link from an output into an input of another node that doesn't go round in a circle; `reason` when not.
bool CanConnect(const MaterialType& type, const Graph& graph, int from, std::string_view output, int to, std::string_view input, std::string* reason);

// The Material node's input a shading model reads.
bool Reads(const MaterialType& type, std::string_view shading_model, std::string_view input);

// The graph compiled, its problems in the program and by node.
struct Compiled {
  MaterialProgram program;
  std::map<int, std::vector<std::string>> node_problems;
};
Compiled Compile(const MaterialType& type, const Graph& graph, std::string_view path);

// A number as HLSL (1.0, 0.25), and `width` floats of a value (float3(1.0, 0.5, 0.0)).
std::string NumberCode(float value);
std::string ConstantCode(const std::array<float, 4>& value, uint8_t width);

// The name of a Material node input's MaterialResult field: "Base Colour" is base_colour.
std::string FieldName(std::string_view pin);

// Problems with what a graph material type declares (its inputs, shading models and nodes); false with `why`.
bool CheckDeclared(const MaterialType& type, std::string* why);

// Each mod texture a graph's nodes sample, by path, for renaming and checking what it names.
void ForEachTexture(nlohmann::json& graph, const std::function<void(std::string& path, const std::string& where)>& visit);

}  // namespace rexfabric::content::material
