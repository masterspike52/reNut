#include "asset_editors.h"

#include <algorithm>
#include <cfloat>
#include <concepts>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

#include <imgui.h>
#include <rexfabric/rexfabric.h>

#include "../content/components.h"
#include "../content/game_texts.h"
#include "../content/map_edits.h"
#include "../content/materials.h"
#include "../content/world_view.h"
#include "../content/localisation.h"
#include "../content/tables.h"
#include "../content/types.h"
#include "avatar_viewport.h"
#include "editor_parts.h"
#include "map_edit_world.h"
#include "material_graph_editor.h"
#include "file_picker.h"
#include "glass.h"
#include "widgets.h"

namespace rexfabric::ui {
namespace {

using content::ClassAsset;
using content::EnumAsset;
using content::Property;
using content::PropertyType;
using content::StructAsset;
using content::ValueType;

// A new editor's size: a share of the screen, kept between these many font sizes.
constexpr float kEditorScreenWidth = 0.3f;
constexpr float kEditorScreenHeight = 0.7f;
constexpr float kEditorMinWidth = 50.0f;
constexpr float kEditorMaxWidth = 100.0f;
constexpr float kEditorMinHeight = 30.0f;
// In multiples of the font size.
constexpr float kTypeColumn = 11.0f;
constexpr float kVersionColumn = 8.0f;
// How far each new editor window opens from the last, and after how many it starts over.
constexpr float kCascadeStep = 1.8f;
constexpr int kCascadeCount = 6;
constexpr const char* kSavePopup = "Unsaved changes###save";
constexpr const char* kNewPropertyName = "NewProperty";
constexpr const char* kNewFieldName = "NewField";
constexpr const char* kNewValueName = "NewValue";
constexpr const char* kNewComponentName = "NewComponent";
constexpr const char* kNewEditName = "NewEdit";
// How far a copied edit is put from the one it copies, along the world's x.
constexpr double kDuplicateOffset = 2.0;
// How finely a transform's numbers drag.
constexpr float kTransformDragSpeed = 0.05f;

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

bool Same(std::string_view a, std::string_view b) { return Lower(a) == Lower(b); }

// The key of an object a name has, ignoring case; empty for none.
std::string KeyOf(const nlohmann::json& object, std::string_view name) {
  for (const auto& [key, value] : object.items()) {
    if (Same(key, name)) return key;
  }
  return {};
}

// `base`, or `base` with the first number after it that `taken` doesn't take: "NewField", "NewField2".
std::string FreeName(const std::string& base, const std::function<bool(const std::string&)>& taken) {
  std::string name = base;
  for (int number = 2; taken(name); ++number) name = base + std::to_string(number);
  return name;
}

// Up, down and remove for row `i` of `count`: which one was pressed, as a move of -1 or 1, or a removal.
enum class RowAction { kNone, kUp, kDown, kRemove };

RowAction RowButtons(size_t i, size_t count) {
  RowAction action = RowAction::kNone;
  ImGui::BeginDisabled(i == 0);
  if (ImGui::ArrowButton("##up", ImGuiDir_Up)) action = RowAction::kUp;
  ImGui::EndDisabled();
  ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
  ImGui::BeginDisabled(i + 1 == count);
  if (ImGui::ArrowButton("##down", ImGuiDir_Down)) action = RowAction::kDown;
  ImGui::EndDisabled();
  ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
  if (glass::SmallButton("Remove", glass::kDanger)) action = RowAction::kRemove;
  return action;
}

// Carries out a row's action on each of the vectors, which go row for row.
template <typename... Rows>
void ApplyRowAction(RowAction action, size_t i, Rows&... rows) {
  if (action == RowAction::kUp) (std::swap(rows[i], rows[i - 1]), ...);
  if (action == RowAction::kDown) (std::swap(rows[i], rows[i + 1]), ...);
  if (action == RowAction::kRemove) (rows.erase(rows.begin() + static_cast<ptrdiff_t>(i)), ...);
}

// The paths a type and a value of it name that the mod may not use: the enum, struct or class the type names, and
// assets the value holds, nested ones too.
void PathProblems(EditorHost& host, const ModInfo& mod, const ValueType& type, nlohmann::json value, const std::string& what, std::vector<EditorProblem>& problems) {
  for (const std::string* named : {&type.of, &type.key_of}) {
    if (auto problem = DependencyProblem(host, mod, *named, what + "'s type")) problems.push_back(std::move(*problem));
  }
  content::WalkValues(host.Types(), type, value, [&](const ValueType& single, nlohmann::json& held) {
    if (single.type != PropertyType::kAsset || !held.is_string()) return;
    if (auto problem = DependencyProblem(host, mod, held.get<std::string>(), what)) problems.push_back(std::move(*problem));
  });
}

// The problems of properties or fields as declared: each checked as the loader checks it, and a name given twice.
void DeclaredProblems(EditorHost& host, const ModInfo& mod, const std::vector<Property>& declared, const std::string& what, std::vector<EditorProblem>& problems) {
  const auto anything = [](std::string_view, std::string*) { return true; };
  for (size_t i = 0; i < declared.size(); ++i) {
    std::vector<Property> one = {declared[i]};
    std::string why;
    if (!content::CheckDeclared(one, host.Types(), anything, &why)) problems.push_back({what + " " + why});
    if (std::any_of(declared.begin(), declared.begin() + static_cast<ptrdiff_t>(i), [&](const Property& other) { return Same(other.name, declared[i].name); })) problems.push_back({"Two " + Lower(what) + "s are named " + declared[i].name});
    PathProblems(host, mod, declared[i].type, declared[i].value, what + " " + declared[i].name, problems);
  }
}

// Changes a declaration's type, keeping its default where it still fits.
void Retype(Property& declared, const ValueType& type, const content::TypeLookup& types) {
  nlohmann::json kept;
  declared.value = content::FitValue(types, type, declared.value, &kept, nullptr) ? kept : content::EmptyValue(types, type);
  declared.type = type;
}

// A table of declarations (a class's own properties, a struct's fields): each a row of its name, type, default and
// buttons, with its help on the row below. Gives the action a row's buttons asked for.
void DrawDeclared(const char* id, std::vector<Property>& declared, EditorHost& host, const TypeChoices& choices, bool reorder, size_t* acted, RowAction* action) {
  if (declared.empty() || !ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingFixedFit)) return;
  const float font = ImGui::GetFontSize();
  ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, font * kNameColumn);
  ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, font * kTypeColumn);
  ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("##buttons", ImGuiTableColumnFlags_WidthFixed);
  ImGui::TableHeadersRow();
  for (size_t i = 0; i < declared.size(); ++i) {
    Property& entry = declared[i];
    ImGui::PushID(static_cast<int>(i));
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    widgets::InputString("##name", entry.name);
    ImGui::TableNextColumn();
    ValueType type = entry.type;
    if (TypeField("##type", type, choices)) Retype(entry, type, host.Types());
    ImGui::TableNextColumn();
    ValueField("##default", entry.type, entry.value, host.Types(), choices);
    ImGui::TableNextColumn();
    if (reorder) {
      if (const RowAction pressed = RowButtons(i, declared.size()); pressed != RowAction::kNone) {
        *acted = i;
        *action = pressed;
      }
    } else if (glass::SmallButton("Remove", glass::kDanger)) {
      *acted = i;
      *action = RowAction::kRemove;
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    widgets::InputString("##help", entry.help, 0, "What it's for");
    ImGui::PopID();
  }
  ImGui::EndTable();
}

// A class's own file, and what it inherits.
class ClassEditor final : public DocumentEditor<ClassAsset> {
 public:
  ClassEditor(EditorHost& host, std::string path, ClassAsset asset) : DocumentEditor(host, std::move(path), std::move(asset)) { Resolve(); }

  bool Save() override;
  void ContentChanged() override;

 protected:
  std::string Title() const override { return std::string(content::NameOf(path_)); }
  float WidthFactor() const override { return IsAvatarClass() ? kAvatarEditorWidth : 1.0f; }
  void DrawContents() override;

 private:
  // What the parent gives: its properties with their values, and where its chain broke.
  void Resolve();
  void DrawBody();
  void DrawClass();
  void DrawInherited();
  void DrawOwn();
  // Avatars (docs/content_and_classes.md, Avatars): the backing, the inherited components and its own.
  bool IsAvatarClass() const { return !avatar_.backing.empty() || !document_.backing.empty() || !document_.components.empty(); }
  // The viewport (The viewport), beside the rest, of the class as it's being edited.
  void DrawViewport();
  content::AvatarValues PreviewValues() const;
  void DrawAvatar();
  void DrawComponents(std::vector<content::Component>& components);
  RowAction DrawComponent(content::Component& component, size_t index, size_t count);
  // Every component name the class and its ancestors take.
  bool NameTaken(const std::string& name) const;
  std::vector<EditorProblem> Problems();

  std::string resolved_parent_;
  std::vector<Property> inherited_;
  content::InheritedAvatar avatar_;
  std::string parent_problem_;
  TypeChoices choices_;
  std::unique_ptr<AvatarViewport> viewport_;
  std::string selected_component_;  // the one of its own the viewport's gizmo is on
};

bool ClassEditor::Save() {
  const std::optional<std::filesystem::path> file = File();
  std::string error;
  if (!file || !content::WriteClassAsset(*file, document_, &error)) {
    if (file) SetStatus(error, true);
    return false;
  }
  MarkSaved();
  host_.Saved(path_);
  SavedStatus(!Problems().empty());
  return true;
}

void ClassEditor::ContentChanged() {
  // A rename elsewhere may have repointed what this file names: shown as it is now, unless it has changes of its own.
  if (const ModInfo* mod = Mod(); mod && !dirty()) {
    if (std::optional<ClassAsset> asset = content::ReadClassAsset(content::AssetFile(*mod, path_), nullptr)) Reload(std::move(*asset));
  }
  Resolve();
}

void ClassEditor::Resolve() {
  const ModInfo* mod = Mod();
  resolved_parent_ = document_.parent;
  parent_problem_.clear();
  inherited_ = content::InheritedProperties(host_.model().mods(), document_.parent, &parent_problem_);
  avatar_ = content::InheritAvatar(host_.model().mods(), document_.parent);
  choices_ = mod ? host_.Choices(mod->folder) : TypeChoices{};
}

void ClassEditor::DrawContents() {
  if (resolved_parent_ != document_.parent) Resolve();
  DrawToolbar(CanUndo(), CanRedo());
  DrawProblems(Problems());
  if (!IsAvatarClass()) {
    viewport_.reset();
    DrawBody();
    Remember();
    return;
  }
  if (ImGui::BeginTable("##avatar_editor", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImGui::GetContentRegionAvail())) {
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::TableSetupColumn("##viewport", ImGuiTableColumnFlags_WidthFixed, std::max(width * kViewportShare, ImGui::GetFontSize() * kViewportMinWidth));
    ImGui::TableSetupColumn("##body", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    DrawViewport();
    ImGui::TableNextColumn();
    DrawBody();
    ImGui::EndTable();
  }
  // A drag of the gizmo is one edit, kept when it's let go.
  if (!viewport_ || !viewport_->held()) Remember();
}

void ClassEditor::DrawBody() {
  ImGui::BeginChild("##body");
  DrawClass();
  DrawInherited();
  DrawOwn();
  DrawAvatar();
  ImGui::EndChild();
}

void ClassEditor::DrawViewport() {
  if (!viewport_) viewport_ = std::make_unique<AvatarViewport>();
  const content::ViewportView view{.class_path = path_, .game_class = avatar_.game_class, .backing = document_.backing.empty() ? avatar_.backing : document_.backing, .values = PreviewValues()};
  viewport_->Draw(ImGui::GetContentRegionAvail(), view, avatar_.components, document_.components, selected_component_);
}

content::AvatarValues ClassEditor::PreviewValues() const {
  content::AvatarValues values;
  for (const Property& property : inherited_) {
    const std::string key = KeyOf(document_.defaults, property.name);
    values[property.name] = content::ToPropertyValue(key.empty() ? property.value : document_.defaults.at(key));
  }
  for (const Property& property : document_.properties) values[property.name] = content::ToPropertyValue(property.value);
  return values;
}

void ClassEditor::DrawClass() {
  ImGui::SeparatorText("Class");
  if (!BeginLabelTable("##class")) return;
  LabelRow("Path");
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(path_.c_str());
  LabelRow("Parent");
  std::vector<std::string> parents;
  for (const std::string& candidate : choices_.classes) {
    if (!Same(candidate, path_)) parents.push_back(candidate);
  }
  PathCombo("##parent", document_.parent.c_str(), document_.parent, parents, nullptr);
  if (std::optional<content::Class> parent = content::FindClass(document_.parent); parent && !parent->help.empty()) ImGui::SetItemTooltip("%s", parent->help.c_str());
  LabelRow("Help");
  widgets::InputStringMultiline("##help", document_.help, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.0f));
  ImGui::EndTable();
}

