// The nodes every graph material may have (docs/materials.md, Nodes), after Unreal's material expressions. Their code
// reads only what every game's shader gives a graph (docs/materials.md, The code): MaterialUv(c, set), c.time,
// c.world_position, c.vertex_normal, c.view, and the texture and parameter calls.

#include <algorithm>
#include <optional>
#include <string>

#include "material_graph.h"

namespace rexfabric::content::material {
namespace {

using Codes = std::vector<std::string>;

constexpr std::array<float, 4> kZero{};
constexpr std::array<float, 4> kOne{1.0f, 1.0f, 1.0f, 1.0f};
constexpr const char* kUv0 = "MaterialUv(c, 0)";
constexpr const char* kReflection = "reflect(-c.view, c.vertex_normal)";

Pin Any(std::string name, float initial = 0.0f) { return {.name = std::move(name), .width = 0, .initial = {initial}}; }
Pin Floats(std::string name, uint8_t width, std::array<float, 4> initial = kZero, std::string unlinked = {}) { return {.name = std::move(name), .width = width, .initial = initial, .unlinked = std::move(unlinked)}; }
Pin Out(std::string name, uint8_t width = 0) { return {.name = std::move(name), .width = width}; }

std::string Code(const std::vector<Value>& in, size_t i) { return in[i].code; }

float FloatSetting(const NodeType& type, const Node& node, std::string_view name) {
  const nlohmann::json value = SettingOf(type, node, name);
  return value.is_number() ? value.get<float>() : 0.0f;
}

std::array<float, 4> FloatsSetting(const NodeType& type, const Node& node, std::string_view name) {
  const nlohmann::json value = SettingOf(type, node, name);
  std::array<float, 4> floats{};
  for (size_t i = 0; value.is_array() && i < value.size() && i < 4; ++i) floats[i] = value[i].is_number() ? value[i].get<float>() : 0.0f;
  return floats;
}


// The outputs of a four float value: RGB, then each channel, then all four.
const std::vector<Pin>& ChannelOutputs() {
  static const std::vector<Pin> outputs = {Out("RGB", 3), Out("R", 1), Out("G", 1), Out("B", 1), Out("A", 1), Out("RGBA", 4)};
  return outputs;
}

Codes Channels(const std::string& value) { return {value + ".rgb", value + ".r", value + ".g", value + ".b", value + ".a", value}; }

std::string TextureName(const nlohmann::json& setting) {
  if (!setting.is_object()) return {};
  if (setting.contains("asset") && setting["asset"].is_string()) {
    const std::string path = setting["asset"].get<std::string>();
    return path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
  }
  if (setting.contains("game") && setting["game"].is_string()) return setting["game"].get<std::string>();
  return {};
}

// The texture a node's Texture setting names, or none with a problem.
std::optional<MaterialTextureUse> TextureOf(Compiler& compiler, const NodeType& type, const Node& node, bool cube) {
  const nlohmann::json setting = SettingOf(type, node, "Texture");
  MaterialTextureUse use{.cube = cube};
  if (setting.is_object() && setting.contains("asset") && setting["asset"].is_string()) use.texture = setting["asset"].get<std::string>();
  else if (setting.is_object() && setting.contains("game") && setting["game"].is_string()) use = {.texture = setting["game"].get<std::string>(), .game = true, .cube = cube};
  if (use.texture.empty()) {
    compiler.Problem("it has no texture: pick one in its details");
    return std::nullopt;
  }
  return use;
}

std::string Parameter(Compiler& compiler, const NodeType& type, const Node& node, uint8_t width, const std::array<float, 4>& value) {
  const nlohmann::json name = SettingOf(type, node, "Name");
  const std::string text = name.is_string() ? name.get<std::string>() : std::string();
  if (text.empty()) {
    compiler.Problem("it has no name: give it one in its details");
    return "0.0";
  }
  const int index = compiler.Parameter(text, width, value);
  return index < 0 ? std::string("0.0") : "MaterialParameter(c, " + std::to_string(index) + ")";
}

class Library {
 public:
  void Unary(std::string id, std::string title, std::string category, std::string function, std::string help, std::string keywords = {}) {
    Add({.id = std::move(id), .title = std::move(title), .category = std::move(category), .help = std::move(help), .keywords = std::move(keywords), .inputs = {Any("In")}, .outputs = {Out("Out")}, .emit = [function](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{function + "(" + Code(in, 0) + ")"}; }});
  }

