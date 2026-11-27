#include "content_browser.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <utility>

#include <imgui.h>

#include <rex/hid/input_system.h>
#include <rexfabric/rexfabric.h>

#include "../content/game_texts.h"
#include "../content/localisation.h"
#include "../content/map_edits.h"
#include "../content/materials.h"
#include "../content/packets.h"
#include "../content/tables.h"
#include "../content/types.h"
#include "file_picker.h"
#include "glass.h"
#include "widgets.h"
#include "world_mesh_editors.h"

namespace rexfabric::ui {
namespace {

using content::AssetInfo;
using content::AssetKind;

// In multiples of the font size, so the layout follows the UI's scale.
constexpr float kTreeWidth = 14.0f;
constexpr float kTileWidth = 8.5f;
constexpr float kSearchWidth = 11.0f;
constexpr float kFilterWidth = 12.0f;
constexpr float kAskWidth = 26.0f;
// How many of the references a delete lists before saying how many more.
constexpr size_t kListedReferences = 8;

constexpr const char* kAssetPayload = "REXFABRIC_ASSET";
constexpr const char* kAskPopup = "###ask";
constexpr const char* kClosePopup = "Unsaved changes###close";
constexpr const char* kMetadataLabel = "Metadata";
constexpr const char* kNewModVersion = "1.0.0";

// The glass of the panes, and the colors of the tiles by what they are.
constexpr ImVec4 kPaneTint{0.05f, 0.06f, 0.11f, 0.42f};
constexpr ImVec4 kModColor{0.45f, 0.62f, 1.0f, 1.0f};
constexpr ImVec4 kMetadataColor{1.0f, 0.38f, 0.36f, 1.0f};
constexpr ImVec4 kTranslationsColor{0.4f, 0.8f, 1.0f, 1.0f};
constexpr ImVec4 kFolderColor{0.95f, 0.75f, 0.35f, 1.0f};
constexpr ImVec4 kTypedClassColor{0.4f, 0.65f, 1.0f, 1.0f};
constexpr ImVec4 kDataClassColor{0.35f, 0.85f, 0.75f, 1.0f};
constexpr ImVec4 kEnumColor{0.6f, 0.88f, 0.38f, 1.0f};
constexpr ImVec4 kStructColor{0.78f, 0.56f, 1.0f, 1.0f};
constexpr ImVec4 kTableColor{1.0f, 0.6f, 0.32f, 1.0f};
constexpr ImVec4 kGameTableColor{0.95f, 0.82f, 0.42f, 1.0f};
constexpr ImVec4 kLanguageColor{1.0f, 0.5f, 0.75f, 1.0f};
constexpr ImVec4 kTextureColor{0.45f, 0.85f, 0.95f, 1.0f};
constexpr ImVec4 kOtherColor{0.6f, 0.6f, 0.65f, 1.0f};
constexpr ImVec4 kMapEditColor{0.55f, 0.85f, 0.45f, 1.0f};
constexpr ImVec4 kMaterialColor{0.85f, 0.55f, 0.95f, 1.0f};
constexpr ImVec4 kMeshColor{0.95f, 0.7f, 0.45f, 1.0f};
constexpr ImVec4 kWorldColor{0.35f, 0.75f, 0.95f, 1.0f};
constexpr ImVec4 kPacketColor{0.9f, 0.72f, 0.42f, 1.0f};
constexpr ImVec4 kSproutColor{0.45f, 0.8f, 0.4f, 1.0f};
constexpr ImVec4 kLandColor{0.45f, 0.8f, 0.4f, 1.0f};
constexpr ImVec4 kPinColor{0.95f, 0.35f, 0.3f, 1.0f};

rex::hid::IInputSystem* GuestInput() { return REX_INTERFACE(rex::hid::IInputSystem); }

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

bool Same(std::string_view a, std::string_view b) { return Lower(a) == Lower(b); }

bool Within(std::string_view path, std::string_view base) {
  const std::string lower = Lower(path);
  const std::string prefix = Lower(base);
  return lower == prefix || (lower.size() > prefix.size() && lower.starts_with(prefix) && lower[prefix.size()] == '/');
}

ImU32 Shade(const ImVec4& color, float light, float alpha = 1.0f) { return ImGui::GetColorU32(ImVec4(color.x * light, color.y * light, color.z * light, alpha)); }

// The text cut to fit `width`, ending in dots when it was cut.
std::string Fit(const std::string& text, float width) {
  if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
  std::string cut = text;
  while (!cut.empty() && ImGui::CalcTextSize((cut + "...").c_str()).x > width) cut.pop_back();
  return cut + "...";
}

// A tile's name on up to two lines, broken after an underscore or a space where there's one to break at, the second
// cut to fit.
std::pair<std::string, std::string> SplitLabel(const std::string& text, float width) {
  if (ImGui::CalcTextSize(text.c_str()).x <= width) return {text, {}};
  size_t fit = 0;
  while (fit < text.size() && ImGui::CalcTextSize(text.substr(0, fit + 1).c_str()).x <= width) ++fit;
  if (fit == 0) return {Fit(text, width), {}};
  const size_t gap = text.find_last_of("_ ", fit - 1);
  const size_t cut = gap == std::string::npos || gap == 0 ? fit : gap + 1;
  return {text.substr(0, cut), Fit(text.substr(cut), width)};
}

// The folder the window was last showing, so it opens again there.
std::string& LastFolder() {
  static std::string folder;
  return folder;
}

}  // namespace

ContentBrowserWindow::ContentBrowserWindow(MenuModel& model, std::function<void()> closed) : model_(model), closed_(std::move(closed)) {
  if (auto* input = GuestInput()) input->AcquireUIBlock();
  // A mod's enum or struct as its file was last read; the game's from the registry.
  types_.find_enum = [this](std::string_view path) -> std::optional<content::Enum> {
    if (const auto found = enums_.find(Lower(path)); found != enums_.end()) return found->second;
    std::optional<content::Enum> known = content::FindEnum(path);
    return known && known->mod.empty() ? known : std::nullopt;
  };
  types_.find_struct = [this](std::string_view path) -> std::optional<content::Struct> {
    if (const auto found = structs_.find(Lower(path)); found != structs_.end()) return found->second;
    std::optional<content::Struct> known = content::FindStruct(path);
    return known && known->mod.empty() ? known : std::nullopt;
  };
  Refresh();
  Navigate(LastFolder());
}

ContentBrowserWindow::~ContentBrowserWindow() {
  if (auto* input = GuestInput()) input->ReleaseUIBlock();
  if (closed_) closed_();
}

void ContentBrowserWindow::Hide() {
  const bool unsaved = std::any_of(editors_.begin(), editors_.end(), [](const auto& editor) { return editor->dirty(); });
  if (unsaved) open_close_prompt_ = true;
  else closing_ = true;
}

bool ContentBrowserWindow::Draw(ImGuiIO& io) {
  if (closing_) return false;
  OnDraw(io);
  return !closing_;
}

TypeChoices ContentBrowserWindow::Choices(std::string_view mod) {
  TypeChoices choices;
  // RexFabric's and the game's first.
  for (const std::string& path : content::ClassPaths()) {
    if (std::optional<content::Class> known = content::FindClass(path); known && known->mod.empty()) choices.classes.push_back(path);
  }
  for (const std::string& path : content::EnumPaths({})) {
    if (path.find('/') == std::string::npos) choices.enums.push_back(path);
  }
  for (const std::string& path : content::StructPaths({})) {
    if (std::optional<content::Struct> known = content::FindStruct(path); known && known->mod.empty()) choices.structs.push_back(path);
  }
  choices.packets = content::GamePacketPaths();
  ForModAndDependencies(mod, [&choices](const content::ModContent& found) {
    for (const AssetInfo& asset : found.assets) {
      if (asset.kind == AssetKind::kClass) choices.classes.push_back(asset.path);
      if (asset.kind == AssetKind::kEnum) choices.enums.push_back(asset.path);
      if (asset.kind == AssetKind::kStruct) choices.structs.push_back(asset.path);
      if (asset.kind == AssetKind::kTexture) choices.textures.push_back(asset.path);
      if (asset.kind == AssetKind::kMaterial) choices.materials.push_back(asset.path);
      if (asset.kind == AssetKind::kMapEdit) choices.map_edits.push_back(asset.path);
      if (asset.kind == AssetKind::kMesh) choices.meshes.push_back(asset.path);
      if (asset.kind == AssetKind::kWorld) choices.worlds.push_back(asset.path);
      if (asset.kind == AssetKind::kPacket) choices.packets.push_back(asset.path);
    }
  });
  return choices;
}

void ContentBrowserWindow::ForModAndDependencies(std::string_view mod, const std::function<void(const content::ModContent&)>& visit) const {
  std::vector<std::string> mods = {std::string(mod)};
  if (const ModInfo* info = model_.mods().Find(mod)) {
    for (const ModDependency& dependency : info->dependencies) mods.push_back(dependency.mod);
  }
  for (const std::string& folder : mods) {
    if (const content::ModContent* found = ContentOf(folder)) visit(*found);
  }
}

ContentBrowserWindow::TileKind ContentBrowserWindow::KindOf(std::string_view path) const {
  const content::ModContent* found = ContentOf(content::ModOf(path));
  if (!found) return TileKind::kOther;
  const auto asset = std::find_if(found->assets.begin(), found->assets.end(), [&path](const AssetInfo& known) { return Same(known.path, path); });
  if (asset == found->assets.end()) return TileKind::kOther;
  switch (asset->kind) {
    case AssetKind::kClass: return TileKind::kClass;
    case AssetKind::kEnum: return TileKind::kEnum;
    case AssetKind::kStruct: return TileKind::kStruct;
    case AssetKind::kTable: return TileKind::kTable;
    case AssetKind::kLanguage: return TileKind::kLanguage;
    case AssetKind::kTexture: return TileKind::kTexture;
    case AssetKind::kMapEdit: return TileKind::kMapEdit;
    case AssetKind::kMaterial: return TileKind::kMaterial;
    case AssetKind::kMesh: return TileKind::kMesh;
    case AssetKind::kWorld: return TileKind::kWorld;
    case AssetKind::kPacket: return TileKind::kPacket;
    case AssetKind::kOther: return TileKind::kOther;
  }
  return TileKind::kOther;
}

void ContentBrowserWindow::Saved(const std::string&) {
  refresh_ = true;
  reload_ = true;
}

ImVec2 ContentBrowserWindow::EditorPosition(const ImVec2& size) {
  // Right of the browser, else left of it, else over it, level with its top as far as the screen lets it.
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float gap = ImGui::GetStyle().ItemSpacing.x * 2.0f;
  const float top = std::clamp(window_top_, 0.0f, std::max(0.0f, display.y - size.y));
  if (window_right_ + gap + size.x <= display.x) return ImVec2(window_right_ + gap, top);
  if (window_left_ - gap - size.x >= 0.0f) return ImVec2(window_left_ - gap - size.x, top);
  return ImVec2(std::max(0.0f, display.x - size.x), top);
}

void ContentBrowserWindow::OnDraw(ImGuiIO& io) {
  widgets::SyncModifiers(io);
  model_.Poll();
  TakePicked();
  // What a save or a change asked for, now that the mods were read again.
  if (reload_) {
    reload_ = false;
    model_.ReloadContent();
  }
  if (refresh_) {
    refresh_ = false;
    Refresh();
    for (const auto& editor : editors_) editor->ContentChanged();
    if (!navigate_after_refresh_.empty()) Navigate(std::exchange(navigate_after_refresh_, {}));
  }

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.6f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(1320.0f, 720.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(640.0f, 360.0f), ImVec2(FLT_MAX, FLT_MAX));
  const glass::Theme theme;
  bool open = true;
  if (glass::BeginWindow("Content Browser###rexfabric_content", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    window_left_ = ImGui::GetWindowPos().x;
    window_right_ = window_left_ + ImGui::GetWindowSize().x;
    window_top_ = ImGui::GetWindowPos().y;
    DrawToolbar();
    ImGui::Spacing();
    const float font = ImGui::GetFontSize();
    const float footer = ImGui::GetFrameHeightWithSpacing();
    glass::BeginChild("##tree", ImVec2(font * kTreeWidth, -footer), ImGuiChildFlags_ResizeX | ImGuiChildFlags_AlwaysUseWindowPadding, 0, kPaneTint);
    DrawTree();
    ImGui::EndChild();
    ImGui::SameLine();
    glass::BeginChild("##grid", ImVec2(0.0f, -footer), ImGuiChildFlags_AlwaysUseWindowPadding, 0, kPaneTint);
    DrawGrid();
    ImGui::EndChild();
    DrawFooter();
    DrawPopups();
    DrawClosePrompt();
  }
  ImGui::End();
  DrawEditors();
  if (!open) Hide();
}

void ContentBrowserWindow::Refresh() {
  content_.clear();
  looks_.clear();
  enums_.clear();
  structs_.clear();
  for (const ModInfo& mod : model_.mods().mods()) content_[Lower(mod.folder)] = content::ScanContent(mod);
  for (const auto& [mod, found] : content_) {
    for (const AssetInfo& asset : found.assets) {
      if (asset.kind == AssetKind::kEnum) {
        if (std::optional<content::EnumAsset> read = content::ReadEnumAsset(asset.file, nullptr)) enums_[Lower(asset.path)] = {.path = asset.path, .mod = found.mod, .help = read->help, .values = read->values};
      } else if (asset.kind == AssetKind::kStruct) {
        if (std::optional<content::StructAsset> read = content::ReadStructAsset(asset.file, nullptr)) structs_[Lower(asset.path)] = {.path = asset.path, .mod = found.mod, .help = read->help, .fields = read->fields};
      }
    }
  }
  for (const auto& [mod, found] : content_) {
    for (const AssetInfo& asset : found.assets) {
      if (asset.kind != AssetKind::kClass) continue;
      ClassLook look{.chain = content::ClassChain(model_.mods(), asset.path)};
      look.typed = std::any_of(look.chain.begin(), look.chain.end(), [](const std::string& at) { return !content::TypeOfClass(at).empty(); });
      looks_[Lower(asset.path)] = std::move(look);
    }
  }
  // What was shown may be gone: the nearest folder still there.
  std::string folder = folder_;
  while (!folder.empty()) {
    const content::ModContent* found = ContentOf(content::ModOf(folder));
    const bool exists = found && (folder.find('/') == std::string::npos || std::any_of(found->folders.begin(), found->folders.end(), [&folder](const std::string& known) { return Same(known, folder); }));
    if (exists) break;
    folder = folder.find('/') == std::string::npos ? std::string() : std::string(content::FolderOf(folder));
  }
  if (folder != folder_) Navigate(folder);
}

// Top ---------------------------------------------------------------------------------------------------

void ContentBrowserWindow::DrawToolbar() {
  if (glass::Button("Add", glass::kCreate)) ImGui::OpenPopup("##add");
  ImGui::SetItemTooltip("A new folder, class or mod");
  DrawAddMenu();
  ImGui::SameLine();
  DrawBreadcrumb();

  // Search, the class filter and Refresh, at the right.
  const float font = ImGui::GetFontSize();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float refresh = ImGui::CalcTextSize("Refresh").x + style.FramePadding.x * 2.0f;
  const float right = font * (kSearchWidth + kFilterWidth) + refresh + style.ItemSpacing.x * 2.0f;
  ImGui::SameLine();
  if (const float room = ImGui::GetContentRegionAvail().x; room > right) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - right);
  ImGui::SetNextItemWidth(font * kSearchWidth);
  widgets::InputString("##search", search_, 0, folder_.empty() ? "Search every mod" : "Search here");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(font * kFilterWidth);
  if (ImGui::BeginCombo("##class_filter", class_filter_.empty() ? "Any class" : class_filter_.c_str(), ImGuiComboFlags_HeightLarge)) {
    if (ImGui::IsWindowAppearing()) combo_filter_.clear();
    ImGui::SetNextItemWidth(-FLT_MIN);
    widgets::InputString("##filter", combo_filter_, 0, "Search");
    if (combo_filter_.empty() && ImGui::Selectable("Any class", class_filter_.empty())) class_filter_.clear();
    std::vector<std::string> classes;
    for (const std::string& path : content::ClassPaths()) {
      if (std::optional<content::Class> known = content::FindClass(path); known && known->mod.empty()) classes.push_back(path);
    }
    for (const auto& [mod, found] : content_) {
      for (const AssetInfo& asset : found.assets) {
        if (asset.kind == AssetKind::kClass) classes.push_back(asset.path);
      }
    }
    for (const std::string& path : classes) {
      if (!combo_filter_.empty() && Lower(path).find(Lower(combo_filter_)) == std::string::npos) continue;
      if (ImGui::Selectable(path.c_str(), Same(path, class_filter_))) class_filter_ = path;
    }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("Only the assets of a class, or of one below it");
  ImGui::SameLine();
  if (glass::Button("Refresh")) refresh_ = true;
  ImGui::SetItemTooltip("Reads the mods' content folders again, for changes made outside the game");
}

void ContentBrowserWindow::DrawAddMenu() {
  if (!ImGui::BeginPopup("##add")) return;
  const bool in_mod = !folder_.empty();
  if (ImGui::MenuItem("New Folder...", nullptr, false, in_mod)) AskFor(Ask::kNewFolder, folder_, "NewFolder");
  if (ImGui::MenuItem("New Class...", nullptr, false, in_mod)) AskFor(Ask::kNewClass, folder_, "NewClass");
  if (ImGui::MenuItem("New Enum...", nullptr, false, in_mod)) AskFor(Ask::kNewEnum, folder_, "NewEnum");
  if (ImGui::MenuItem("New Struct...", nullptr, false, in_mod)) AskFor(Ask::kNewStruct, folder_, "NewStruct");
  if (ImGui::MenuItem("New Data Table...", nullptr, false, in_mod)) AskFor(Ask::kNewTable, folder_, "NewTable");
  if (content::HasGameTexts() && ImGui::MenuItem("New Language...", nullptr, false, in_mod)) AskFor(Ask::kNewLanguage, folder_, "NewLanguage");
  if (!content::Worlds().empty() && ImGui::MenuItem("New Map Edit...", nullptr, false, in_mod)) AskFor(Ask::kNewMapEdit, folder_, "NewMapEdit");
  if (!content::MaterialTypePaths().empty() && ImGui::MenuItem("New Material...", nullptr, false, in_mod)) AskFor(Ask::kNewMaterial, folder_, "NewMaterial");
  if (!content::Worlds().empty() && ImGui::MenuItem("New World...", nullptr, false, in_mod)) AskFor(Ask::kNewWorld, folder_, "NewWorld");
  if (content::HasPackets() && ImGui::MenuItem("New Packet...", nullptr, false, in_mod)) AskFor(Ask::kNewPacket, folder_, "NewPacket");
  if (ImGui::MenuItem("Import Image...", nullptr, false, in_mod && !picking_)) StartImport(folder_);
  if (ImGui::MenuItem("Import Mesh...", nullptr, false, in_mod && !picking_)) StartImport(folder_, true);
  if (!in_mod) ImGui::TextDisabled("Open a mod to add content to it");
  ImGui::Separator();
  if (ImGui::MenuItem("New Mod...")) AskFor(Ask::kNewMod, {}, "MyMod");
  ImGui::EndPopup();
}

void ContentBrowserWindow::DrawBreadcrumb() {
  if (glass::Button("All Mods")) Navigate({});
  if (game_tables_) {
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(">");
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    glass::Button("Game Tables");
    return;
  }
  if (folder_.empty()) return;
  std::string path;
  size_t start = 0;
  while (start <= folder_.size()) {
    const size_t slash = folder_.find('/', start);
    const std::string part = folder_.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    path = path.empty() ? part : path + "/" + part;
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(">");
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    const ModInfo* mod = path.find('/') == std::string::npos ? model_.mods().Find(path) : nullptr;
    const std::string label = (mod ? mod->name : part) + "##crumb" + path;
    if (glass::Button(label.c_str())) Navigate(path);
    AcceptMove(path);
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
}

// Left --------------------------------------------------------------------------------------------------

void ContentBrowserWindow::DrawTree() {
  if (ImGui::Selectable("All Mods", folder_.empty() && !game_tables_)) Navigate({});
  if (!content::GameTablePaths().empty()) {
    if (ImGui::Selectable("Game Tables", game_tables_)) NavigateGameTables();
    ImGui::SetItemTooltip("The game's own tables, made from its data: read only");
  }
  for (const ModInfo& mod : model_.mods().mods()) {
    const content::ModContent* found = ContentOf(mod.folder);
    const bool has_folders = found && std::any_of(found->folders.begin(), found->folders.end(), [&mod](const std::string& folder) { return Same(content::FolderOf(folder), mod.folder); });
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (Same(folder_, mod.folder)) flags |= ImGuiTreeNodeFlags_Selected;
    if (!has_folders) flags |= ImGuiTreeNodeFlags_Leaf;
    if (reveal_ && Within(folder_, mod.folder)) ImGui::SetNextItemOpen(true);
    if (!mod.enabled) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool open = ImGui::TreeNodeEx(mod.folder.c_str(), flags, "%s", mod.name.c_str());
    if (!mod.enabled) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) Navigate(mod.folder);
    ImGui::SetItemTooltip("%s%s", mod.folder.c_str(), mod.enabled ? "" : ", off: its content is here to edit, but doesn't load");
    AcceptMove(mod.folder);
    if (open) {
      DrawTreeFolder(mod.folder);
      ImGui::TreePop();
    }
  }
  reveal_ = false;
}

void ContentBrowserWindow::DrawTreeFolder(const std::string& folder) {
  const content::ModContent* found = ContentOf(content::ModOf(folder));
  if (!found) return;
  for (const std::string& child : found->folders) {
    if (!Same(content::FolderOf(child), folder)) continue;
    const bool has_folders = std::any_of(found->folders.begin(), found->folders.end(), [&child](const std::string& below) { return Same(content::FolderOf(below), child); });
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (Same(folder_, child)) flags |= ImGuiTreeNodeFlags_Selected;
    if (!has_folders) flags |= ImGuiTreeNodeFlags_Leaf;
    if (reveal_ && Within(folder_, child) && !Same(folder_, child)) ImGui::SetNextItemOpen(true);
    const bool open = ImGui::TreeNodeEx(child.c_str(), flags, "%s", std::string(content::NameOf(child)).c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) Navigate(child);
    AcceptMove(child);
    if (open) {
      DrawTreeFolder(child);
      ImGui::TreePop();
    }
  }
}

// Right -------------------------------------------------------------------------------------------------

std::vector<ContentBrowserWindow::Tile> ContentBrowserWindow::Tiles() const {
  std::vector<Tile> tiles;
  const auto add_asset = [this, &tiles](const AssetInfo& asset, std::string sub) { tiles.push_back({KindOf(asset.path), asset.path, asset.name, std::move(sub)}); };
  // What a tile says it is below its name: a class's parent, or the kind of asset.
  const auto parent_of = [](const AssetInfo& asset) {
    switch (asset.kind) {
      case AssetKind::kClass: return std::string(content::NameOf(asset.parent));
      case AssetKind::kEnum: return std::string("Enum");
      case AssetKind::kStruct: return std::string("Struct");
      case AssetKind::kTable: return std::string("Data table");
      case AssetKind::kLanguage: return std::string("Language");
      case AssetKind::kTexture: return std::string("Texture");
      case AssetKind::kMapEdit: return std::string("Map edit");
      case AssetKind::kMaterial: return std::string("Material");
      case AssetKind::kMesh: return std::string("Mesh");
      case AssetKind::kWorld: return std::string("World");
      case AssetKind::kPacket: return std::string("Packet");
      case AssetKind::kOther: break;
    }
    return std::string("Unknown kind");
  };

  if (game_tables_) {
    for (const std::string& path : content::GameTablePaths()) tiles.push_back({TileKind::kGameTable, path, std::string(content::NameOf(path)), "Game table"});
    return tiles;
  }
  // A search or a class filter finds assets anywhere below the folder shown.
  if (!search_.empty() || !class_filter_.empty()) {
    const std::string wanted = Lower(search_);
    for (const auto& [mod, found] : content_) {
      for (const AssetInfo& asset : found.assets) {
        if ((!folder_.empty() && !Within(asset.path, folder_)) || Lower(asset.name).find(wanted) == std::string::npos || !PassesFilter(asset)) continue;
        add_asset(asset, std::string(content::FolderOf(asset.path)));
      }
    }
    return tiles;
  }
  if (folder_.empty()) {
    for (const ModInfo& mod : model_.mods().mods()) tiles.push_back({TileKind::kMod, mod.folder, mod.name, mod.enabled ? std::string() : std::string("off"), !mod.enabled});
    return tiles;
  }
  const content::ModContent* found = ContentOf(content::ModOf(folder_));
  if (!found) return tiles;
  if (folder_.find('/') == std::string::npos) {
    tiles.push_back({TileKind::kMetadata, folder_, kMetadataLabel, "mod.toml"});
    tiles.push_back({TileKind::kTranslations, folder_ + std::string(kTranslationsSuffix), "Translations", "localisation"});
  }
  for (const std::string& folder : found->folders) {
    if (Same(content::FolderOf(folder), folder_)) tiles.push_back({TileKind::kFolder, folder, std::string(content::NameOf(folder)), "Folder"});
  }
  for (const AssetInfo& asset : found->assets) {
    if (Same(content::FolderOf(asset.path), folder_) && PassesFilter(asset)) add_asset(asset, parent_of(asset));
  }
  return tiles;
}

bool ContentBrowserWindow::PassesFilter(const AssetInfo& asset) const {
  if (class_filter_.empty()) return true;
  const auto look = looks_.find(Lower(asset.path));
  if (look == looks_.end()) return false;
  return std::any_of(look->second.chain.begin(), look->second.chain.end(), [this](const std::string& at) { return Same(at, class_filter_); });
}

void ContentBrowserWindow::DrawGrid() {
  const std::vector<Tile> tiles = Tiles();
  const float font = ImGui::GetFontSize();
  const ImGuiStyle& style = ImGui::GetStyle();
  // The glyph, then the name on up to two lines and what it is on a third.
  const ImVec2 size(font * kTileWidth, font * kTileWidth * 0.6f + style.ItemSpacing.y + ImGui::GetTextLineHeightWithSpacing() * 3.0f + style.FramePadding.y * 2.0f);
  const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / (size.x + style.ItemSpacing.x)));