void ClassEditor::DrawInherited() {
  const float font = ImGui::GetFontSize();
  std::string declared_by;
  bool table = false;
  for (const Property& property : inherited_) {
    // A table per class declaring them, under its name.
    if (property.declared_by != declared_by) {
      if (table) ImGui::EndTable();
      declared_by = property.declared_by;
      ImGui::SeparatorText(("From " + declared_by).c_str());
      table = ImGui::BeginTable(declared_by.c_str(), 3, ImGuiTableFlags_SizingFixedFit);
      if (!table) continue;
      ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed, font * kNameColumn);
      ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed);
    }
    if (!table) continue;
    ImGui::PushID(property.name.c_str());
    const std::string key = KeyOf(document_.defaults, property.name);
    const bool overridden = !key.empty();
    nlohmann::json value = overridden ? document_.defaults[key] : property.value;

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    if (overridden) ImGui::TextColored(kOverriddenColor, "%s", property.name.c_str());
    else ImGui::TextUnformatted(property.name.c_str());
    ImGui::SetItemTooltip("%s%s%s, %s", property.help.c_str(), property.help.empty() ? "" : "\n", content::Describe(property.type).c_str(), overridden ? "set here" : "as its parent has it");
    ImGui::TableNextColumn();
    if (ValueField("##value", property.type, value, host_.Types(), choices_)) document_.defaults[overridden ? key : property.name] = value;
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(!overridden);
    if (glass::SmallButton("Reset")) document_.defaults.erase(key);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Back to its parent's value");
    ImGui::PopID();
  }
  if (table) ImGui::EndTable();
}

void ClassEditor::DrawOwn() {
  ImGui::SeparatorText("Added here");
  size_t acted = 0;
  RowAction action = RowAction::kNone;
  DrawDeclared("##own", document_.properties, host_, choices_, false, &acted, &action);
  ApplyRowAction(action, acted, document_.properties);
  if (document_.properties.empty()) ImGui::TextDisabled("No properties of its own yet");
  if (glass::Button("Add Property", glass::kCreate)) {
    const auto taken = [this](const std::string& name) {
      const auto named = [&name](const Property& property) { return Same(property.name, name); };
      return std::any_of(document_.properties.begin(), document_.properties.end(), named) || std::any_of(inherited_.begin(), inherited_.end(), named);
    };
    document_.properties.push_back({.name = FreeName(kNewPropertyName, taken), .type = {.type = PropertyType::kInt}, .value = 0});
  }
}

// The component names of a tree, into `names`.
void CollectNames(const std::vector<content::Component>& components, std::vector<std::string>& names) {
  for (const content::Component& component : components) {
    names.push_back(component.name);
    CollectNames(component.children, names);
  }
}

bool ClassEditor::NameTaken(const std::string& name) const {
  std::vector<std::string> names;
  CollectNames(avatar_.components, names);
  CollectNames(document_.components, names);
  return std::any_of(names.begin(), names.end(), [&name](const std::string& taken) { return Same(taken, name); });
}

void ClassEditor::DrawAvatar() {
  if (!IsAvatarClass()) return;
  ImGui::SeparatorText("Avatar");
  if (BeginLabelTable("##avatar")) {
    LabelRow("Backing");
    const std::string parents = "As its parent's: " + (avatar_.backing.empty() ? std::string("none") : avatar_.backing);
    if (ImGui::BeginCombo("##backing", document_.backing.empty() ? parents.c_str() : document_.backing.c_str())) {
      if (ImGui::Selectable(parents.c_str(), document_.backing.empty())) document_.backing.clear();
      for (const std::string& offered : content::BackingsFor(avatar_.game_class)) {
        if (ImGui::Selectable(offered.c_str(), Same(offered, document_.backing))) document_.backing = offered;
        if (const auto backing = content::FindBacking(offered); backing && !backing->help.empty()) ImGui::SetItemTooltip("%s", backing->help.c_str());
      }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("What the game makes in the world for each of its avatars");
    ImGui::EndTable();
  }
  if (!avatar_.components.empty()) {
    ImGui::TextDisabled("Inherited components, set in the classes above it:");
    const std::function<void(const content::Component&, int)> list = [&list](const content::Component& component, int depth) {
      ImGui::Indent(ImGui::GetFontSize() * static_cast<float>(depth + 1));
      ImGui::BulletText("%s (%s)", component.name.c_str(), component.type.c_str());
      ImGui::Unindent(ImGui::GetFontSize() * static_cast<float>(depth + 1));
      for (const content::Component& child : component.children) list(child, depth + 1);
    };
    for (const content::Component& component : avatar_.components) list(component, 0);
  }
  DrawComponents(document_.components);
  if (document_.components.empty()) ImGui::TextDisabled("No components of its own yet");
  if (glass::Button("Add Component", glass::kCreate)) {
    const std::vector<std::string> types = content::ComponentTypePaths();
    document_.components.push_back({.name = FreeName(kNewComponentName, [this](const std::string& name) { return NameTaken(name); }), .type = types.empty() ? std::string() : types.front()});
  }
  ImGui::SetItemTooltip("A component of the avatar itself; Add Child puts one on another component");
}

void ClassEditor::DrawComponents(std::vector<content::Component>& components) {
  size_t acted = 0;
  RowAction action = RowAction::kNone;
  for (size_t i = 0; i < components.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    if (const RowAction pressed = DrawComponent(components[i], i, components.size()); pressed != RowAction::kNone) {
      action = pressed;
      acted = i;
    }
    ImGui::PopID();
  }
  ApplyRowAction(action, acted, components);
}

RowAction ClassEditor::DrawComponent(content::Component& component, size_t index, size_t count) {
  const std::shared_ptr<const content::ComponentType> type = content::FindComponentType(component.type);
  const bool selected = Same(component.name, selected_component_);
  const bool open = ImGui::TreeNodeEx("##component", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth | (selected ? ImGuiTreeNodeFlags_Selected : 0), "%s (%s)", component.name.c_str(), component.type.c_str());
  // A click on its name puts the viewport's gizmo on it; its arrow, or a double click, opens and closes it.
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selected_component_ = component.name;
  if (type && !type->help.empty()) ImGui::SetItemTooltip("%s", type->help.c_str());
  ImGui::SameLine();
  const RowAction action = RowButtons(index, count);
  if (!open) return action;
  if (BeginLabelTable("##fields")) {
    LabelRow("Name");
    widgets::InputString("##name", component.name);
    LabelRow("Type");
    if (ImGui::BeginCombo("##type", component.type.c_str())) {
      for (const std::string& path : content::ComponentTypePaths()) {
        if (ImGui::Selectable(path.c_str(), Same(path, component.type)) && !Same(path, component.type)) {
          // Another type's values would only be problems.
          component.type = path;
          component.properties = nlohmann::json::object();
        }
      }
      ImGui::EndCombo();
    }
    if (!type || type->transform) {
      LabelRow("Position");
      ImGui::DragScalarN("##position", ImGuiDataType_Double, component.transform.position.data(), 3, kTransformDragSpeed);
      LabelRow("Rotation");
      ImGui::DragScalarN("##rotation", ImGuiDataType_Double, component.transform.rotation.data(), 3, 1.0f);
      ImGui::SetItemTooltip("Pitch, yaw and roll, in degrees");
      LabelRow("Scale");
      ImGui::DragScalarN("##scale", ImGuiDataType_Double, component.transform.scale.data(), 3, kTransformDragSpeed);
    }
    ImGui::EndTable();
  }
  // Its type's properties, each with a reset to the type's default once the class sets it.
  if (type && !type->properties.empty() && ImGui::BeginTable("##properties", 3, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * kNameColumn);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed);
    for (const Property& property : type->properties) {
      ImGui::PushID(property.name.c_str());
      const std::string key = KeyOf(component.properties, property.name);
      nlohmann::json value = key.empty() ? property.value : component.properties[key];
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (key.empty()) ImGui::TextUnformatted(property.name.c_str());
      else ImGui::TextColored(kOverriddenColor, "%s", property.name.c_str());
      ImGui::SetItemTooltip("%s%s%s", property.help.c_str(), property.help.empty() ? "" : "\n", content::Describe(property.type).c_str());
      ImGui::TableNextColumn();
      if (ValueField("##value", property.type, value, host_.Types(), choices_)) component.properties[key.empty() ? property.name : key] = value;
      ImGui::TableNextColumn();
      ImGui::BeginDisabled(key.empty());
      if (glass::SmallButton("Reset")) component.properties.erase(key);
      ImGui::EndDisabled();
      ImGui::SetItemTooltip("Back to the component's default");
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  DrawComponents(component.children);
  if (glass::SmallButton("Add Child", glass::kCreate)) {
    const std::vector<std::string> types = content::ComponentTypePaths();
    component.children.push_back({.name = FreeName(kNewComponentName, [this](const std::string& name) { return NameTaken(name); }), .type = types.empty() ? std::string() : types.front()});
  }
  ImGui::SetItemTooltip("A component on this one: where it is, it is relative to this");
  ImGui::TreePop();
  return action;
}

std::vector<EditorProblem> ClassEditor::Problems() {
  std::vector<EditorProblem> problems;
  const ModInfo* mod = Mod();
  if (!mod) return problems;
  if (!document_.backing.empty()) {
    const std::vector<std::string> offered = content::BackingsFor(avatar_.game_class);
    if (std::none_of(offered.begin(), offered.end(), [this](const std::string& path) { return Same(path, document_.backing); })) problems.push_back({"Its backing " + document_.backing + " isn't one the game offers for classes below " + (avatar_.game_class.empty() ? std::string("its game class") : avatar_.game_class), "Use its parent's", [this] { document_.backing.clear(); }});
  }
  if (!document_.components.empty() && document_.backing.empty() && avatar_.backing.empty()) problems.push_back({"It has components, but nothing gives it a backing, so it isn't an avatar class"});
  if (!document_.components.empty()) {
    std::vector<std::string> taken;
    CollectNames(avatar_.components, taken);
    std::vector<content::Component> checked = document_.components;
    if (std::string why; !content::CheckComponents(checked, taken, &why)) problems.push_back({"Components: " + why});
  }
  if (!parent_problem_.empty()) problems.push_back({"Its parent: " + parent_problem_});
  const std::vector<std::string> chain = content::ClassChain(host_.model().mods(), document_.parent);
  if (std::any_of(chain.begin(), chain.end(), [this](const std::string& at) { return Same(at, path_); })) problems.push_back({document_.parent + " comes from this class, so it can't be its parent"});
  if (auto problem = DependencyProblem(host_, *mod, document_.parent, "Its parent")) problems.push_back(std::move(*problem));

  DeclaredProblems(host_, *mod, document_.properties, "Property", problems);
  for (const Property& property : document_.properties) {
    if (std::any_of(inherited_.begin(), inherited_.end(), [&property](const Property& other) { return Same(other.name, property.name); })) problems.push_back({"Property " + property.name + " is inherited already"});
  }

  for (const auto& [name, value] : document_.defaults.items()) {
    const std::string key = name;
    const auto found = std::find_if(inherited_.begin(), inherited_.end(), [&key](const Property& property) { return Same(property.name, key); });
    if (found == inherited_.end()) {
      problems.push_back({"A default for " + key + ", which " + document_.parent + " doesn't have", "Remove it", [this, key] { document_.defaults.erase(key); }});
      continue;
    }
    std::string why;
    if (!content::FitValue(host_.Types(), found->type, value, nullptr, &why)) {
      problems.push_back({"The default for " + key + ": " + why, "Reset it", [this, key] { document_.defaults.erase(key); }});
      continue;
    }
    nlohmann::json walked = value;
    content::WalkValues(host_.Types(), found->type, walked, [&](const ValueType& single, nlohmann::json& held) {
      if (single.type != PropertyType::kAsset || !held.is_string()) return;
      if (auto problem = DependencyProblem(host_, *mod, held.get<std::string>(), "The default for " + key)) problems.push_back(std::move(*problem));
    });
  }
  return problems;
}

// What an enum or a struct editor edits: the asset, and the name each of its rows had when last saved, so a rename
// can be carried to the values holding it. Only the asset counts for changes.
template <typename Asset>
struct Renaming {
  Asset asset;
  std::vector<std::string> originals;  // row for row; empty for a row added since
  bool operator==(const Renaming& other) const { return asset == other.asset; }
};

// The renames since the last save: a row's name then, to its name now.
template <typename Row>
std::map<std::string, std::string> Renames(const std::vector<Row>& rows, const std::vector<std::string>& originals) {
  std::map<std::string, std::string> renames;
  for (size_t i = 0; i < rows.size() && i < originals.size(); ++i) {
    if (!originals[i].empty() && originals[i] != rows[i].name) renames[originals[i]] = rows[i].name;
  }
  return renames;
}

template <typename Row>
std::vector<std::string> NamesOf(const std::vector<Row>& rows) {
  std::vector<std::string> names;
  for (const Row& row : rows) names.push_back(row.name);
  return names;
}

// An enum's values.
class EnumEditor final : public DocumentEditor<Renaming<EnumAsset>> {
 public:
  EnumEditor(EditorHost& host, std::string path, EnumAsset asset) : DocumentEditor(host, std::move(path), {asset, NamesOf(asset.values)}) {}

  bool Save() override;
  void ContentChanged() override;

 protected:
  std::string Title() const override { return std::string(content::NameOf(path_)) + " (enum)"; }
  void DrawContents() override;

 private:
  std::vector<EditorProblem> Problems() const;
};

bool EnumEditor::Save() {
  const std::optional<std::filesystem::path> file = File();
  std::string error;
  if (!file || !content::WriteEnumAsset(*file, document_.asset, &error)) {
    if (file) SetStatus(error, true);
    return false;
  }
  const std::map<std::string, std::string> renames = Renames(document_.asset.values, document_.originals);
  const std::string failed = renames.empty() ? std::string() : content::RenameEnumValues(host_.model().mods(), path_, renames);
  document_.originals = NamesOf(document_.asset.values);
  MarkSaved();
  host_.Saved(path_);
  if (!failed.empty()) SetStatus("Saved, but the renamed values couldn't be renamed everywhere: " + failed, true);
  else SavedStatus(!Problems().empty());
  return true;
}

void EnumEditor::ContentChanged() {
  if (const ModInfo* mod = Mod(); mod && !dirty()) {
    if (std::optional<EnumAsset> asset = content::ReadEnumAsset(content::AssetFile(*mod, path_), nullptr)) Reload({*asset, NamesOf(asset->values)});
  }
}

void EnumEditor::DrawContents() {
  DrawToolbar(CanUndo(), CanRedo());
  DrawProblems(Problems());
  ImGui::BeginChild("##body");
  ImGui::SeparatorText("Enum");
  if (BeginLabelTable("##enum")) {
    LabelRow("Path");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(path_.c_str());
    LabelRow("Help");
    widgets::InputStringMultiline("##help", document_.asset.help, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.0f));
    ImGui::EndTable();
  }

  ImGui::SeparatorText("Values");
  std::vector<content::EnumValue>& values = document_.asset.values;
  size_t acted = 0;
  RowAction action = RowAction::kNone;
  if (!values.empty() && ImGui::BeginTable("##values", 4, ImGuiTableFlags_SizingFixedFit)) {
    const float font = ImGui::GetFontSize();
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, font * kNameColumn);
    ImGui::TableSetupColumn("Shown as", ImGuiTableColumnFlags_WidthFixed, font * kTypeColumn);
    ImGui::TableSetupColumn("Help", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##buttons", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < values.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-FLT_MIN);
      widgets::InputString("##name", values[i].name);
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-FLT_MIN);
      widgets::InputString("##display", values[i].display, 0, values[i].name.c_str());
      ImGui::SetItemTooltip("What the player sees; its name when empty");
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-FLT_MIN);
      widgets::InputString("##help", values[i].help, 0, "What it means");
      ImGui::TableNextColumn();
      if (const RowAction pressed = RowButtons(i, values.size()); pressed != RowAction::kNone) {
        acted = i;
        action = pressed;
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ApplyRowAction(action, acted, values, document_.originals);
  if (values.empty()) ImGui::TextDisabled("No values yet");
  if (glass::Button("Add Value", glass::kCreate)) {
    values.push_back({.name = FreeName(kNewValueName, [&values](const std::string& name) { return std::any_of(values.begin(), values.end(), [&name](const content::EnumValue& value) { return Same(value.name, name); }); })});
    document_.originals.emplace_back();
  }
  ImGui::EndChild();
  Remember();
}

std::vector<EditorProblem> EnumEditor::Problems() const {
  std::vector<EditorProblem> problems;
  const std::vector<content::EnumValue>& values = document_.asset.values;
  if (values.empty()) problems.push_back({"It has no values yet, so nothing can hold one"});
  std::string why;
  for (size_t i = 0; i < values.size(); ++i) {
    if (!content::ValidName(values[i].name, &why)) problems.push_back({"Value " + values[i].name + ": " + why});
    if (std::any_of(values.begin(), values.begin() + static_cast<ptrdiff_t>(i), [&](const content::EnumValue& other) { return Same(other.name, values[i].name); })) problems.push_back({"Two values are named " + values[i].name});
  }
  return problems;
}

// Whether a type holds the struct `path`, itself or inside a struct it holds, which would make a value endless.
bool HoldsStruct(const content::TypeLookup& types, const ValueType& type, std::string_view path, int depth) {
  if (type.type != PropertyType::kStruct || depth > 8) return false;
  if (Same(type.of, path)) return true;
  const std::optional<content::Struct> held = types.find_struct(type.of);
  return held && std::any_of(held->fields.begin(), held->fields.end(), [&](const Property& field) { return HoldsStruct(types, field.type, path, depth + 1); });
}

// A struct's fields.
class StructEditor final : public DocumentEditor<Renaming<StructAsset>> {
 public:
  StructEditor(EditorHost& host, std::string path, StructAsset asset) : DocumentEditor(host, std::move(path), {asset, NamesOf(asset.fields)}) { Refresh(); }

  bool Save() override;
  void ContentChanged() override;

 protected:
  std::string Title() const override { return std::string(content::NameOf(path_)) + " (struct)"; }
  void DrawContents() override;

 private:
  void Refresh();
  std::vector<EditorProblem> Problems();

  TypeChoices choices_;
};

void StructEditor::Refresh() {
  const ModInfo* mod = Mod();
  choices_ = mod ? host_.Choices(mod->folder) : TypeChoices{};
  // A struct can't hold itself.
  std::erase_if(choices_.structs, [this](const std::string& path) { return Same(path, path_); });
}

bool StructEditor::Save() {
  const std::optional<std::filesystem::path> file = File();
  std::string error;
  if (!file || !content::WriteStructAsset(*file, document_.asset, &error)) {
    if (file) SetStatus(error, true);
    return false;
  }
  const std::map<std::string, std::string> renames = Renames(document_.asset.fields, document_.originals);
  const std::string failed = renames.empty() ? std::string() : content::RenameStructFields(host_.model().mods(), path_, renames);
  document_.originals = NamesOf(document_.asset.fields);
  MarkSaved();
  host_.Saved(path_);
  if (!failed.empty()) SetStatus("Saved, but the renamed fields couldn't be renamed everywhere: " + failed, true);
  else SavedStatus(!Problems().empty());
  return true;
}

void StructEditor::ContentChanged() {
  if (const ModInfo* mod = Mod(); mod && !dirty()) {
    if (std::optional<StructAsset> asset = content::ReadStructAsset(content::AssetFile(*mod, path_), nullptr)) Reload({*asset, NamesOf(asset->fields)});
  }
  Refresh();
}

void StructEditor::DrawContents() {
  DrawToolbar(CanUndo(), CanRedo());
  DrawProblems(Problems());
  ImGui::BeginChild("##body");
  ImGui::SeparatorText("Struct");
  if (BeginLabelTable("##struct")) {
    LabelRow("Path");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(path_.c_str());
    LabelRow("Help");
    widgets::InputStringMultiline("##help", document_.asset.help, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.0f));
    ImGui::EndTable();
  }
  ImGui::SeparatorText("Fields");
  std::vector<Property>& fields = document_.asset.fields;
  size_t acted = 0;
  RowAction action = RowAction::kNone;
  DrawDeclared("##fields", fields, host_, choices_, true, &acted, &action);
  ApplyRowAction(action, acted, fields, document_.originals);
  if (fields.empty()) ImGui::TextDisabled("No fields yet");
  if (glass::Button("Add Field", glass::kCreate)) {
    fields.push_back({.name = FreeName(kNewFieldName, [&fields](const std::string& name) { return std::any_of(fields.begin(), fields.end(), [&name](const Property& field) { return Same(field.name, name); }); }), .type = {.type = PropertyType::kInt}, .value = 0});
    document_.originals.emplace_back();
  }
  ImGui::EndChild();
  Remember();
}