  void Operator(std::string id, std::string title, std::string op, float initial, std::string help) {
    Add({.id = std::move(id), .title = std::move(title), .category = "Math", .help = std::move(help), .keywords = op, .inputs = {Any("A", initial), Any("B", initial)}, .outputs = {Out("Out")}, .emit = [op](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"(" + Code(in, 0) + " " + op + " " + Code(in, 1) + ")"}; }});
  }

  void Binary(std::string id, std::string title, std::string function, std::array<std::string, 2> names, std::array<float, 2> initial, std::string help) {
    Add({.id = std::move(id), .title = std::move(title), .category = "Math", .help = std::move(help), .inputs = {Any(names[0], initial[0]), Any(names[1], initial[1])}, .outputs = {Out("Out")}, .emit = [function](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{function + "(" + Code(in, 0) + ", " + Code(in, 1) + ")"}; }});
  }

  void Add(NodeType type) { nodes_.push_back(std::move(type)); }

  std::vector<NodeType> Take() { return std::move(nodes_); }

 private:
  std::vector<NodeType> nodes_;
};

void AddConstants(Library& library) {
  library.Add({.id = "constant.float", .title = "Constant", .category = "Constants", .help = "A number", .keywords = "scalar 1", .outputs = {Out("Value", 1)}, .settings = {{.name = "Value", .kind = SettingKind::kFloat, .initial = 1.0}}, .shown = "Value",
               .emit = [](Compiler&, const NodeType& self, const Node& node, const std::vector<Value>&) {
                 return Codes{NumberCode(FloatSetting(self, node, "Value"))};
               }});
  for (uint8_t width = 2; width <= 4; ++width) {
    const std::string id = "constant.float" + std::to_string(width);
    library.Add({.id = id, .title = "Constant " + std::to_string(width) + " Vector", .category = "Constants", .help = std::to_string(width) + " numbers", .keywords = std::to_string(width), .outputs = {Out("Value", width)}, .settings = {{.name = "Value", .kind = SettingKind::kVector, .initial = nlohmann::json::array({0.0, 0.0, 0.0, 1.0}), .width = width}}, .shown = "Value",
                 .emit = [width](Compiler&, const NodeType& self, const Node& node, const std::vector<Value>&) {
                   return Codes{ConstantCode(FloatsSetting(self, node, "Value"), width)};
                 }});
  }
  library.Add({.id = "constant.colour", .title = "Colour", .category = "Constants", .help = "A colour, and its alpha", .keywords = "color rgb constant3", .outputs = ChannelOutputs(), .settings = {{.name = "Colour", .kind = SettingKind::kColour, .initial = nlohmann::json::array({1.0, 1.0, 1.0, 1.0})}}, .shown = "Colour",
               .emit = [](Compiler& compiler, const NodeType& self, const Node& node, const std::vector<Value>&) {
                 return Channels(compiler.Local(ConstantCode(FloatsSetting(self, node, "Colour"), 4), 4));
               }});
  library.Add({.id = "constant.pi", .title = "Pi", .category = "Constants", .help = "Pi, times a number", .outputs = {Out("Value", 1)}, .settings = {{.name = "Times", .kind = SettingKind::kFloat, .initial = 1.0}},
               .emit = [](Compiler&, const NodeType& self, const Node& node, const std::vector<Value>&) {
                 return Codes{"(3.14159265 * " + NumberCode(FloatSetting(self, node, "Times")) + ")"};
               }});
  library.Add({.id = "constant.time", .title = "Time", .category = "Constants", .help = "Seconds the game has run, times Speed, and waves of it", .outputs = {Out("Time", 1), Out("Sine", 1), Out("Cosine", 1), Out("Frac", 1)}, .settings = {{.name = "Speed", .kind = SettingKind::kFloat, .initial = 1.0}},
               .emit = [](Compiler& compiler, const NodeType& self, const Node& node, const std::vector<Value>&) {
                 const std::string time = compiler.Local("c.time * " + NumberCode(FloatSetting(self, node, "Speed")), 1);
                 return Codes{time, "sin(" + time + ")", "cos(" + time + ")", "frac(" + time + ")"};
               }});
}

void AddParameters(Library& library) {
  library.Add({.id = "parameter.scalar", .title = "Scalar Parameter", .category = "Parameters", .help = "A named number, which what's drawn with the material may change without the material changing", .keywords = "float", .outputs = {Out("Value", 1)}, .settings = {{.name = "Name", .kind = SettingKind::kName, .initial = "Amount"}, {.name = "Value", .kind = SettingKind::kFloat, .initial = 1.0}}, .shown = "Value",
               .title_of = [](const Node& node) {
                 const auto name = node.settings.find("Name");
                 return name != node.settings.end() && name->is_string() ? name->get<std::string>() : std::string("Amount");
               },
               .emit = [](Compiler& compiler, const NodeType& self, const Node& node, const std::vector<Value>&) {
                 return Codes{Parameter(compiler, self, node, 1, {FloatSetting(self, node, "Value")}) + ".x"};
               }});
  library.Add({.id = "parameter.vector", .title = "Vector Parameter", .category = "Parameters", .help = "A named colour, which what's drawn with the material may change without the material changing", .keywords = "colour color", .outputs = ChannelOutputs(), .settings = {{.name = "Name", .kind = SettingKind::kName, .initial = "Colour"}, {.name = "Value", .kind = SettingKind::kColour, .initial = nlohmann::json::array({1.0, 1.0, 1.0, 1.0})}}, .shown = "Value",
               .title_of = [](const Node& node) {
                 const auto name = node.settings.find("Name");
                 return name != node.settings.end() && name->is_string() ? name->get<std::string>() : std::string("Colour");
               },
               .emit = [](Compiler& compiler, const NodeType& self, const Node& node, const std::vector<Value>&) {
                 return Channels(compiler.Local(Parameter(compiler, self, node, 4, FloatsSetting(self, node, "Value")), 4));
               }});
}

void AddCoordinates(Library& library) {
  library.Add({.id = "coordinates.uv", .title = "Texture Coordinate", .category = "Coordinates", .help = "A UV set of the model, tiled", .keywords = "uv texcoord", .outputs = {Out("UV", 2)}, .settings = {{.name = "Set", .kind = SettingKind::kInt, .initial = 0, .help = "Which of the model's UV sets, 0 to 3"}, {.name = "Tiling", .kind = SettingKind::kVector, .initial = nlohmann::json::array({1.0, 1.0}), .width = 2}},
               .emit = [](Compiler&, const NodeType& self, const Node& node, const std::vector<Value>&) {
                 const int set = std::clamp(static_cast<int>(FloatSetting(self, node, "Set")), 0, 3);
                 return Codes{"(MaterialUv(c, " + std::to_string(set) + ") * " + ConstantCode(FloatsSetting(self, node, "Tiling"), 2) + ")"};
               }});
  library.Add({.id = "coordinates.panner", .title = "Panner", .category = "Coordinates", .help = "UVs moving over time: scrolling water, lava", .keywords = "scroll", .inputs = {Floats("UV", 2, kZero, kUv0), Floats("Speed", 2, {0.1f, 0.0f}), Floats("Time", 1, kZero, "c.time")}, .outputs = {Out("UV", 2)},
               .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"(" + Code(in, 0) + " + " + Code(in, 1) + " * " + Code(in, 2) + ")"}; }});
  library.Add({.id = "coordinates.rotator", .title = "Rotator", .category = "Coordinates", .help = "UVs turned about a centre by an angle in radians", .keywords = "rotate spin", .inputs = {Floats("UV", 2, kZero, kUv0), Floats("Centre", 2, {0.5f, 0.5f}), Floats("Angle", 1, kZero, "c.time")}, .outputs = {Out("UV", 2)},
               .emit = [](Compiler& compiler, const NodeType&, const Node&, const std::vector<Value>& in) {
                 compiler.Helper("MaterialRotate", "float2 MaterialRotate(float2 uv, float2 centre, float angle) {\n  float s = sin(angle), k = cos(angle);\n  float2 d = uv - centre;\n  return centre + float2(d.x * k - d.y * s, d.x * s + d.y * k);\n}");
                 return Codes{"MaterialRotate(" + Code(in, 0) + ", " + Code(in, 1) + ", " + Code(in, 2) + ")"};
               }});
  library.Add({.id = "coordinates.world_position", .title = "World Position", .category = "Coordinates", .help = "Where the point drawn is in the world", .outputs = {Out("Position", 3)}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>&) { return Codes{"c.world_position"}; }});
  library.Add({.id = "coordinates.vertex_normal", .title = "Vertex Normal", .category = "Coordinates", .help = "The way the model's surface faces, in the world, before a normal map", .outputs = {Out("Normal", 3)}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>&) { return Codes{"c.vertex_normal"}; }});
  library.Add({.id = "coordinates.camera_vector", .title = "Camera Vector", .category = "Coordinates", .help = "From the point drawn toward the camera, one unit long", .keywords = "view eye", .outputs = {Out("Direction", 3)}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>&) { return Codes{"c.view"}; }});
  library.Add({.id = "coordinates.reflection", .title = "Reflection Vector", .category = "Coordinates", .help = "The camera's view bounced off the surface, for sampling a cube map", .outputs = {Out("Direction", 3)}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>&) { return Codes{kReflection}; }});
}