  if (folder_.empty() && model_.mods().mods().empty()) ImGui::TextDisabled("No mods yet. Add one to start.");
  else if (tiles.empty()) ImGui::TextDisabled(search_.empty() && class_filter_.empty() ? "Nothing here yet. Right click, or use Add, to make something." : "Nothing matches.");
  for (size_t i = 0; i < tiles.size(); ++i) {
    if (i % static_cast<size_t>(columns) != 0) ImGui::SameLine();
    DrawTile(tiles[i], size);
  }

  // A right click on empty space makes something here.
  if (!folder_.empty() && ImGui::BeginPopupContextWindow("##empty", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
    if (ImGui::MenuItem("New Folder...")) AskFor(Ask::kNewFolder, folder_, "NewFolder");
    if (ImGui::MenuItem("New Class...")) AskFor(Ask::kNewClass, folder_, "NewClass");
    if (ImGui::MenuItem("New Enum...")) AskFor(Ask::kNewEnum, folder_, "NewEnum");
    if (ImGui::MenuItem("New Struct...")) AskFor(Ask::kNewStruct, folder_, "NewStruct");
    if (ImGui::MenuItem("New Data Table...")) AskFor(Ask::kNewTable, folder_, "NewTable");
    if (content::HasGameTexts() && ImGui::MenuItem("New Language...")) AskFor(Ask::kNewLanguage, folder_, "NewLanguage");
    if (!content::Worlds().empty() && ImGui::MenuItem("New Map Edit...")) AskFor(Ask::kNewMapEdit, folder_, "NewMapEdit");
    if (!content::MaterialTypePaths().empty() && ImGui::MenuItem("New Material...")) AskFor(Ask::kNewMaterial, folder_, "NewMaterial");
    if (!content::Worlds().empty() && ImGui::MenuItem("New World...")) AskFor(Ask::kNewWorld, folder_, "NewWorld");
    if (content::HasPackets() && ImGui::MenuItem("New Packet...")) AskFor(Ask::kNewPacket, folder_, "NewPacket");
    if (ImGui::MenuItem("Import Image...", nullptr, false, !picking_)) StartImport(folder_);
    if (ImGui::MenuItem("Import Mesh...", nullptr, false, !picking_)) StartImport(folder_, true);
    ImGui::EndPopup();
  }
  if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput && !selected_.empty()) {
    const auto chosen = std::find_if(tiles.begin(), tiles.end(), [this](const Tile& tile) { return tile.path == selected_ && tile.kind != TileKind::kMetadata; });
    if (chosen != tiles.end() && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) Open(*chosen);
    const bool deletable = chosen != tiles.end() && chosen->kind != TileKind::kMod && chosen->kind != TileKind::kGameTable && chosen->kind != TileKind::kTranslations;
    if (deletable && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) AskFor(Ask::kDelete, chosen->path);
  }
}

