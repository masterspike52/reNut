#include "value_widgets.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>

#include <imgui.h>

#include "../content/assets.h"
#include "../content/tables.h"
#include "widgets.h"

namespace rexfabric::ui {
namespace {

using content::Container;
using content::PropertyType;
using content::ValueType;

// How deep values open inside values before the editor stops showing them.
constexpr int kMaxDepth = 8;
// A row's label column (an element's number), and a map's key column against its value's, in font sizes.
constexpr float kIndexWidth = 2.2f;
constexpr float kKeyShare = 0.4f;
// A new string key, then numbered: "Key", "Key2".
constexpr const char* kNewKey = "Key";
// The types offered as they are; enums, structs and assets of a class are picked by name below them.
constexpr std::string_view kTextureClass = "Texture";
constexpr std::string_view kMaterialClass = "Material";
constexpr std::string_view kMapEditClass = "MapEdit";
constexpr std::string_view kMeshClass = "Mesh";
constexpr std::string_view kWorldClass = "World";
constexpr std::string_view kPacketClass = "Packet";
constexpr PropertyType kPlainTypes[] = {PropertyType::kBool, PropertyType::kInt, PropertyType::kFloat, PropertyType::kString, PropertyType::kText, PropertyType::kAsset};
constexpr PropertyType kPlainKeys[] = {PropertyType::kBool, PropertyType::kInt, PropertyType::kString, PropertyType::kAsset};

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

bool Matches(const std::string& text, const std::string& filter) { return filter.empty() || Lower(text).find(Lower(filter)) != std::string::npos; }

// What an asset value of `type` picks from: the textures, materials, map edits, meshes, worlds or packets for an asset of
// those built-in classes, whose assets are files of their own kind, and the classes for an asset of any other class.
const std::vector<std::string>& OfferedAssets(const ValueType& type, const TypeChoices& choices) {
  if (type.of == kTextureClass) return choices.textures;
  if (type.of == kMaterialClass) return choices.materials;
  if (type.of == kMapEditClass) return choices.map_edits;
  if (type.of == kMeshClass) return choices.meshes;
  if (type.of == kWorldClass) return choices.worlds;
  if (type.of == kPacketClass) return choices.packets;
  return choices.classes;
}

// The search text of the list open now; one list is open at a time.
std::string& Filter() {
  static std::string filter;
  return filter;
}

const char* TypeLabel(PropertyType type) {
  switch (type) {
    case PropertyType::kBool: return "Bool";
    case PropertyType::kInt: return "Int";
    case PropertyType::kFloat: return "Float";
    case PropertyType::kString: return "String";
    case PropertyType::kText: return "Text";
    case PropertyType::kAsset: return "Asset";
    case PropertyType::kEnum: return "Enum";
    case PropertyType::kStruct: return "Struct";
  }
  return "Int";
}

// A colour written as "#RRGGBB": its channels from 0 to 1, or none for a string that isn't one.
std::optional<std::array<float, 3>> HexColour(const std::string& text) {
  if (text.size() != 7 || text[0] != '#') return std::nullopt;
  std::array<float, 3> colour{};
  for (int i = 0; i < 3; ++i) {
    unsigned value = 0;
    for (int k = 0; k < 2; ++k) {
      const char c = text[1 + i * 2 + k];
      const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (digit < 0) return std::nullopt;
      value = value * 16 + static_cast<unsigned>(digit);
    }
    colour[i] = static_cast<float>(value) / 255.0f;
  }
  return colour;
}

std::string HexText(const std::array<float, 3>& colour) {
  char text[8];
  std::snprintf(text, sizeof(text), "#%02X%02X%02X", static_cast<unsigned>(std::lround(std::clamp(colour[0], 0.0f, 1.0f) * 255.0f)), static_cast<unsigned>(std::lround(std::clamp(colour[1], 0.0f, 1.0f) * 255.0f)), static_cast<unsigned>(std::lround(std::clamp(colour[2], 0.0f, 1.0f) * 255.0f)));
  return text;
}

// A list entry that, picked, makes `type` this single type.
bool PickSingle(const char* label, PropertyType kind, const std::string& of, PropertyType& type, std::string& type_of) {
  if (!ImGui::Selectable(label, type == kind && type_of == of)) return false;
  const bool changed = type != kind || type_of != of;
  type = kind;
  type_of = of;
  return changed;
}

// The map key's picker, inside the type's list.
bool KeyCombo(ValueType& type, const TypeChoices& choices) {
  bool changed = false;
  const std::string preview = content::Describe(content::KeyType(type));
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (!ImGui::BeginCombo("##key", preview.c_str(), ImGuiComboFlags_HeightLarge)) return false;
  for (const PropertyType kind : kPlainKeys) changed |= PickSingle(TypeLabel(kind), kind, {}, type.key, type.key_of);
  if (!choices.enums.empty()) ImGui::SeparatorText("Enums");
  for (const std::string& path : choices.enums) changed |= PickSingle(path.c_str(), PropertyType::kEnum, path, type.key, type.key_of);
  ImGui::EndCombo();
  return changed;
}

// A key no entry of the map has yet, or null when every one is taken.
nlohmann::json NewKey(const ValueType& key, const nlohmann::json& entries, const content::TypeLookup& types) {
  const auto taken = [&entries](const nlohmann::json& candidate) { return std::any_of(entries.begin(), entries.end(), [&](const nlohmann::json& pair) { return pair.is_array() && !pair.empty() && pair[0] == candidate; }); };
  switch (key.type) {
    case PropertyType::kBool:
      for (const bool candidate : {false, true}) {
        if (!taken(candidate)) return candidate;
      }
      return nullptr;
    case PropertyType::kInt: {
      int64_t next = 0;
      for (const nlohmann::json& pair : entries) {
        if (pair.is_array() && !pair.empty() && pair[0].is_number_integer()) next = std::max(next, pair[0].get<int64_t>() + 1);
      }
      return next;
    }
    case PropertyType::kEnum: {
      const std::optional<content::Enum> found = types.find_enum ? types.find_enum(key.of) : std::nullopt;
      if (!found) return nullptr;
      for (const content::EnumValue& value : found->values) {
        if (!taken(value.name)) return value.name;
      }
      return nullptr;
    }
    case PropertyType::kAsset: return nullptr;  // none; a second none is a problem the editor reports
    default: {
      std::string name = kNewKey;
      for (int number = 2; taken(name); ++number) name = kNewKey + std::to_string(number);
      return name;
    }
  }
}

bool Value(const char* id, const ValueType& type, nlohmann::json& value, const content::TypeLookup& types, const TypeChoices& choices, int depth);

// A value that isn't the shape its type needs, with a way to start it again.
bool Misshapen(const ValueType& type, nlohmann::json& value, const content::TypeLookup& types) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(widgets::kErrorColor, "Not a %s", content::Describe(type).c_str());
  ImGui::SameLine();
  if (!ImGui::SmallButton("Reset")) return false;
  value = content::EmptyValue(types, type);
  return true;
}

bool StructValue(const ValueType& type, nlohmann::json& value, const content::TypeLookup& types, const TypeChoices& choices, int depth) {
  const std::optional<content::Struct> found = types.find_struct ? types.find_struct(type.of) : std::nullopt;
  if (!found) {
    ImGui::TextColored(widgets::kErrorColor, "There's no struct %s", type.of.c_str());
    return false;
  }
  if (!value.is_object()) return Misshapen(type, value, types);
  if (!ImGui::TreeNodeEx("##struct", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", type.of.c_str())) return false;
  bool changed = false;
  if (ImGui::BeginTable("##fields", 2, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    for (const content::Property& field : found->fields) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(field.name.c_str());
      if (!field.help.empty()) ImGui::SetItemTooltip("%s", field.help.c_str());
      ImGui::TableNextColumn();
      // A field the value lacks shows its default, and joins the value once it's changed.
      const auto held = value.find(field.name);
      nlohmann::json shown = held != value.end() ? *held : field.value;
      if (Value(field.name.c_str(), field.type, shown, types, choices, depth + 1)) {
        value[field.name] = std::move(shown);
        changed = true;
      }
    }
    ImGui::EndTable();
  }
  ImGui::TreePop();
  return changed;
}

bool Single(const char* id, const ValueType& type, nlohmann::json& value, const content::TypeLookup& types, const TypeChoices& choices, int depth) {
  ImGui::PushID(id);
  bool changed = false;
  switch (type.type) {
    case PropertyType::kBool: {
      bool on = value.is_boolean() && value.get<bool>();
      if ((changed = ImGui::Checkbox("##value", &on))) value = on;
      break;
    }
    case PropertyType::kInt: {
      int64_t number = value.is_number_integer() ? value.get<int64_t>() : 0;
      ImGui::SetNextItemWidth(-FLT_MIN);
      if ((changed = ImGui::InputScalar("##value", ImGuiDataType_S64, &number))) value = number;
      break;
    }
    case PropertyType::kFloat: {
      double number = value.is_number() ? value.get<double>() : 0.0;
      ImGui::SetNextItemWidth(-FLT_MIN);
      if ((changed = ImGui::InputDouble("##value", &number, 0.0, 0.0, "%.6g"))) value = number;
      break;
    }
    case PropertyType::kString:
    case PropertyType::kText: {
      std::string text = value.is_string() ? value.get<std::string>() : std::string();
      // A string from a table: typed, or picked from the table's rows (a game's fill once its data is there).
      const bool from_table = type.type == PropertyType::kString && !type.of.empty();
      // A string holding a colour ("#RRGGBB", as a game's colour properties take one) gets a swatch to pick it with.
      std::optional<std::array<float, 3>> colour = type.type == PropertyType::kString ? HexColour(text) : std::nullopt;
      const float button = ImGui::GetFrameHeight();
      const float buttons = (from_table ? button + ImGui::GetStyle().ItemInnerSpacing.x : 0.0f) + (colour ? button + ImGui::GetStyle().ItemInnerSpacing.x : 0.0f);
      if (colour) {
        if (ImGui::ColorEdit3("##colour", colour->data(), ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
          text = HexText(*colour);
          changed = true;
        }
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
      }
      ImGui::SetNextItemWidth(buttons > 0.0f ? std::max(ImGui::GetContentRegionAvail().x - (from_table ? button + ImGui::GetStyle().ItemInnerSpacing.x : 0.0f), button) : -FLT_MIN);
      changed |= widgets::InputString("##value", text);
      if (from_table) {
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        const std::vector<std::string> rows = content::RowNames(type.of);
        changed |= PathCombo("##pick", "", text, rows, nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft);
        ImGui::SetItemTooltip("%s", rows.empty() ? ("Nothing in " + type.of + " yet: a game's table fills once its data is loaded").c_str() : ("Pick from " + type.of).c_str());
      }
      if (changed) value = text;
      break;
    }
    case PropertyType::kAsset: {
      std::string path = value.is_string() ? value.get<std::string>() : std::string();
      const float button = ImGui::GetFrameHeight();
      ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - button - ImGui::GetStyle().ItemInnerSpacing.x, button));
      changed = widgets::InputString("##value", path, 0, "None");
      ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
      const std::vector<std::string>& offered = OfferedAssets(type, choices);
      changed |= PathCombo("##pick", "", path, offered, "None", ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft);
      ImGui::SetItemTooltip("Pick an asset");
      if (changed) value = path.empty() ? nlohmann::json(nullptr) : nlohmann::json(path);
      break;
    }
    case PropertyType::kEnum: {
      const std::optional<content::Enum> found = types.find_enum ? types.find_enum(type.of) : std::nullopt;
      const std::string current = value.is_string() ? value.get<std::string>() : std::string();
      const auto known = found ? std::find_if(found->values.begin(), found->values.end(), [&current](const content::EnumValue& entry) { return entry.name == current; }) : std::vector<content::EnumValue>::const_iterator{};
      const bool is_value = found && known != found->values.end();
      const std::string preview = is_value ? (known->display.empty() ? known->name : known->display) : current;
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (!is_value) ImGui::PushStyleColor(ImGuiCol_Text, widgets::kErrorColor);
      const bool open = ImGui::BeginCombo("##value", preview.c_str());
      if (!is_value) ImGui::PopStyleColor();
      if (!is_value && found) ImGui::SetItemTooltip("%s isn't one of its values", current.c_str());
      if (!found) ImGui::SetItemTooltip("There's no enum %s", type.of.c_str());
      if (open) {
        for (const content::EnumValue& entry : found ? found->values : std::vector<content::EnumValue>{}) {
          const std::string label = (entry.display.empty() ? entry.name : entry.display) + "##" + entry.name;
          if (ImGui::Selectable(label.c_str(), entry.name == current) && entry.name != current) {
            value = entry.name;
            changed = true;
          }
          if (!entry.help.empty()) ImGui::SetItemTooltip("%s", entry.help.c_str());
        }
        ImGui::EndCombo();
      }
      break;
    }
    case PropertyType::kStruct:
      if (depth >= kMaxDepth) ImGui::TextDisabled("(too deep to show)");
      else changed = StructValue(type, value, types, choices, depth);
      break;
  }
  ImGui::PopID();
  return changed;
}

bool ArrayValue(const ValueType& type, nlohmann::json& value, const content::TypeLookup& types, const TypeChoices& choices, int depth) {
  if (!value.is_array()) return Misshapen(type, value, types);
  bool changed = false;
  const bool open = ImGui::TreeNodeEx("##array", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap, "%zu element%s", value.size(), value.size() == 1 ? "" : "s");
  ImGui::SameLine();
  if (ImGui::SmallButton("Add")) {
    value.push_back(content::EmptyValue(types, content::ElementType(type)));
    changed = true;
  }
  if (!open) return changed;
  // A move or a removal waits for the rows to be drawn.
  std::optional<size_t> remove;
  std::optional<std::pair<size_t, size_t>> swap;
  if (ImGui::BeginTable("##elements", 3, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("##index", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * kIndexWidth);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##buttons", ImGuiTableColumnFlags_WidthFixed);
    for (size_t i = 0; i < value.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::Text("%zu", i + 1);
      ImGui::TableNextColumn();
      changed |= Single("##element", content::ElementType(type), value[i], types, choices, depth + 1);
      ImGui::TableNextColumn();
      ImGui::BeginDisabled(i == 0);
      if (ImGui::ArrowButton("##up", ImGuiDir_Up)) swap = std::pair(i, i - 1);
      ImGui::EndDisabled();
      ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
      ImGui::BeginDisabled(i + 1 == value.size());
      if (ImGui::ArrowButton("##down", ImGuiDir_Down)) swap = std::pair(i, i + 1);
      ImGui::EndDisabled();
      ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
      if (ImGui::SmallButton("x")) remove = i;
      ImGui::SetItemTooltip("Remove it");
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::TreePop();
  if (swap) {
    std::swap(value[swap->first], value[swap->second]);
    changed = true;
  }
  if (remove) {
    value.erase(*remove);
    changed = true;
  }
  return changed;
}

bool MapValue(const ValueType& type, nlohmann::json& value, const content::TypeLookup& types, const TypeChoices& choices, int depth) {
  if (!value.is_array()) return Misshapen(type, value, types);
  bool changed = false;
  const bool open = ImGui::TreeNodeEx("##map", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap, "%zu entr%s", value.size(), value.size() == 1 ? "y" : "ies");
  ImGui::SameLine();
  const nlohmann::json key = NewKey(content::KeyType(type), value, types);
  const bool full = key.is_null() && type.key != PropertyType::kAsset;
  ImGui::BeginDisabled(full);
  if (ImGui::SmallButton("Add")) {
    value.push_back(nlohmann::json::array({key, content::EmptyValue(types, content::ElementType(type))}));
    changed = true;
  }
  ImGui::EndDisabled();
  if (full) ImGui::SetItemTooltip("Every key is taken");
  if (!open) return changed;
  std::optional<size_t> remove;
  if (ImGui::BeginTable("##entries", 3, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("##key", ImGuiTableColumnFlags_WidthStretch, kKeyShare);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 1.0f - kKeyShare);
    ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed);
    for (size_t i = 0; i < value.size(); ++i) {
      nlohmann::json& pair = value[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      if (!pair.is_array() || pair.size() != 2) {
        ImGui::TableNextColumn();
        ImGui::TextColored(widgets::kErrorColor, "Not a [key, value] pair");
        ImGui::TableNextColumn();
      } else {
        ImGui::TableNextColumn();
        changed |= Single("##key", content::KeyType(type), pair[0], types, choices, depth + 1);
        ImGui::TableNextColumn();
        changed |= Single("##value", content::ElementType(type), pair[1], types, choices, depth + 1);
      }
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("x")) remove = i;
      ImGui::SetItemTooltip("Remove it");
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::TreePop();
  if (remove) {
    value.erase(*remove);
    changed = true;
  }
  return changed;
}

bool Value(const char* id, const ValueType& type, nlohmann::json& value, const content::TypeLookup& types, const TypeChoices& choices, int depth) {
  if (type.container == Container::kSingle) return Single(id, type, value, types, choices, depth);
  ImGui::PushID(id);
  const bool changed = type.container == Container::kArray ? ArrayValue(type, value, types, choices, depth) : MapValue(type, value, types, choices, depth);
  ImGui::PopID();
  return changed;
}

}  // namespace

bool PathCombo(const char* id, const char* preview, std::string& picked, const std::vector<std::string>& paths, const char* none, int flags) {
  bool changed = false;
  if (!ImGui::BeginCombo(id, preview, flags | ImGuiComboFlags_HeightLarge)) return false;
  if (ImGui::IsWindowAppearing()) {
    Filter().clear();
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::SetNextItemWidth(-FLT_MIN);
  widgets::InputString("##filter", Filter(), 0, "Search");
  if (none && Filter().empty() && ImGui::Selectable(none, picked.empty())) {
    changed = !picked.empty();
    picked.clear();
  }
  // Only the rows in view are drawn: a game's tables run to thousands (Trouble in Paradise's textures).
  std::vector<const std::string*> shown;
  for (const std::string& path : paths) {
    if (Matches(path, Filter())) shown.push_back(&path);
  }
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(shown.size()));
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      const std::string& path = *shown[static_cast<size_t>(i)];
      if (ImGui::Selectable(path.c_str(), Lower(path) == Lower(picked))) {
        changed = path != picked;
        picked = path;
      }
    }
  }
  ImGui::EndCombo();
  return changed;
}

bool TypeField(const char* id, ValueType& type, const TypeChoices& choices) {
  ImGui::PushID(id);
  bool changed = false;
  // Shown by the last part of the paths it names, whole in its tooltip.
  ValueType brief = type;
  brief.of = content::NameOf(type.of);
  brief.key_of = content::NameOf(type.key_of);
  const std::string preview = content::Describe(brief);
  ImGui::SetNextItemWidth(-FLT_MIN);
  const bool open = ImGui::BeginCombo("##type", preview.c_str(), ImGuiComboFlags_HeightLarge);
  if (!open) ImGui::SetItemTooltip("%s", content::Describe(type).c_str());
  if (open) {
    if (ImGui::IsWindowAppearing()) Filter().clear();
    // What it holds: one value, an array of them, or a map of them under keys.
    int holds = static_cast<int>(type.container);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Holds");
    ImGui::SameLine();
    bool picked = ImGui::RadioButton("One", &holds, static_cast<int>(Container::kSingle));
    ImGui::SameLine();
    picked |= ImGui::RadioButton("Array", &holds, static_cast<int>(Container::kArray));
    ImGui::SameLine();
    picked |= ImGui::RadioButton("Map", &holds, static_cast<int>(Container::kMap));
    if (picked && holds != static_cast<int>(type.container)) {
      type.container = static_cast<Container>(holds);
      changed = true;
    }
    if (type.container == Container::kMap) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("Key");
      ImGui::SameLine();
      changed |= KeyCombo(type, choices);
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(-FLT_MIN);
    widgets::InputString("##filter", Filter(), 0, "Search");
    for (const PropertyType kind : kPlainTypes) {
      if (Matches(TypeLabel(kind), Filter())) changed |= PickSingle(TypeLabel(kind), kind, {}, type.type, type.of);
    }
    const auto section = [&](const char* title, const std::vector<std::string>& paths, PropertyType kind, const char* prefix) {
      bool shown = false;
      for (const std::string& path : paths) {
        if (!Matches(path, Filter())) continue;
        if (!shown) ImGui::SeparatorText(title);
        shown = true;
        changed |= PickSingle((prefix + path).c_str(), kind, path, type.type, type.of);
      }
    };
    section("Enums", choices.enums, PropertyType::kEnum, "");
    section("Structs", choices.structs, PropertyType::kStruct, "");
    section("Assets of a class", choices.classes, PropertyType::kAsset, "Asset of ");
    ImGui::EndCombo();
  }
  ImGui::PopID();
  return changed;
}

bool ValueField(const char* id, const ValueType& type, nlohmann::json& value, const content::TypeLookup& types, const TypeChoices& choices) { return Value(id, type, value, types, choices, 0); }

namespace {

std::string SingleText(const ValueType& type, const nlohmann::json& value, const content::TypeLookup& types, int depth) {
  if (type.type == PropertyType::kEnum && value.is_string()) {
    const std::string name = value.get<std::string>();
    if (const std::optional<content::Enum> found = types.find_enum ? types.find_enum(type.of) : std::nullopt) {
      for (const content::EnumValue& entry : found->values) {
        if (entry.name == name) return entry.display.empty() ? entry.name : entry.display;
      }
    }
    return name;
  }
  if (type.type == PropertyType::kStruct && value.is_object()) {
    if (depth >= kMaxDepth) return "{...}";
    const std::optional<content::Struct> found = types.find_struct ? types.find_struct(type.of) : std::nullopt;
    if (!found) return value.dump();
    std::string text = "{";
    for (size_t i = 0; i < found->fields.size(); ++i) {
      const auto held = value.find(found->fields[i].name);
      text += (i ? ", " : "") + found->fields[i].name + ": " + ValueText(found->fields[i].type, held != value.end() ? *held : found->fields[i].value, types);
    }
    return text + "}";
  }
  if (value.is_string()) return value.get<std::string>();
  if (value.is_null()) return "None";
  if (value.is_number_float()) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value.get<double>());
    return buffer;
  }
  return value.dump();
}

}  // namespace

std::string ValueText(const ValueType& type, const nlohmann::json& value, const content::TypeLookup& types) {
  if (type.container == Container::kSingle) return SingleText(type, value, types, 0);
  if (!value.is_array()) return value.dump();
  std::string text = type.container == Container::kArray ? "[" : "{";
  for (size_t i = 0; i < value.size(); ++i) {
    text += i ? ", " : "";
    if (type.container == Container::kArray) {
      text += SingleText(content::ElementType(type), value[i], types, 1);
    } else if (value[i].is_array() && value[i].size() == 2) {
      text += SingleText(content::KeyType(type), value[i][0], types, 1) + ": " + SingleText(content::ElementType(type), value[i][1], types, 1);
    }
  }
  return text + (type.container == Container::kArray ? "]" : "}");
}

}  // namespace rexfabric::ui