void AddTextures(Library& library) {
  const Setting texture{.name = "Texture", .kind = SettingKind::kTexture, .initial = nlohmann::json::object(), .help = "One of the mod's textures, or one of the game's"};
  const auto title = [](std::string base) {
    return [base](const Node& node) {
      const auto setting = node.settings.find("Texture");
      const std::string name = setting != node.settings.end() ? TextureName(*setting) : std::string();
      return name.empty() ? base : base + " " + name;
    };
  };
  library.Add({.id = "texture.sample", .title = "Texture Sample", .category = "Texture", .help = "A texture's colour at a UV. As a normal map, its x and y are a two channel normal map's, unpacked", .keywords = "image picture",
               .inputs = {Floats("UV", 2, kZero, kUv0)}, .outputs = ChannelOutputs(), .settings = {texture, {.name = "Normal Map", .kind = SettingKind::kBool, .initial = false, .help = "Unpacks a two channel normal map: RGB is the normal in the surface's own space"}}, .title_of = title("Texture"),
               .emit = [](Compiler& compiler, const NodeType& self, const Node& node, const std::vector<Value>& in) {
                 const std::optional<MaterialTextureUse> use = TextureOf(compiler, self, node, false);
                 if (!use) return Channels("float4(0.0, 0.0, 0.0, 1.0)");
                 std::string sample = compiler.Local("MaterialSample2D(c, " + std::to_string(compiler.Texture(*use)) + ", " + Code(in, 0) + ")", 4);
                 const nlohmann::json normal = SettingOf(self, node, "Normal Map");
                 if (normal.is_boolean() && normal.get<bool>()) {
                   compiler.Helper("MaterialUnpackNormal", "float4 MaterialUnpackNormal(float4 t) {\n  float2 xy = t.xy * 2.0 - 1.0;\n  return float4(xy, sqrt(saturate(1.0 - dot(xy, xy))), t.w);\n}");
                   sample = compiler.Local("MaterialUnpackNormal(" + sample + ")", 4);
                 }
                 return Channels(sample);
               }});
  library.Add({.id = "texture.cube", .title = "Cube Sample", .category = "Texture", .help = "A cube map's colour in a direction: a reflection of the sky, as the game's metals have", .keywords = "environment reflection",
               .inputs = {Floats("Direction", 3, kZero, kReflection)}, .outputs = ChannelOutputs(), .settings = {texture}, .title_of = title("Cube"),
               .emit = [](Compiler& compiler, const NodeType& self, const Node& node, const std::vector<Value>& in) {
                 const std::optional<MaterialTextureUse> use = TextureOf(compiler, self, node, true);
                 if (!use) return Channels("float4(0.0, 0.0, 0.0, 1.0)");
                 return Channels(compiler.Local("MaterialSampleCube(c, " + std::to_string(compiler.Texture(*use)) + ", " + Code(in, 0) + ")", 4));
               }});
}