void ContentBrowserWindow::DrawTile(const Tile& tile, const ImVec2& size) {
  ImGui::PushID(static_cast<int>(tile.kind));
  ImGui::PushID(tile.path.c_str());
  const ImVec2 min = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##tile", size);
  const bool hovered = ImGui::IsItemHovered();
  const bool selected = tile.path == selected_ && tile.kind != TileKind::kMetadata;
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) selected_ = tile.kind == TileKind::kMetadata ? std::string() : tile.path;
  if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) Open(tile);

  const bool movable = tile.kind != TileKind::kMod && tile.kind != TileKind::kMetadata && tile.kind != TileKind::kTranslations && tile.kind != TileKind::kGameTable;
  if (movable && ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload(kAssetPayload, tile.path.data(), tile.path.size());
    ImGui::Text("Move %s", tile.path.c_str());
    ImGui::EndDragDropSource();
  }
  if (tile.kind == TileKind::kFolder) AcceptMove(tile.path);
  if (ImGui::BeginPopupContextItem("##menu")) {
    DrawTileMenu(tile);
    ImGui::EndPopup();
  }
  if (hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
    if (tile.kind == TileKind::kClass) {
      const auto look = looks_.find(Lower(tile.path));
      std::string chain;
      if (look != looks_.end()) {
        for (size_t i = 1; i < look->second.chain.size(); ++i) chain += (i > 1 ? " > " : "") + look->second.chain[i];
      }
      ImGui::SetTooltip("%s\nFrom %s", tile.path.c_str(), chain.empty() ? "a class that isn't there" : chain.c_str());
    } else if (tile.kind == TileKind::kMetadata) {
      ImGui::SetTooltip("The mod's name, version, author, description and what it depends on");
    } else if (tile.kind == TileKind::kTranslations) {
      ImGui::SetTooltip("The mod's texts, and their translations into the game's other languages");
    } else {
      ImGui::SetTooltip("%s", tile.path.c_str());
    }
  }

  // The tile: a card, lit when hovered or picked, with its glyph above its name.
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 max(min.x + size.x, min.y + size.y);
  const float rounding = ImGui::GetStyle().FrameRounding + 2.0f;
  const ImU32 card = selected ? ImGui::GetColorU32(ImVec4(0.3f, 0.5f, 0.95f, 0.45f)) : ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, hovered ? 0.12f : 0.05f));
  draw->AddRectFilled(min, max, card, rounding);
  if (selected) draw->AddRect(min, max, ImGui::GetColorU32(ImVec4(0.5f, 0.7f, 1.0f, 0.9f)), rounding, 0, 1.5f);
  const float glyph = size.x * 0.75f;
  DrawGlyph(tile, ImVec2(min.x + (size.x - glyph) * 0.5f, min.y + ImGui::GetStyle().FramePadding.y), glyph);

  const float padding = ImGui::GetStyle().FramePadding.x;
  const float text_width = size.x - padding * 2.0f;
  const ImU32 text = ImGui::GetColorU32(tile.dim ? ImGuiCol_TextDisabled : ImGuiCol_Text);
  float line_y = min.y + ImGui::GetStyle().FramePadding.y + glyph * 0.8f + ImGui::GetStyle().ItemSpacing.y;
  const auto centered = [&](const std::string& line, ImU32 color) {
    draw->AddText(ImVec2(min.x + (size.x - ImGui::CalcTextSize(line.c_str()).x) * 0.5f, line_y), color, line.c_str());
    line_y += ImGui::GetTextLineHeightWithSpacing();
  };
  const auto [first, second] = SplitLabel(tile.label, text_width);
  centered(first, text);
  if (!second.empty()) centered(second, text);
  centered(Fit(tile.sub, text_width), ImGui::GetColorU32(ImGuiCol_TextDisabled));
  ImGui::PopID();
  ImGui::PopID();
}

