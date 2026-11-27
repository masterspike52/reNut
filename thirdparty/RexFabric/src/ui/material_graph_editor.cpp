#include "material_graph_editor.h"

#include <algorithm>
#include <cfloat>
#include <functional>
#include <optional>
#include <set>

#include <imgui.h>
#include <imgui_node_editor.h>

#include "../content/components.h"
#include "../content/material_graph.h"
#include "../content/materials.h"
#include "../content/values.h"
#include "avatar_viewport.h"
#include "editor_parts.h"
#include "glass.h"
#include "node_graph_style.h"
#include "node_graph_widgets.h"
#include "value_widgets.h"
#include "widgets.h"

namespace ed = ax::NodeEditor;

namespace rexfabric::ui {
namespace {

using content::MaterialType;
using content::material::Graph;
using content::material::Link;
using content::material::Node;
using content::material::NodePins;
using content::material::NodeType;
using content::material::Pin;
using content::material::Setting;
using content::material::SettingKind;
using content::material::Widths;
using node_widgets::PinShape;

// The window is wider than most, for the viewport and details beside the canvas, which start at this share of it.
constexpr float kWidthFactor = 2.6f;
constexpr float kLeftShare = 0.28f;
constexpr float kLeftMinWidth = 18.0f;
// The share of the canvas's height the code takes while it's shown.
constexpr float kCodeShare = 0.35f;
// In multiples of the font size.
constexpr float kInlineFloatWidth = 3.5f;
constexpr float kFitMargin = 3.0f;
constexpr float kPasteOffset = 40.0f;
constexpr float kLinkThickness = 2.0f;
constexpr float kActivePulseSeconds = 1.2f;

// Canvas ids: nodes are the graph's own ids; pins and links are moved out of their way. A link is known by the input
// it goes into, which takes one.
constexpr uint64_t kPinBase = uint64_t(1) << 40;
constexpr uint64_t kLinkBase = uint64_t(2) << 40;
constexpr uint64_t kPinsPerNode = 128;
constexpr uint64_t kOutputOffset = 64;

constexpr const char* kAddMenu = "AddMaterialNode";
// What a copy of nodes says it is on the clipboard.
constexpr const char* kClipboardKind = "rexfabric.material_nodes";

constexpr ImVec4 kAcceptColor{0.4f, 1.0f, 0.4f, 1.0f};
constexpr ImVec4 kRejectColor{1.0f, 0.3f, 0.3f, 1.0f};
constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};
constexpr ImVec4 kTitleColor{1.0f, 1.0f, 1.0f, 0.95f};
constexpr ImVec4 kResultColor{1.0f, 0.78f, 0.35f, 1.0f};
constexpr ImVec4 kPaneTint{0.05f, 0.06f, 0.11f, 0.42f};

ed::NodeId NodeId(int node) { return ed::NodeId(static_cast<size_t>(node)); }
ed::PinId PinId(int node, size_t index, bool output) { return ed::PinId(static_cast<size_t>(kPinBase + static_cast<uint64_t>(node) * kPinsPerNode + (output ? kOutputOffset : 0) + index)); }
ed::LinkId LinkId(int node, size_t input) { return ed::LinkId(static_cast<size_t>(kLinkBase + static_cast<uint64_t>(node) * kPinsPerNode + input)); }

struct DecodedPin {
  int node = 0;
  size_t index = 0;
  bool output = false;
};

std::optional<DecodedPin> DecodePin(uint64_t value, uint64_t base) {
  if (value < base || value >= base + (uint64_t(1) << 40)) return std::nullopt;
  const uint64_t local = value - base;
  const uint64_t slot = local % kPinsPerNode;
  const bool output = slot >= kOutputOffset;
  return DecodedPin{static_cast<int>(local / kPinsPerNode), static_cast<size_t>(output ? slot - kOutputOffset : slot), output};
}

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

bool Same(std::string_view a, std::string_view b) { return Lower(a) == Lower(b); }

const char* WidthName(uint8_t width) {
  static constexpr const char* kNames[] = {"any width", "float", "float2", "float3", "float4"};
  return kNames[std::min<uint8_t>(width, 4)];
}

// A pin's color by how many floats it carries, so links of the same width match.
ImU32 WidthColor(uint8_t width) {
  switch (width) {
    case 1: return IM_COL32(150, 220, 110, 255);
    case 2: return IM_COL32(110, 200, 240, 255);
    case 3: return IM_COL32(240, 205, 90, 255);
    case 4: return IM_COL32(235, 125, 205, 255);
    default: return IM_COL32(190, 190, 190, 255);
  }
}

ImVec4 CategoryColor(const NodeType* type) {
  if (!type) return kRejectColor;
  if (type->id == content::material::kResult) return kResultColor;
  const std::string_view category = type->category;
  if (category.starts_with("Constants")) return ImVec4(0.55f, 0.85f, 0.45f, 1.0f);
  if (category.starts_with("Parameters")) return ImVec4(0.35f, 0.85f, 0.75f, 1.0f);
  if (category.starts_with("Coordinates")) return ImVec4(1.0f, 0.5f, 0.45f, 1.0f);
  if (category.starts_with("Texture")) return ImVec4(0.45f, 0.62f, 1.0f, 1.0f);
  if (category.starts_with("Vector")) return ImVec4(0.75f, 0.75f, 0.85f, 1.0f);
  if (category.starts_with("Colour")) return ImVec4(0.95f, 0.55f, 0.85f, 1.0f);
  if (category.starts_with("Math")) return ImVec4(0.6f, 0.62f, 0.7f, 1.0f);
  return ImVec4(1.0f, 0.65f, 0.3f, 1.0f);  // the game's own
}

ImVec4 FloatsColor(const std::array<float, 4>& value) { return ImVec4(value[0], value[1], value[2], 1.0f); }

std::array<float, 4> FloatsOf(const nlohmann::json& json, std::array<float, 4> fallback = {}) {
  for (size_t i = 0; json.is_array() && i < json.size() && i < 4; ++i) {
    if (json[i].is_number()) fallback[i] = json[i].get<float>();
  }
  if (json.is_number()) fallback[0] = json.get<float>();
  return fallback;
}

nlohmann::json JsonOf(const std::array<float, 4>& value) { return nlohmann::json::array({value[0], value[1], value[2], value[3]}); }

// A graph material: its graph on a canvas, its preview, and the details of it or of a node.
class MaterialGraphEditor final : public DocumentEditor<content::MaterialAsset> {
 public:
  MaterialGraphEditor(EditorHost& host, std::string path, content::MaterialAsset asset) : DocumentEditor(host, std::move(path), std::move(asset)) {
    Resolve();
    ResetCanvas();
  }
  // What uses it goes back to the material as it was saved.
  ~MaterialGraphEditor() override {
    content::EndMaterialPreview(path_);
    if (canvas_) ed::DestroyEditor(canvas_);
  }