std::vector<EditorProblem> StructEditor::Problems() {
  std::vector<EditorProblem> problems;
  const ModInfo* mod = Mod();
  if (!mod) return problems;
  DeclaredProblems(host_, *mod, document_.asset.fields, "Field", problems);
  for (const Property& field : document_.asset.fields) {
    if (HoldsStruct(host_.Types(), field.type, path_, 0)) problems.push_back({"Field " + field.name + " holds this struct, so a value of it would never end"});
  }
  return problems;
}

// A data table's rows, as a grid: a row per entry, a column per field of its struct.
class TableEditor final : public DocumentEditor<content::TableAsset> {
 public:
  TableEditor(EditorHost& host, std::string path, content::TableAsset asset) : DocumentEditor(host, std::move(path), std::move(asset)) { Refresh(); }

  bool Save() override;
  void ContentChanged() override;

 protected:
  std::string Title() const override { return std::string(content::NameOf(path_)) + " (data table)"; }
  void DrawContents() override;

 private:
  void Refresh();
  void DrawGrid(const content::Struct& row);
  std::vector<EditorProblem> Problems();

  TypeChoices choices_;
};

void TableEditor::Refresh() {
  const ModInfo* mod = Mod();
  choices_ = mod ? host_.Choices(mod->folder) : TypeChoices{};
}

bool TableEditor::Save() {
  const std::optional<std::filesystem::path> file = File();
  std::string error;
  if (!file || !content::WriteTableAsset(*file, document_, &error)) {
    if (file) SetStatus(error, true);
    return false;
  }
  MarkSaved();
  host_.Saved(path_);
  SavedStatus(!Problems().empty());
  return true;
}

void TableEditor::ContentChanged() {
  if (const ModInfo* mod = Mod(); mod && !dirty()) {
    if (std::optional<content::TableAsset> asset = content::ReadTableAsset(content::AssetFile(*mod, path_), nullptr)) Reload(std::move(*asset));
  }
  Refresh();
}

void TableEditor::DrawContents() {
  DrawToolbar(CanUndo(), CanRedo());
  DrawProblems(Problems());
  ImGui::BeginChild("##body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::SeparatorText("Data Table");
  if (BeginLabelTable("##table")) {
    LabelRow("Path");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(path_.c_str());
    LabelRow("Rows of");
    if (PathCombo("##row", document_.row.empty() ? "Pick a struct" : document_.row.c_str(), document_.row, choices_.structs, nullptr)) {
      // Each row keeps what still fits the new struct.
      for (auto& [name, value] : document_.rows) {
        nlohmann::json kept;
        value = content::FitValue(host_.Types(), {.type = PropertyType::kStruct, .of = document_.row}, value, &kept, nullptr) ? kept : content::EmptyValue(host_.Types(), {.type = PropertyType::kStruct, .of = document_.row});
      }
    }
    ImGui::SetItemTooltip("The struct each row is");
    LabelRow("Help");
    widgets::InputStringMultiline("##help", document_.help, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.0f));
    ImGui::EndTable();
  }
  ImGui::SeparatorText("Rows");
  if (const std::optional<content::Struct> row = host_.Types().find_struct(document_.row)) DrawGrid(*row);
  else ImGui::TextDisabled("Pick the struct its rows are");
  ImGui::EndChild();
  Remember();
}

void TableEditor::DrawGrid(const content::Struct& row) {
  const float font = ImGui::GetFontSize();
  const ValueType row_type{.type = PropertyType::kStruct, .of = document_.row};
  size_t acted = 0;
  RowAction action = RowAction::kNone;
  const int columns = static_cast<int>(row.fields.size()) + 2;
  if (!document_.rows.empty() && ImGui::BeginTable("##rows", columns, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, font * kNameColumn);
    for (const Property& field : row.fields) ImGui::TableSetupColumn(field.name.c_str(), ImGuiTableColumnFlags_WidthFixed, font * kTypeColumn);
    ImGui::TableSetupColumn("##buttons", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < document_.rows.size(); ++i) {
      auto& [name, value] = document_.rows[i];
      if (!value.is_object()) value = content::EmptyValue(host_.Types(), row_type);
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-FLT_MIN);
      widgets::InputString("##name", name);
      for (const Property& field : row.fields) {
        ImGui::TableNextColumn();
        // A field the row lacks shows its default, and joins the row once it's changed.
        const auto held = value.find(field.name);
        nlohmann::json shown = held != value.end() ? *held : field.value;
        if (ValueField(field.name.c_str(), field.type, shown, host_.Types(), choices_)) value[field.name] = std::move(shown);
      }
      ImGui::TableNextColumn();
      if (const RowAction pressed = RowButtons(i, document_.rows.size()); pressed != RowAction::kNone) {
        acted = i;
        action = pressed;
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ApplyRowAction(action, acted, document_.rows);
  if (document_.rows.empty()) ImGui::TextDisabled("No rows yet");
  if (glass::Button("Add Row", glass::kCreate)) {
    const auto taken = [this](const std::string& name) { return std::any_of(document_.rows.begin(), document_.rows.end(), [&name](const auto& held) { return Same(held.first, name); }); };
    document_.rows.emplace_back(FreeName("Row", taken), content::EmptyValue(host_.Types(), row_type));
  }
}

std::vector<EditorProblem> TableEditor::Problems() {
  std::vector<EditorProblem> problems;
  const ModInfo* mod = Mod();
  if (!mod) return problems;
  if (!host_.Types().find_struct(document_.row)) {
    problems.push_back({document_.row.empty() ? std::string("It has no struct for its rows yet") : document_.row + " isn't a struct"});
    return problems;
  }
  if (auto problem = DependencyProblem(host_, *mod, document_.row, "Its rows' struct")) problems.push_back(std::move(*problem));
  const ValueType row_type{.type = PropertyType::kStruct, .of = document_.row};
  std::string why;
  for (size_t i = 0; i < document_.rows.size(); ++i) {
    const auto& [name, value] = document_.rows[i];
    if (!content::ValidName(name, &why)) problems.push_back({"Row " + name + ": " + why});
    if (std::any_of(document_.rows.begin(), document_.rows.begin() + static_cast<ptrdiff_t>(i), [&name](const auto& other) { return Same(other.first, name); })) problems.push_back({"Two rows are named " + name});
    if (!content::FitValue(host_.Types(), row_type, value, nullptr, &why)) problems.push_back({"Row " + name + ": " + why});
    nlohmann::json walked = value;
    content::WalkValues(host_.Types(), row_type, walked, [&](const ValueType& single, nlohmann::json& held) {
      if (single.type != PropertyType::kAsset || !held.is_string()) return;
      if (auto problem = DependencyProblem(host_, *mod, held.get<std::string>(), "Row " + name)) problems.push_back(std::move(*problem));
    });
  }
  return problems;
}

// One of the game's tables, read only: its rows as the game has them now.
class GameTableEditor final : public AssetEditor {
 public:
  GameTableEditor(EditorHost& host, std::string path) : AssetEditor(host, std::move(path)) {}

  bool dirty() const override { return false; }
  bool Save() override { return true; }

 protected:
  std::string Title() const override { return std::string(content::NameOf(path_)) + " (the game's)"; }
  void DrawContents() override;

 private:
  content::Table table_;
  uint64_t version_ = 0;
};

void GameTableEditor::DrawContents() {
  // Read again only when the game made its rows again.
  const uint64_t version = content::TableVersion(path_);
  if (version != version_) {
    version_ = version;
    if (std::optional<content::Table> found = content::FindTable(path_)) table_ = std::move(*found);
  }
  ImGui::TextWrapped("%s", table_.help.c_str());
  ImGui::TextDisabled("Made from the game's data, so it can't be changed here: the game's own functions change what it describes, and it follows.");
  if (glass::Button("Read Again")) content::TableChanged(path_);
  ImGui::SetItemTooltip("Asks the game for its rows again");
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  if (version_ == 0) ImGui::TextColored(widgets::kWarningColor, "The game hasn't made its rows yet; it does once its data is there");
  else ImGui::TextDisabled("%zu rows of %s", table_.rows.size(), table_.row.c_str());

  const std::optional<content::Struct> row = content::FindStruct(table_.row);
  if (!row || table_.rows.empty()) return;
  const float font = ImGui::GetFontSize();
  const int columns = static_cast<int>(row->fields.size()) + 1;
  if (!ImGui::BeginTable("##rows", columns, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY)) return;
  ImGui::TableSetupScrollFreeze(1, 1);
  ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, font * kNameColumn);
  for (const Property& field : row->fields) ImGui::TableSetupColumn(field.name.c_str(), ImGuiTableColumnFlags_WidthFixed, font * kTypeColumn);
  ImGui::TableHeadersRow();
  for (const auto& [name, value] : table_.rows) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(name.c_str());
    for (const Property& field : row->fields) {
      ImGui::TableNextColumn();
      const auto held = value.find(field.name);
      const std::string text = ValueText(field.type, held != value.end() ? *held : field.value, host_.Types());
      ImGui::TextUnformatted(text.c_str());
      if (ImGui::IsItemHovered() && text.size() > 24) ImGui::SetTooltip("%s", text.c_str());
    }
  }
  ImGui::EndTable();
}