void ContentBrowserWindow::DrawGlyph(const Tile& tile, const ImVec2& min, float size) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float height = size * 0.8f;
  const ImVec2 center(min.x + size * 0.5f, min.y + height * 0.5f);
  const float alpha = tile.dim ? 0.45f : 1.0f;
  const auto folder = [&](const ImVec4& color) {
    const float w = size * 0.62f;
    const float h = height * 0.56f;
    const ImVec2 at(center.x - w * 0.5f, center.y - h * 0.45f);
    draw->AddRectFilled(at, ImVec2(at.x + w * 0.42f, at.y + h * 0.2f), Shade(color, 0.7f, alpha), 3.0f);
    draw->AddRectFilled(ImVec2(at.x, at.y + h * 0.12f), ImVec2(at.x + w, at.y + h), Shade(color, 1.0f, alpha), 4.0f);
  };
  // A sheet with its corner folded, and lines of writing.
  const auto sheet = [&](const ImVec4& color, bool lines) {
    const float w = height * 0.5f;
    const float h = height * 0.66f;
    const ImVec2 at(center.x - w * 0.5f, center.y - h * 0.5f);
    const float fold = w * 0.3f;
    const ImVec2 points[] = {at, ImVec2(at.x + w - fold, at.y), ImVec2(at.x + w, at.y + fold), ImVec2(at.x + w, at.y + h), ImVec2(at.x, at.y + h)};
    draw->AddConvexPolyFilled(points, 5, Shade(color, 0.85f, alpha));
    draw->AddTriangleFilled(ImVec2(at.x + w - fold, at.y), ImVec2(at.x + w - fold, at.y + fold), ImVec2(at.x + w, at.y + fold), Shade(color, 0.55f, alpha));
    if (!lines) return;
    for (int line = 0; line < 3; ++line) {
      const float y = at.y + h * (0.42f + 0.16f * static_cast<float>(line));
      draw->AddLine(ImVec2(at.x + w * 0.18f, y), ImVec2(at.x + w * (line == 2 ? 0.6f : 0.82f), y), Shade(color, 0.4f, alpha), 2.0f);
    }
  };
  // A cube, its three faces in three shades.
  const auto cube = [&](const ImVec4& color) {
    const float r = height * 0.34f;
    const float x = r * 0.866f;
    const ImVec2 top(center.x, center.y - r);
    const ImVec2 upper_left(center.x - x, center.y - r * 0.5f);
    const ImVec2 upper_right(center.x + x, center.y - r * 0.5f);
    const ImVec2 lower_left(center.x - x, center.y + r * 0.5f);
    const ImVec2 lower_right(center.x + x, center.y + r * 0.5f);
    const ImVec2 bottom(center.x, center.y + r);
    const ImVec2 lid[] = {top, upper_right, center, upper_left};
    const ImVec2 left[] = {upper_left, center, bottom, lower_left};
    const ImVec2 right[] = {center, upper_right, lower_right, bottom};
    draw->AddConvexPolyFilled(lid, 4, Shade(color, 1.0f, alpha));
    draw->AddConvexPolyFilled(left, 4, Shade(color, 0.72f, alpha));
    draw->AddConvexPolyFilled(right, 4, Shade(color, 0.5f, alpha));
  };

  switch (tile.kind) {
    case TileKind::kMod: {
      const ModInfo* mod = model_.mods().Find(tile.path);
      const ImTextureID icon = mod ? model_.ModIcon(*mod) : ImTextureID_Invalid;
      if (icon == ImTextureID_Invalid) {
        folder(kModColor);
        break;
      }
      const float side = height * 0.82f;
      draw->AddImageRounded(icon, ImVec2(center.x - side * 0.5f, center.y - side * 0.5f), ImVec2(center.x + side * 0.5f, center.y + side * 0.5f), ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)), 4.0f);
      break;
    }
    case TileKind::kMetadata: sheet(kMetadataColor, true); break;
    case TileKind::kTranslations: {
      // Two speech bubbles, one over the other: a text in two languages.
      const float w = height * 0.42f;
      const float h = height * 0.3f;
      for (int bubble = 0; bubble < 2; ++bubble) {
        const ImVec2 at(center.x - w * (bubble ? 0.1f : 0.9f), center.y - h * (bubble ? 0.15f : 0.85f));
        draw->AddRectFilled(at, ImVec2(at.x + w, at.y + h), Shade(kTranslationsColor, bubble ? 0.7f : 1.0f, alpha), h * 0.3f);
        const ImVec2 tail(at.x + w * (bubble ? 0.75f : 0.25f), at.y + h);
        draw->AddTriangleFilled(ImVec2(tail.x - h * 0.15f, tail.y - 1.0f), ImVec2(tail.x + h * 0.15f, tail.y - 1.0f), ImVec2(tail.x + (bubble ? h * 0.2f : -h * 0.2f), tail.y + h * 0.3f), Shade(kTranslationsColor, bubble ? 0.7f : 1.0f, alpha));
      }
      break;
    }
    case TileKind::kFolder: folder(kFolderColor); break;
    case TileKind::kClass: {
      const auto look = looks_.find(Lower(tile.path));
      cube(look != looks_.end() && look->second.typed ? kTypedClassColor : kDataClassColor);
      break;
    }
    case TileKind::kEnum: {
      // A list: three bars, each with its bullet.
      const float w = height * 0.56f;
      const float bar = height * 0.09f;
      for (int row = 0; row < 3; ++row) {
        const float y = center.y + (static_cast<float>(row) - 1.0f) * bar * 2.2f;
        draw->AddCircleFilled(ImVec2(center.x - w * 0.5f, y), bar * 0.6f, Shade(kEnumColor, 1.0f, alpha));
        draw->AddRectFilled(ImVec2(center.x - w * 0.32f, y - bar * 0.5f), ImVec2(center.x + w * 0.5f, y + bar * 0.5f), Shade(kEnumColor, 0.8f, alpha), bar * 0.5f);
      }
      break;
    }
    case TileKind::kStruct: {
      // Fields side by side: four blocks in a square.
      const float side = height * 0.2f;
      const float gap = side * 0.25f;
      for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
          const ImVec2 at(center.x + (static_cast<float>(column) - 1.0f) * (side + gap) + gap * 0.5f, center.y + (static_cast<float>(row) - 1.0f) * (side + gap) + gap * 0.5f);
          draw->AddRectFilled(at, ImVec2(at.x + side, at.y + side), Shade(kStructColor, row == column ? 1.0f : 0.7f, alpha), 3.0f);
        }
      }
      break;
    }
    case TileKind::kTable:
    case TileKind::kGameTable: {
      // A grid: a header row, then rows, in three columns.
      const ImVec4& color = tile.kind == TileKind::kTable ? kTableColor : kGameTableColor;
      const float w = height * 0.62f;
      const float h = height * 0.5f;
      const ImVec2 at(center.x - w * 0.5f, center.y - h * 0.5f);
      draw->AddRectFilled(at, ImVec2(at.x + w, at.y + h * 0.25f), Shade(color, 1.0f, alpha), 3.0f);
      draw->AddRect(at, ImVec2(at.x + w, at.y + h), Shade(color, 0.85f, alpha), 3.0f, 0, 2.0f);
      for (int line = 1; line < 3; ++line) {
        const float y = at.y + h * (0.25f + 0.25f * static_cast<float>(line));
        draw->AddLine(ImVec2(at.x, y), ImVec2(at.x + w, y), Shade(color, 0.6f, alpha), 1.5f);
        const float x = at.x + w * static_cast<float>(line) / 3.0f;
        draw->AddLine(ImVec2(x, at.y + h * 0.25f), ImVec2(x, at.y + h), Shade(color, 0.6f, alpha), 1.5f);
      }
      break;
    }
    case TileKind::kTexture: {
      // Its own picture, as large as fits, or a framed landscape while there's none to show.
      const content::ModContent* found = ContentOf(content::ModOf(tile.path));
      const auto asset = found ? std::find_if(found->assets.begin(), found->assets.end(), [&tile](const AssetInfo& info) { return Same(info.path, tile.path); }) : std::vector<AssetInfo>::const_iterator{};
      const MenuModel::Picture picture = found && asset != found->assets.end() ? model_.AssetPicture(content::TextureImageFile(asset->file)) : MenuModel::Picture{};
      if (picture.id != ImTextureID_Invalid && picture.width && picture.height) {
        const float box = height * 0.86f;
        const float scale = box / static_cast<float>(std::max(picture.width, picture.height));
        const ImVec2 half(static_cast<float>(picture.width) * scale * 0.5f, static_cast<float>(picture.height) * scale * 0.5f);
        draw->AddImageRounded(picture.id, ImVec2(center.x - half.x, center.y - half.y), ImVec2(center.x + half.x, center.y + half.y), ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)), 3.0f);
        break;
      }
      const float w = height * 0.62f;
      const float h = height * 0.48f;
      const ImVec2 at(center.x - w * 0.5f, center.y - h * 0.5f);
      draw->AddRectFilled(at, ImVec2(at.x + w, at.y + h), Shade(kTextureColor, 0.45f, alpha), 3.0f);
      const ImVec2 hills[] = {ImVec2(at.x, at.y + h), ImVec2(at.x + w * 0.35f, at.y + h * 0.45f), ImVec2(at.x + w * 0.6f, at.y + h * 0.75f), ImVec2(at.x + w * 0.78f, at.y + h * 0.55f), ImVec2(at.x + w, at.y + h)};
      draw->AddConvexPolyFilled(hills, 5, Shade(kTextureColor, 1.0f, alpha));
      draw->AddCircleFilled(ImVec2(at.x + w * 0.75f, at.y + h * 0.28f), h * 0.12f, Shade(kTextureColor, 1.2f, alpha));
      break;
    }
    case TileKind::kLanguage: {
      // A speech bubble of lines of text.
      const float w = height * 0.62f;
      const float h = height * 0.44f;
      const ImVec2 at(center.x - w * 0.5f, center.y - h * 0.6f);
      draw->AddRectFilled(at, ImVec2(at.x + w, at.y + h), Shade(kLanguageColor, 1.0f, alpha), h * 0.25f);
      const ImVec2 tail(at.x + w * 0.3f, at.y + h);
      draw->AddTriangleFilled(ImVec2(tail.x - h * 0.12f, tail.y - 1.0f), ImVec2(tail.x + h * 0.16f, tail.y - 1.0f), ImVec2(tail.x - h * 0.2f, tail.y + h * 0.3f), Shade(kLanguageColor, 1.0f, alpha));
      for (int line = 0; line < 3; ++line) {
        const float y = at.y + h * (0.28f + 0.22f * static_cast<float>(line));
        const float length = line == 2 ? 0.45f : 0.7f;
        draw->AddLine(ImVec2(at.x + w * 0.15f, y), ImVec2(at.x + w * (0.15f + length), y), Shade(kLanguageColor, 0.45f, alpha), h * 0.09f);
      }
      break;
    }
    case TileKind::kMapEdit: {
      // A map folded in three, a pin on it.
      const float w = height * 0.62f;
      const float h = height * 0.46f;
      const ImVec2 at(center.x - w * 0.5f, center.y - h * 0.4f);
      for (int panel = 0; panel < 3; ++panel) {
        const float x0 = at.x + w * static_cast<float>(panel) / 3.0f, x1 = at.x + w * static_cast<float>(panel + 1) / 3.0f;
        const float lift = panel == 1 ? h * 0.08f : 0.0f;
        const ImVec2 points[] = {ImVec2(x0, at.y + (panel == 1 ? 0.0f : lift + h * 0.08f)), ImVec2(x1, at.y + (panel == 1 ? h * 0.08f : 0.0f)), ImVec2(x1, at.y + h + (panel == 1 ? h * 0.08f : 0.0f)), ImVec2(x0, at.y + h + (panel == 1 ? 0.0f : h * 0.08f))};
        draw->AddConvexPolyFilled(points, 4, Shade(kMapEditColor, panel == 1 ? 0.75f : 1.0f, alpha));
      }
      const ImVec2 pin(center.x + w * 0.12f, at.y + h * 0.3f);
      draw->AddCircleFilled(pin, h * 0.16f, Shade(kPinColor, 0.35f, alpha));
      draw->AddCircleFilled(pin, h * 0.13f, Shade(kPinColor, 1.0f, alpha));
      draw->AddTriangleFilled(ImVec2(pin.x - h * 0.11f, pin.y + h * 0.06f), ImVec2(pin.x + h * 0.11f, pin.y + h * 0.06f), ImVec2(pin.x, pin.y + h * 0.36f), Shade(kPinColor, 1.0f, alpha));
      break;
    }
    case TileKind::kMaterial: {
      // A shaded ball: its colour, darker below, a highlight up and to the left.
      const float r = height * 0.27f;
      const ImVec2 ball(center.x, center.y + height * 0.02f);
      draw->AddCircleFilled(ball, r, Shade(kMaterialColor, 0.55f, alpha), 32);
      draw->AddCircleFilled(ImVec2(ball.x - r * 0.12f, ball.y - r * 0.12f), r * 0.86f, Shade(kMaterialColor, 0.85f, alpha), 32);
      draw->AddCircleFilled(ImVec2(ball.x - r * 0.3f, ball.y - r * 0.3f), r * 0.45f, Shade(kMaterialColor, 1.0f, alpha), 24);
      draw->AddCircleFilled(ImVec2(ball.x - r * 0.42f, ball.y - r * 0.42f), r * 0.16f, IM_COL32(255, 255, 255, static_cast<int>(200 * alpha)), 16);
      break;
    }
    case TileKind::kMesh: {
      // A box seen from above a corner, its three faces shaded.
      const float s = height * 0.24f;
      const ImVec2 top(center.x, center.y - s), left(center.x - s * 0.87f, center.y - s * 0.5f), right(center.x + s * 0.87f, center.y - s * 0.5f);
      const ImVec2 middle(center.x, center.y), low_left(left.x, left.y + s), low_right(right.x, right.y + s), bottom(center.x, center.y + s);
      const ImVec2 lid[] = {top, right, middle, left};
      const ImVec2 front_left[] = {left, middle, bottom, low_left};
      const ImVec2 front_right[] = {middle, right, low_right, bottom};
      draw->AddConvexPolyFilled(lid, 4, Shade(kMeshColor, 1.15f, alpha));
      draw->AddConvexPolyFilled(front_left, 4, Shade(kMeshColor, 0.85f, alpha));
      draw->AddConvexPolyFilled(front_right, 4, Shade(kMeshColor, 0.6f, alpha));
      break;
    }
    case TileKind::kWorld: {
      // A globe: water, two lands, the lines of a grid.
      const float r = height * 0.27f;
      draw->AddCircleFilled(center, r, Shade(kWorldColor, 0.8f, alpha), 32);
      draw->AddCircleFilled(ImVec2(center.x - r * 0.35f, center.y - r * 0.2f), r * 0.38f, Shade(kLandColor, 1.0f, alpha), 20);
      draw->AddCircleFilled(ImVec2(center.x + r * 0.4f, center.y + r * 0.35f), r * 0.28f, Shade(kLandColor, 0.9f, alpha), 20);
      draw->AddEllipse(center, ImVec2(r * 0.45f, r), Shade(kWorldColor, 1.3f, alpha), 0.0f, 32, 1.5f);
      draw->AddLine(ImVec2(center.x - r, center.y), ImVec2(center.x + r, center.y), Shade(kWorldColor, 1.3f, alpha), 1.5f);
      draw->AddCircle(center, r, Shade(kWorldColor, 1.3f, alpha), 32, 1.5f);
      break;
    }
    case TileKind::kPacket: {
      // A seed packet: a paper bag with its top folded over and a sprout on its front.
      const float w = height * 0.42f;
      const float h = height * 0.54f;
      const ImVec2 at(center.x - w * 0.5f, center.y - h * 0.5f);
      draw->AddRectFilled(at, ImVec2(at.x + w, at.y + h), Shade(kPacketColor, 0.85f, alpha), 3.0f);
      draw->AddRectFilled(at, ImVec2(at.x + w, at.y + h * 0.2f), Shade(kPacketColor, 1.15f, alpha), 3.0f);
      const ImVec2 stem(center.x, at.y + h * 0.82f);
      draw->AddLine(stem, ImVec2(stem.x, stem.y - h * 0.38f), Shade(kSproutColor, 0.8f, alpha), 2.0f);
      draw->AddCircleFilled(ImVec2(stem.x - w * 0.14f, stem.y - h * 0.34f), w * 0.13f, Shade(kSproutColor, 1.0f, alpha), 16);
      draw->AddCircleFilled(ImVec2(stem.x + w * 0.14f, stem.y - h * 0.42f), w * 0.13f, Shade(kSproutColor, 1.1f, alpha), 16);
      break;
    }
    case TileKind::kOther: sheet(kOtherColor, false); break;
  }
}