void AddMath(Library& library) {
  library.Operator("math.add", "Add", "+", 0.0f, "A plus B");
  library.Operator("math.subtract", "Subtract", "-", 0.0f, "A minus B");
  library.Operator("math.multiply", "Multiply", "*", 1.0f, "A times B");
  library.Operator("math.divide", "Divide", "/", 1.0f, "A divided by B");
  library.Binary("math.min", "Min", "min", {"A", "B"}, {0.0f, 0.0f}, "The smaller of A and B");
  library.Binary("math.max", "Max", "max", {"A", "B"}, {0.0f, 0.0f}, "The larger of A and B");
  library.Binary("math.fmod", "Fmod", "fmod", {"A", "B"}, {0.0f, 1.0f}, "What's left of A once B is taken out of it as often as it goes");
  library.Binary("math.step", "Step", "step", {"Edge", "X"}, {0.5f, 0.0f}, "1 where X is at least Edge, else 0");
  library.Binary("math.atan2", "Arctangent2", "atan2", {"Y", "X"}, {0.0f, 1.0f}, "The angle of (X, Y)");
  library.Add({.id = "math.power", .title = "Power", .category = "Math", .help = "Base to the power of Exp (a negative base counts as 0)", .keywords = "pow exponent", .inputs = {Any("Base", 1.0f), Any("Exp", 2.0f)}, .outputs = {Out("Out")}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"pow(max(" + Code(in, 0) + ", 0.0), " + Code(in, 1) + ")"}; }});
  library.Add({.id = "math.lerp", .title = "Lerp", .category = "Math", .help = "From A at Alpha 0 to B at Alpha 1", .keywords = "linear interpolate blend mix", .inputs = {Any("A", 0.0f), Any("B", 1.0f), Any("Alpha", 0.5f)}, .outputs = {Out("Out")}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"lerp(" + Code(in, 0) + ", " + Code(in, 1) + ", " + Code(in, 2) + ")"}; }});
  library.Add({.id = "math.clamp", .title = "Clamp", .category = "Math", .help = "In, kept between Min and Max", .inputs = {Any("In"), Any("Min", 0.0f), Any("Max", 1.0f)}, .outputs = {Out("Out")}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"clamp(" + Code(in, 0) + ", " + Code(in, 1) + ", " + Code(in, 2) + ")"}; }});
  library.Add({.id = "math.smoothstep", .title = "SmoothStep", .category = "Math", .help = "0 below Min, 1 above Max, a smooth curve between", .inputs = {Any("Min", 0.0f), Any("Max", 1.0f), Any("Value")}, .outputs = {Out("Out")}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"smoothstep(" + Code(in, 0) + ", " + Code(in, 1) + ", " + Code(in, 2) + ")"}; }});
  library.Add({.id = "math.remap", .title = "Remap Value Range", .category = "Math", .help = "In taken from the range In Min to In Max into Out Min to Out Max", .keywords = "range map", .inputs = {Any("In"), Any("In Min", 0.0f), Any("In Max", 1.0f), Any("Out Min", 0.0f), Any("Out Max", 1.0f)}, .outputs = {Out("Out")},
               .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"(" + Code(in, 3) + " + (" + Code(in, 0) + " - " + Code(in, 1) + ") / (" + Code(in, 2) + " - " + Code(in, 1) + ") * (" + Code(in, 4) + " - " + Code(in, 3) + "))"}; }});
  library.Add({.id = "math.one_minus", .title = "One Minus", .category = "Math", .help = "1 minus In", .keywords = "invert 1-x", .inputs = {Any("In")}, .outputs = {Out("Out")}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"(1.0 - " + Code(in, 0) + ")"}; }});
  library.Add({.id = "math.negate", .title = "Negate", .category = "Math", .help = "Minus In", .keywords = "-x", .inputs = {Any("In")}, .outputs = {Out("Out")}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"(-" + Code(in, 0) + ")"}; }});
  library.Add({.id = "math.reciprocal", .title = "Reciprocal", .category = "Math", .help = "1 divided by In", .keywords = "1/x", .inputs = {Any("In", 1.0f)}, .outputs = {Out("Out")}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"(1.0 / " + Code(in, 0) + ")"}; }});
  library.Add({.id = "math.square", .title = "Square", .category = "Math", .help = "In times itself", .keywords = "x*x", .inputs = {Any("In")}, .outputs = {Out("Out")}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"(" + Code(in, 0) + " * " + Code(in, 0) + ")"}; }});
  const std::pair<const char*, const char*> unary[] = {{"saturate", "Saturate"}, {"abs", "Abs"}, {"frac", "Frac"}, {"floor", "Floor"}, {"ceil", "Ceil"}, {"round", "Round"}, {"trunc", "Truncate"}, {"sign", "Sign"}, {"sqrt", "Square Root"}, {"sin", "Sine"}, {"cos", "Cosine"}, {"tan", "Tangent"}, {"asin", "Arcsine"}, {"acos", "Arccosine"}, {"atan", "Arctangent"}, {"exp", "Exp"}, {"exp2", "Exp2"}, {"log", "Log"}, {"log2", "Log2"}, {"log10", "Log10"}};
  for (const auto& [function, title] : unary) library.Unary(std::string("math.") + function, title, "Math", function, std::string(title) + " of In", function);
  library.Add({.id = "math.if", .title = "If", .category = "Math", .help = "One of three values, by how A compares with B", .keywords = "compare branch", .inputs = {Floats("A", 1), Floats("B", 1), Any("A > B", 1.0f), Any("A == B", 0.0f), Any("A < B", 0.0f)}, .outputs = {Out("Out")},
               .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"(" + Code(in, 0) + " > " + Code(in, 1) + " ? " + Code(in, 2) + " : (" + Code(in, 0) + " < " + Code(in, 1) + " ? " + Code(in, 4) + " : " + Code(in, 3) + "))"}; }});
}

