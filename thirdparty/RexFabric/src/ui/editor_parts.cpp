#include "editor_parts.h"

#include <algorithm>
#include <cctype>

#include "../content/classes.h"
#include "../content/types.h"
#include "../content/values.h"
#include "../mods.h"
#include "glass.h"
#include "value_widgets.h"

namespace rexfabric::ui {
namespace {

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

bool Same(std::string_view a, std::string_view b) { return Lower(a) == Lower(b); }

void AddDependency(EditorHost& host, const std::string& folder, const std::string& on) {
  const ModInfo* mod = host.model().mods().Find(folder);
  const ModInfo* dependency = host.model().mods().Find(on);
  if (!mod || !dependency) return;
  ModInfo changed = *mod;
  changed.dependencies.push_back({dependency->folder, dependency->version});
  std::string error;
  if (!WriteMetadata(changed, &error)) return;
  NoteModsChanged();
  host.Saved(folder);
}

}  // namespace

void LabelRow(const char* label) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::TableNextColumn();
  ImGui::SetNextItemWidth(-FLT_MIN);
}

bool BeginLabelTable(const char* id) {
  if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
  ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * kNameColumn);
  ImGui::TableSetupColumn("##field", ImGuiTableColumnFlags_WidthStretch);
  return true;
}

bool DrawSetValues(const char* id, const std::vector<content::Property>& properties, nlohmann::json& values, EditorHost& host, const TypeChoices& choices) {
  if (properties.empty() || !ImGui::BeginTable(id, 3, ImGuiTableFlags_SizingFixedFit)) return false;
  ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * kNameColumn);
  ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed);
  bool changed = false;
  for (const content::Property& property : properties) {
    ImGui::PushID(property.name.c_str());
    std::string key;
    for (const auto& [name, held] : values.items()) {
      if (Same(name, property.name)) key = name;
    }
    nlohmann::json value = key.empty() ? property.value : values[key];
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    if (key.empty()) ImGui::TextUnformatted(property.name.c_str());
    else ImGui::TextColored(kOverriddenColor, "%s", property.name.c_str());
    ImGui::SetItemTooltip("%s%s%s, %s", property.help.c_str(), property.help.empty() ? "" : "\n", content::Describe(property.type).c_str(), key.empty() ? "the default" : "set here");
    ImGui::TableNextColumn();
    if (ValueField("##value", property.type, value, host.Types(), choices)) {
      values[key.empty() ? property.name : key] = value;
      changed = true;
    }
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(key.empty());
    if (glass::SmallButton("Reset")) {
      values.erase(key);
      changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Back to the default");
    ImGui::PopID();
  }
  ImGui::EndTable();
  return changed;
}

bool MayUse(const ModInfo& mod, std::string_view path) {
  if (path.find('/') == std::string_view::npos) return true;  // Object, a game type or enum
  if (std::optional<content::Class> known = content::FindClass(path); known && known->mod.empty()) return true;
  if (std::optional<content::Struct> known = content::FindStruct(path); known && known->mod.empty()) return true;
  const std::string_view owner = content::ModOf(path);
  if (Same(owner, mod.folder)) return true;
  return std::any_of(mod.dependencies.begin(), mod.dependencies.end(), [owner](const ModDependency& dependency) { return Same(dependency.mod, owner); });
}

std::optional<EditorProblem> DependencyProblem(EditorHost& host, const ModInfo& mod, std::string_view path, const std::string& what) {
  if (path.empty() || MayUse(mod, path)) return std::nullopt;
  const std::string owner(content::ModOf(path));
  if (!host.model().mods().Find(owner)) return EditorProblem{what + " names " + std::string(path) + ", in " + owner + ", which isn't installed"};
  const std::string folder = mod.folder;
  return EditorProblem{what + " names " + std::string(path) + ", in " + owner + ", which " + folder + " doesn't depend on", "Add " + owner + " as a dependency", [&host, folder, owner] { AddDependency(host, folder, owner); }};
}

}  // namespace rexfabric::ui