void ContentBrowserWindow::DrawTileMenu(const Tile& tile) {
  if (ImGui::MenuItem("Open")) Open(tile);
  if (tile.kind == TileKind::kGameTable) {
    if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(tile.path.c_str());
    return;
  }
  if (tile.kind == TileKind::kMod || tile.kind == TileKind::kMetadata) {
    if (ImGui::MenuItem("Edit Metadata")) OpenEditor(tile.path, TileKind::kMetadata);
    if (ImGui::MenuItem("Translations")) OpenEditor(tile.path + std::string(kTranslationsSuffix), TileKind::kTranslations);
    return;
  }
  if (tile.kind == TileKind::kTranslations) return;
  if (tile.kind == TileKind::kClass && ImGui::MenuItem("New Subclass...")) {
    AskFor(Ask::kNewClass, std::string(content::FolderOf(tile.path)), tile.label + "_Child");
    ask_parent_ = tile.path;
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Rename...")) AskFor(Ask::kRename, tile.path, tile.label);
  if (tile.kind != TileKind::kFolder && ImGui::MenuItem("Duplicate")) DuplicateAsset(tile.path);
  if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(tile.path.c_str());
  ImGui::Separator();
  if (ImGui::MenuItem("Delete...")) AskFor(Ask::kDelete, tile.path);
}

void ContentBrowserWindow::DrawFooter() {
  ImGui::AlignTextToFramePadding();
  if (!status_.empty() && ImGui::GetTime() - status_time_ < widgets::kStatusSeconds) {
    ImGui::TextColored(status_error_ ? widgets::kErrorColor : widgets::kGoodColor, "%s", status_.c_str());
    return;
  }
  if (game_tables_) {
    ImGui::TextDisabled("The game's own tables, made from its data. Mods read them as they read their own, and can't change them.");
    return;
  }
  if (folder_.empty()) {
    ImGui::TextDisabled("%zu mods. Open one to see its content.", model_.mods().mods().size());
    return;
  }
  const ModInfo* mod = ModOf(folder_);
  if (mod && !mod->enabled) ImGui::TextColored(widgets::kWarningColor, "%s is off: its content is here to edit, but doesn't load until it's on.", mod->name.c_str());
  else ImGui::TextDisabled("%s", folder_.c_str());
}

void ContentBrowserWindow::AcceptMove(const std::string& folder) {
  if (!ImGui::BeginDragDropTarget()) return;
  if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
    const std::string path(static_cast<const char*>(payload->Data), static_cast<size_t>(payload->DataSize));
    MoveTo(path, folder);
  }
  ImGui::EndDragDropTarget();
}