  bool Save() override;
  void ContentChanged() override { Resolve(); }

 protected:
  std::string Title() const override { return std::string(content::NameOf(path_)) + " (material)"; }
  float WidthFactor() const override { return kWidthFactor; }
  void DrawContents() override;

 private:
  // A pin a link was dragged from into empty space; the node added next connects to it.
  struct PendingPin {
    int node = 0;
    std::string pin;
    bool output = false;
  };

  void Resolve();
  void ResetCanvas();
  std::shared_ptr<const MaterialType> Type() const { return content::FindMaterialType(document_.type); }
  // The graph as the document has it, read again after an undo or a redo.
  void ReadGraph();
  // An edit to the graph: the document takes it.
  void Changed();
  void Compile(const MaterialType& type);

  // Left
  void DrawViewport(float height);
  void DrawDetails(const MaterialType& type);
  void DrawMaterialDetails(const MaterialType& type);
  void DrawNodeDetails(const MaterialType& type, Node& node);
  bool DrawSetting(const MaterialType& type, const NodeType& desc, Node& node, const Setting& setting);
  bool DrawTextureSetting(const MaterialType& type, Node& node, const Setting& setting);
  bool DrawInputValue(Node& node, const Pin& pin);

  // Middle
  void DrawCanvas(const MaterialType& type, float height);
  void FitView();
  void DrawNode(const MaterialType& type, Node& node);
  bool DrawInlineValue(Node& node, const Pin& pin);
  bool DrawShownSetting(const NodeType& desc, Node& node);
  void HandleCreate(const MaterialType& type);
  void HandleDelete(const MaterialType& type);
  void HandleClipboard(const MaterialType& type);
  std::string Copy(const std::vector<int>& nodes) const;
  void Paste(const MaterialType& type, const std::string& text, float x, float y);
  void DrawAddMenu(const MaterialType& type);
  void AddNode(const MaterialType& type, const NodeType& desc);
  void SyncFromCanvas();
  void DrawCode();

  std::vector<EditorProblem> Problems();

  TypeChoices choices_;
  std::unique_ptr<AvatarViewport> viewport_;
  std::vector<content::Component> none_;  // the viewport's own components: a material has none
  std::string selected_component_;

  Graph graph_;
  // The document's graph as graph_ was last read from it or written to it.
  nlohmann::json read_;
  bool read_once_ = false;
  content::material::Compiled compiled_;
  nlohmann::json compiled_graph_;
  std::string compiled_type_;
  bool compiled_once_ = false;

  ed::EditorContext* canvas_ = nullptr;
  NodeGraphStyle style_;
  bool place_nodes_ = true;
  bool fit_view_ = true;
  bool moving_ = false;
  int selected_node_ = 0;
  std::string pin_tooltip_;