void AddVectors(Library& library) {
  library.Add({.id = "vector.normalize", .title = "Normalize", .category = "Vector", .help = "In made one unit long", .keywords = "unit", .inputs = {Any("In", 1.0f)}, .outputs = {Out("Out")}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"normalize(" + Code(in, 0) + ")"}; }});
  library.Add({.id = "vector.dot", .title = "Dot", .category = "Vector", .help = "The dot product of A and B", .keywords = "dot product", .inputs = {Any("A"), Any("B")}, .outputs = {Out("Out", 1)}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"dot(" + Code(in, 0) + ", " + Code(in, 1) + ")"}; }});
  library.Add({.id = "vector.cross", .title = "Cross", .category = "Vector", .help = "The cross product of A and B", .inputs = {Floats("A", 3, {1.0f}), Floats("B", 3, {0.0f, 1.0f})}, .outputs = {Out("Out", 3)}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"cross(" + Code(in, 0) + ", " + Code(in, 1) + ")"}; }});
  library.Add({.id = "vector.length", .title = "Length", .category = "Vector", .help = "How long In is", .inputs = {Any("In")}, .outputs = {Out("Out", 1)}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"length(" + Code(in, 0) + ")"}; }});
  library.Add({.id = "vector.distance", .title = "Distance", .category = "Vector", .help = "How far A is from B", .inputs = {Any("A"), Any("B")}, .outputs = {Out("Out", 1)}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"distance(" + Code(in, 0) + ", " + Code(in, 1) + ")"}; }});
  library.Add({.id = "vector.reflect", .title = "Reflect", .category = "Vector", .help = "Direction bounced off a surface facing Normal", .inputs = {Floats("Direction", 3, {0.0f, 0.0f, -1.0f}), Floats("Normal", 3, {0.0f, 0.0f, 1.0f})}, .outputs = {Out("Out", 3)}, .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"reflect(" + Code(in, 0) + ", " + Code(in, 1) + ")"}; }});
  library.Add({.id = "vector.mask", .title = "Component Mask", .category = "Vector", .help = "Some of In's channels, in order", .keywords = "swizzle channels", .inputs = {Any("In")}, .outputs = {Out("Out")},
               .settings = {{.name = "R", .kind = SettingKind::kBool, .initial = true}, {.name = "G", .kind = SettingKind::kBool, .initial = true}, {.name = "B", .kind = SettingKind::kBool, .initial = false}, {.name = "A", .kind = SettingKind::kBool, .initial = false}}, .mixed = true,
               .widths = [](const Node& node, const std::vector<uint8_t>&) {
                 uint8_t count = 0;
                 for (const char* channel : {"R", "G", "B", "A"}) {
                   const auto found = node.settings.find(channel);
                   const bool on = found != node.settings.end() ? found->is_boolean() && found->get<bool>() : (channel[0] == 'R' || channel[0] == 'G');
                   count += on ? 1 : 0;
                 }
                 return std::vector<uint8_t>{std::max<uint8_t>(count, 1)};
               },
               .title_of = [](const Node& node) {
                 std::string title = "Mask (";
                 for (const char* channel : {"R", "G", "B", "A"}) {
                   const auto found = node.settings.find(channel);
                   if (found != node.settings.end() ? found->is_boolean() && found->get<bool>() : (channel[0] == 'R' || channel[0] == 'G')) title += channel;
                 }
                 return title + ")";
               },
               .emit = [](Compiler& compiler, const NodeType&, const Node& node, const std::vector<Value>& in) {
                 std::string swizzle;
                 const char* names = "xyzw";
                 for (int i = 0; i < 4; ++i) {
                   const char channel[2] = {"RGBA"[i], 0};
                   const auto found = node.settings.find(channel);
                   const bool on = found != node.settings.end() ? found->is_boolean() && found->get<bool>() : i < 2;
                   if (!on) continue;
                   if (in[0].width > 1 && i >= in[0].width) compiler.Problem(std::string("In has no ") + channel + " channel");
                   swizzle += names[in[0].width > 1 ? i : 0];
                 }
                 if (swizzle.empty()) swizzle = "x";
                 return Codes{"(" + Code(in, 0) + ")." + swizzle};
               }});
  library.Add({.id = "vector.append", .title = "Append", .category = "Vector", .help = "A's channels, then B's: two floats make a float2", .keywords = "combine", .inputs = {Any("A"), Any("B")}, .outputs = {Out("Out")}, .mixed = true,
               .widths = [](const Node&, const std::vector<uint8_t>& in) { return std::vector<uint8_t>{static_cast<uint8_t>(std::min(4, in[0] + in[1]))}; },
               .emit = [](Compiler& compiler, const NodeType&, const Node&, const std::vector<Value>& in) {
                 static constexpr const char* kNames[] = {"float", "float", "float2", "float3", "float4"};
                 if (in[0].width + in[1].width > 4) compiler.Problem("A and B together are more than four floats");
                 return Codes{std::string(kNames[std::min(4, in[0].width + in[1].width)]) + "(" + Code(in, 0) + ", " + Code(in, 1) + ")"};
               }});
  library.Add({.id = "vector.break", .title = "Break Out Components", .category = "Vector", .help = "Each of In's channels on its own", .keywords = "split", .inputs = {Floats("In", 4)}, .outputs = {Out("R", 1), Out("G", 1), Out("B", 1), Out("A", 1)},
               .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{Code(in, 0) + ".x", Code(in, 0) + ".y", Code(in, 0) + ".z", Code(in, 0) + ".w"}; }});
  for (uint8_t width = 2; width <= 4; ++width) {
    std::vector<Pin> inputs;
    for (uint8_t i = 0; i < width; ++i) inputs.push_back(Floats(std::string(1, "RGBA"[i]), 1, i == 3 ? kOne : kZero));
    static constexpr const char* kNames[] = {"float", "float", "float2", "float3", "float4"};
    library.Add({.id = "vector.make" + std::to_string(width), .title = "Make Float" + std::to_string(width), .category = "Vector", .help = "A " + std::string(kNames[width]) + " of its channels", .inputs = inputs, .outputs = {Out("Out", width)},
                 .emit = [width](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) {
                   std::string code = std::string(kNames[width]) + "(";
                   for (uint8_t i = 0; i < width; ++i) code += (i ? ", " : "") + Code(in, i);
                   return Codes{code + ")"};
                 }});
  }
}