// Editors -----------------------------------------------------------------------------------------------

void ContentBrowserWindow::DrawEditors() {
  // Drawn by index: a save in one reads the content again, which doesn't touch the list.
  for (size_t i = 0; i < editors_.size();) {
    if (editors_[i]->Draw()) ++i;
    else editors_.erase(editors_.begin() + static_cast<ptrdiff_t>(i));
  }
}

void ContentBrowserWindow::OpenEditor(const std::string& path, TileKind kind) {
  // A metadata editor's path is its mod's, which no asset has.
  const auto open = std::find_if(editors_.begin(), editors_.end(), [&path](const auto& editor) { return Same(editor->path(), path); });
  if (open != editors_.end()) {
    (*open)->Focus();
    return;
  }
  std::unique_ptr<AssetEditor> editor;
  switch (kind) {
    case TileKind::kMetadata: editor = OpenMetadataEditor(*this, path); break;
    case TileKind::kTranslations: editor = OpenTranslationEditor(*this, path.substr(0, path.size() - kTranslationsSuffix.size())); break;
    case TileKind::kClass: editor = OpenClassEditor(*this, path); break;
    case TileKind::kEnum: editor = OpenEnumEditor(*this, path); break;
    case TileKind::kStruct: editor = OpenStructEditor(*this, path); break;
    case TileKind::kTable: editor = OpenTableEditor(*this, path); break;
    case TileKind::kGameTable: editor = OpenGameTableEditor(*this, path); break;
    case TileKind::kLanguage: editor = OpenLanguageEditor(*this, path); break;
    case TileKind::kTexture: editor = OpenTextureEditor(*this, path); break;
    case TileKind::kMapEdit: editor = OpenMapEditEditor(*this, path); break;
    case TileKind::kMaterial: editor = OpenMaterialEditor(*this, path); break;
    case TileKind::kMesh: editor = OpenMeshEditor(*this, path); break;
    case TileKind::kWorld: editor = OpenWorldEditor(*this, path); break;
    case TileKind::kPacket: editor = OpenPacketEditor(*this, path); break;
    default: break;
  }
  if (!editor) {
    SetStatus("Couldn't open " + path + "; its file isn't one RexFabric can read", true);
    return;
  }
  editors_.push_back(std::move(editor));
}

void ContentBrowserWindow::Open(const Tile& tile) {
  switch (tile.kind) {
    case TileKind::kMod:
    case TileKind::kFolder: Navigate(tile.path); break;
    case TileKind::kMetadata:
    case TileKind::kTranslations:
    case TileKind::kClass:
    case TileKind::kEnum:
    case TileKind::kStruct:
    case TileKind::kTable:
    case TileKind::kLanguage:
    case TileKind::kTexture:
    case TileKind::kMapEdit:
    case TileKind::kMaterial:
    case TileKind::kMesh:
    case TileKind::kWorld:
    case TileKind::kPacket:
    case TileKind::kGameTable: OpenEditor(tile.path, tile.kind); break;
    case TileKind::kOther: SetStatus("No editor opens this kind of asset yet", true); break;
  }
}