// A mod's texts and their translations into one of the game's languages, side by side.
class TranslationEditor final : public DocumentEditor<content::StringTable> {
 public:
  TranslationEditor(EditorHost& host, const ModInfo& mod);

  bool Save() override;
  void ContentChanged() override;

 protected:
  std::string Title() const override { return folder_ + " (translations)"; }
  void DrawContents() override;

 private:
  enum class Show { kAll, kMissing, kOutOfDate };
  enum class State { kMissing, kOutOfDate, kDone };

  const ModInfo* ModOfEditor() const { return host_.model().mods().Find(folder_); }
  State StateOf(const content::TextEntry& text) const;
  void Pick(const std::string& language);
  void DrawRows();

  std::string folder_;
  std::string language_;
  std::vector<content::TextEntry> texts_;
  Show show_ = Show::kAll;
  std::string search_;
};

TranslationEditor::TranslationEditor(EditorHost& host, const ModInfo& mod) : DocumentEditor(host, mod.folder + std::string(kTranslationsSuffix), {}), folder_(mod.folder) {
  texts_ = content::ModTexts(host_.model().mods(), mod);
  // The first of the game's languages the mod isn't written in.
  const std::string own = content::OwnLanguage(mod);
  for (const content::Language& language : content::Languages()) {
    if (language.code != own) {
      Pick(language.code);
      break;
    }
  }
}

void TranslationEditor::Pick(const std::string& language) {
  language_ = language;
  if (const ModInfo* mod = ModOfEditor()) Reload(content::ReadStringTable(*mod, language));
}

TranslationEditor::State TranslationEditor::StateOf(const content::TextEntry& text) const {
  const auto found = document_.find(text.key);
  if (found == document_.end() || found->second.text.empty()) return State::kMissing;
  return found->second.source == text.source ? State::kDone : State::kOutOfDate;
}

bool TranslationEditor::Save() {
  const ModInfo* mod = ModOfEditor();
  std::string error;
  if (!mod || language_.empty() || !content::WriteStringTable(*mod, language_, document_, &error)) {
    SetStatus(mod ? error : "The mod " + folder_ + " is gone", true);
    return false;
  }
  MarkSaved();
  host_.Saved(path_);
  SetStatus("Saved");
  return true;
}

void TranslationEditor::ContentChanged() {
  const ModInfo* mod = ModOfEditor();
  if (!mod) return;
  texts_ = content::ModTexts(host_.model().mods(), *mod);
  if (!dirty() && !language_.empty()) Reload(content::ReadStringTable(*mod, language_));
}