void AddColour(Library& library) {
  library.Add({.id = "colour.desaturation", .title = "Desaturation", .category = "Colour", .help = "A colour toward its grey by Fraction", .keywords = "grey gray", .inputs = {Floats("In", 3, kOne), Floats("Fraction", 1, kOne)}, .outputs = {Out("Out", 3)},
               .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"lerp(" + Code(in, 0) + ", dot(" + Code(in, 0) + ", float3(0.3, 0.59, 0.11)).xxx, " + Code(in, 1) + ")"}; }});
  library.Add({.id = "colour.hue_shift", .title = "Hue Shift", .category = "Colour", .help = "A colour's hue turned by Amount, a whole turn at 1", .keywords = "rainbow", .inputs = {Floats("In", 3, {1.0f}), Floats("Amount", 1)}, .outputs = {Out("Out", 3)},
               .emit = [](Compiler& compiler, const NodeType&, const Node&, const std::vector<Value>& in) {
                 compiler.Helper("MaterialHueShift", "float3 MaterialHueShift(float3 colour, float amount) {\n  const float3 k = 0.57735026;\n  float a = amount * 6.28318531;\n  return colour * cos(a) + cross(k, colour) * sin(a) + k * dot(k, colour) * (1.0 - cos(a));\n}");
                 return Codes{"MaterialHueShift(" + Code(in, 0) + ", " + Code(in, 1) + ")"};
               }});
  library.Add({.id = "colour.fresnel", .title = "Fresnel", .category = "Colour", .help = "How edge on the surface is to the camera: 0 facing it, 1 at the rim", .keywords = "rim edge", .inputs = {Floats("Exponent", 1, {5.0f}), Floats("Base Reflect", 1, {0.04f}), Floats("Normal", 3, kZero, "c.vertex_normal")}, .outputs = {Out("Out", 1)},
               .emit = [](Compiler&, const NodeType&, const Node&, const std::vector<Value>& in) { return Codes{"(" + Code(in, 1) + " + (1.0 - " + Code(in, 1) + ") * pow(1.0 - saturate(dot(normalize(" + Code(in, 2) + "), c.view)), " + Code(in, 0) + "))"}; }});
  library.Add({.id = "colour.noise", .title = "Noise", .category = "Colour", .help = "Smooth value noise over a position, 0 to 1", .keywords = "random clouds", .inputs = {Floats("Position", 3, kZero, "c.world_position"), Floats("Scale", 1, {0.1f})}, .outputs = {Out("Out", 1)},
               .emit = [](Compiler& compiler, const NodeType&, const Node&, const std::vector<Value>& in) {
                 compiler.Helper("MaterialHash", "float MaterialHash(float3 p) {\n  p = frac(p * 0.3183099 + 0.1);\n  p *= 17.0;\n  return frac(p.x * p.y * p.z * (p.x + p.y + p.z));\n}");
                 compiler.Helper("MaterialNoise", "float MaterialNoise(float3 x) {\n  float3 i = floor(x), f = frac(x);\n  f = f * f * (3.0 - 2.0 * f);\n  return lerp(lerp(lerp(MaterialHash(i), MaterialHash(i + float3(1, 0, 0)), f.x), lerp(MaterialHash(i + float3(0, 1, 0)), MaterialHash(i + float3(1, 1, 0)), f.x), f.y),\n              lerp(lerp(MaterialHash(i + float3(0, 0, 1)), MaterialHash(i + float3(1, 0, 1)), f.x), lerp(MaterialHash(i + float3(0, 1, 1)), MaterialHash(i + float3(1, 1, 1)), f.x), f.y), f.z);\n}");
                 return Codes{"MaterialNoise(" + Code(in, 0) + " * " + Code(in, 1) + ")"};
               }});
}

}  // namespace

const std::vector<NodeType>& BuiltinNodes() {
  static const std::vector<NodeType> nodes = [] {
    Library library;
    AddConstants(library);
    AddParameters(library);
    AddCoordinates(library);
    AddTextures(library);
    AddMath(library);
    AddVectors(library);
    AddColour(library);
    return library.Take();
  }();
  return nodes;
}

}  // namespace rexfabric::content::material