void ContentBrowserWindow::DrawClosePrompt() {
  if (open_close_prompt_) {
    open_close_prompt_ = false;
    ImGui::OpenPopup(kClosePopup);
  }
  if (!ImGui::BeginPopupModal(kClosePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::TextUnformatted("These have unsaved changes:");
  for (const auto& editor : editors_) {
    if (editor->dirty()) ImGui::BulletText("%s", editor->path().c_str());
  }
  if (glass::Button("Save All", glass::kAccent)) {
    bool saved = true;
    for (const auto& editor : editors_) {
      if (editor->dirty()) saved &= editor->Save();
    }
    closing_ = saved;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (glass::Button("Don't Save", glass::kDanger)) {
    closing_ = true;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (glass::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// Changes -----------------------------------------------------------------------------------------------

const content::ModContent* ContentBrowserWindow::ContentOf(std::string_view mod) const {
  const auto found = content_.find(Lower(mod));
  return found != content_.end() ? &found->second : nullptr;
}

const ModInfo* ContentBrowserWindow::ModOf(std::string_view path) const { return model_.mods().Find(content::ModOf(path)); }

void ContentBrowserWindow::NavigateGameTables() {
  Navigate({});
  game_tables_ = true;
}

void ContentBrowserWindow::Navigate(std::string folder) {
  if (!folder.empty() && !ModOf(folder)) folder.clear();
  game_tables_ = false;
  folder_ = std::move(folder);
  LastFolder() = folder_;
  selected_.clear();
  reveal_ = true;
}

void ContentBrowserWindow::AskFor(Ask ask, std::string target, std::string text) {
  ask_ = ask;
  ask_target_ = std::move(target);
  ask_text_ = std::move(text);
  // A new class starts below Object; a new table's struct is picked; a new language is of the one the game shows.
  ask_parent_ = ask == Ask::kNewClass ? "Object" : ask == Ask::kNewLanguage ? content::CurrentLanguage() : "";
  // A map edit starts on the world the game is in, or its first.
  if (ask == Ask::kNewMapEdit) ask_parent_ = !content::CurrentWorld().empty() ? content::CurrentWorld() : content::Worlds().empty() ? std::string() : content::Worlds().front().path;
  // A material starts as the game's first material type.
  if (ask == Ask::kNewMaterial) ask_parent_ = content::MaterialTypePaths().empty() ? std::string() : content::MaterialTypePaths().front();
  // A world starts on the game's first world.
  if (ask == Ask::kNewWorld) ask_parent_ = content::Worlds().empty() ? std::string() : content::Worlds().front().path;
  // A packet starts on the game's first packet base.
  if (ask == Ask::kNewPacket) {
    const std::optional<content::Enum> bases = content::FindEnum(content::PacketBaseEnum());
    ask_parent_ = bases && !bases->values.empty() ? bases->values.front().name : std::string();
  }
  if (ask == Ask::kNewLanguage && ask_parent_.empty() && !content::Languages().empty()) ask_parent_ = content::Languages().front().code;
  ask_references_ = ask == Ask::kRename || ask == Ask::kDelete ? content::ReferencesTo(model_.mods(), ask_target_) : std::vector<content::Reference>{};
  open_ask_ = true;
}

const char* ContentBrowserWindow::AskTitle(Ask ask) {
  switch (ask) {
    case Ask::kNone: return "";
    case Ask::kNewMod: return "New Mod";
    case Ask::kNewFolder: return "New Folder";
    case Ask::kNewClass: return "New Class";
    case Ask::kNewEnum: return "New Enum";
    case Ask::kNewStruct: return "New Struct";
    case Ask::kNewTable: return "New Data Table";
    case Ask::kNewLanguage: return "New Language";
    case Ask::kNewMapEdit: return "New Map Edit";
    case Ask::kNewMaterial: return "New Material";
    case Ask::kNewWorld: return "New World";
    case Ask::kNewPacket: return "New Packet";
    case Ask::kImportImage: return "Import Image";
    case Ask::kImportMesh: return "Import Mesh";
    case Ask::kRename: return "Rename";
    case Ask::kDelete: return "Delete";
  }
  return "";
}

void ContentBrowserWindow::DrawPopups() {
  if (open_ask_) {
    open_ask_ = false;
    ImGui::OpenPopup(kAskPopup);
  }
  const std::string title = std::string(AskTitle(ask_)) + kAskPopup;
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * kAskWidth, 0.0f));
  if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoResize)) return;

  bool done = false;
  const ModInfo* mod = ModOf(ask_target_);
  if (ask_ == Ask::kDelete) {
    std::error_code code;
    const bool folder = ask_target_.find('/') != std::string::npos && mod && std::filesystem::is_directory(content::FolderOnDisk(*mod, ask_target_), code);
    ImGui::TextWrapped("Delete %s%s? This can't be undone.", ask_target_.c_str(), folder ? " and everything in it" : "");
    if (!ask_references_.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, widgets::kWarningColor);
      ImGui::TextWrapped("These name it, and won't load until they're fixed:");
      ImGui::PopStyleColor();
      for (size_t i = 0; i < std::min(ask_references_.size(), kListedReferences); ++i) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s (%s)", ask_references_[i].from.c_str(), ask_references_[i].where.c_str());
      }
      if (ask_references_.size() > kListedReferences) ImGui::TextDisabled("and %zu more", ask_references_.size() - kListedReferences);
    }
    if (glass::Button("Delete", glass::kDanger)) {
      DeleteTarget();
      done = true;
    }
  } else {
    // The name, checked as it's typed.
    std::string problem;
    if (ask_ == Ask::kNewMod) {
      std::string why;
      if (!content::ValidName(ask_text_, &why)) problem = why;
      else if (std::error_code code; model_.mods().Find(ask_text_) || std::filesystem::exists(model_.mods().root() / ask_text_, code)) problem = "There's a mod folder named " + ask_text_ + " already";
      ImGui::TextWrapped("A folder in the mods folder, with its metadata and a content folder to start.");
    } else if (mod) {
      const bool rename = ask_ == Ask::kRename;
      problem = rename && ask_text_ == content::NameOf(ask_target_) ? std::string() : content::NameProblem(*mod, rename ? content::FolderOf(ask_target_) : ask_target_, ask_text_, rename ? ask_target_ : std::string());
      if (rename && !ask_references_.empty()) ImGui::TextWrapped("%zu thing%s naming it will name it by its new path.", ask_references_.size(), ask_references_.size() == 1 ? "" : "s");
      if (!rename) ImGui::TextWrapped("In %s", ask_target_.c_str());
    }
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool entered = widgets::InputString("##name", ask_text_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (ask_ == Ask::kNewClass) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Parent");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::BeginCombo("##parent", ask_parent_.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) combo_filter_.clear();
        ImGui::SetNextItemWidth(-FLT_MIN);
        widgets::InputString("##filter", combo_filter_, 0, "Search");
        for (const std::string& path : Choices(content::ModOf(ask_target_)).classes) {
          if (!combo_filter_.empty() && Lower(path).find(Lower(combo_filter_)) == std::string::npos) continue;
          if (ImGui::Selectable(path.c_str(), path == ask_parent_)) ask_parent_ = path;
          if (std::optional<content::Class> known = content::FindClass(path); known && !known->help.empty()) ImGui::SetItemTooltip("%s", known->help.c_str());
        }
        ImGui::EndCombo();
      }
    }
    if (ask_ == Ask::kNewTable) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Rows of");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-FLT_MIN);
      PathCombo("##row", ask_parent_.empty() ? "Pick a struct" : ask_parent_.c_str(), ask_parent_, Choices(content::ModOf(ask_target_)).structs, nullptr);
      ImGui::SetItemTooltip("The struct each row is: a column per field");
      if (problem.empty() && ask_parent_.empty()) problem = "Pick the struct its rows are";
    }
    if (ask_ == Ask::kNewLanguage) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Of");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::BeginCombo("##language", content::LanguageName(ask_parent_).c_str())) {
        for (const content::Language& language : content::Languages()) {
          if (ImGui::Selectable(language.name.c_str(), language.code == ask_parent_)) ask_parent_ = language.code;
        }
        ImGui::EndCombo();
      }
      ImGui::SetItemTooltip("The language whose texts it changes: they show while the game is in it");
      if (problem.empty() && ask_parent_.empty()) problem = "Pick the language it's of";
    }
    if (ask_ == Ask::kNewMapEdit) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Of");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::BeginCombo("##world", ask_parent_.empty() ? "Pick a world" : ask_parent_.c_str())) {
        for (const content::WorldDecl& world : content::Worlds()) {
          if (ImGui::Selectable(world.path.c_str(), world.path == ask_parent_)) ask_parent_ = world.path;
          if (!world.help.empty()) ImGui::SetItemTooltip("%s", world.help.c_str());
        }
        ImGui::EndCombo();
      }
      ImGui::SetItemTooltip("The world it changes: its edits are made each time the game enters it");
      if (problem.empty() && ask_parent_.empty()) problem = "Pick the world it changes";
    }
    if (ask_ == Ask::kNewMaterial) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Of");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::BeginCombo("##material_type", ask_parent_.empty() ? "Pick a type" : ask_parent_.c_str())) {
        for (const std::string& type : content::MaterialTypePaths()) {
          if (ImGui::Selectable(type.c_str(), type == ask_parent_)) ask_parent_ = type;
          if (const auto declared = content::FindMaterialType(type); declared && !declared->help.empty()) ImGui::SetItemTooltip("%s", declared->help.c_str());
        }
        ImGui::EndCombo();
      }
      ImGui::SetItemTooltip("The game's kind of material it is: what it can set on what's drawn with it");
      if (problem.empty() && ask_parent_.empty()) problem = "Pick the material's type";
    }
    if (ask_ == Ask::kNewWorld) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("On");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::BeginCombo("##base", ask_parent_.empty() ? "Pick a world" : ask_parent_.c_str())) {
        for (const content::WorldDecl& world : content::Worlds()) {
          if (ImGui::Selectable(world.path.c_str(), world.path == ask_parent_)) ask_parent_ = world.path;
          if (!world.help.empty()) ImGui::SetItemTooltip("%s", world.help.c_str());
        }
        ImGui::EndCombo();
      }
      ImGui::SetItemTooltip("The world it's built on: entered as it, with the new world's map edits besides its own");
      if (problem.empty() && ask_parent_.empty()) problem = "Pick the world it's built on";
    }
    if (ask_ == Ask::kNewPacket) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Pours as");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::BeginCombo("##packet_base", ask_parent_.empty() ? "Pick a base" : ask_parent_.c_str())) {
        if (const std::optional<content::Enum> bases = content::FindEnum(content::PacketBaseEnum())) {
          for (const content::EnumValue& base : bases->values) {
            if (ImGui::Selectable(base.name.c_str(), base.name == ask_parent_)) ask_parent_ = base.name;
            if (!base.help.empty()) ImGui::SetItemTooltip("%s", base.help.c_str());
          }
        }
        ImGui::EndCombo();
      }
      ImGui::SetItemTooltip("The game's packet it's a variant of: it pours, grows and counts as that one does, and looks and sells as it says");
      if (problem.empty() && ask_parent_.empty()) problem = "Pick the packet it pours as";
    }
    if (ask_ == Ask::kImportImage || ask_ == Ask::kImportMesh) ImGui::TextDisabled("From %s", ask_parent_.c_str());
    if (!problem.empty()) ImGui::TextColored(widgets::kErrorColor, "%s", problem.c_str());
    ImGui::BeginDisabled(!problem.empty());
    if (glass::Button("OK", glass::kAccent) || (entered && problem.empty())) {
      if (ask_ == Ask::kNewMod) CreateMod(ask_text_);
      if (ask_ == Ask::kNewFolder) CreateFolder(ask_text_);
      if (ask_ == Ask::kNewClass) CreateClass(ask_text_, ask_parent_);
      if (ask_ == Ask::kNewEnum) CreateType(ask_text_, TileKind::kEnum);
      if (ask_ == Ask::kNewStruct) CreateType(ask_text_, TileKind::kStruct);
      if (ask_ == Ask::kNewTable) CreateTable(ask_text_, ask_parent_);
      if (ask_ == Ask::kNewLanguage) CreateLanguage(ask_text_, ask_parent_);
      if (ask_ == Ask::kNewMapEdit) CreateMapEdit(ask_text_, ask_parent_);
      if (ask_ == Ask::kNewMaterial) CreateMaterial(ask_text_, ask_parent_);
      if (ask_ == Ask::kNewWorld) CreateWorld(ask_text_, ask_parent_);
      if (ask_ == Ask::kNewPacket) CreatePacket(ask_text_, ask_parent_);
      if (ask_ == Ask::kImportImage) ImportImage(ask_text_, ask_parent_);
      if (ask_ == Ask::kImportMesh) ImportMesh(ask_text_, ask_parent_);
      if (ask_ == Ask::kRename) RenameTarget(ask_text_);
      done = true;
    }
    ImGui::EndDisabled();
  }
  ImGui::SameLine();
  if (glass::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done = true;
  if (done) {
    ask_ = Ask::kNone;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void ContentBrowserWindow::CreateMod(const std::string& name) {
  const std::filesystem::path dir = model_.mods().root() / name;
  std::error_code code;
  std::filesystem::create_directories(dir / content::kContentFolder, code);
  if (code) {
    SetStatus("Couldn't make " + dir.string() + ": " + code.message(), true);
    return;
  }
  ModInfo mod{.folder = name, .dir = dir, .name = name, .version = kNewModVersion};
  std::string error;
  if (!WriteMetadata(mod, &error)) SetStatus(error, true);
  // On at the end of the load order; the mods are read again next frame, and the window goes into it then.
  SetModEnabled(name, true);
  refresh_ = true;
  navigate_after_refresh_ = name;
  SetStatus("Made " + name);
}

void ContentBrowserWindow::CreateFolder(const std::string& name) {
  const ModInfo* mod = ModOf(ask_target_);
  std::string error;
  if (!mod || !content::CreateFolder(*mod, ask_target_, name, &error)) {
    SetStatus(error, true);
    return;
  }
  Refresh();
  selected_ = ask_target_ + "/" + name;
  SetStatus("Made " + selected_);
}

void ContentBrowserWindow::CreateClass(const std::string& name, const std::string& parent) {
  const ModInfo* mod = ModOf(ask_target_);
  std::string error;
  if (!mod || !content::CreateClass(*mod, ask_target_, name, parent, &error)) {
    SetStatus(error, true);
    return;
  }
  const std::string path = ask_target_ + "/" + name;
  Saved(path);
  Refresh();
  selected_ = path;
  OpenEditor(path, TileKind::kClass);
  SetStatus("Made " + path);
}

void ContentBrowserWindow::CreateTable(const std::string& name, const std::string& row) {
  const ModInfo* mod = ModOf(ask_target_);
  std::string error;
  if (!mod || !content::CreateTable(*mod, ask_target_, name, row, &error)) {
    SetStatus(error, true);
    return;
  }
  const std::string path = ask_target_ + "/" + name;
  Saved(path);
  Refresh();
  selected_ = path;
  OpenEditor(path, TileKind::kTable);
  SetStatus("Made " + path);
}

void ContentBrowserWindow::CreateLanguage(const std::string& name, const std::string& language) {
  const ModInfo* mod = ModOf(ask_target_);
  std::string error;
  if (!mod || !content::CreateLanguage(*mod, ask_target_, name, language, &error)) {
    SetStatus(error, true);
    return;
  }
  const std::string path = ask_target_ + "/" + name;
  Saved(path);
  Refresh();
  selected_ = path;
  OpenEditor(path, TileKind::kLanguage);
  SetStatus("Made " + path);
}

void ContentBrowserWindow::CreateMaterial(const std::string& name, const std::string& type) {
  const ModInfo* mod = ModOf(ask_target_);
  std::string error;
  if (!mod || !content::CreateMaterial(*mod, ask_target_, name, type, &error)) {
    SetStatus(error, true);
    return;
  }
  const std::string path = ask_target_ + "/" + name;
  Saved(path);
  Refresh();
  selected_ = path;
  OpenEditor(path, TileKind::kMaterial);
  SetStatus("Made " + path);
}

void ContentBrowserWindow::CreateMapEdit(const std::string& name, const std::string& world) {
  const ModInfo* mod = ModOf(ask_target_);
  std::string error;
  if (!mod || !content::CreateMapEdit(*mod, ask_target_, name, world, &error)) {
    SetStatus(error, true);
    return;
  }
  const std::string path = ask_target_ + "/" + name;
  Saved(path);
  Refresh();
  selected_ = path;
  OpenEditor(path, TileKind::kMapEdit);
  SetStatus("Made " + path);
}

void ContentBrowserWindow::CreateWorld(const std::string& name, const std::string& base) {
  const ModInfo* mod = ModOf(ask_target_);
  std::string error;
  if (!mod || !content::CreateWorld(*mod, ask_target_, name, base, &error)) {
    SetStatus(error, true);
    return;
  }
  const std::string path = ask_target_ + "/" + name;
  Saved(path);
  Refresh();
  selected_ = path;
  OpenEditor(path, TileKind::kWorld);
  SetStatus("Made " + path);
}

void ContentBrowserWindow::CreatePacket(const std::string& name, const std::string& base) {
  const ModInfo* mod = ModOf(ask_target_);
  std::string error;
  if (!mod || !content::CreatePacket(*mod, ask_target_, name, base, &error)) {
    SetStatus(error, true);
    return;
  }
  const std::string path = ask_target_ + "/" + name;
  Saved(path);
  Refresh();
  selected_ = path;
  OpenEditor(path, TileKind::kPacket);
  SetStatus("Made " + path);
}

void ContentBrowserWindow::StartImport(const std::string& folder, bool mesh) {
  picking_ = true;
  file_picker::Choose(mesh ? file_picker::Files::kMeshes : file_picker::Files::kPictures, [picked = picked_, folder, mesh](std::optional<std::filesystem::path> file, std::string error) {
    std::lock_guard lock(picked->mutex);
    picked->done = true;
    picked->file = std::move(file);
    picked->error = std::move(error);
    picked->folder = folder;
    picked->mesh = mesh;
  });
}

void ContentBrowserWindow::TakePicked() {
  std::optional<std::filesystem::path> file;
  std::string error;
  std::string folder;
  bool mesh = false;
  {
    std::lock_guard lock(picked_->mutex);
    if (!picked_->done) return;
    picked_->done = false;
    file = std::move(picked_->file);
    error = std::move(picked_->error);
    folder = std::move(picked_->folder);
    mesh = picked_->mesh;
  }
  picking_ = false;
  if (!error.empty()) SetStatus(error, true);
  if (!file) return;
  // Named after its file, as far as the name rules let it be.
  std::string name;
  for (const char c : file->stem().string()) name += std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 0x80 ? c : '_';
  if (name.empty() || !std::isalpha(static_cast<unsigned char>(name.front()))) name = (mesh ? "Mesh_" : "Image_") + name;
  name.resize(std::min<size_t>(name.size(), 64));
  AskFor(mesh ? Ask::kImportMesh : Ask::kImportImage, folder, name);
  ask_parent_ = file->string();
}

void ContentBrowserWindow::ImportImage(const std::string& name, const std::string& source) {
  const ModInfo* mod = ModOf(ask_target_);
  std::string error;
  if (!mod || !content::CreateTexture(*mod, ask_target_, name, std::filesystem::path(source), &error)) {
    SetStatus(error, true);
    return;
  }
  const std::string path = ask_target_ + "/" + name;
  Saved(path);
  Refresh();
  selected_ = path;
  OpenEditor(path, TileKind::kTexture);
  SetStatus("Imported " + path);
}

void ContentBrowserWindow::ImportMesh(const std::string& name, const std::string& source) {
  const ModInfo* mod = ModOf(ask_target_);
  std::string error;
  if (!mod || !content::CreateMesh(*mod, ask_target_, name, std::filesystem::path(source), &error)) {
    SetStatus(error, true);
    return;
  }
  const std::string path = ask_target_ + "/" + name;
  Saved(path);
  Refresh();
  selected_ = path;
  OpenEditor(path, TileKind::kMesh);
  SetStatus("Imported " + path);
}

void ContentBrowserWindow::CreateType(const std::string& name, TileKind kind) {
  const ModInfo* mod = ModOf(ask_target_);
  std::string error;
  const bool made = mod && (kind == TileKind::kEnum ? content::CreateEnum(*mod, ask_target_, name, &error) : content::CreateStruct(*mod, ask_target_, name, &error));
  if (!made) {
    SetStatus(error, true);
    return;
  }
  const std::string path = ask_target_ + "/" + name;
  Saved(path);
  Refresh();
  selected_ = path;
  OpenEditor(path, kind);
  SetStatus("Made " + path);
}

bool ContentBrowserWindow::Busy(const std::string& path, const std::vector<content::Reference>& references) {
  for (const auto& editor : editors_) {
    if (!editor->dirty()) continue;
    const bool touched = Within(editor->path(), path) || std::any_of(references.begin(), references.end(), [&editor](const content::Reference& reference) { return Same(reference.from, editor->path()); });
    if (touched) {
      SetStatus("Save or close " + editor->path() + " first, as this changes it", true);
      editor->Focus();
      return true;
    }
  }
  return false;
}

void ContentBrowserWindow::Relocated(const std::string& from, const std::string& to) {
  std::vector<std::string> reopen;
  for (auto it = editors_.begin(); it != editors_.end();) {
    // A metadata editor's path is a mod's, never inside an asset or folder.
    if (Within((*it)->path(), from) && (*it)->path().find('/') != std::string::npos) {
      if (!to.empty()) reopen.push_back(to + (*it)->path().substr(from.size()));
      it = editors_.erase(it);
    } else {
      ++it;
    }
  }
  Saved(to);
  Refresh();
  for (const std::string& path : reopen) OpenEditor(path, KindOf(path));
}

void ContentBrowserWindow::RenameTarget(const std::string& name) {
  const std::string from = ask_target_;
  if (name == content::NameOf(from) || Busy(from, ask_references_)) return;
  std::string error;
  if (!content::Rename(model_.mods(), from, name, &error)) {
    SetStatus(error, true);
    return;
  }
  const std::string to = std::string(content::FolderOf(from)) + "/" + name;
  Relocated(from, to);
  if (Same(folder_, from) || Within(folder_, from)) Navigate(to + folder_.substr(from.size()));
  selected_ = to;
  SetStatus(error.empty() ? "Renamed to " + to : error, !error.empty());
}

void ContentBrowserWindow::MoveTo(const std::string& path, const std::string& folder) {
  if (Same(content::FolderOf(path), folder) || Busy(path, content::ReferencesTo(model_.mods(), path))) return;
  std::string error;
  if (!content::Move(model_.mods(), path, folder, &error)) {
    SetStatus(error, true);
    return;
  }
  const std::string to = folder + "/" + std::string(content::NameOf(path));
  Relocated(path, to);
  selected_ = to;
  SetStatus(error.empty() ? "Moved to " + to : error, !error.empty());
}

void ContentBrowserWindow::DuplicateAsset(const std::string& path) {
  const ModInfo* mod = ModOf(path);
  std::string made;
  std::string error;
  if (!mod || !content::Duplicate(*mod, path, &made, &error)) {
    SetStatus(error, true);
    return;
  }
  Saved(made);
  Refresh();
  selected_ = made;
  SetStatus("Made " + made);
}

void ContentBrowserWindow::DeleteTarget() {
  const std::string path = ask_target_;
  const ModInfo* mod = ModOf(path);
  if (!mod || Busy(path, {})) return;
  std::string error;
  if (!content::Delete(*mod, path, &error)) {
    SetStatus(error, true);
    return;
  }
  Relocated(path, {});
  SetStatus("Deleted " + path);
}

void ContentBrowserWindow::SetStatus(std::string text, bool error) {
  status_ = std::move(text);
  status_error_ = error;
  status_time_ = ImGui::GetTime();
}

}  // namespace rexfabric::ui