  bool open_add_menu_ = false;
  float add_x_ = 0.0f;
  float add_y_ = 0.0f;
  std::string add_filter_;
  std::optional<PendingPin> pending_;
  bool show_code_ = false;
};

void MaterialGraphEditor::Resolve() {
  const ModInfo* mod = Mod();
  choices_ = mod ? host_.Choices(mod->folder) : TypeChoices{};
}

void MaterialGraphEditor::ResetCanvas() {
  if (canvas_) ed::DestroyEditor(canvas_);
  // Node positions are kept in the graph, so the canvas keeps nothing in imgui.ini.
  ed::Config config;
  config.SettingsName = nullptr;
  canvas_ = ed::CreateEditor(&config);
  ed::SetCurrentEditor(canvas_);
  NodeGraphStyle::Apply();
  ed::SetCurrentEditor(nullptr);
  style_ = NodeGraphStyle();
  place_nodes_ = true;
  fit_view_ = true;
}

bool MaterialGraphEditor::Save() {
  const std::optional<std::filesystem::path> file = File();
  std::string error;
  if (!file || !content::WriteMaterialAsset(*file, document_, &error)) {
    if (file) SetStatus(error, true);
    return false;
  }
  MarkSaved();
  host_.Saved(path_);
  SavedStatus(!Problems().empty());
  return true;
}

void MaterialGraphEditor::ReadGraph() {
  if (read_once_ && document_.graph == read_) return;
  graph_ = Graph::Read(document_.graph);
  read_ = document_.graph;
  read_once_ = true;
  place_nodes_ = true;
  if (selected_node_ && !graph_.Find(selected_node_)) selected_node_ = 0;
}

void MaterialGraphEditor::Changed() {
  document_.graph = graph_.Write();
  read_ = document_.graph;
}

void MaterialGraphEditor::Compile(const MaterialType& type) {
  if (compiled_once_ && compiled_graph_ == document_.graph && compiled_type_ == document_.type) return;
  compiled_ = content::material::Compile(type, graph_, path_);
  compiled_graph_ = document_.graph;
  compiled_type_ = document_.type;
  compiled_once_ = true;
}

void MaterialGraphEditor::DrawContents() {
  DrawToolbar(CanUndo(), CanRedo());
  const std::shared_ptr<const MaterialType> type = Type();
  if (type && type->HasGraph()) {
    ReadGraph();
    Compile(*type);
  }
  DrawProblems(Problems());
  if (!type || !type->HasGraph()) {
    ImGui::TextDisabled("The game has no graph material type %s", document_.type.c_str());
    return;
  }

  const float font = ImGui::GetFontSize();
  const ImVec2 available = ImGui::GetContentRegionAvail();
  const float left = std::max(available.x * kLeftShare, font * kLeftMinWidth);
  glass::BeginChild("##left", ImVec2(left, available.y), ImGuiChildFlags_ResizeX, 0, kPaneTint);
  DrawViewport(std::min(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y * 0.5f));
  ImGui::BeginChild("##details", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
  DrawDetails(*type);
  ImGui::EndChild();
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("##middle", ImVec2(0.0f, available.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  if (glass::SmallButton(show_code_ ? "Hide Code" : "Show Code")) show_code_ = !show_code_;
  ImGui::SetItemTooltip("The HLSL the graph makes, which the game compiles into its shader");
  ImGui::SameLine();
  if (glass::SmallButton("Fit")) fit_view_ = true;
  ImGui::SetItemTooltip("Fits the graph to the canvas");
  ImGui::SameLine();
  ImGui::TextDisabled("%s, %zu textures, %zu parameters", compiled_.program.shading_model.empty() ? "no shading model" : compiled_.program.shading_model.c_str(), compiled_.program.textures.size(), compiled_.program.parameters.size());
  const float canvas_height = ImGui::GetContentRegionAvail().y * (show_code_ ? 1.0f - kCodeShare : 1.0f);
  DrawCanvas(*type, canvas_height);
  if (show_code_) DrawCode();
  ImGui::EndChild();

  // What's drawn with it shows it as it is here, saved or not.
  content::PreviewMaterial(path_, document_);
  // A node being dragged is one edit, kept once it's let go.
  if (!moving_) Remember();
}

// The type's preview: its component alone, with the type's values, this material, and what it's shown on.
void MaterialGraphEditor::DrawViewport(float height) {
  const std::shared_ptr<const MaterialType> type = Type();
  if (!viewport_) viewport_ = std::make_unique<AvatarViewport>();
  std::vector<content::Component> shown;
  if (type && !type->preview_component.empty()) {
    content::Component preview{.name = "Preview", .type = type->preview_component};
    for (const auto& [name, value] : type->preview_values) preview.properties[name] = content::ToJson(value);
    if (!type->preview_material.empty()) preview.properties[type->preview_material] = path_;
    if (!type->preview_choice.empty() && !document_.preview.is_null()) preview.properties[type->preview_choice] = document_.preview;
    shown.push_back(std::move(preview));
  }
  viewport_->Draw(ImVec2(ImGui::GetContentRegionAvail().x, height), content::ViewportView{}, shown, none_, selected_component_);
}

void MaterialGraphEditor::DrawDetails(const MaterialType& type) {
  Node* node = selected_node_ ? graph_.Find(selected_node_) : nullptr;
  if (node && node->type != content::material::kResult) DrawNodeDetails(type, *node);
  else DrawMaterialDetails(type);
}

void MaterialGraphEditor::DrawMaterialDetails(const MaterialType& type) {
  ImGui::SeparatorText("Material");
  if (BeginLabelTable("##material")) {
    LabelRow("Path");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(path_.c_str());
    LabelRow("Type");
    if (ImGui::BeginCombo("##type", document_.type.c_str())) {
      // Only graph types: a type without a graph has the other material editor.
      for (const std::string& path : content::MaterialTypePaths()) {
        const std::shared_ptr<const MaterialType> declared = content::FindMaterialType(path);
        if (!declared || !declared->HasGraph()) continue;
        if (ImGui::Selectable(path.c_str(), Same(path, document_.type)) && !Same(path, document_.type)) {
          document_.type = path;
          document_.values = nlohmann::json::object();
          document_.preview = nullptr;
        }
        if (!declared->help.empty()) ImGui::SetItemTooltip("%s", declared->help.c_str());
      }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", type.help.c_str());
    LabelRow("Help");
    widgets::InputStringMultiline("##help", document_.help, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 2.0f));
    LabelRow("Shading Model");
    if (ImGui::BeginCombo("##shading", graph_.shading_model.empty() ? "Pick one" : graph_.shading_model.c_str())) {
      for (const content::ShadingModelDecl& model : type.shading_models) {
        if (ImGui::Selectable(model.name.c_str(), Same(model.name, graph_.shading_model)) && !Same(model.name, graph_.shading_model)) {
          graph_.shading_model = model.name;
          Changed();
        }
        if (!model.help.empty()) ImGui::SetItemTooltip("%s", model.help.c_str());
      }
      ImGui::EndCombo();
    }
    if (const content::ShadingModelDecl* model = type.FindShadingModel(graph_.shading_model); model && !model->help.empty()) ImGui::SetItemTooltip("%s", model->help.c_str());
    // What the viewport shows it on, kept with the material.
    const std::shared_ptr<const content::ComponentType> component = content::FindComponentType(type.preview_component);
    const auto choice = component ? std::find_if(component->properties.begin(), component->properties.end(), [&type](const content::Property& property) { return Same(property.name, type.preview_choice); }) : std::vector<content::Property>::const_iterator{};
    if (component && choice != component->properties.end()) {
      LabelRow("Shown on");
      nlohmann::json value = document_.preview.is_null() ? (type.preview_values.contains(choice->name) ? content::ToJson(type.preview_values.at(choice->name)) : choice->value) : document_.preview;
      if (ValueField("##preview", choice->type, value, host_.Types(), choices_)) document_.preview = value;
      ImGui::SetItemTooltip("What the viewport shows the material on, its %s; kept with the material, but only for the editor", choice->name.c_str());
    }
    ImGui::EndTable();
  }
  if (!type.properties.empty()) {
    ImGui::SeparatorText("Settings");
    DrawSetValues("##values", type.properties, document_.values, host_, choices_);
  }
  const Node* result = graph_.Result();
  if (result) {
    ImGui::SeparatorText("Inputs");
    ImGui::TextDisabled("Each input's value while nothing links to it");
    Node* editable = graph_.Find(result->id);
    for (const Pin& pin : content::material::PinsOf(type, *editable).inputs) {
      if (graph_.LinkInto(editable->id, pin.name)) continue;
      ImGui::BeginDisabled(!content::material::Reads(type, graph_.shading_model, pin.name));
      if (DrawInputValue(*editable, pin)) Changed();
      ImGui::EndDisabled();
    }
  }
  if (!compiled_.program.parameters.empty()) {
    ImGui::SeparatorText("Parameters");
    ImGui::TextDisabled("What's drawn with it may change these");
    for (const content::MaterialParameterUse& parameter : compiled_.program.parameters) ImGui::BulletText("%s (%s)", parameter.name.c_str(), WidthName(parameter.width));
  }
}

void MaterialGraphEditor::DrawNodeDetails(const MaterialType& type, Node& node) {
  const NodeType* desc = content::material::FindNodeType(type, node.type);
  ImGui::SeparatorText(content::material::TitleOf(type, node).c_str());
  if (!desc) {
    ImGui::TextColored(kErrorColor, "The game has no node %s", node.type.c_str());
    return;
  }
  if (!desc->help.empty()) ImGui::TextWrapped("%s", desc->help.c_str());
  const auto problems = compiled_.node_problems.find(node.id);
  if (problems != compiled_.node_problems.end()) {
    for (const std::string& problem : problems->second) ImGui::TextColored(kErrorColor, "%s", problem.c_str());
  }
  bool changed = false;
  if (!desc->settings.empty() && BeginLabelTable("##settings")) {
    for (const Setting& setting : desc->settings) changed |= DrawSetting(type, *desc, node, setting);
    ImGui::EndTable();
  }
  const NodePins pins = content::material::PinsOf(type, node);
  bool header = false;
  for (const Pin& pin : pins.inputs) {
    if (graph_.LinkInto(node.id, pin.name) || !pin.unlinked.empty()) continue;
    if (!header) {
      ImGui::SeparatorText("Inputs");
      ImGui::TextDisabled("Each input's value while nothing links to it");
      header = true;
    }
    changed |= DrawInputValue(node, pin);
  }
  if (changed) Changed();
}

bool MaterialGraphEditor::DrawSetting(const MaterialType& type, const NodeType& desc, Node& node, const Setting& setting) {
  ImGui::PushID(setting.name.c_str());
  LabelRow(setting.name.c_str());
  const nlohmann::json value = content::material::SettingOf(desc, node, setting.name);
  bool changed = false;
  switch (setting.kind) {
    case SettingKind::kFloat: {
      float number = value.is_number() ? value.get<float>() : 0.0f;
      if (ImGui::DragFloat("##value", &number, 0.01f, 0.0f, 0.0f, "%.4g")) {
        node.settings[setting.name] = number;
        changed = true;
      }
      break;
    }
    case SettingKind::kInt: {
      int number = value.is_number() ? value.get<int>() : 0;
      if (ImGui::InputInt("##value", &number)) {
        node.settings[setting.name] = number;
        changed = true;
      }
      break;
    }
    case SettingKind::kBool: {
      bool on = value.is_boolean() && value.get<bool>();
      if (ImGui::Checkbox("##value", &on)) {
        node.settings[setting.name] = on;
        changed = true;
      }
      break;
    }
    case SettingKind::kColour: {
      std::array<float, 4> colour = FloatsOf(value, {1.0f, 1.0f, 1.0f, 1.0f});
      if (ImGui::ColorEdit4("##value", colour.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_AlphaPreviewHalf)) {
        node.settings[setting.name] = JsonOf(colour);
        changed = true;
      }
      break;
    }
    case SettingKind::kVector: {
      std::array<float, 4> floats = FloatsOf(value);
      if (ImGui::DragScalarN("##value", ImGuiDataType_Float, floats.data(), std::clamp<int>(setting.width, 1, 4), 0.01f, nullptr, nullptr, "%.4g")) {
        node.settings[setting.name] = JsonOf(floats);
        changed = true;
      }
      break;
    }
    case SettingKind::kName: {
      std::string text = value.is_string() ? value.get<std::string>() : std::string();
      if (widgets::InputString("##value", text)) {
        node.settings[setting.name] = text;
        changed = true;
      }
      break;
    }
    case SettingKind::kTexture: changed = DrawTextureSetting(type, node, setting); break;
    case SettingKind::kChoice: {
      const std::string current = value.is_string() ? value.get<std::string>() : std::string();
      if (ImGui::BeginCombo("##value", current.c_str())) {
        for (const std::string& choice : setting.choices) {
          if (ImGui::Selectable(choice.c_str(), choice == current) && choice != current) {
            node.settings[setting.name] = choice;
            changed = true;
          }
        }
        ImGui::EndCombo();
      }
      break;
    }
  }
  if (!setting.help.empty()) ImGui::SetItemTooltip("%s", setting.help.c_str());
  ImGui::PopID();
  return changed;
}

// A texture: one of the mod's Texture assets, or one of the game's textures when the game names them.
bool MaterialGraphEditor::DrawTextureSetting(const MaterialType& type, Node& node, const Setting& setting) {
  nlohmann::json& value = node.settings[setting.name];
  if (!value.is_object()) value = nlohmann::json::object();
  bool game = value.contains("game");
  bool changed = false;
  if (!type.game_textures.empty()) {
    if (ImGui::RadioButton("Mod", !game) && game) {
      value = {{"asset", ""}};
      game = false;
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Game", game) && !game) {
      value = {{"game", ""}};
      game = true;
      changed = true;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
  }
  const content::ValueType field_type = game ? content::ValueType{.type = content::PropertyType::kString, .of = type.game_textures} : content::ValueType{.type = content::PropertyType::kAsset, .of = "Texture"};
  const char* key = game ? "game" : "asset";
  nlohmann::json held = value.contains(key) ? value[key] : nlohmann::json("");
  if (ValueField("##texture", field_type, held, host_.Types(), choices_)) {
    value = {{key, held.is_string() ? held.get<std::string>() : std::string()}};
    changed = true;
  }
  return changed;
}

// An unlinked input's value, in the details: a number, or a vector with its colour beside it.
bool MaterialGraphEditor::DrawInputValue(Node& node, const Pin& pin) {
  const uint8_t width = pin.width ? pin.width : 1;
  const auto typed = node.values.find(pin.name);
  std::array<float, 4> value = typed != node.values.end() ? typed->second : pin.initial;
  ImGui::PushID(pin.name.c_str());
  bool changed = false;
  const float swatch = width >= 3 ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x : 0.0f;
  ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 6.0f, ImGui::GetContentRegionAvail().x * 0.65f - swatch));
  changed |= ImGui::DragScalarN(pin.name.c_str(), ImGuiDataType_Float, value.data(), width, 0.01f, nullptr, nullptr, "%.3g");
  if (width >= 3) {
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    changed |= width == 3 ? ImGui::ColorEdit3("##colour", value.data(), ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR) : ImGui::ColorEdit4("##colour", value.data(), ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_AlphaPreviewHalf);
  }
  if (!pin.help.empty()) ImGui::SetItemTooltip("%s", pin.help.c_str());
  if (changed) node.values[pin.name] = value;
  ImGui::PopID();
  return changed;
}

// Middle ------------------------------------------------------------------------------------------------

void MaterialGraphEditor::DrawCanvas(const MaterialType& type, float height) {
  const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
  const ImVec2 canvas_max(canvas_min.x + ImGui::GetContentRegionAvail().x, canvas_min.y + height);
  style_.DrawBackdrop(canvas_min, canvas_max, 0.0f);
  pin_tooltip_.clear();
  ed::SetCurrentEditor(canvas_);
  ed::Begin("##material_graph", ImVec2(0.0f, height));

  for (Node& node : graph_.nodes) {
    if (place_nodes_) ed::SetNodePosition(NodeId(node.id), ImVec2(node.x, node.y));
    DrawNode(type, node);
  }
  place_nodes_ = false;

  for (const Link& link : graph_.links) {
    const Node* source = graph_.Find(link.from);
    const Node* target = graph_.Find(link.to);
    if (!source || !target) continue;
    const NodePins source_pins = content::material::PinsOf(type, *source);
    const NodePins target_pins = content::material::PinsOf(type, *target);
    const auto from = std::find_if(source_pins.outputs.begin(), source_pins.outputs.end(), [&](const Pin& pin) { return Same(pin.name, link.output); });
    const auto to = std::find_if(target_pins.inputs.begin(), target_pins.inputs.end(), [&](const Pin& pin) { return Same(pin.name, link.input); });
    if (from == source_pins.outputs.end() || to == target_pins.inputs.end()) continue;
    const size_t output = static_cast<size_t>(from - source_pins.outputs.begin());
    const size_t input = static_cast<size_t>(to - target_pins.inputs.begin());
    const Widths widths = content::material::WidthsOf(type, graph_, *source);
    const uint8_t width = output < widths.outputs.size() ? widths.outputs[output] : 1;
    ed::Link(LinkId(link.to, input), PinId(link.from, output, true), PinId(link.to, input, false), ImGui::ColorConvertU32ToFloat4(WidthColor(width)), kLinkThickness);
    if ((style_.IsNodeActive(link.from) || style_.IsNodeActive(link.to)) && style_.Pulse(kActivePulseSeconds)) ed::Flow(LinkId(link.to, input));
  }

  HandleCreate(type);
  HandleDelete(type);
  HandleClipboard(type);

  // Popups and tooltips draw in screen space, outside the canvas transform.
  ed::Suspend();
  if (!pin_tooltip_.empty() && !ImGui::IsPopupOpen(kAddMenu)) ImGui::SetTooltip("%s", pin_tooltip_.c_str());
  if (ed::ShowBackgroundContextMenu()) {
    pending_.reset();
    open_add_menu_ = true;
  }
  if (open_add_menu_) {
    open_add_menu_ = false;
    const ImVec2 position = ed::ScreenToCanvas(ImGui::GetMousePos());
    add_x_ = position.x;
    add_y_ = position.y;
    add_filter_.clear();
    ImGui::OpenPopup(kAddMenu);
  }
  DrawAddMenu(type);
  ed::Resume();

  SyncFromCanvas();
  if (fit_view_) {
    FitView();
    fit_view_ = false;
  }
  ed::End();
  ed::SetCurrentEditor(nullptr);
  if (graph_.nodes.size() <= 1) ImGui::GetWindowDrawList()->AddText(ImVec2(canvas_min.x + 16.0f, canvas_min.y + 12.0f), IM_COL32(255, 255, 255, 110), "Right click to add a node, and link it into the Material node: a Texture Sample into Base Colour.");
}

void MaterialGraphEditor::FitView() {
  ImVec2 min(FLT_MAX, FLT_MAX);
  ImVec2 max(-FLT_MAX, -FLT_MAX);
  for (const Node& node : graph_.nodes) {
    const ImVec2 position = ed::GetNodePosition(NodeId(node.id));
    const ImVec2 size = ed::GetNodeSize(NodeId(node.id));
    min = ImVec2(std::min(min.x, position.x), std::min(min.y, position.y));
    max = ImVec2(std::max(max.x, position.x + size.x), std::max(max.y, position.y + size.y));
  }
  const ImVec2 screen = ed::GetScreenSize();
  if (min.x > max.x || screen.x <= 0.0f || screen.y <= 0.0f) return;
  // With a margin, at zoom 1 when that fits, else zoomed out just enough.
  const float margin = ImGui::GetFontSize() * kFitMargin;
  const float scale = std::max({1.0f, (max.x - min.x + margin * 2.0f) / screen.x, (max.y - min.y + margin * 2.0f) / screen.y});
  const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
  const ImVec2 half(screen.x * scale * 0.5f, screen.y * scale * 0.5f);
  ed::SetViewState({ImVec2(center.x - half.x, center.y - half.y), ImVec2(center.x + half.x, center.y + half.y)});
}

void MaterialGraphEditor::DrawNode(const MaterialType& type, Node& node) {
  const NodeType* desc = content::material::FindNodeType(type, node.type);
  const NodePins pins = content::material::PinsOf(type, node);
  const Widths widths = content::material::WidthsOf(type, graph_, node);
  const bool result = node.type == content::material::kResult;
  bool changed = false;

  style_.BeginNode(node.id);
  ed::BeginNode(NodeId(node.id));
  ImGui::PushID(node.id);

  const float content_x = ImGui::GetCursorPosX();
  ImGui::TextColored(kTitleColor, "%s", content::material::TitleOf(type, node).c_str());
  const float title_width = ImGui::GetItemRectSize().x;
  const float header_bottom = ImGui::GetItemRectMax().y;
  ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
  if (desc && !desc->shown.empty()) changed |= DrawShownSetting(*desc, node);

  float inputs_width = 0.0f;
  if (!pins.inputs.empty()) ImGui::BeginGroup();
  for (size_t index = 0; index < pins.inputs.size(); ++index) {
    const Pin& pin = pins.inputs[index];
    const bool linked = graph_.LinkInto(node.id, pin.name) != nullptr;
    const bool read = !result || content::material::Reads(type, graph_.shading_model, pin.name);
    const uint8_t width = index < widths.inputs.size() ? widths.inputs[index] : pin.width;
    ed::BeginPin(PinId(node.id, index, false), ed::PinKind::Input);
    const ImU32 color = WidthColor(pin.width ? pin.width : width);
    node_widgets::PinIcon(linked, read ? color : (color & 0x00FFFFFF) | 0x50000000, PinShape::kCircle);
    ed::EndPin();
    if (ImGui::IsItemHovered()) pin_tooltip_ = pin.name + ": " + WidthName(pin.width) + (read ? std::string() : std::string(", which ") + graph_.shading_model + " doesn't read") + (pin.help.empty() ? std::string() : "\n" + pin.help);
    ImGui::SameLine();
    if (read) ImGui::TextUnformatted(pin.name.c_str());
    else ImGui::TextDisabled("%s", pin.name.c_str());
    if (!linked && pin.unlinked.empty() && read) changed |= DrawInlineValue(node, pin);
  }
  if (!pins.inputs.empty()) {
    ImGui::EndGroup();
    inputs_width = ImGui::GetItemRectSize().x;
  }

  if (!pins.outputs.empty()) {
    float column_width = 0.0f;
    for (const Pin& pin : pins.outputs) column_width = std::max(column_width, node_widgets::OutputRowWidth(pin.name));
    const float column_x = node_widgets::PlaceOutputColumn(content_x, title_width, inputs_width, column_width);
    ImGui::BeginGroup();
    for (size_t index = 0; index < pins.outputs.size(); ++index) {
      const Pin& pin = pins.outputs[index];
      const uint8_t width = index < widths.outputs.size() ? widths.outputs[index] : pin.width;
      node_widgets::AlignOutputRow(column_x, column_width, pin.name);
      ed::BeginPin(PinId(node.id, index, true), ed::PinKind::Output);
      ImGui::TextUnformatted(pin.name.c_str());
      ImGui::SameLine();
      node_widgets::PinIcon(graph_.HasLinkFrom(node.id, pin.name), WidthColor(width), PinShape::kCircle);
      ed::EndPin();
      if (ImGui::IsItemHovered()) pin_tooltip_ = pin.name + ": " + WidthName(width);
    }
    ImGui::EndGroup();
  }

  ImGui::PopID();
  ed::EndNode();
  style_.EndNode(node.id, CategoryColor(desc), header_bottom, compiled_.node_problems.contains(node.id) || !desc);
  if (changed) Changed();
}

// An unlinked input on the node: a single float to drag; anything wider is shown and typed into in the details.
bool MaterialGraphEditor::DrawInlineValue(Node& node, const Pin& pin) {
  const auto typed = node.values.find(pin.name);
  std::array<float, 4> value = typed != node.values.end() ? typed->second : pin.initial;
  ImGui::SameLine();
  if (pin.width == 0 || pin.width == 1) {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * kInlineFloatWidth);
    if (ImGui::DragFloat(("##" + pin.name).c_str(), &value[0], 0.01f, 0.0f, 0.0f, "%.3g")) {
      node.values[pin.name] = value;
      return true;
    }
    return false;
  }
  if (pin.width >= 3) {
    ImGui::ColorButton(("##" + pin.name).c_str(), FloatsColor(value), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_Float, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
  } else {
    ImGui::TextDisabled("(%.2g, %.2g)", value[0], value[1]);
  }
  return false;
}

// The setting a node shows on itself (a Constant's value): a number to drag, a colour to see.
bool MaterialGraphEditor::DrawShownSetting(const NodeType& desc, Node& node) {
  const auto setting = std::find_if(desc.settings.begin(), desc.settings.end(), [&desc](const Setting& entry) { return entry.name == desc.shown; });
  if (setting == desc.settings.end()) return false;
  const nlohmann::json value = content::material::SettingOf(desc, node, setting->name);
  if (setting->kind == SettingKind::kFloat) {
    float number = value.is_number() ? value.get<float>() : 0.0f;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * kInlineFloatWidth * 1.5f);
    if (ImGui::DragFloat("##shown", &number, 0.01f, 0.0f, 0.0f, "%.4g")) {
      node.settings[setting->name] = number;
      return true;
    }
  } else if (setting->kind == SettingKind::kColour) {
    const std::array<float, 4> colour = FloatsOf(value, {1.0f, 1.0f, 1.0f, 1.0f});
    ImGui::ColorButton("##shown", ImVec4(colour[0], colour[1], colour[2], colour[3]), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2(ImGui::GetFontSize() * 4.0f, ImGui::GetFrameHeight()));
  } else if (setting->kind == SettingKind::kVector) {
    const std::array<float, 4> floats = FloatsOf(value);
    std::string text = "(";
    for (int i = 0; i < std::clamp<int>(setting->width, 1, 4); ++i) text += (i ? ", " : "") + content::material::NumberCode(floats[i]);
    ImGui::TextDisabled("%s)", text.c_str());
  }
  return false;
}

void MaterialGraphEditor::HandleCreate(const MaterialType& type) {
  if (!ed::BeginCreate(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), kLinkThickness)) return;
  const auto name_of = [&](const DecodedPin& pin) -> std::optional<std::string> {
    const Node* node = graph_.Find(pin.node);
    if (!node) return std::nullopt;
    const NodePins pins = content::material::PinsOf(type, *node);
    const std::vector<Pin>& side = pin.output ? pins.outputs : pins.inputs;
    if (pin.index >= side.size()) return std::nullopt;
    return side[pin.index].name;
  };

  ed::PinId start_id;
  ed::PinId end_id;
  if (ed::QueryNewLink(&start_id, &end_id)) {
    std::optional<DecodedPin> start = DecodePin(start_id.Get(), kPinBase);
    std::optional<DecodedPin> end = DecodePin(end_id.Get(), kPinBase);
    if (start && end) {
      if (!start->output) std::swap(start, end);
      const std::optional<std::string> output = start->output ? name_of(*start) : std::nullopt;
      const std::optional<std::string> input = !end->output ? name_of(*end) : std::nullopt;
      std::string reason;
      if (!output || !input || !content::material::CanConnect(type, graph_, start->node, *output, end->node, *input, &reason)) {
        ed::RejectNewItem(kRejectColor, kLinkThickness);
        if (!reason.empty()) {
          ed::Suspend();
          ImGui::SetTooltip("%s", reason.c_str());
          ed::Resume();
        }
      } else if (ed::AcceptNewItem(kAcceptColor, kLinkThickness + 1.0f)) {
        graph_.Connect(start->node, *output, end->node, *input);
        Changed();
      }
    }
  }

  // A link dropped on empty space: a node there, connected to the pin.
  ed::PinId pin_id;
  if (ed::QueryNewNode(&pin_id) && ed::AcceptNewItem()) {
    if (const std::optional<DecodedPin> decoded = DecodePin(pin_id.Get(), kPinBase)) {
      if (const std::optional<std::string> name = name_of(*decoded)) {
        pending_ = PendingPin{decoded->node, *name, decoded->output};
        open_add_menu_ = true;
      }
    }
  }
  ed::EndCreate();
}

void MaterialGraphEditor::HandleDelete(const MaterialType& type) {
  if (!ed::BeginDelete()) return;
  bool changed = false;
  ed::LinkId link_id;
  while (ed::QueryDeletedLink(&link_id)) {
    if (!ed::AcceptDeletedItem()) continue;
    const std::optional<DecodedPin> input = DecodePin(link_id.Get(), kLinkBase);
    const Node* node = input ? graph_.Find(input->node) : nullptr;
    if (!node) continue;
    const NodePins pins = content::material::PinsOf(type, *node);
    if (input->index < pins.inputs.size()) {
      graph_.Disconnect(input->node, pins.inputs[input->index].name);
      changed = true;
    }
  }
  ed::NodeId node_id;
  while (ed::QueryDeletedNode(&node_id)) {
    const int id = static_cast<int>(node_id.Get());
    const Node* node = graph_.Find(id);
    if (node && node->type == content::material::kResult) {
      ed::RejectDeletedItem();
      SetStatus("A material keeps its Material node");
      continue;
    }
    if (!ed::AcceptDeletedItem()) continue;
    graph_.Remove(id);
    if (selected_node_ == id) selected_node_ = 0;
    changed = true;
  }
  ed::EndDelete();
  if (changed) Changed();
}

void MaterialGraphEditor::HandleClipboard(const MaterialType& type) {
  using node_widgets::ClipboardAction;
  const node_widgets::ClipboardRequest request = node_widgets::TakeClipboardRequest();
  if (request.action == ClipboardAction::kNone) return;
  if (request.action == ClipboardAction::kPaste) {
    const char* text = ImGui::GetClipboardText();
    const ImVec2 at = ImGui::GetMousePos();  // in canvas units between ed::Begin and ed::End
    if (text) Paste(type, text, at.x, at.y);
    return;
  }
  if (request.nodes.empty()) return;
  const std::string copied = Copy(request.nodes);
  if (request.action == ClipboardAction::kDuplicate) {
    const Node* first = graph_.Find(request.nodes.front());
    if (first) Paste(type, copied, first->x + kPasteOffset, first->y + kPasteOffset);
    return;
  }
  ImGui::SetClipboardText(copied.c_str());
  if (request.action == ClipboardAction::kCut) {
    for (const int id : request.nodes) {
      const Node* node = graph_.Find(id);
      if (node && node->type != content::material::kResult) graph_.Remove(id);
    }
    selected_node_ = 0;
    Changed();
  }
}

// The nodes, without the Material node, and the links between them.
std::string MaterialGraphEditor::Copy(const std::vector<int>& ids) const {
  Graph copied;
  for (const int id : ids) {
    const Node* node = graph_.Find(id);
    if (node && node->type != content::material::kResult) copied.nodes.push_back(*node);
  }
  for (const Link& link : graph_.links) {
    if (copied.Find(link.from) && copied.Find(link.to)) copied.links.push_back(link);
  }
  nlohmann::json json = copied.Write();
  json["kind"] = kClipboardKind;
  return json.dump();
}

// Copied nodes, their first at (x, y), with new ids and their links between them.
void MaterialGraphEditor::Paste(const MaterialType& type, const std::string& text, float x, float y) {
  const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
  if (!json.is_object() || json.value("kind", std::string()) != kClipboardKind) return;
  const Graph copied = Graph::Read(json);
  if (copied.nodes.empty()) return;
  const float dx = x - copied.nodes.front().x;
  const float dy = y - copied.nodes.front().y;
  std::map<int, int> ids;
  std::vector<int> added;
  for (const Node& node : copied.nodes) {
    if (node.type == content::material::kResult || !content::material::FindNodeType(type, node.type)) continue;
    Node& made = graph_.Add(node.type, node.x + dx, node.y + dy);
    made.settings = node.settings;
    made.values = node.values;
    ids[node.id] = made.id;
    added.push_back(made.id);
    ed::SetNodePosition(NodeId(made.id), ImVec2(made.x, made.y));
  }
  for (const Link& link : copied.links) {
    if (ids.contains(link.from) && ids.contains(link.to)) graph_.Connect(ids[link.from], link.output, ids[link.to], link.input);
  }
  node_widgets::SelectNodes(added);
  Changed();
}

void MaterialGraphEditor::DrawAddMenu(const MaterialType& type) {
  if (!ImGui::BeginPopup(kAddMenu)) return;
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
  node_widgets::InputString("##search", add_filter_, 0, "Search nodes");

  std::vector<const NodeType*> entries;
  for (const NodeType* desc : content::material::NodeTypes(type)) {
    if (desc->id == content::material::kResult) continue;
    // Dragged from a pin: a node with a pin on the other side, which any value goes into.
    if (pending_ && (pending_->output ? desc->inputs.empty() : desc->outputs.empty())) continue;
    entries.push_back(desc);
  }

  const NodeType* chosen = nullptr;
  const auto item = [&](const NodeType* desc) {
    const bool picked = ImGui::MenuItem((desc->title + "##" + desc->id).c_str());
    if (ImGui::IsItemHovered() && !desc->help.empty()) ImGui::SetTooltip("%s", desc->help.c_str());
    return picked;
  };
  if (!add_filter_.empty()) {
    // A flat list of matches; Enter takes the first.
    bool first = true;
    for (const NodeType* desc : entries) {
      if (!node_widgets::ContainsIgnoringCase(desc->title, add_filter_) && !node_widgets::ContainsIgnoringCase(desc->category, add_filter_) && !node_widgets::ContainsIgnoringCase(desc->keywords, add_filter_)) continue;
      if (item(desc) || (first && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
        chosen = desc;
        break;
      }
      first = false;
    }
    if (first) ImGui::TextDisabled("Nothing matches");
  } else {
    // Categories nest at their slashes.
    const std::function<void(const std::string&)> level = [&](const std::string& prefix) {
      std::vector<std::string> children;
      for (const NodeType* desc : entries) {
        if (desc->category.size() <= prefix.size() || desc->category.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string rest = desc->category.substr(prefix.size());
        const std::string child = rest.substr(0, rest.find('/'));
        if (std::find(children.begin(), children.end(), child) == children.end()) children.push_back(child);
      }
      for (const std::string& child : children) {
        if (!ImGui::BeginMenu(child.c_str())) continue;
        level(prefix + child + "/");
        ImGui::EndMenu();
      }
      const std::string own = prefix.empty() ? std::string() : prefix.substr(0, prefix.size() - 1);
      for (const NodeType* desc : entries) {
        if (!prefix.empty() && desc->category == own && item(desc)) chosen = desc;
      }
    };
    level({});
  }
  if (chosen) {
    AddNode(type, *chosen);
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void MaterialGraphEditor::AddNode(const MaterialType& type, const NodeType& desc) {
  Node& node = graph_.Add(desc.id, add_x_, add_y_);
  const int id = node.id;
  ed::SetNodePosition(NodeId(id), ImVec2(add_x_, add_y_));
  // Opened from a dragged link: connected through the first pin on the other side.
  if (pending_) {
    const NodePins pins = content::material::PinsOf(type, node);
    const std::vector<Pin>& side = pending_->output ? pins.inputs : pins.outputs;
    if (!side.empty()) {
      if (pending_->output) graph_.Connect(pending_->node, pending_->pin, id, side.front().name);
      else if (!graph_.Feeds(pending_->node, id)) graph_.Connect(id, side.front().name, pending_->node, pending_->pin);
    }
    pending_.reset();
  }
  ed::ClearSelection();
  ed::SelectNode(NodeId(id));
  selected_node_ = id;
  Changed();
}

void MaterialGraphEditor::SyncFromCanvas() {
  // Positions are kept in the graph. A drag is one undo step, taken when the mouse lets go.
  bool moved = false;
  for (Node& node : graph_.nodes) {
    const ImVec2 position = ed::GetNodePosition(NodeId(node.id));
    if (position.x != node.x || position.y != node.y) {
      node.x = position.x;
      node.y = position.y;
      moved = true;
    }
  }
  if (moved) moving_ = true;
  if (moving_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    moving_ = false;
    Changed();
  }
  ed::NodeId selected[1];
  selected_node_ = ed::GetSelectedNodes(selected, 1) > 0 ? static_cast<int>(selected[0].Get()) : 0;
}

void MaterialGraphEditor::DrawCode() {
  const std::string& code = compiled_.program.code;
  if (glass::SmallButton("Copy", glass::kAccent)) ImGui::SetClipboardText(code.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled(code.empty() ? "No code until its problems are fixed" : "What the game compiles into its shader");
  std::string shown = code;
  ImGui::InputTextMultiline("##code", shown.data(), shown.size() + 1, ImGui::GetContentRegionAvail(), ImGuiInputTextFlags_ReadOnly);
}

std::vector<EditorProblem> MaterialGraphEditor::Problems() {
  std::vector<EditorProblem> problems;
  const ModInfo* mod = Mod();
  if (!mod) return problems;
  const std::shared_ptr<const MaterialType> type = Type();
  if (!type) {
    problems.push_back({"The game has no material type " + document_.type});
    return problems;
  }
  for (const std::string& problem : compiled_.program.problems) problems.push_back({problem});
  // What the game's own compiler makes of the code.
  if (type->check_program && !compiled_.program.code.empty()) {
    for (const std::string& problem : type->check_program(compiled_.program)) problems.push_back({problem});
  }
  for (const auto& [name, value] : document_.values.items()) {
    const auto found = std::find_if(type->properties.begin(), type->properties.end(), [&name](const content::Property& property) { return Same(property.name, name); });
    if (found == type->properties.end()) {
      problems.push_back({"It sets " + name + ", which its type has no property of", "Remove it", [this, key = name] { document_.values.erase(key); }});
      continue;
    }
    nlohmann::json kept;
    if (std::string why; !content::FitValue(host_.Types(), found->type, value, &kept, &why)) problems.push_back({name + ": " + why});
  }
  nlohmann::json graph = document_.graph;
  content::material::ForEachTexture(graph, [&](std::string& path, const std::string& where) {
    if (auto problem = DependencyProblem(host_, *mod, path, where)) problems.push_back(std::move(*problem));
  });
  return problems;
}

}  // namespace

std::unique_ptr<AssetEditor> OpenMaterialGraphEditor(EditorHost& host, const std::string& path, content::MaterialAsset asset) {
  // A material written before its type had a graph starts with its Material node.
  if (asset.graph.is_null()) {
    if (const std::shared_ptr<const MaterialType> type = content::FindMaterialType(asset.type)) asset.graph = content::material::DefaultGraph(*type).Write();
  }
  return std::make_unique<MaterialGraphEditor>(host, path, std::move(asset));
}

}  // namespace rexfabric::ui