void TranslationEditor::DrawContents() {
  DrawToolbar(CanUndo(), CanRedo());
  const ModInfo* mod = ModOfEditor();
  if (!mod) return;
  const std::string own = content::OwnLanguage(*mod);
  ImGui::TextDisabled("Written in %s; its texts show translated when the game is in another language.", content::LanguageName(own).c_str());

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Translating into");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * kTypeColumn);
  ImGui::BeginDisabled(dirty());
  if (ImGui::BeginCombo("##language", language_.empty() ? "No other language" : content::LanguageName(language_).c_str())) {
    for (const content::Language& language : content::Languages()) {
      if (language.code != own && ImGui::Selectable(language.name.c_str(), language.code == language_) && language.code != language_) Pick(language.code);
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  if (dirty()) ImGui::SetItemTooltip("Save first");

  size_t missing = 0, out_of_date = 0;
  for (const content::TextEntry& text : texts_) {
    const State state = StateOf(text);
    missing += state == State::kMissing;
    out_of_date += state == State::kOutOfDate;
  }
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%zu texts: %zu missing, %zu out of date", texts_.size(), missing, out_of_date);

  int show = static_cast<int>(show_);
  ImGui::RadioButton("All", &show, static_cast<int>(Show::kAll));
  ImGui::SameLine();
  ImGui::RadioButton("Missing", &show, static_cast<int>(Show::kMissing));
  ImGui::SameLine();
  ImGui::RadioButton("Out of date", &show, static_cast<int>(Show::kOutOfDate));
  show_ = static_cast<Show>(show);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * kTypeColumn);
  widgets::InputString("##search", search_, 0, "Search");

  // Translations whose text went away: kept until dropped, in case it comes back.
  const size_t unused = static_cast<size_t>(std::count_if(document_.begin(), document_.end(), [this](const auto& held) { return std::none_of(texts_.begin(), texts_.end(), [&held](const content::TextEntry& text) { return text.key == held.first; }); }));
  if (unused > 0) {
    ImGui::TextColored(widgets::kWarningColor, "%zu translation%s of texts it doesn't have any more", unused, unused == 1 ? "" : "s");
    ImGui::SameLine();
    if (glass::SmallButton("Drop them", glass::kDanger)) std::erase_if(document_, [this](const auto& held) { return std::none_of(texts_.begin(), texts_.end(), [&held](const content::TextEntry& text) { return text.key == held.first; }); });
  }

  ImGui::BeginChild("##body");
  if (language_.empty()) ImGui::TextDisabled("The game has no other language to translate into");
  else if (texts_.empty()) ImGui::TextDisabled("No texts yet: text properties, fields and rows, and enums' display names, are translated here");
  else DrawRows();
  ImGui::EndChild();
  Remember();
}

void TranslationEditor::DrawRows() {
  if (!ImGui::BeginTable("##texts", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) return;
  ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 0.25f);
  ImGui::TableSetupColumn(content::LanguageName(content::OwnLanguage(*ModOfEditor())).c_str(), ImGuiTableColumnFlags_WidthStretch, 0.3f);
  ImGui::TableSetupColumn(content::LanguageName(language_).c_str(), ImGuiTableColumnFlags_WidthStretch, 0.35f);
  ImGui::TableSetupColumn("##state", ImGuiTableColumnFlags_WidthStretch, 0.1f);
  ImGui::TableHeadersRow();
  for (const content::TextEntry& text : texts_) {
    const State state = StateOf(text);
    if ((show_ == Show::kMissing && state != State::kMissing) || (show_ == Show::kOutOfDate && state != State::kOutOfDate)) continue;
    if (!search_.empty() && Lower(text.key + " " + text.source).find(Lower(search_)) == std::string::npos) continue;
    ImGui::PushID(text.key.c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", std::string(content::NameOf(text.asset)).c_str());
    ImGui::TextDisabled("%s", text.where.c_str());
    ImGui::SetItemTooltip("%s", text.key.c_str());
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", text.source.c_str());
    ImGui::TableNextColumn();
    const auto found = document_.find(text.key);
    std::string translation = found != document_.end() ? found->second.text : std::string();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (widgets::InputString("##translation", translation)) {
      // A translation typed now is of the text as it is now.
      if (translation.empty()) document_.erase(text.key);
      else document_[text.key] = {text.source, translation};
    }
    ImGui::TableNextColumn();
    switch (state) {
      case State::kMissing: ImGui::TextColored(widgets::kErrorColor, "Missing"); break;
      case State::kDone: ImGui::TextColored(widgets::kGoodColor, "Done"); break;
      case State::kOutOfDate:
        ImGui::TextColored(widgets::kWarningColor, "Out of date");
        ImGui::SetItemTooltip("Translated from: %s", found->second.source.c_str());
        if (glass::SmallButton("Still right")) document_[text.key].source = text.source;
        ImGui::SetItemTooltip("The translation fits the text as it is now");
        break;
    }
    ImGui::PopID();
  }
  ImGui::EndTable();
}

// A language asset: the game's own texts, each beside the mod's text in its place, in one of the game's languages.
class LanguageEditor final : public DocumentEditor<content::LanguageAsset> {
 public:
  LanguageEditor(EditorHost& host, std::string path, content::LanguageAsset asset) : DocumentEditor(host, std::move(path), std::move(asset)) {}

  bool Save() override;

 protected:
  std::string Title() const override { return std::string(content::NameOf(path_)) + " (language)"; }
  void DrawContents() override;
  void Undo() override {
    DocumentEditor::Undo();
    stale_ = true;
  }
  void Redo() override {
    DocumentEditor::Redo();
    stale_ = true;
  }

 private:
  enum class Show { kAll, kChanged, kOutOfDate };
  enum class State { kGame, kChanged, kOutOfDate };
  // A text by its group and its place there.
  struct Shown {
    size_t group;
    size_t text;
  };

  // How many texts are changed and out of date, in all and by group, and how many of the asset's the game doesn't have.
  struct Counts {
    size_t changed = 0;
    size_t out_of_date = 0;
    size_t unknown = 0;
    std::vector<size_t> group_changed;
  };

  State StateOf(const content::GameText& text) const;
  // The texts the group, the filter and the search let through, made again only when one of them changed.
  void Refilter();
  // The counts, made again only when the asset's texts, the game's or its language changed.
  void Recount();
  void DrawHeader();
  void DrawGroups();
  void DrawRows();

  std::shared_ptr<const content::GameTexts> texts_;
  // The language the game shows, read each frame, and whether it's the asset's, so the game's texts are the ones the
  // asset's were written over.
  std::string shown_language_;
  bool same_ = false;
  Counts counts_;
  content::StringTable counted_texts_;
  uint64_t counted_version_ = 0;
  bool counted_same_ = false;
  bool counted_ = false;
  // -1 for every group.
  int group_ = -1;
  Show show_ = Show::kAll;
  std::string search_;
  std::vector<Shown> shown_;
  size_t page_ = 0;
  bool stale_ = true;
  int filtered_group_ = -1;
  Show filtered_show_ = Show::kAll;
  std::string filtered_search_;
  uint64_t filtered_version_ = 0;
};

// How many texts a page shows: they can run to tens of thousands.
constexpr size_t kLanguagePageRows = 100;
constexpr float kGroupColumn = 13.0f;
constexpr int kMaxTextLines = 8;

LanguageEditor::State LanguageEditor::StateOf(const content::GameText& text) const {
  const auto found = document_.texts.find(text.id);
  if (found == document_.texts.end() || found->second.text.empty()) return State::kGame;
  // Whether the game's text changed since can only be told in the asset's language.
  return !same_ || found->second.source == text.text ? State::kChanged : State::kOutOfDate;
}

void LanguageEditor::Recount() {
  if (counted_ && counted_version_ == texts_->version && counted_same_ == same_ && counted_texts_ == document_.texts) return;
  counted_ = true;
  counted_version_ = texts_->version;
  counted_same_ = same_;
  counted_texts_ = document_.texts;
  counts_ = {.unknown = document_.texts.size(), .group_changed = std::vector<size_t>(texts_->groups.size())};
  for (size_t g = 0; g < texts_->groups.size(); ++g) {
    for (const content::GameText& text : texts_->groups[g].texts) {
      const State state = StateOf(text);
      counts_.changed += state != State::kGame;
      counts_.group_changed[g] += state != State::kGame;
      counts_.out_of_date += state == State::kOutOfDate;
      counts_.unknown -= document_.texts.contains(text.id);
    }
  }
}

bool LanguageEditor::Save() {
  const std::optional<std::filesystem::path> file = File();
  std::string error;
  if (!file || !content::WriteLanguageAsset(*file, document_, &error)) {
    if (file) SetStatus(error, true);
    return false;
  }
  MarkSaved();
  host_.Saved(path_);
  SavedStatus(false);
  return true;
}

void LanguageEditor::Refilter() {
  const uint64_t version = texts_ ? texts_->version : 0;
  if (!stale_ && filtered_group_ == group_ && filtered_show_ == show_ && filtered_search_ == search_ && filtered_version_ == version) return;
  stale_ = false;
  filtered_group_ = group_;
  filtered_show_ = show_;
  filtered_search_ = search_;
  filtered_version_ = version;
  page_ = 0;
  shown_.clear();
  if (!texts_) return;
  const std::string wanted = Lower(search_);
  for (size_t g = 0; g < texts_->groups.size(); ++g) {
    if (group_ >= 0 && static_cast<size_t>(group_) != g) continue;
    const content::GameTextGroup& group = texts_->groups[g];
    for (size_t t = 0; t < group.texts.size(); ++t) {
      const content::GameText& text = group.texts[t];
      const State state = StateOf(text);
      if ((show_ == Show::kChanged && state == State::kGame) || (show_ == Show::kOutOfDate && state != State::kOutOfDate)) continue;
      if (!wanted.empty()) {
        const auto mod = document_.texts.find(text.id);
        const std::string haystack = Lower(text.id + " " + text.label + " " + text.text + " " + (mod != document_.texts.end() ? mod->second.text : std::string()));
        if (haystack.find(wanted) == std::string::npos) continue;
      }
      shown_.push_back({g, t});
    }
  }
}

void LanguageEditor::DrawContents() {
  DrawToolbar(CanUndo(), CanRedo());
  texts_ = content::ReadGameTexts();
  shown_language_ = content::CurrentLanguage();
  same_ = shown_language_ == document_.language;
  Recount();
  DrawHeader();
  Refilter();
  const float font = ImGui::GetFontSize();
  if (texts_->version == 0) {
    ImGui::TextColored(widgets::kWarningColor, "The game hasn't listed its texts yet; it does once it's running");
  } else {
    if (ImGui::BeginChild("##groups", ImVec2(font * kGroupColumn, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX)) DrawGroups();
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##texts")) DrawRows();
    ImGui::EndChild();
  }
  Remember();
}

void LanguageEditor::DrawHeader() {
  const float font = ImGui::GetFontSize();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Of");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(font * kTypeColumn);
  if (ImGui::BeginCombo("##language", content::LanguageName(document_.language).c_str())) {
    for (const content::Language& language : content::Languages()) {
      if (ImGui::Selectable(language.name.c_str(), language.code == document_.language)) {
        document_.language = language.code;
        stale_ = true;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("Its texts show in place of the game's while the game is in this language");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-FLT_MIN);
  widgets::InputString("##help", document_.help, 0, "What it changes");

  if (!same_) {
    const std::string shown = content::LanguageName(shown_language_);
    const std::string own = content::LanguageName(document_.language);
    ImGui::TextColored(widgets::kWarningColor, "The game is in %s, so the game's texts here are its %s ones. The mod's show once the game is in %s, which is when whether one is out of date can be told.", shown.c_str(), shown.c_str(), own.c_str());
  }

  int show = static_cast<int>(show_);
  ImGui::RadioButton("All", &show, static_cast<int>(Show::kAll));
  ImGui::SameLine();
  ImGui::RadioButton("Changed", &show, static_cast<int>(Show::kChanged));
  ImGui::SameLine();
  ImGui::RadioButton("Out of date", &show, static_cast<int>(Show::kOutOfDate));
  show_ = static_cast<Show>(show);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(font * kTypeColumn);
  widgets::InputString("##search", search_, 0, "Search");
  ImGui::SameLine();
  if (glass::Button("Read Again")) content::GameTextsChanged();
  ImGui::SetItemTooltip("Asks the game for its texts again, with the names of the ones it looked up since");
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%zu changed, %zu out of date", counts_.changed, counts_.out_of_date);

  // Texts of ids the game doesn't have, once it listed them: kept until dropped, in case a later version has them.
  if (texts_->version != 0 && counts_.unknown > 0) {
    ImGui::TextColored(widgets::kWarningColor, "%zu text%s for ones the game doesn't have", counts_.unknown, counts_.unknown == 1 ? "" : "s");
    ImGui::SameLine();
    if (glass::SmallButton("Drop them", glass::kDanger)) {
      std::set<std::string> known;
      for (const content::GameTextGroup& group : texts_->groups) {
        for (const content::GameText& text : group.texts) known.insert(text.id);
      }
      std::erase_if(document_.texts, [&known](const auto& held) { return !known.contains(held.first); });
      stale_ = true;
    }
  }
}

void LanguageEditor::DrawGroups() {
  size_t total = 0;
  for (const content::GameTextGroup& group : texts_->groups) total += group.texts.size();
  const std::string all = "Every group (" + std::to_string(total) + ")";
  if (ImGui::Selectable(all.c_str(), group_ < 0)) group_ = -1;
  // A game can have thousands: only the ones in view are drawn.
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(texts_->groups.size()));
  while (clipper.Step()) {
    for (int g = clipper.DisplayStart; g < clipper.DisplayEnd; ++g) {
      const content::GameTextGroup& group = texts_->groups[static_cast<size_t>(g)];
      const size_t changed = static_cast<size_t>(g) < counts_.group_changed.size() ? counts_.group_changed[static_cast<size_t>(g)] : 0;
      ImGui::PushID(g);
      if (ImGui::Selectable(group.name.c_str(), group_ == g)) group_ = g;
      ImGui::SetItemTooltip("%s: %zu texts, %zu changed", group.name.c_str(), group.texts.size(), changed);
      if (changed > 0) {
        ImGui::SameLine();
        ImGui::TextColored(kOverriddenColor, "%zu", changed);
      }
      ImGui::PopID();
    }
  }
}

void LanguageEditor::DrawRows() {
  if (shown_.empty()) {
    ImGui::TextDisabled("Nothing matches.");
    return;
  }
  const size_t pages = (shown_.size() + kLanguagePageRows - 1) / kLanguagePageRows;
  page_ = std::min(page_, pages - 1);
  const size_t first = page_ * kLanguagePageRows;
  const size_t last = std::min(first + kLanguagePageRows, shown_.size());
  ImGui::BeginDisabled(page_ == 0);
  if (glass::SmallButton("Previous")) --page_;
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(page_ + 1 >= pages);
  if (glass::SmallButton("Next")) ++page_;
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("%zu to %zu of %zu", first + 1, last, shown_.size());

  if (!ImGui::BeginTable("##rows", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) return;
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch, 0.2f);
  ImGui::TableSetupColumn(("The game's (" + content::LanguageName(shown_language_) + ")").c_str(), ImGuiTableColumnFlags_WidthStretch, 0.35f);
  ImGui::TableSetupColumn(("The mod's (" + content::LanguageName(document_.language) + ")").c_str(), ImGuiTableColumnFlags_WidthStretch, 0.35f);
  ImGui::TableSetupColumn("##state", ImGuiTableColumnFlags_WidthStretch, 0.1f);
  ImGui::TableHeadersRow();
  for (size_t i = first; i < last; ++i) {
    const content::GameText& text = texts_->groups[shown_[i].group].texts[shown_[i].text];
    const State state = StateOf(text);
    ImGui::PushID(text.id.c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    const std::string_view id = text.id;
    const std::string_view number = id.substr(id.rfind('#') + 1);
    if (text.label.empty()) ImGui::TextDisabled("#%.*s", static_cast<int>(number.size()), number.data());
    else ImGui::TextWrapped("%s", text.label.c_str());
    ImGui::SetItemTooltip("%s", text.id.c_str());
    if (group_ < 0) ImGui::TextDisabled("%s", texts_->groups[shown_[i].group].name.c_str());
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", text.text.c_str());
    ImGui::TableNextColumn();
    const auto found = document_.texts.find(text.id);
    std::string changed = found != document_.texts.end() ? found->second.text : std::string();
    // As tall as its lines, as the game's texts can have several.
    const int lines = std::clamp(static_cast<int>(std::count(changed.begin(), changed.end(), '\n')) + 1, 1, kMaxTextLines);
    const ImVec2 size(-FLT_MIN, ImGui::GetTextLineHeight() * static_cast<float>(lines) + ImGui::GetStyle().FramePadding.y * 2.0f);
    if (widgets::InputStringMultiline("##text", changed, size)) {
      // A text typed now is over the game's as it is now, when the game shows the asset's language.
      const std::string source = same_ ? text.text : found != document_.texts.end() ? found->second.source : std::string();
      if (changed.empty()) document_.texts.erase(text.id);
      else document_.texts[text.id] = {source, changed};
    }
    if (state == State::kGame && !ImGui::IsItemActive() && ImGui::IsItemHovered()) ImGui::SetTooltip("The game's text shows; type to show the mod's in its place");
    ImGui::TableNextColumn();
    switch (state) {
      case State::kGame: break;
      case State::kChanged:
        ImGui::TextColored(widgets::kGoodColor, "Changed");
        if (glass::SmallButton("Reset")) document_.texts.erase(text.id);
        ImGui::SetItemTooltip("Shows the game's text again");
        break;
      case State::kOutOfDate:
        ImGui::TextColored(widgets::kWarningColor, "Out of date");
        ImGui::SetItemTooltip("Written over: %s", found->second.source.c_str());
        if (glass::SmallButton("Still right")) document_.texts[text.id].source = text.text;
        ImGui::SetItemTooltip("The mod's text fits the game's as it is now");
        break;
    }
    ImGui::PopID();
  }
  ImGui::EndTable();
}

// A map edit: the world it changes and its edits, each a spawn of an avatar class or one of the game's kinds of edit,
// with where it is and the values it sets (docs/content_and_classes.md, Map edits).
class MapEditEditor final : public DocumentEditor<content::MapEditAsset> {
 public:
  MapEditEditor(EditorHost& host, std::string path, content::MapEditAsset asset) : DocumentEditor(host, std::move(path), std::move(asset)) { Resolve(); }
  // The world goes back to the map edit as it was saved.
  ~MapEditEditor() override {
    world_.Stop();
    content::EndPreview(path_);
  }

  bool Save() override;
  void ContentChanged() override { Resolve(); }

 protected:
  std::string Title() const override { return std::string(content::NameOf(path_)) + " (map edit)"; }
  void DrawContents() override;

 private:
  void Resolve();
  void DrawWorld();
  RowAction DrawEdit(content::MapEditEntry& edit, size_t index, size_t count);
  // The properties an edit's values are of: its class's for a spawn, its kind's for a game kind.
  std::vector<Property> DeclaredFor(const content::MapEditEntry& edit) const;
  void DrawValues(content::MapEditEntry& edit, const std::vector<Property>& declared);
  // Editing in the world: its tools, and what the overlay did (a new spawn placed, an edit selected).
  void DrawWorldTools();
  void DrawInWorld();
  std::vector<EditorProblem> Problems();

  TypeChoices choices_;
  std::vector<std::string> spawnable_;  // the avatar classes the mod may spawn
  MapEditWorld world_;
  std::string selected_;     // the edit selected, by name, in the tree and the world
  bool reveal_ = false;      // the selected edit was picked in the world: its node is opened and scrolled to
  std::string place_class_;  // the class Place spawns
};

bool MapEditEditor::Save() {
  const std::optional<std::filesystem::path> file = File();
  std::string error;
  if (!file || !content::WriteMapEditAsset(*file, document_, &error)) {
    if (file) SetStatus(error, true);
    return false;
  }
  MarkSaved();
  host_.Saved(path_);
  SavedStatus(!Problems().empty());
  return true;
}

void MapEditEditor::Resolve() {
  const ModInfo* mod = Mod();
  choices_ = mod ? host_.Choices(mod->folder) : TypeChoices{};
  spawnable_ = mod ? content::AvatarClassPaths(mod->folder) : std::vector<std::string>{};
}

std::vector<Property> MapEditEditor::DeclaredFor(const content::MapEditEntry& edit) const {
  if (edit.kind == content::kSpawnEdit) return edit.spawn.empty() ? std::vector<Property>{} : content::InheritedProperties(host_.model().mods(), edit.spawn);
  const std::shared_ptr<const content::MapEditKind> kind = content::FindMapEditKind(edit.kind);
  return kind ? kind->properties : std::vector<Property>{};
}

void MapEditEditor::DrawContents() {
  DrawToolbar(CanUndo(), CanRedo());
  DrawProblems(Problems());
  DrawInWorld();
  ImGui::BeginChild("##body");
  DrawWorld();
  DrawWorldTools();
  ImGui::SeparatorText("Edits");
  size_t acted = 0;
  RowAction action = RowAction::kNone;
  for (size_t i = 0; i < document_.edits.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    if (const RowAction pressed = DrawEdit(document_.edits[i], i, document_.edits.size()); pressed != RowAction::kNone) {
      action = pressed;
      acted = i;
    }
    ImGui::PopID();
  }
  ApplyRowAction(action, acted, document_.edits);
  if (document_.edits.empty()) ImGui::TextDisabled("No edits yet");
  if (glass::Button("Add Spawn", glass::kCreate)) {
    const auto taken = [this](const std::string& name) { return std::any_of(document_.edits.begin(), document_.edits.end(), [&name](const content::MapEditEntry& edit) { return Same(edit.name, name); }); };
    document_.edits.push_back({.name = FreeName(kNewEditName, taken), .spawn = spawnable_.empty() ? std::string() : spawnable_.front()});
  }
  ImGui::SetItemTooltip("An avatar of one of the classes the mod may spawn, placed in the world");
  ImGui::EndChild();
  // The world shows the edits as they are here, saved or not.
  content::PreviewMapEdit(path_, document_);
  if (!world_.held()) Remember();
}

void MapEditEditor::DrawWorldTools() {
  const ModInfo* mod = Mod();
  std::string reason;
  if (!content::HasWorldView()) reason = "The game gives no view of its world to edit in";
  else if (document_.world.empty() || !Same(content::CurrentWorld(), document_.world)) reason = "The game isn't in " + (document_.world.empty() ? std::string("its world") : document_.world) + " now";
  else if (!mod || !mod->enabled) reason = "The mod is off, so its edits aren't in the world";
  const bool has_selected = std::any_of(document_.edits.begin(), document_.edits.end(), [this](const content::MapEditEntry& edit) { return edit.name == selected_; });
  world_.DrawToolbar(reason.empty(), reason, has_selected);
  if (!world_.placing()) return;
  // What Place makes: a spawn of one of the classes the mod may use, or one of the game's kinds with a place, at first
  // the selected edit's.
  std::vector<std::string> placeable = spawnable_;
  for (const std::string& path : content::MapEditKindPaths()) {
    if (const auto kind = content::FindMapEditKind(path); kind && kind->transform) placeable.push_back(path);
  }
  if (place_class_.empty() || std::none_of(placeable.begin(), placeable.end(), [this](const std::string& path) { return Same(path, place_class_); })) {
    const auto chosen = std::find_if(document_.edits.begin(), document_.edits.end(), [this](const content::MapEditEntry& edit) { return edit.name == selected_; });
    const std::string from_selected = chosen == document_.edits.end() ? std::string() : chosen->kind == content::kSpawnEdit ? chosen->spawn : chosen->kind;
    place_class_ = !from_selected.empty() ? from_selected : placeable.empty() ? std::string() : placeable.front();
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Places");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * kNameColumn * 1.5f);
  PathCombo("##place_class", place_class_.empty() ? "Pick a class or kind" : place_class_.c_str(), place_class_, placeable, nullptr);
  ImGui::SetItemTooltip("What a click in the world makes: a spawn of an avatar class, or one of the game's kinds of edit, which changes what's nearest");
}

void MapEditEditor::DrawInWorld() {
  const std::string before = selected_;
  const MapEditWorld::Result result = world_.Draw(document_.edits, selected_, ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows));
  const auto taken = [this](const std::string& name) { return std::any_of(document_.edits.begin(), document_.edits.end(), [&name](const content::MapEditEntry& edit) { return Same(edit.name, name); }); };
  if (result.place_at && !place_class_.empty()) {
    // A kind of the game's, or else a class to spawn.
    const bool kind = content::FindMapEditKind(place_class_) != nullptr;
    content::MapEditEntry edit{.name = FreeName(std::string(content::NameOf(place_class_)), taken), .kind = kind ? place_class_ : std::string(content::kSpawnEdit), .spawn = kind ? std::string() : place_class_};
    for (int i = 0; i < 3; ++i) edit.transform.position[i] = std::round((*result.place_at)[i] * 10000.0) / 10000.0 + 0.0;
    document_.edits.push_back(std::move(edit));
    selected_ = document_.edits.back().name;
  }
  const auto chosen = std::find_if(document_.edits.begin(), document_.edits.end(), [this](const content::MapEditEntry& edit) { return edit.name == selected_; });
  if (result.duplicate && chosen != document_.edits.end()) {
    // A copy beside it, a step along the world's x, so it's seen apart.
    content::MapEditEntry copy = *chosen;
    copy.name = FreeName(chosen->name, taken);
    copy.transform.position[0] += kDuplicateOffset;
    selected_ = copy.name;
    document_.edits.insert(chosen + 1, std::move(copy));
  } else if (result.remove && chosen != document_.edits.end()) {
    document_.edits.erase(chosen);
    selected_.clear();
  }
  // Picked in the world (a marker clicked, a spawn placed): its node is shown.
  if (selected_ != before) reveal_ = true;
}

void MapEditEditor::DrawWorld() {
  ImGui::SeparatorText("Map edit");
  if (!BeginLabelTable("##map_edit")) return;
  LabelRow("Path");
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(path_.c_str());
  LabelRow("World");
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::BeginCombo("##world", document_.world.empty() ? "Pick a world" : document_.world.c_str())) {
    for (const content::WorldDecl& world : content::Worlds()) {
      if (ImGui::Selectable(world.path.c_str(), Same(world.path, document_.world))) document_.world = world.path;
      if (!world.help.empty()) ImGui::SetItemTooltip("%s", world.help.c_str());
    }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("The world it changes: its edits are made each time the game enters it, in order");
  LabelRow("Help");
  widgets::InputStringMultiline("##help", document_.help, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 2.0f));
  ImGui::EndTable();
  const ModInfo* mod = Mod();
  if (Same(content::CurrentWorld(), document_.world) && !document_.world.empty()) {
    if (mod && mod->enabled) ImGui::TextDisabled("The game is in %s: what you change shows there as you change it.", document_.world.c_str());
    else ImGui::TextDisabled("The game is in %s, but this mod is off, so its edits aren't made.", document_.world.c_str());
  } else if (!document_.world.empty()) {
    ImGui::TextDisabled("Its edits are made when the game enters %s.", document_.world.c_str());
  }
}

RowAction MapEditEditor::DrawEdit(content::MapEditEntry& edit, size_t index, size_t count) {
  const bool spawn = edit.kind == content::kSpawnEdit;
  const std::shared_ptr<const content::MapEditKind> kind = spawn ? nullptr : content::FindMapEditKind(edit.kind);
  const std::string label = spawn ? edit.name + " (spawns " + (edit.spawn.empty() ? std::string("nothing") : edit.spawn) + ")" : edit.name + " (" + edit.kind + ")";
  const bool is_selected = edit.name == selected_;
  if (is_selected && reveal_) ImGui::SetNextItemOpen(true);
  const bool open = ImGui::TreeNodeEx("##edit", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | (is_selected ? ImGuiTreeNodeFlags_Selected : 0), "%s", label.c_str());
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selected_ = edit.name;
  if (is_selected && reveal_) {
    ImGui::SetScrollHereY();
    reveal_ = false;
  }
  if (kind && !kind->help.empty()) ImGui::SetItemTooltip("%s", kind->help.c_str());
  ImGui::SameLine();
  const RowAction action = RowButtons(index, count);
  // What the world made of it, while the game is in it.
  if (Same(content::CurrentWorld(), document_.world)) {
    if (const std::string problem = content::MapEditProblem(path_, edit.name); !problem.empty()) {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(widgets::kErrorColor, "In the world: %s", problem.c_str());
      ImGui::PopTextWrapPos();
    }
  }
  if (!open) return action;
  if (BeginLabelTable("##fields")) {
    LabelRow("Name");
    widgets::InputString("##name", edit.name);
    LabelRow("Kind");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##kind", edit.kind.c_str())) {
      std::vector<std::string> kinds{std::string(content::kSpawnEdit)};
      for (const std::string& path : content::MapEditKindPaths()) kinds.push_back(path);
      for (const std::string& path : kinds) {
        if (ImGui::Selectable(path.c_str(), Same(path, edit.kind)) && !Same(path, edit.kind)) {
          // Another kind's values would only be problems.
          edit.kind = path;
          edit.values = nlohmann::json::object();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Spawn places an avatar of a class; the game's own kinds change what's there already");
    if (spawn) {
      LabelRow("Class");
      PathCombo("##class", edit.spawn.empty() ? "Pick a class" : edit.spawn.c_str(), edit.spawn, spawnable_, nullptr);
      ImGui::SetItemTooltip("The avatar class it spawns: one the mod may use, with a backing");
    }
    if (spawn || (kind && kind->transform)) {
      LabelRow("Position");
      ImGui::DragScalarN("##position", ImGuiDataType_Double, edit.transform.position.data(), 3, kTransformDragSpeed);
      ImGui::SetItemTooltip("Where it is in the world, in the game's units and axes, y up");
      LabelRow("Rotation");
      ImGui::DragScalarN("##rotation", ImGuiDataType_Double, edit.transform.rotation.data(), 3, 1.0f);
      ImGui::SetItemTooltip("Pitch, yaw and roll, in degrees");
      LabelRow("Scale");
      ImGui::DragScalarN("##scale", ImGuiDataType_Double, edit.transform.scale.data(), 3, kTransformDragSpeed);
    }
    ImGui::EndTable();
  }
  DrawValues(edit, DeclaredFor(edit));
  ImGui::TreePop();
  return action;
}

// Each property with the value this edit gives it, in blue with a reset when it sets one: a spawn's set on its avatar
// alone, over its class's; a kind's over the kind's default.
void MapEditEditor::DrawValues(content::MapEditEntry& edit, const std::vector<Property>& declared) {
  if (declared.empty() || !ImGui::BeginTable("##values", 3, ImGuiTableFlags_SizingFixedFit)) return;
  ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * kNameColumn);
  ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed);
  for (const Property& property : declared) {
    ImGui::PushID(property.name.c_str());
    const std::string key = KeyOf(edit.values, property.name);
    nlohmann::json value = key.empty() ? property.value : edit.values[key];
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    if (key.empty()) ImGui::TextUnformatted(property.name.c_str());
    else ImGui::TextColored(kOverriddenColor, "%s", property.name.c_str());
    ImGui::SetItemTooltip("%s%s%s, %s", property.help.c_str(), property.help.empty() ? "" : "\n", content::Describe(property.type).c_str(), key.empty() ? (edit.kind == content::kSpawnEdit ? "as its class has it" : "the default") : "set here");
    ImGui::TableNextColumn();
    if (ValueField("##value", property.type, value, host_.Types(), choices_)) edit.values[key.empty() ? property.name : key] = value;
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(key.empty());
    if (glass::SmallButton("Reset")) edit.values.erase(key);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(edit.kind == content::kSpawnEdit ? "Back to its class's value" : "Back to the default");
    ImGui::PopID();
  }
  ImGui::EndTable();
}

std::vector<EditorProblem> MapEditEditor::Problems() {
  std::vector<EditorProblem> problems;
  const ModInfo* mod = Mod();
  if (!mod) return problems;
  if (document_.world.empty()) problems.push_back({"It changes no world: pick one"});
  else if (!content::HasWorld(document_.world)) problems.push_back({"The game has no world " + document_.world});
  std::vector<std::string> names;
  for (size_t i = 0; i < document_.edits.size(); ++i) {
    content::MapEditEntry& edit = document_.edits[i];
    const std::string what = "Edit " + (edit.name.empty() ? std::to_string(i + 1) : edit.name);
    if (std::string why; !content::ValidName(edit.name, &why)) problems.push_back({what + ": " + why});
    if (std::any_of(names.begin(), names.end(), [&edit](const std::string& name) { return Same(name, edit.name); })) problems.push_back({what + ": the name is taken by an edit above it"});
    names.push_back(edit.name);
    if (edit.kind == content::kSpawnEdit) {
      if (edit.spawn.empty()) {
        problems.push_back({what + ": it spawns no class"});
        continue;
      }
      if (!content::FindClass(edit.spawn) && content::ClassChain(host_.model().mods(), edit.spawn).empty()) problems.push_back({what + ": there's no class " + edit.spawn});
      else if (std::none_of(spawnable_.begin(), spawnable_.end(), [&edit](const std::string& path) { return Same(path, edit.spawn); })) problems.push_back({what + ": " + edit.spawn + " isn't an avatar class (nothing gives it a backing), or it's a mod's this one doesn't depend on"});
      if (auto problem = DependencyProblem(host_, *mod, edit.spawn, what + "'s class")) problems.push_back(std::move(*problem));
    } else if (!content::FindMapEditKind(edit.kind)) {
      problems.push_back({what + ": the game has no kind of edit " + edit.kind});
      continue;
    }
    // Values for properties it has, each of their types.
    const std::vector<Property> declared = DeclaredFor(edit);
    for (const auto& [name, value] : edit.values.items()) {
      const auto found = std::find_if(declared.begin(), declared.end(), [&name](const Property& property) { return Same(property.name, name); });
      if (found == declared.end()) {
        problems.push_back({what + " sets " + name + ", which it has no property of", "Remove it", [&edit, key = name] { edit.values.erase(key); }});
        continue;
      }
      nlohmann::json kept;
      if (std::string why; !content::FitValue(host_.Types(), found->type, value, &kept, &why)) problems.push_back({what + ", " + name + ": " + why});
    }
  }
  return problems;
}

// A material: its type, the values it gives the type's properties, and the game's preview of it in a viewport, which
// shows it as the editor has it, saved or not (docs/content_and_classes.md, Materials).
class MaterialEditor final : public DocumentEditor<content::MaterialAsset> {
 public:
  MaterialEditor(EditorHost& host, std::string path, content::MaterialAsset asset) : DocumentEditor(host, std::move(path), std::move(asset)) { Resolve(); }
  // What uses it goes back to the material as it was saved.
  ~MaterialEditor() override { content::EndMaterialPreview(path_); }

  bool Save() override;
  void ContentChanged() override { Resolve(); }

 protected:
  std::string Title() const override { return std::string(content::NameOf(path_)) + " (material)"; }
  float WidthFactor() const override { return kAvatarEditorWidth; }
  void DrawContents() override;

 private:
  void Resolve();
  void DrawViewport();
  void DrawBody();
  void DrawValues(const content::MaterialType& type);
  std::vector<EditorProblem> Problems();

  TypeChoices choices_;
  std::unique_ptr<AvatarViewport> viewport_;
  std::vector<content::Component> none_;  // the viewport's own components: a material has none
  std::string selected_;
};

bool MaterialEditor::Save() {
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

void MaterialEditor::Resolve() {
  const ModInfo* mod = Mod();
  choices_ = mod ? host_.Choices(mod->folder) : TypeChoices{};
}

void MaterialEditor::DrawContents() {
  DrawToolbar(CanUndo(), CanRedo());
  DrawProblems(Problems());
  if (ImGui::BeginTable("##material_editor", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImGui::GetContentRegionAvail())) {
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::TableSetupColumn("##viewport", ImGuiTableColumnFlags_WidthFixed, std::max(width * kViewportShare, ImGui::GetFontSize() * kViewportMinWidth));
    ImGui::TableSetupColumn("##body", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    DrawViewport();
    ImGui::TableNextColumn();
    DrawBody();
    ImGui::EndTable();
  }
  // What's drawn with it shows it as it is here, saved or not.
  content::PreviewMaterial(path_, document_);
  Remember();
}

// The type's preview: its component alone, with the type's values, this material, and the model (or whatever the type
// lets the editor pick) the material is shown on.
void MaterialEditor::DrawViewport() {
  const std::shared_ptr<const content::MaterialType> type = content::FindMaterialType(document_.type);
  if (!viewport_) viewport_ = std::make_unique<AvatarViewport>();
  std::vector<content::Component> shown;
  if (type && !type->preview_component.empty()) {
    content::Component preview{.name = "Preview", .type = type->preview_component};
    for (const auto& [name, value] : type->preview_values) preview.properties[name] = content::ToJson(value);
    if (!type->preview_material.empty()) preview.properties[type->preview_material] = path_;
    if (!type->preview_choice.empty() && !document_.preview.is_null()) preview.properties[type->preview_choice] = document_.preview;
    shown.push_back(std::move(preview));
  }
  viewport_->Draw(ImGui::GetContentRegionAvail(), content::ViewportView{}, shown, none_, selected_);
}

void MaterialEditor::DrawBody() {
  ImGui::BeginChild("##body");
  ImGui::SeparatorText("Material");
  const std::shared_ptr<const content::MaterialType> type = content::FindMaterialType(document_.type);
  if (BeginLabelTable("##material")) {
    LabelRow("Path");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(path_.c_str());
    LabelRow("Type");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##type", document_.type.empty() ? "Pick a type" : document_.type.c_str())) {
      for (const std::string& path : content::MaterialTypePaths()) {
        // A graph material has an editor of its own.
        if (const auto declared = content::FindMaterialType(path); declared && declared->HasGraph()) continue;
        if (ImGui::Selectable(path.c_str(), Same(path, document_.type)) && !Same(path, document_.type)) {
          // Another type's values would only be problems.
          document_.type = path;
          document_.values = nlohmann::json::object();
          document_.preview = nullptr;
        }
        if (const auto declared = content::FindMaterialType(path); declared && !declared->help.empty()) ImGui::SetItemTooltip("%s", declared->help.c_str());
      }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", type && !type->help.empty() ? type->help.c_str() : "The game's kind of material it is");
    LabelRow("Help");
    widgets::InputStringMultiline("##help", document_.help, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 2.0f));
    // What the viewport shows it on, kept with the material.
    const std::shared_ptr<const content::ComponentType> component = type ? content::FindComponentType(type->preview_component) : nullptr;
    const auto choice = component ? std::find_if(component->properties.begin(), component->properties.end(), [&type](const Property& property) { return Same(property.name, type->preview_choice); }) : std::vector<Property>::const_iterator{};
    if (component && choice != component->properties.end()) {
      LabelRow("Shown on");
      nlohmann::json value = document_.preview.is_null() ? (type->preview_values.contains(choice->name) ? content::ToJson(type->preview_values.at(choice->name)) : choice->value) : document_.preview;
      if (ValueField("##preview", choice->type, value, host_.Types(), choices_)) document_.preview = value;
      ImGui::SetItemTooltip("What the viewport shows the material on, its %s; kept with the material, but only for the editor", choice->name.c_str());
    }
    ImGui::EndTable();
  }
  if (type) {
    ImGui::SeparatorText("Values");
    DrawValues(*type);
  }
  ImGui::EndChild();
}

// Each of the type's properties with the value this material gives it, in blue with a reset when it gives one.
void MaterialEditor::DrawValues(const content::MaterialType& type) { DrawSetValues("##values", type.properties, document_.values, host_, choices_); }

std::vector<EditorProblem> MaterialEditor::Problems() {
  std::vector<EditorProblem> problems;
  const ModInfo* mod = Mod();
  if (!mod) return problems;
  const std::shared_ptr<const content::MaterialType> type = content::FindMaterialType(document_.type);
  if (document_.type.empty()) {
    problems.push_back({"It has no type: pick one"});
    return problems;
  }
  if (!type) {
    problems.push_back({"The game has no material type " + document_.type});
    return problems;
  }
  for (const auto& [name, value] : document_.values.items()) {
    const auto found = std::find_if(type->properties.begin(), type->properties.end(), [&name](const Property& property) { return Same(property.name, name); });
    if (found == type->properties.end()) {
      problems.push_back({"It sets " + name + ", which its type has no property of", "Remove it", [this, key = name] { document_.values.erase(key); }});
      continue;
    }
    nlohmann::json kept;
    if (std::string why; !content::FitValue(host_.Types(), found->type, value, &kept, &why)) {
      problems.push_back({name + ": " + why});
      continue;
    }
    nlohmann::json walked = value;
    content::WalkValues(host_.Types(), found->type, walked, [&](const ValueType& single, nlohmann::json& held) {
      if (single.type != PropertyType::kAsset || !held.is_string()) return;
      if (auto problem = DependencyProblem(host_, *mod, held.get<std::string>(), name)) problems.push_back(std::move(*problem));
    });
  }
  return problems;
}

// A texture: its picture shown as large as the window allows, what it's for, and another picture taken in its place.
class TextureEditor final : public DocumentEditor<content::TextureAsset> {
 public:
  TextureEditor(EditorHost& host, std::string path, content::TextureAsset asset) : DocumentEditor(host, std::move(path), std::move(asset)) {}

  bool Save() override;

 protected:
  std::string Title() const override { return std::string(content::NameOf(path_)) + " (texture)"; }
  void DrawContents() override;

 private:
  // The Open dialog's answer, written on its thread.
  struct Picked {
    std::mutex mutex;
    bool done = false;
    std::optional<std::filesystem::path> file;
    std::string error;
  };
  void TakePicked();

  std::shared_ptr<Picked> picked_ = std::make_shared<Picked>();
  bool picking_ = false;
};

bool TextureEditor::Save() {
  const std::optional<std::filesystem::path> file = File();
  std::string error;
  if (!file || !content::WriteTextureAsset(*file, document_, &error)) {
    if (file) SetStatus(error, true);
    return false;
  }
  MarkSaved();
  host_.Saved(path_);
  SavedStatus(false);
  return true;
}

// A new picture is taken in at once: it's a file beside the asset, not part of what Save writes.
void TextureEditor::TakePicked() {
  std::optional<std::filesystem::path> picked;
  std::string error;
  {
    std::lock_guard lock(picked_->mutex);
    if (!picked_->done) return;
    picked_->done = false;
    picked = std::move(picked_->file);
    error = std::move(picked_->error);
  }
  picking_ = false;
  if (!error.empty()) SetStatus(error, true);
  const std::optional<std::filesystem::path> file = picked ? File() : std::nullopt;
  if (!file) return;
  if (!content::SetTextureImage(*file, *picked, &error)) {
    SetStatus(error, true);
    return;
  }
  host_.Saved(path_);
  SetStatus("Took in " + picked->filename().string());
}

void TextureEditor::DrawContents() {
  TakePicked();
  DrawToolbar(CanUndo(), CanRedo());
  const std::optional<std::filesystem::path> file = File();
  const std::filesystem::path image = file ? content::TextureImageFile(*file) : std::filesystem::path();
  const MenuModel::Picture picture = image.empty() ? MenuModel::Picture{} : host_.model().AssetPicture(image);
  if (BeginLabelTable("##texture")) {
    LabelRow("Help");
    widgets::InputString("##help", document_.help, 0, "What it's for");
    LabelRow("Picture");
    ImGui::AlignTextToFramePadding();
    if (image.empty()) ImGui::TextColored(widgets::kErrorColor, "There's no picture beside it");
    else if (picture.id == ImTextureID_Invalid) ImGui::TextColored(widgets::kErrorColor, "%s can't be read as a picture", image.filename().string().c_str());
    else ImGui::Text("%s, %u x %u", image.filename().string().c_str(), picture.width, picture.height);
    ImGui::SameLine();
    ImGui::BeginDisabled(picking_);
    if (glass::SmallButton("Replace...")) {
      picking_ = true;
      file_picker::ChoosePicture([picked = picked_](std::optional<std::filesystem::path> chosen, std::string error) {
        std::lock_guard lock(picked->mutex);
        picked->done = true;
        picked->file = std::move(chosen);
        picked->error = std::move(error);
      });
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Takes another picture in its place, at once: whatever shows it shows the new one");
    ImGui::EndTable();
  }
  if (picture.id != ImTextureID_Invalid && picture.width && picture.height) {
    // As large as fits, its shape kept.
    const ImVec2 room = ImGui::GetContentRegionAvail();
    const float scale = std::min(room.x / static_cast<float>(picture.width), room.y / static_cast<float>(picture.height));
    ImGui::Image(ImTextureRef(picture.id), ImVec2(static_cast<float>(picture.width) * scale, static_cast<float>(picture.height) * scale));
  }
  Remember();
}

// A mod's mod.toml, as the browser's metadata asset.
class MetadataEditor final : public DocumentEditor<ModInfo> {
 public:
  MetadataEditor(EditorHost& host, const ModInfo& mod) : DocumentEditor(host, mod.folder, mod) {}

  bool Save() override;
  void ContentChanged() override;

 protected:
  bool Equal(const ModInfo& a, const ModInfo& b) const override;
  std::string Title() const override { return document_.name + " (metadata)"; }
  void DrawContents() override;

 private:
  const ModInfo* Mod() const { return host_.model().mods().Find(path_); }
  void DrawFields();
  void DrawDependencies();
  std::vector<EditorProblem> Problems() const;
  // Whether the mod loaded, or why not.
  std::string LoadState(bool* loads) const;
};

bool MetadataEditor::Equal(const ModInfo& a, const ModInfo& b) const {
  const auto same_dependency = [](const ModDependency& x, const ModDependency& y) { return x.mod == y.mod && x.version == y.version; };
  return a.name == b.name && a.description == b.description && a.version == b.version && a.author == b.author && a.min_game_version == b.min_game_version && a.language == b.language && std::equal(a.dependencies.begin(), a.dependencies.end(), b.dependencies.begin(), b.dependencies.end(), same_dependency);
}

bool MetadataEditor::Save() {
  std::string error;
  if (!WriteMetadata(document_, &error)) {
    SetStatus(error, true);
    return false;
  }
  MarkSaved();
  NoteModsChanged();
  host_.Saved(path_);
  SetStatus("Saved");
  return true;
}

void MetadataEditor::ContentChanged() {
  // The file changed from elsewhere (a dependency added from a class's problems): shown as it is now, unless this
  // editor has changes of its own.
  if (const ModInfo* mod = Mod(); mod && !dirty()) Reload(*mod);
}

std::string MetadataEditor::LoadState(bool* loads) const {
  *loads = false;
  const ModInfo* mod = Mod();
  if (!mod) return "It isn't in the mods folder any more";
  if (!mod->enabled) return "It's off, so it doesn't load; turn it on in the mods menu";
  const std::vector<ModInfo>& mods = host_.model().mods().mods();
  const std::vector<ModInfo> enabled(mods.begin(), mods.begin() + static_cast<ptrdiff_t>(host_.model().mods().EnabledCount()));
  std::vector<RefusedMod> refused;
  LoadableMods(enabled, GetConfig().game_version, &refused);
  for (const RefusedMod& entry : refused) {
    if (SameModName(entry.folder, mod->folder)) return "It doesn't load: it " + entry.reason;
  }
  *loads = true;
  return "It loads";
}

void MetadataEditor::DrawContents() {
  DrawToolbar(CanUndo(), CanRedo());
  bool loads = false;
  const std::string state = LoadState(&loads);
  ImGui::TextColored(loads ? widgets::kGoodColor : widgets::kWarningColor, "%s", state.c_str());
  DrawProblems(Problems());
  ImGui::BeginChild("##body");
  DrawFields();
  DrawDependencies();
  ImGui::EndChild();
  Remember();
}

void MetadataEditor::DrawFields() {
  ImGui::SeparatorText("Mod");
  if (!BeginLabelTable("##fields")) return;
  const float font = ImGui::GetFontSize();
  LabelRow("Folder");
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(document_.folder.c_str());
  ImGui::SetItemTooltip("Its name in paths; renamed in the mods menu");
  if (const ModInfo* mod = Mod()) {
    LabelRow("Icon");
    const ImTextureID icon = host_.model().ModIcon(*mod);
    if (icon != ImTextureID_Invalid) ImGui::Image(icon, ImVec2(font * 4.0f, font * 4.0f));
    else ImGui::TextDisabled("None");
    ImGui::SetItemTooltip("icon.png in the mod's folder");
  }
  LabelRow("Name");
  widgets::InputString("##name", document_.name);
  LabelRow("Version");
  widgets::InputString("##version", document_.version, 0, "1.0.0");
  LabelRow("Author");
  widgets::InputString("##author", document_.author);
  LabelRow("Written in");
  const std::vector<content::Language> languages = content::Languages();
  const std::string first = languages.empty() ? std::string("The game's first language") : languages.front().name + " (the game's first)";
  if (ImGui::BeginCombo("##language", document_.language.empty() ? first.c_str() : content::LanguageName(document_.language).c_str())) {
    if (ImGui::Selectable(first.c_str(), document_.language.empty())) document_.language.clear();
    for (const content::Language& language : languages) {
      if (ImGui::Selectable(language.name.c_str(), language.code == document_.language)) document_.language = language.code;
    }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("The language its texts are written in; their translations into the others are in its translations");
  LabelRow("Game version");
  const std::string& running = GetConfig().game_version;
  const std::string hint = running.empty() ? std::string("Any") : "Any; this game is " + running;
  widgets::InputString("##game", document_.min_game_version, 0, hint.c_str());
  ImGui::SetItemTooltip("The lowest version of the game it works with");
  LabelRow("Description");
  widgets::InputStringMultiline("##description", document_.description, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f));
  ImGui::EndTable();
}

void MetadataEditor::DrawDependencies() {
  ImGui::SeparatorText("Dependencies");
  const std::vector<ModInfo>& mods = host_.model().mods().mods();
  const auto label = [&mods](const std::string& folder) {
    const auto found = std::find_if(mods.begin(), mods.end(), [&folder](const ModInfo& mod) { return SameModName(mod.folder, folder); });
    return found == mods.end() || found->name == found->folder ? folder : found->name + " (" + found->folder + ")";
  };
  std::optional<size_t> remove;
  if (!document_.dependencies.empty() && ImGui::BeginTable("##dependencies", 3, ImGuiTableFlags_SizingFixedFit)) {
    const float font = ImGui::GetFontSize();
    ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Lowest version", ImGuiTableColumnFlags_WidthFixed, font * kVersionColumn);
    ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < document_.dependencies.size(); ++i) {
      ModDependency& dependency = document_.dependencies[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(label(dependency.mod).c_str());
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-FLT_MIN);
      widgets::InputString("##version", dependency.version, 0, "Any");
      ImGui::TableNextColumn();
      if (glass::SmallButton("Remove", glass::kDanger)) remove = i;
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (remove) document_.dependencies.erase(document_.dependencies.begin() + static_cast<ptrdiff_t>(*remove));
  if (document_.dependencies.empty()) ImGui::TextDisabled("None: it uses only its own content and the game's");

  if (glass::Button("Add Dependency", glass::kCreate)) ImGui::OpenPopup("##add_dependency");
  if (ImGui::BeginPopup("##add_dependency")) {
    bool any = false;
    for (const ModInfo& mod : mods) {
      const auto named = [&mod](const ModDependency& dependency) { return SameModName(dependency.mod, mod.folder); };
      if (SameModName(mod.folder, document_.folder) || std::any_of(document_.dependencies.begin(), document_.dependencies.end(), named)) continue;
      any = true;
      if (ImGui::Selectable(label(mod.folder).c_str())) document_.dependencies.push_back({mod.folder, mod.version});
    }
    if (!any) ImGui::TextDisabled("No other mods to depend on");
    ImGui::EndPopup();
  }
}

std::vector<EditorProblem> MetadataEditor::Problems() const {
  std::vector<EditorProblem> problems;
  std::string why;
  if (!content::ValidName(document_.folder, &why)) problems.push_back({"Its folder's name can't name content (" + why + "), so its content doesn't load; rename it in the mods menu"});
  const std::string& running = GetConfig().game_version;
  if (!running.empty() && !document_.min_game_version.empty() && CompareVersions(running, document_.min_game_version) < 0) problems.push_back({"It needs game version " + document_.min_game_version + ", and this is " + running});
  for (const ModDependency& dependency : document_.dependencies) {
    const ModInfo* mod = host_.model().mods().Find(dependency.mod);
    if (!mod) problems.push_back({dependency.mod + " isn't installed"});
    else if (!mod->enabled) problems.push_back({mod->name + " is off, so this mod doesn't load until it's on"});
    else if (!dependency.version.empty() && CompareVersions(mod->version, dependency.version) < 0) problems.push_back({mod->name + " is " + (mod->version.empty() ? std::string("unversioned") : mod->version) + ", below the " + dependency.version + " asked for"});
  }
  return problems;
}

}  // namespace

bool AssetEditor::Draw() {
  if (closed_) return false;
  const float font = ImGui::GetFontSize();
  if (!placed_) {
    static int opened = 0;
    const float step = font * kCascadeStep * static_cast<float>(opened++ % kCascadeCount);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float wide = WidthFactor();
    const ImVec2 size(std::min(std::clamp(display.x * kEditorScreenWidth * wide, font * kEditorMinWidth, font * kEditorMaxWidth * wide), display.x), std::max(display.y * kEditorScreenHeight, font * kEditorMinHeight));
    const ImVec2 at = host_.EditorPosition(size);
    ImGui::SetNextWindowPos(ImVec2(at.x + step, at.y + step), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    placed_ = true;
  }
  if (focus_) {
    ImGui::SetNextWindowFocus();
    focus_ = false;
  }
  bool open = true;
  const std::string title = Title() + (dirty() ? " *" : "") + "###asset:" + path_;
  if (glass::BeginWindow(title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) Save();
      // A text field has undo of its own.
      if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? Redo() : Undo();
      if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
    }
    DrawContents();

    if (open_save_prompt_) {
      open_save_prompt_ = false;
      ImGui::OpenPopup(kSavePopup);
    }
    if (ImGui::BeginPopupModal(kSavePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("Save the changes to %s first?", Title().c_str());
      if (glass::Button("Save", glass::kAccent)) {
        closed_ = Save();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (glass::Button("Don't Save", glass::kDanger)) {
        closed_ = true;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (glass::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  }
  ImGui::End();
  if (!open) RequestClose();
  return !closed_;
}

void AssetEditor::RequestClose() {
  if (dirty()) {
    open_save_prompt_ = true;
    focus_ = true;
  } else {
    closed_ = true;
  }
}

void AssetEditor::SetStatus(std::string text, bool error) {
  status_ = std::move(text);
  status_error_ = error;
  status_time_ = ImGui::GetTime();
}

void AssetEditor::DrawToolbar(bool can_undo, bool can_redo) {
  ImGui::BeginDisabled(!dirty());
  if (glass::Button("Save", glass::kAccent)) Save();
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Writes it, and loads the content again (Ctrl+S)");
  ImGui::SameLine();
  ImGui::BeginDisabled(!can_undo);
  if (glass::Button("Undo")) Undo();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!can_redo);
  if (glass::Button("Redo")) Redo();
  ImGui::EndDisabled();
  if (!status_.empty() && ImGui::GetTime() - status_time_ < widgets::kStatusSeconds) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(status_error_ ? widgets::kErrorColor : widgets::kGoodColor, "%s", status_.c_str());
  }
}

void AssetEditor::DrawProblems(const std::vector<EditorProblem>& problems) {
  if (problems.empty()) return;
  // A fix runs after the list, as it changes what the list is made from.
  std::function<void()> fix;
  for (size_t i = 0; i < problems.size(); ++i) {
    const EditorProblem& problem = problems[i];
    ImGui::PushID(static_cast<int>(i));
    if (problem.fix) {
      if (glass::SmallButton(problem.fix_label.c_str(), glass::kCreate)) fix = problem.fix;
      ImGui::SameLine();
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(widgets::kErrorColor, "%s", problem.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopID();
  }
  ImGui::Separator();
  if (fix) fix();
}

std::unique_ptr<AssetEditor> OpenClassEditor(EditorHost& host, const std::string& path) {
  const ModInfo* mod = host.model().mods().Find(content::ModOf(path));
  std::optional<ClassAsset> asset = mod ? content::ReadClassAsset(content::AssetFile(*mod, path), nullptr) : std::nullopt;
  return asset ? std::make_unique<ClassEditor>(host, path, std::move(*asset)) : nullptr;
}

std::unique_ptr<AssetEditor> OpenEnumEditor(EditorHost& host, const std::string& path) {
  const ModInfo* mod = host.model().mods().Find(content::ModOf(path));
  std::optional<EnumAsset> asset = mod ? content::ReadEnumAsset(content::AssetFile(*mod, path), nullptr) : std::nullopt;
  return asset ? std::make_unique<EnumEditor>(host, path, std::move(*asset)) : nullptr;
}

std::unique_ptr<AssetEditor> OpenStructEditor(EditorHost& host, const std::string& path) {
  const ModInfo* mod = host.model().mods().Find(content::ModOf(path));
  std::optional<StructAsset> asset = mod ? content::ReadStructAsset(content::AssetFile(*mod, path), nullptr) : std::nullopt;
  return asset ? std::make_unique<StructEditor>(host, path, std::move(*asset)) : nullptr;
}

std::unique_ptr<AssetEditor> OpenTableEditor(EditorHost& host, const std::string& path) {
  const ModInfo* mod = host.model().mods().Find(content::ModOf(path));
  std::optional<content::TableAsset> asset = mod ? content::ReadTableAsset(content::AssetFile(*mod, path), nullptr) : std::nullopt;
  return asset ? std::make_unique<TableEditor>(host, path, std::move(*asset)) : nullptr;
}

std::unique_ptr<AssetEditor> OpenGameTableEditor(EditorHost& host, const std::string& path) { return std::make_unique<GameTableEditor>(host, path); }

std::unique_ptr<AssetEditor> OpenTextureEditor(EditorHost& host, const std::string& path) {
  const ModInfo* mod = host.model().mods().Find(content::ModOf(path));
  std::optional<content::TextureAsset> asset = mod ? content::ReadTextureAsset(content::AssetFile(*mod, path), nullptr) : std::nullopt;
  return asset ? std::make_unique<TextureEditor>(host, path, std::move(*asset)) : nullptr;
}

std::unique_ptr<AssetEditor> OpenMaterialEditor(EditorHost& host, const std::string& path) {
  const ModInfo* mod = host.model().mods().Find(content::ModOf(path));
  std::optional<content::MaterialAsset> asset = mod ? content::ReadMaterialAsset(content::AssetFile(*mod, path), nullptr) : std::nullopt;
  if (!asset) return nullptr;
  // A graph material opens in the graph editor.
  if (const std::shared_ptr<const content::MaterialType> type = content::FindMaterialType(asset->type); type && type->HasGraph()) return OpenMaterialGraphEditor(host, path, std::move(*asset));
  return std::make_unique<MaterialEditor>(host, path, std::move(*asset));
}

std::unique_ptr<AssetEditor> OpenMapEditEditor(EditorHost& host, const std::string& path) {
  const ModInfo* mod = host.model().mods().Find(content::ModOf(path));
  std::optional<content::MapEditAsset> asset = mod ? content::ReadMapEditAsset(content::AssetFile(*mod, path), nullptr) : std::nullopt;
  return asset ? std::make_unique<MapEditEditor>(host, path, std::move(*asset)) : nullptr;
}

std::unique_ptr<AssetEditor> OpenLanguageEditor(EditorHost& host, const std::string& path) {
  const ModInfo* mod = host.model().mods().Find(content::ModOf(path));
  std::optional<content::LanguageAsset> asset = mod ? content::ReadLanguageAsset(content::AssetFile(*mod, path), nullptr) : std::nullopt;
  return asset ? std::make_unique<LanguageEditor>(host, path, std::move(*asset)) : nullptr;
}

std::unique_ptr<AssetEditor> OpenTranslationEditor(EditorHost& host, const std::string& mod) {
  const ModInfo* found = host.model().mods().Find(mod);
  return found ? std::make_unique<TranslationEditor>(host, *found) : nullptr;
}

std::unique_ptr<AssetEditor> OpenMetadataEditor(EditorHost& host, const std::string& mod) {
  const ModInfo* found = host.model().mods().Find(mod);
  return found ? std::make_unique<MetadataEditor>(host, *found) : nullptr;
}

}  // namespace rexfabric::ui
