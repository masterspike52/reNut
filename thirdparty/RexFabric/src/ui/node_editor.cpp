#include "node_editor.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>

#include <imgui.h>
#include <imgui_node_editor.h>

#include <rex/hid/input_system.h>
#include <rexfabric/rexfabric.h>

#include "../graph/graph_file.h"
#include "../graph/node_library.h"
#include "../lua/lua_scripts.h"
#include "glass.h"
#include "node_graph_widgets.h"

namespace ed = ax::NodeEditor;

namespace rexfabric::ui {
namespace {

using graph::Node;
using graph::NodeDesc;
using graph::NodePins;
using graph::NodeRole;
using graph::Pin;
using graph::PinKind;
using graph::PinRef;
using graph::PinType;
using graph::Routine;
using graph::RoutineKind;
using node_widgets::PinShape;

// In multiples of the font size, so the layout follows the UI's scale.
constexpr float kSidebarWidth = 15.0f;
constexpr float kDetailsWidth = 20.0f;
constexpr float kInlineNumberWidth = 5.0f;
constexpr float kInlineFloatWidth = 4.0f;
constexpr float kInlineTextWidth = 7.0f;
// A reroute's body, which moves it, and the grip on either side of it that a link is dragged from or dropped on.
// The dot drawn is smaller than the body, so it's easy to pick up.
constexpr float kRerouteDot = 2.0f;
constexpr float kRerouteGrip = 0.6f;
constexpr float kRerouteDrawn = 0.27f;
// The room left around a graph fitted to the view.
constexpr float kFitMargin = 3.0f;

// Canvas ids: nodes are the graph's own ids; pins and links are moved out of their way.
constexpr uint64_t kPinBase = uint64_t(1) << 40;
constexpr uint64_t kLinkBase = uint64_t(2) << 40;
constexpr uint64_t kPinsPerNode = 128;
constexpr uint64_t kOutputOffset = 64;

constexpr float kLinkThickness = 2.0f;
// Exec links pulse with a flow of light this often, like a heartbeat; the links of the node being worked on more
// often.
constexpr float kExecPulseSeconds = 2.4f;
constexpr float kActivePulseSeconds = 1.2f;
// How long a status message stays.
constexpr double kStatusSeconds = 5.0;
// How far a duplicate lands from the original, in canvas units.
constexpr float kDuplicateOffset = 40.0f;

constexpr ImVec4 kAcceptColor{0.4f, 1.0f, 0.4f, 1.0f};
constexpr ImVec4 kRejectColor{1.0f, 0.3f, 0.3f, 1.0f};
constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};
constexpr ImVec4 kWarningColor{1.0f, 0.75f, 0.35f, 1.0f};
constexpr ImVec4 kGoodColor{0.5f, 0.9f, 0.5f, 1.0f};
constexpr ImVec4 kTitleColor{1.0f, 1.0f, 1.0f, 0.95f};
constexpr ImVec4 kFunctionColor{0.45f, 0.62f, 1.0f, 1.0f};
constexpr ImVec4 kMacroColor{0.72f, 0.5f, 1.0f, 1.0f};
// The glass of the panels beside the canvas.
constexpr ImVec4 kPaneTint{0.05f, 0.06f, 0.11f, 0.42f};

constexpr const char* kAddMenu = "AddNode";
constexpr const char* kDropMenu = "DropVariable";
constexpr const char* kNodeMenu = "NodeMenu";
constexpr const char* kChoiceMenu = "PinChoices";
// The pin's dropdown is wider than typed text, as enum names are long; its list, in font sizes.
constexpr float kInlineChoiceWidth = 12.0f;
constexpr float kChoiceListWidth = 24.0f;
constexpr float kChoiceListHeight = 20.0f;
// What the sidebar's rows carry when dragged onto the canvas: a variable's name, a function's or macro's id.
constexpr const char* kVariablePayload = "REXFABRIC_VARIABLE";
constexpr const char* kRoutinePayload = "REXFABRIC_ROUTINE";
constexpr const char* kDispatcherPayload = "REXFABRIC_DISPATCHER";

// What a variable or dispatcher dropped on the canvas can become, in the order the menu offers them.
constexpr std::array<const char*, 5> kVariableNodes = {"var.get", "var.set", "var.branch", "var.increment", "var.decrement"};
constexpr std::array<const char*, 5> kDispatcherNodes = {"dispatch.call", "dispatch.bind", "dispatch.unbind", "dispatch.unbind_all", "event.custom"};
constexpr const char* kAskPopup = "###ask";
constexpr const char* kSavePopup = "Unsaved changes###save";

constexpr std::array<PinKind, 5> kVariableKinds = {PinKind::kBool, PinKind::kInt, PinKind::kFloat, PinKind::kString, PinKind::kAny};

ed::NodeId NodeId(int node) { return ed::NodeId(static_cast<size_t>(node)); }
ed::PinId PinId(int node, size_t index, bool output) { return ed::PinId(static_cast<size_t>(kPinBase + static_cast<uint64_t>(node) * kPinsPerNode + (output ? kOutputOffset : 0) + index)); }
ed::LinkId LinkId(int link) { return ed::LinkId(static_cast<size_t>(kLinkBase + static_cast<uint64_t>(link))); }

struct DecodedPin {
  int node = 0;
  size_t index = 0;
  bool output = false;
};

std::optional<DecodedPin> DecodePin(ed::PinId id) {
  const uint64_t value = id.Get();
  if (value < kPinBase || value >= kLinkBase) return std::nullopt;
  const uint64_t local = value - kPinBase;
  const uint64_t slot = local % kPinsPerNode;
  const bool output = slot >= kOutputOffset;
  return DecodedPin{static_cast<int>(local / kPinsPerNode), static_cast<size_t>(output ? slot - kOutputOffset : slot), output};
}

rex::hid::IInputSystem* GuestInput() { return REX_INTERFACE(rex::hid::IInputSystem); }

ImU32 PinColor(PinKind kind) {
  switch (kind) {
    case PinKind::kExec: return IM_COL32(240, 240, 240, 255);
    case PinKind::kBool: return IM_COL32(220, 70, 70, 255);
    case PinKind::kInt: return IM_COL32(80, 220, 190, 255);
    case PinKind::kFloat: return IM_COL32(140, 230, 100, 255);
    case PinKind::kNumber: return IM_COL32(200, 230, 120, 255);
    case PinKind::kString: return IM_COL32(250, 110, 200, 255);
    case PinKind::kObject: return IM_COL32(80, 160, 255, 255);
    case PinKind::kRegisters: return IM_COL32(255, 170, 60, 255);
    case PinKind::kDelegate: return IM_COL32(255, 70, 70, 255);
    case PinKind::kAny: return IM_COL32(190, 190, 190, 255);
  }
  return IM_COL32(190, 190, 190, 255);
}

bool IsRoutineNode(const Node& node) { return node.type == graph::kRoutineEntry || node.type == graph::kRoutineExit || node.type == graph::kRoutineCall; }

// A function's or macro's inputs and outputs, which its graph always keeps.
bool IsRoutineEnd(const Node& node) { return node.type == graph::kRoutineEntry || node.type == graph::kRoutineExit; }

ImVec4 RoutineColor(RoutineKind kind) { return kind == RoutineKind::kMacro ? kMacroColor : kFunctionColor; }

// Pure nodes take the color of what they give, so a node and its links match; the rest go by what they're about.
ImVec4 HeaderColor(const Node& node, const NodeDesc* desc, const NodePins& pins, const graph::Graph& graph) {
  if (!desc) return kRejectColor;
  if (IsRoutineNode(node)) {
    if (const Routine* routine = graph::RoutineOf(node, graph)) return RoutineColor(routine->kind);
  }
  const NodeRole role = graph::RoleOf(node, graph);
  if (role == NodeRole::kEvent) return ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
  if (role == NodeRole::kPure && !pins.outputs.empty()) return ImGui::ColorConvertU32ToFloat4(PinColor(pins.outputs.front().type.kind));
  const std::string_view category = desc->category;
  if (category.starts_with("Flow")) return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
  if (category.starts_with("Hooks")) return ImVec4(1.0f, 0.65f, 0.3f, 1.0f);
  if (category.starts_with("Memory")) return ImVec4(0.75f, 0.55f, 1.0f, 1.0f);
  if (category.starts_with("Variables")) return ImVec4(0.5f, 0.8f, 1.0f, 1.0f);
  if (category.starts_with("Game")) return ImVec4(0.35f, 0.85f, 0.75f, 1.0f);
  return ImVec4(0.55f, 0.7f, 1.0f, 1.0f);
}

// Exec pins named like the usual ones need no label.
std::string_view PinLabel(const Pin& pin) {
  if (pin.type.kind == PinKind::kExec && (pin.name == graph::kExecIn || pin.name == graph::kExecOut)) return {};
  return pin.name;
}

PinShape ShapeOf(const Pin& pin) {
  if (pin.type.kind == PinKind::kExec) return PinShape::kArrow;
  return pin.type.kind == PinKind::kDelegate ? PinShape::kSquare : PinShape::kCircle;
}

// Whether an output of `from` could go into `to` once its node is linked: a Number becomes whatever its inputs are.
bool MayFit(const PinType& from, const PinType& to) {
  if (from.kind == PinKind::kNumber) return to.kind == PinKind::kInt || to.kind == PinKind::kFloat || to.kind == PinKind::kNumber || to.kind == PinKind::kAny;
  return graph::Fits(from, to);
}

// A node of this type as the add menu would make it, to see its pins before it's added.
Node Prototype(const NodeDesc& desc, const decltype(Node::settings)& settings) {
  Node node;
  node.type = desc.id;
  for (const graph::SettingDesc& entry : desc.settings) node.settings[entry.name] = entry.initial;
  for (const auto& [name, value] : settings) node.settings[name] = value;
  return node;
}

// The setting of a node's description that picks one of the graph's variables or dispatchers, or null.
const graph::SettingDesc* MemberSetting(const NodeDesc& desc) {
  const auto found = std::find_if(desc.settings.begin(), desc.settings.end(), [](const graph::SettingDesc& setting) { return setting.kind == graph::SettingKind::kVariable || setting.kind == graph::SettingKind::kDispatcher; });
  return found != desc.settings.end() ? &*found : nullptr;
}

// What a menu calls a node picking `member`: "Get Count", "Call Scored", or "Custom Event (Scored)" for a setting
// that only borrows from it.
std::string MemberLabel(const NodeDesc& desc, const graph::SettingDesc& setting, const std::string& member) { return setting.optional ? desc.title + " (" + member + ")" : desc.title + " " + member; }

const char* KindLabel(PinKind kind) {
  switch (kind) {
    case PinKind::kExec: return "Exec";
    case PinKind::kDelegate: return "Event";
    case PinKind::kBool: return "Bool";
    case PinKind::kInt: return "Int";
    case PinKind::kFloat: return "Float";
    case PinKind::kString: return "String";
    default: return "Any";
  }
}

// The mod and graph last open, so the window opens again where it was.
struct LastOpen {
  std::string mod;
  std::string graph;
};

LastOpen& Last() {
  static LastOpen last;
  return last;
}

// The SDK's ImGui drawer reports Ctrl, Shift and Alt as keys but not as ImGui's modifiers, which ImGui rebuilds
// io.KeyCtrl and the rest from each frame, so its shortcuts never see them. While the editor is open the modifiers
// follow their keys, a frame behind, so Ctrl+S, the canvas's Ctrl+C and text fields' shortcuts work.
void SyncModifiers(ImGuiIO& io) {
  constexpr std::array<std::array<ImGuiKey, 3>, 3> kModifiers = {{
      {ImGuiMod_Ctrl, ImGuiKey_LeftCtrl, ImGuiKey_RightCtrl},
      {ImGuiMod_Shift, ImGuiKey_LeftShift, ImGuiKey_RightShift},
      {ImGuiMod_Alt, ImGuiKey_LeftAlt, ImGuiKey_RightAlt},
  }};
  for (const auto& [modifier, left, right] : kModifiers) {
    const bool held = ImGui::IsKeyDown(left) || ImGui::IsKeyDown(right);
    if (held != ImGui::IsKeyDown(modifier)) io.AddKeyEvent(modifier, held);
  }
}

// The small swatch of color a sidebar row starts with: a variable's type, a function or macro.
void Swatch(ImU32 color) {
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float size = ImGui::GetTextLineHeight() * 0.6f;
  ImGui::Dummy(ImVec2(size, ImGui::GetTextLineHeight()));
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(start.x, start.y + size * 0.33f), ImVec2(start.x + size, start.y + size * 1.33f), color, 2.0f);
  ImGui::SameLine();
}

// A section of the sidebar: its title, with a round + at its right that adds to it. True when the + is pressed.
bool SectionHeader(const char* title, const char* add_tooltip, bool can_add) {
  ImGui::Spacing();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(title);
  const float button = ImGui::GetFrameHeight();
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(ImGui::GetContentRegionAvail().x - button, 0.0f));
  ImGui::PushID(title);
  ImGui::BeginDisabled(!can_add);
  const bool add = glass::Button("##add", glass::kCreate, ImVec2(button, button));
  // The + is drawn rather than typed, as a font's + doesn't sit in the middle of its box. Whole pixels and an even
  // thickness, so both bars are the same and crisp.
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  const ImVec2 center(std::floor((min.x + max.x) * 0.5f), std::floor((min.y + max.y) * 0.5f));
  const float arm = std::floor(button * 0.24f);
  const float half = std::max(1.0f, std::floor(button * 0.05f));
  const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(center.x - arm, center.y - half), ImVec2(center.x + arm, center.y + half), color);
  draw->AddRectFilled(ImVec2(center.x - half, center.y - arm), ImVec2(center.x + half, center.y + arm), color);
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("%s", add_tooltip);
  ImGui::PopID();
  ImGui::Separator();
  return add;
}

// "Value", "Value1": a name none of `params` has.
std::string UniqueParamName(const std::vector<graph::Param>& params, const std::string& base) {
  const auto taken = [&params](const std::string& name) { return std::any_of(params.begin(), params.end(), [&](const graph::Param& param) { return param.name == name; }); };
  std::string name = base;
  for (int suffix = 1; taken(name); ++suffix) name = base + std::to_string(suffix);
  return name;
}

// A name a function, macro, variable or dispatcher can take: free, and a Lua name that hides nothing.
bool CheckName(const graph::Graph& graph, const std::string& name, std::string* reason) {
  if (graph.NameTaken(name)) {
    *reason = "Something in this graph is named " + name + " already";
    return false;
  }
  return graph::IsVariableName(name, graph::GameGlobals(), reason);
}

bool TextValue(const nlohmann::json& value, std::string& text) {
  if (value.is_string()) text = value.get<std::string>();
  else if (value.is_number()) text = value.dump();
  else text.clear();
  return true;
}

}  // namespace

NodeEditorWindow::NodeEditorWindow(MenuModel& model, std::function<void()> closed) : model_(model), closed_(std::move(closed)) {
  if (auto* input = GuestInput()) input->AcquireUIBlock();
  // Where the window was last time, or else the first mod with graphs, as that's likely the one being worked on.
  const LastOpen last = Last();
  if (model_.mods().Find(last.mod)) {
    OpenMod(last.mod);
    if (std::find(graphs_.begin(), graphs_.end(), last.graph) != graphs_.end()) OpenGraph(last.graph);
    return;
  }
  const std::vector<ModInfo>& mods = model_.mods().mods();
  const auto with_graphs = std::find_if(mods.begin(), mods.end(), [](const ModInfo& mod) { return !graph::ListGraphs(mod.dir).empty(); });
  if (with_graphs != mods.end()) OpenMod(with_graphs->folder);
  else if (!mods.empty()) OpenMod(mods.front().folder);
}

NodeEditorWindow::~NodeEditorWindow() {
  if (canvas_) ed::DestroyEditor(canvas_);
  if (auto* input = GuestInput()) input->ReleaseUIBlock();
  if (closed_) closed_();
}

void NodeEditorWindow::Hide() {
  AfterSaving([this] { closing_ = true; });
}

bool NodeEditorWindow::Draw(ImGuiIO& io) {
  if (closing_) return false;
  OnDraw(io);
  return !closing_;
}

void NodeEditorWindow::OnDraw(ImGuiIO& io) {
  SyncModifiers(io);
  model_.Poll();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(1320.0f, 780.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(820.0f, 480.0f), ImVec2(FLT_MAX, FLT_MAX));
  // Glass all over when there's glass: the theme, the window, and the canvas's light behind everything in it.
  const glass::Theme theme;
  bool open = true;
  const std::string title = document_ ? "Node Editor: " + document_->name() + (document_->dirty() ? " *" : "") + "###rexfabric_node_editor" : std::string("Node Editor###rexfabric_node_editor");
  if (glass::BeginWindow(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    // Under the title bar, to the window's rounded bottom.
    const ImVec2 window_min = ImGui::GetWindowPos();
    const ImVec2 window_max(window_min.x + ImGui::GetWindowSize().x, window_min.y + ImGui::GetWindowSize().y);
    style_.DrawBackdrop(ImVec2(window_min.x, window_min.y + ImGui::GetFrameHeight()), window_max, ImGui::GetStyle().WindowRounding);

    Shortcuts();
    DrawToolbar();
    ImGui::Spacing();

    const float font = ImGui::GetFontSize();
    glass::BeginChild("##left", ImVec2(font * kSidebarWidth, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_AlwaysUseWindowPadding, 0, kPaneTint);
    DrawGraphs();
    DrawRoutines(RoutineKind::kFunction);
    DrawRoutines(RoutineKind::kMacro);
    DrawVariables();
    DrawDispatchers();
    ImGui::EndChild();

    ImGui::SameLine();
    const float middle = std::max(ImGui::GetContentRegionAvail().x - font * kDetailsWidth - ImGui::GetStyle().ItemSpacing.x, font * 10.0f);
    ImGui::BeginChild("##middle", ImVec2(middle, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (!document_) {
      ImGui::TextDisabled(mod_folder_.empty() ? "Pick a mod, or make a new one, to start." : "Make a graph for this mod to start.");
    } else if (ImGui::BeginTabBar("##views")) {
      if (ImGui::BeginTabItem("Graph")) {
        DrawBreadcrumb();
        DrawCanvas();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Lua")) {
        DrawLua();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    glass::BeginChild("##details", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding, 0, kPaneTint);
    DrawDetails();
    ImGui::EndChild();

    DrawPopups();
  }
  ImGui::End();
  if (!open) Hide();
}

// Top ---------------------------------------------------------------------------------------------------

void NodeEditorWindow::DrawToolbar() {
  DrawModPicker();

  ImGui::SameLine();
  ImGui::BeginDisabled(!document_);
  if (glass::Button("Save", glass::kAccent)) Save(false);
  ImGui::SetItemTooltip("Writes the graph and its script (Ctrl+S)");
  ImGui::SameLine();
  if (glass::Button("Save and Reload", glass::kCreate)) Save(true);
  ImGui::SetItemTooltip("Saves, then runs every enabled Lua mod again, this one included");
  ImGui::SameLine();
  ImGui::BeginDisabled(!document_ || !document_->CanUndo());
  if (glass::Button("Undo")) Undo();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!document_ || !document_->CanRedo());
  if (glass::Button("Redo")) Redo();
  ImGui::EndDisabled();
  ImGui::EndDisabled();

  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  if (!status_.empty() && ImGui::GetTime() - status_time_ < kStatusSeconds) {
    ImGui::TextColored(status_error_ ? kErrorColor : kGoodColor, "%s", status_.c_str());
  } else if (document_) {
    const auto& problems = document_->compiled().problems;
    const size_t errors = static_cast<size_t>(std::count_if(problems.begin(), problems.end(), [](const graph::Problem& problem) { return !problem.warning; }));
    if (errors) ImGui::TextColored(kErrorColor, "%zu problem%s; it won't run until they're fixed", errors, errors == 1 ? "" : "s");
    else if (!problems.empty()) ImGui::TextColored(kWarningColor, "%zu warning%s", problems.size(), problems.size() == 1 ? "" : "s");
    else ImGui::TextColored(kGoodColor, "No problems");
  }
}

void NodeEditorWindow::DrawModPicker() {
  const ModInfo* mod = Mod();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Mod");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
  if (!ImGui::BeginCombo("##mod", mod ? mod->name.c_str() : "Pick a mod")) return;
  for (const ModInfo& entry : model_.mods().mods()) {
    const std::string label = entry.name + (entry.enabled ? "" : "  (off)") + "##" + entry.folder;
    if (ImGui::Selectable(label.c_str(), entry.folder == mod_folder_) && entry.folder != mod_folder_) {
      const std::string folder = entry.folder;
      AfterSaving([this, folder] { OpenMod(folder); });
    }
  }
  ImGui::Separator();
  if (ImGui::Selectable("New Mod...")) AskFor(Ask::kNewMod, "");
  ImGui::EndCombo();
}

void NodeEditorWindow::Shortcuts() {
  if (!document_ || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
  const ImGuiIO& io = ImGui::GetIO();
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) Save(false);
  // A text field has undo of its own.
  if (io.WantTextInput) return;
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? Redo() : Undo();
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
}

// Left --------------------------------------------------------------------------------------------------

void NodeEditorWindow::DrawGraphs() {
  const std::string next = graphs_.empty() ? "Main" : "Graph" + std::to_string(graphs_.size() + 1);
  if (SectionHeader("Graphs", "A new graph: a script of its own in this mod", Mod() != nullptr)) AskFor(Ask::kNewGraph, next);

  for (const std::string& name : graphs_) {
    const bool open = document_ && document_->name() == name;
    if (ImGui::Selectable((name + "##graph").c_str(), open)) {
      if (open) ShowGraph(0);
      else AfterSaving([this, name] { OpenGraph(name); });
    }
    ImGui::SetItemTooltip("Right click to rename or delete");
    if (open && ImGui::BeginPopupContextItem()) {
      if (ImGui::MenuItem("Rename...")) AskFor(Ask::kRenameGraph, name);
      if (ImGui::MenuItem("Delete...")) AskFor(Ask::kDeleteGraph, name);
      ImGui::EndPopup();
    }
    // The open graph's own event graph, under it, as its functions and macros are below.
    if (open) {
      ImGui::Indent();
      if (ImGui::Selectable("Event Graph", owner_ == 0)) ShowGraph(0);
      ImGui::Unindent();
    }
  }
  if (Mod() && graphs_.empty()) ImGui::TextDisabled("None yet");
}

void NodeEditorWindow::DrawRoutines(RoutineKind kind) {
  const bool macro = kind == RoutineKind::kMacro;
  const char* add_tooltip = macro ? "A new macro: nodes written out wherever it's used, with exec pins of its own if it wants" : "A new function: a graph of its own, called from the event graph or other functions";
  if (SectionHeader(macro ? "Macros" : "Functions", add_tooltip, document_ != nullptr)) {
    graph::Graph& graph = document_->graph();
    Routine& made = graph::CreateRoutine(graph, kind, graph.UniqueRoutineName(macro ? "NewMacro" : "NewFunction"));
    // A macro starts as flow: in through one exec pin and out through another.
    if (macro) {
      made.inputs.push_back({"In", PinKind::kExec});
      made.outputs.push_back({"Out", PinKind::kExec});
    }
    selected_routine_ = made.id;
    focus_ = Focus::kRoutine;
    ShowGraph(made.id);
    Edited();
  }
  if (!document_) return;
  graph::Graph& graph = document_->graph();

  int removed = 0;
  bool any = false;
  for (const Routine& routine : graph.routines()) {
    if (routine.kind != kind) continue;
    any = true;
    ImGui::PushID(routine.id);
    Swatch(ImGui::ColorConvertFloat4ToU32(RoutineColor(kind)));
    if (ImGui::Selectable(routine.name.c_str(), owner_ == routine.id || (focus_ == Focus::kRoutine && selected_routine_ == routine.id))) {
      selected_routine_ = routine.id;
      focus_ = Focus::kRoutine;
      ShowGraph(routine.id);
    }
    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload(kRoutinePayload, &routine.id, sizeof(routine.id));
      ImGui::Text("Use %s", routine.name.c_str());
      ImGui::EndDragDropSource();
    }
    ImGui::SetItemTooltip("%s%sDrag onto a graph to use it", routine.comment.c_str(), routine.comment.empty() ? "" : "\n");
    if (ImGui::BeginPopupContextItem()) {
      if (ImGui::MenuItem("Delete")) removed = routine.id;
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  if (!any) ImGui::TextDisabled("None yet");
  if (removed) {
    graph::DeleteRoutine(graph, removed);
    if (owner_ == removed) ShowGraph(0);
    if (selected_routine_ == removed) focus_ = Focus::kNone;
    Edited();
  }
}

void NodeEditorWindow::DrawVariables() {
  if (SectionHeader("Variables", "A new variable: a value kept between events", document_ != nullptr)) {
    graph::Graph& graph = document_->graph();
    graph::Variable variable;
    variable.name = graph.UniqueVariableName("NewVar");
    variable.type = PinKind::kFloat;
    variable.initial = graph::DefaultValue(PinKind::kFloat);
    graph.variables().push_back(variable);
    selected_variable_ = variable.name;
    focus_ = Focus::kVariable;
    Edited();
  }
  if (!document_) return;
  graph::Graph& graph = document_->graph();

  std::string removed;
  for (const graph::Variable& variable : graph.variables()) {
    ImGui::PushID(variable.name.c_str());
    Swatch(PinColor(variable.type));
    if (ImGui::Selectable(variable.name.c_str(), focus_ == Focus::kVariable && selected_variable_ == variable.name)) {
      selected_variable_ = variable.name;
      focus_ = Focus::kVariable;
    }
    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload(kVariablePayload, variable.name.data(), variable.name.size());
      ImGui::Text("Get or set %s", variable.name.c_str());
      ImGui::EndDragDropSource();
    }
    ImGui::SetItemTooltip("%s%s%s\nDrag onto the graph to get or set it", KindLabel(variable.type), variable.comment.empty() ? "" : "\n", variable.comment.c_str());
    if (ImGui::BeginPopupContextItem()) {
      if (ImGui::MenuItem("Delete")) removed = variable.name;
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  if (graph.variables().empty()) ImGui::TextDisabled("None yet");
  if (!removed.empty()) {
    // The nodes using it go with it.
    graph::DeleteVariable(graph, removed);
    if (selected_variable_ == removed) focus_ = Focus::kNone;
    Edited();
  }
}

// Middle: the canvas -------------------------------------------------------------------------------------

void NodeEditorWindow::DrawDispatchers() {
  if (SectionHeader("Event Dispatchers", "A new event dispatcher: events bound to it run when it's called", document_ != nullptr)) {
    graph::Graph& graph = document_->graph();
    graph::Dispatcher dispatcher;
    dispatcher.name = graph.UniqueDispatcherName("NewDispatcher");
    graph.dispatchers().push_back(dispatcher);
    selected_dispatcher_ = dispatcher.name;
    focus_ = Focus::kDispatcher;
    Edited();
  }
  if (!document_) return;
  graph::Graph& graph = document_->graph();

  std::string removed;
  for (const graph::Dispatcher& dispatcher : graph.dispatchers()) {
    ImGui::PushID(dispatcher.name.c_str());
    Swatch(PinColor(PinKind::kDelegate));
    if (ImGui::Selectable(dispatcher.name.c_str(), focus_ == Focus::kDispatcher && selected_dispatcher_ == dispatcher.name)) {
      selected_dispatcher_ = dispatcher.name;
      focus_ = Focus::kDispatcher;
    }
    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload(kDispatcherPayload, dispatcher.name.data(), dispatcher.name.size());
      ImGui::Text("Call or bind to %s", dispatcher.name.c_str());
      ImGui::EndDragDropSource();
    }
    ImGui::SetItemTooltip("%s%sDrag onto the graph to call it, or bind an event to it", dispatcher.comment.c_str(), dispatcher.comment.empty() ? "" : "\n");
    if (ImGui::BeginPopupContextItem()) {
      if (ImGui::MenuItem("Delete")) removed = dispatcher.name;
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  if (graph.dispatchers().empty()) ImGui::TextDisabled("None yet");
  if (!removed.empty()) {
    graph::DeleteDispatcher(graph, removed);
    if (selected_dispatcher_ == removed) focus_ = Focus::kNone;
    Edited();
  }
}

void NodeEditorWindow::DrawBreadcrumb() {
  const Routine* routine = document_->graph().FindRoutine(owner_);
  if (glass::SmallButton("Event Graph", routine ? glass::kNeutral : glass::kAccent)) ShowGraph(0);
  if (!routine) return;
  ImGui::SameLine();
  ImGui::TextDisabled(">");
  ImGui::SameLine();
  ImGui::TextColored(RoutineColor(routine->kind), "%s %s", routine->kind == RoutineKind::kMacro ? "Macro" : "Function", routine->name.c_str());
}

void NodeEditorWindow::ShowGraph(int owner) {
  if (owner == owner_) return;
  owner_ = owner;
  if (focus_ == Focus::kNode) focus_ = Focus::kNone;
  selected_node_ = 0;
  pending_pin_.reset();
  place_nodes_ = true;
  fit_view_ = true;
  if (canvas_) {
    ed::SetCurrentEditor(canvas_);
    ed::ClearSelection();
    ed::SetCurrentEditor(nullptr);
  }
}

void NodeEditorWindow::DrawCanvas() {
  graph::Graph& graph = document_->graph();
  problem_nodes_.clear();
  for (const graph::Problem& problem : document_->compiled().problems) {
    if (!problem.warning) problem_nodes_.insert(problem.node);
  }

  const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
  pin_tooltip_.clear();
  ed::SetCurrentEditor(canvas_);
  ed::Begin("##graph");

  // The graph shown: the event graph's nodes, or one function's or macro's.
  for (Node& node : graph.nodes()) {
    if (node.owner != owner_) continue;
    if (place_nodes_) ed::SetNodePosition(NodeId(node.id), ImVec2(node.x, node.y));
    DrawNode(node);
  }
  place_nodes_ = false;

  for (const graph::Link& link : graph.links()) {
    const Node* source = graph.FindNode(link.from.node);
    const Node* target = graph.FindNode(link.to.node);
    if (!source || !target || source->owner != owner_) continue;
    const NodePins source_pins = graph::PinsOf(*source, graph);
    const NodePins target_pins = graph::PinsOf(*target, graph);
    const auto from = std::find_if(source_pins.outputs.begin(), source_pins.outputs.end(), [&](const Pin& pin) { return pin.name == link.from.pin; });
    const auto to = std::find_if(target_pins.inputs.begin(), target_pins.inputs.end(), [&](const Pin& pin) { return pin.name == link.to.pin; });
    if (from == source_pins.outputs.end() || to == target_pins.inputs.end()) continue;

    const PinType carried = graph::OutputType(graph, link.from);
    const bool fits = graph::Fits(carried, to->type);
    const ImVec4 color = fits ? ImGui::ColorConvertU32ToFloat4(PinColor(carried.kind)) : kRejectColor;
    ed::Link(LinkId(link.id), PinId(link.from.node, static_cast<size_t>(from - source_pins.outputs.begin()), true), PinId(link.to.node, static_cast<size_t>(to - target_pins.inputs.begin()), false), color, kLinkThickness);

    // Light flows along exec links now and then, and along every link of the node being worked on.
    const bool exec = from->type.kind == PinKind::kExec;
    const bool active = style_.IsNodeActive(link.from.node) || style_.IsNodeActive(link.to.node);
    if ((exec && style_.Pulse(kExecPulseSeconds)) || (active && style_.Pulse(kActivePulseSeconds))) ed::Flow(LinkId(link.id));
  }

  HandleCreate();
  HandleDoubleClick();
  HandleDelete();
  HandleClipboard();

  if (navigate_to_) {
    ed::ClearSelection();
    ed::SelectNode(NodeId(navigate_to_));
    ed::NavigateToSelection(false, 0.3f);
    navigate_to_ = 0;
    // Going to a node in another graph shows that graph at the node, not fitted.
    fit_view_ = false;
  }

  // Popups and tooltips draw in screen space, outside the canvas transform.
  ed::Suspend();
  if (!pin_tooltip_.empty() && !ImGui::IsPopupOpen(kAddMenu)) ImGui::SetTooltip("%s", pin_tooltip_.c_str());
  ed::NodeId context_node;
  if (ed::ShowNodeContextMenu(&context_node)) {
    context_node_ = static_cast<int>(context_node.Get());
    open_node_menu_ = true;
  } else if (ed::ShowBackgroundContextMenu()) {
    pending_pin_.reset();
    open_add_menu_ = true;
  }
  if (open_node_menu_) {
    open_node_menu_ = false;
    ImGui::OpenPopup(kNodeMenu);
  }
  DrawNodeMenu();
  if (open_drop_menu_) {
    open_drop_menu_ = false;
    ImGui::OpenPopup(kDropMenu);
  }
  DrawDropMenu();
  if (open_add_menu_) {
    open_add_menu_ = false;
    const ImVec2 position = ed::ScreenToCanvas(ImGui::GetMousePos());
    add_x_ = position.x;
    add_y_ = position.y;
    add_filter_.clear();
    ImGui::OpenPopup(kAddMenu);
  }
  DrawAddMenu();
  if (open_choice_menu_) {
    open_choice_menu_ = false;
    choice_filter_.clear();
    ImGui::OpenPopup(kChoiceMenu);
  }
  DrawChoiceMenu();
  ed::Resume();

  SyncFromCanvas();
  if (fit_view_) {
    // The graph shown fits the view, zoomed out when it's big but never in past its size, so a small function's graph
    // isn't blown up to fill the canvas.
    FitView();
    fit_view_ = false;
  }
  ed::End();
  // The canvas is the last item now: variables, functions and macros dragged in from the sidebar land on it.
  if (ImGui::BeginDragDropTarget()) {
    AcceptDrop(ed::ScreenToCanvas(ImGui::GetMousePos()));
    ImGui::EndDragDropTarget();
  }
  ed::SetCurrentEditor(nullptr);

  const bool empty = std::none_of(graph.nodes().begin(), graph.nodes().end(), [this](const Node& node) { return node.owner == owner_; });
  if (empty) ImGui::GetWindowDrawList()->AddText(ImVec2(canvas_min.x + 16.0f, canvas_min.y + 12.0f), IM_COL32(255, 255, 255, 110), "Right click to add a node. Start with an event: On Frame, Key Pressed, Before Function.");
}

void NodeEditorWindow::FitView() {
  // The bounds of the graph shown, in canvas units, known once its nodes are drawn this frame.
  ImVec2 min(FLT_MAX, FLT_MAX);
  ImVec2 max(-FLT_MAX, -FLT_MAX);
  for (const Node& node : document_->graph().nodes()) {
    if (node.owner != owner_) continue;
    const ImVec2 position = ed::GetNodePosition(NodeId(node.id));
    const ImVec2 size = ed::GetNodeSize(NodeId(node.id));
    min = ImVec2(std::min(min.x, position.x), std::min(min.y, position.y));
    max = ImVec2(std::max(max.x, position.x + size.x), std::max(max.y, position.y + size.y));
  }
  const ImVec2 screen = ed::GetScreenSize();
  if (min.x > max.x || screen.x <= 0.0f || screen.y <= 0.0f) return;

  // With a margin, at zoom 1 when that fits, else zoomed out just enough; a canvas unit is a pixel at zoom 1.
  const float margin = ImGui::GetFontSize() * kFitMargin;
  const float scale = std::max({1.0f, (max.x - min.x + margin * 2.0f) / screen.x, (max.y - min.y + margin * 2.0f) / screen.y});
  const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
  const ImVec2 half(screen.x * scale * 0.5f, screen.y * scale * 0.5f);
  ed::SetViewState({ImVec2(center.x - half.x, center.y - half.y), ImVec2(center.x + half.x, center.y + half.y)});
}

void NodeEditorWindow::DrawNode(Node& node) {
  const graph::Graph& graph = document_->graph();
  const NodeDesc* desc = graph::FindDesc(node);
  const NodePins pins = graph::PinsOf(node, graph);
  if (node.type == graph::kReroute) {
    DrawReroute(node);
    return;
  }
  if (desc && desc->compact) {
    DrawCompactNode(node, pins);
    return;
  }

  style_.BeginNode(node.id);
  ed::BeginNode(NodeId(node.id));
  ImGui::PushID(node.id);

  const float content_x = ImGui::GetCursorPosX();
  ImGui::TextColored(kTitleColor, "%s", graph::TitleOf(node).c_str());
  // The title row's width, which the output column lines up with: the title, and an event's delegate pin.
  const float title_left = ImGui::GetItemRectMin().x;
  float title_width = ImGui::GetItemRectSize().x;
  float header_bottom = ImGui::GetItemRectMax().y;

  // An event's delegate pin, the last of its outputs, sits in the title's top right corner as in Unreal: at the
  // node's right edge as it was last frame, or beside the title the first time.
  const size_t delegate = desc && desc->delegate && !pins.outputs.empty() ? pins.outputs.size() - 1 : pins.outputs.size();
  if (delegate < pins.outputs.size()) {
    const Pin& pin = pins.outputs[delegate];
    const float icon = ImGui::GetTextLineHeight();
    const float right = ed::GetNodePosition(NodeId(node.id)).x + ed::GetNodeSize(NodeId(node.id)).x - ed::GetStyle().NodePadding.z - icon;
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, right), ImGui::GetCursorScreenPos().y));
    ed::BeginPin(PinId(node.id, delegate, true), ed::PinKind::Output);
    node_widgets::PinIcon(graph.HasLinkAt({node.id, pin.name}, true), PinColor(pin.type.kind), ShapeOf(pin));
    ed::EndPin();
    if (ImGui::IsItemHovered()) pin_tooltip_ = "Event: link it to Set Timer by Event or Bind Event, and they run this event's chain";
    header_bottom = std::max(header_bottom, ImGui::GetItemRectMax().y);
    // The pin sits at the node's right edge, so the outputs below right align with it.
    title_width = ImGui::GetItemRectMax().x - title_left;
  }
  ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));

  float inputs_width = 0.0f;
  if (!pins.inputs.empty()) ImGui::BeginGroup();
  for (size_t index = 0; index < pins.inputs.size(); ++index) {
    const Pin& pin = pins.inputs[index];
    const bool linked = graph.HasLinkAt({node.id, pin.name}, false);
    ed::BeginPin(PinId(node.id, index, false), ed::PinKind::Input);
    node_widgets::PinIcon(linked, PinColor(pin.type.kind), ShapeOf(pin));
    ed::EndPin();
    if (ImGui::IsItemHovered()) pin_tooltip_ = pin.name + ": " + graph::TypeName(pin.type);

    const std::string_view label = PinLabel(pin);
    if (!label.empty()) {
      ImGui::SameLine();
      ImGui::TextUnformatted(label.data(), label.data() + label.size());
    }
    // Unlinked values are typed in place, beside the pin so a click doesn't start a link.
    if (!linked) DrawInlineValue(node, pin);
  }
  if (!pins.inputs.empty()) {
    ImGui::EndGroup();
    inputs_width = ImGui::GetItemRectSize().x;
  }

  if (pins.outputs.size() > (delegate < pins.outputs.size() ? 1u : 0u)) {
    float column_width = 0.0f;
    for (size_t index = 0; index < pins.outputs.size(); ++index) {
      if (index != delegate) column_width = std::max(column_width, node_widgets::OutputRowWidth(PinLabel(pins.outputs[index])));
    }
    const float column_x = node_widgets::PlaceOutputColumn(content_x, title_width, inputs_width, column_width);
    ImGui::BeginGroup();
    for (size_t index = 0; index < pins.outputs.size(); ++index) {
      if (index == delegate) continue;
      const Pin& pin = pins.outputs[index];
      const std::string_view label = PinLabel(pin);
      // A Number output shows what it resolves to.
      const PinType carried = pin.type.kind == PinKind::kNumber ? graph::OutputType(graph, {node.id, pin.name}) : pin.type;
      node_widgets::AlignOutputRow(column_x, column_width, label);
      ed::BeginPin(PinId(node.id, index, true), ed::PinKind::Output);
      if (!label.empty()) {
        ImGui::TextUnformatted(label.data(), label.data() + label.size());
        ImGui::SameLine();
      }
      node_widgets::PinIcon(graph.HasLinkAt({node.id, pin.name}, true), PinColor(carried.kind), ShapeOf(pin));
      ed::EndPin();
      if (ImGui::IsItemHovered()) pin_tooltip_ = pin.name + ": " + graph::TypeName(carried);
    }
    ImGui::EndGroup();
  }

  ImGui::PopID();
  ed::EndNode();
  style_.EndNode(node.id, HeaderColor(node, desc, pins, graph), header_bottom, problem_nodes_.contains(node.id));
}

void NodeEditorWindow::DrawReroute(Node& node) {
  // A dot in the color of what passes through, with a grip either side for links: in on the left, out on the right.
  // Both links meet at the dot's middle.
  const graph::Graph& graph = document_->graph();
  const PinType carried = graph::OutputType(graph, {node.id, std::string(graph::kRerouteOut)});
  const float dot = ImGui::GetFontSize() * kRerouteDot;
  const float grip = ImGui::GetFontSize() * kRerouteGrip;
  const ed::NodeId id = NodeId(node.id);

  ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ed::BeginNode(id);
  ImGui::PushID(node.id);
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const ImVec2 center(start.x + grip + dot * 0.5f, start.y + dot * 0.5f);
  ed::BeginPin(PinId(node.id, 0, false), ed::PinKind::Input);
  ed::PinPivotRect(center, center);
  ImGui::Dummy(ImVec2(grip, dot));
  ed::EndPin();
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::Dummy(ImVec2(dot, dot));
  ImGui::SameLine(0.0f, 0.0f);
  ed::BeginPin(PinId(node.id, 0, true), ed::PinKind::Output);
  ed::PinPivotRect(center, center);
  ImGui::Dummy(ImVec2(grip, dot));
  ed::EndPin();
  if (ImGui::IsItemHovered() || ed::GetHoveredNode() == id) pin_tooltip_ = "Reroute: " + graph::TypeName(carried);
  ImGui::PopID();
  ed::EndNode();
  ed::PopStyleVar();

  // Over the links, which meet under it.
  ImDrawList* draw = ed::GetNodeBackgroundDrawList(id);
  if (!draw) return;
  const bool lit = ed::IsNodeSelected(id) || ed::GetHoveredNode() == id;
  const bool exec = carried.kind == PinKind::kExec;
  const float radius = dot * kRerouteDrawn;
  draw->AddCircleFilled(center, radius, IM_COL32(12, 14, 24, 220));
  draw->AddCircleFilled(center, radius * (exec ? 0.5f : 0.6f), PinColor(carried.kind));
  draw->AddCircle(center, radius, lit ? IM_COL32(255, 255, 255, 230) : IM_COL32(255, 255, 255, 90), 0, lit ? 2.0f : 1.0f);
  if (problem_nodes_.contains(node.id)) draw->AddCircle(center, radius + 2.0f, ImGui::ColorConvertFloat4ToU32(kErrorColor), 0, 1.5f);
}

void NodeEditorWindow::HandleDoubleClick() {
  const ed::LinkId clicked = ed::GetDoubleClickedLink();
  if (!clicked) return;
  // Between ed::Begin and ed::End the mouse is in canvas units. The reroute is centered on it.
  const float font = ImGui::GetFontSize();
  const ImVec2 mouse = ImGui::GetMousePos();
  const float x = mouse.x - font * (kRerouteGrip + kRerouteDot * 0.5f);
  const float y = mouse.y - font * kRerouteDot * 0.5f;
  const int reroute = graph::InsertReroute(document_->graph(), static_cast<int>(clicked.Get() - kLinkBase), x, y);
  if (!reroute) return;
  ed::SetNodePosition(NodeId(reroute), ImVec2(x, y));
  ed::ClearSelection();
  ed::SelectNode(NodeId(reroute));
  selected_node_ = reroute;
  focus_ = Focus::kNode;
  Edited();
}

void NodeEditorWindow::DrawCompactNode(Node& node, const NodePins& pins) {
  // One row and no header: its inputs, its title, its outputs, without their labels. A Get is the variable's name
  // and its value.
  const graph::Graph& graph = document_->graph();
  style_.BeginNode(node.id);
  ed::BeginNode(NodeId(node.id));
  ImGui::PushID(node.id);
  for (size_t index = 0; index < pins.inputs.size(); ++index) {
    const Pin& pin = pins.inputs[index];
    ed::BeginPin(PinId(node.id, index, false), ed::PinKind::Input);
    node_widgets::PinIcon(graph.HasLinkAt({node.id, pin.name}, false), PinColor(pin.type.kind), ShapeOf(pin));
    ed::EndPin();
    if (ImGui::IsItemHovered()) pin_tooltip_ = pin.name + ": " + graph::TypeName(pin.type);
    ImGui::SameLine();
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(kTitleColor, "%s", graph::TitleOf(node).c_str());
  for (size_t index = 0; index < pins.outputs.size(); ++index) {
    const Pin& pin = pins.outputs[index];
    const PinType carried = pin.type.kind == PinKind::kNumber ? graph::OutputType(graph, {node.id, pin.name}) : pin.type;
    ImGui::SameLine();
    ed::BeginPin(PinId(node.id, index, true), ed::PinKind::Output);
    node_widgets::PinIcon(graph.HasLinkAt({node.id, pin.name}, true), PinColor(carried.kind), ShapeOf(pin));
    ed::EndPin();
    if (ImGui::IsItemHovered()) pin_tooltip_ = graph::TypeName(carried);
  }
  ImGui::PopID();
  ed::EndNode();
  style_.EndNode(node.id, HeaderColor(node, graph::FindDesc(node), pins, graph), NodeGraphStyle::kNoHeader, problem_nodes_.contains(node.id));
}

void NodeEditorWindow::DrawInlineValue(Node& node, const Pin& pin) {
  const PinKind kind = pin.type.kind;
  if (kind == PinKind::kExec || kind == PinKind::kObject || kind == PinKind::kRegisters) return;

  const auto stored = node.values.find(pin.name);
  const nlohmann::json value = stored != node.values.end() ? stored->second : pin.initial;
  const float font = ImGui::GetFontSize();
  const std::string id = "##" + pin.name;

  ImGui::SameLine();
  if (kind == PinKind::kBool) {
    bool checked = value.is_boolean() && value.get<bool>();
    if (ImGui::Checkbox(id.c_str(), &checked)) {
      node.values[pin.name] = checked;
      Edited();
    }
    return;
  }

  if (kind == PinKind::kFloat) {
    float number = value.is_number() ? value.get<float>() : 0.0f;
    ImGui::SetNextItemWidth(font * kInlineFloatWidth);
    if (ImGui::DragFloat(id.c_str(), &number, 0.01f, 0.0f, 0.0f, "%.3g")) {
      node.values[pin.name] = number;
      Edited(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) Edited();
    return;
  }

  // Ints and numbers are typed as text, so hex addresses stay hex.
  std::string text;
  TextValue(value, text);

  // Picked from a list instead: a button with the chosen label opens it once the canvas is suspended.
  if (pin.choices) {
    const graph::ChoiceList& list = ChoicesOf(*pin.choices);
    const auto chosen = std::find_if(list.begin(), list.end(), [&text](const auto& choice) { return choice.second == text; });
    const std::string label = chosen != list.end() ? chosen->first : text.empty() ? std::string("None") : text;
    if (ImGui::Button((label + id).c_str(), ImVec2(font * kInlineChoiceWidth, 0.0f))) {
      choice_node_ = node.id;
      choice_pin_ = pin.name;
      open_choice_menu_ = true;
    }
    if (ImGui::IsItemHovered()) pin_tooltip_ = label;
    return;
  }
  ImGui::SetNextItemWidth(font * (kind == PinKind::kInt || kind == PinKind::kNumber ? kInlineNumberWidth : kInlineTextWidth));
  const char* hint = kind == PinKind::kInt && pin.name == "Address" ? "0x82000000" : nullptr;
  if (node_widgets::InputString(id.c_str(), text, 0, hint)) {
    node.values[pin.name] = text;
    Edited(false);
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) Edited();
}

void NodeEditorWindow::HandleCreate() {
  // EndCreate only follows a BeginCreate that returned true.
  if (!ed::BeginCreate(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), kLinkThickness)) return;
  graph::Graph& graph = document_->graph();

  const auto resolve = [&graph](const DecodedPin& decoded) -> std::optional<PinRef> {
    const Node* node = graph.FindNode(decoded.node);
    if (!node) return std::nullopt;
    const NodePins pins = graph::PinsOf(*node, graph);
    const std::vector<Pin>& side = decoded.output ? pins.outputs : pins.inputs;
    if (decoded.index >= side.size()) return std::nullopt;
    return PinRef{decoded.node, side[decoded.index].name};
  };

  // New links: output to input, dragged either way.
  ed::PinId start_id;
  ed::PinId end_id;
  if (ed::QueryNewLink(&start_id, &end_id)) {
    std::optional<DecodedPin> start = DecodePin(start_id);
    std::optional<DecodedPin> end = DecodePin(end_id);
    if (start && end) {
      if (!start->output) std::swap(start, end);
      const std::optional<PinRef> from = start->output ? resolve(*start) : std::nullopt;
      const std::optional<PinRef> to = !end->output ? resolve(*end) : std::nullopt;
      std::string reason;
      if (!from || !to || !graph::CanConnect(graph, *from, *to, &reason)) {
        ed::RejectNewItem(kRejectColor, kLinkThickness);
        if (!reason.empty()) {
          ed::Suspend();
          ImGui::SetTooltip("%s", reason.c_str());
          ed::Resume();
        }
      } else if (ed::AcceptNewItem(kAcceptColor, kLinkThickness + 1.0f) && graph::Connect(graph, *from, *to)) {
        Edited();
      }
    }
  }

  // A link dropped on empty space: add a node there, connected to the pin.
  ed::PinId pin_id;
  if (ed::QueryNewNode(&pin_id) && ed::AcceptNewItem()) {
    if (const std::optional<DecodedPin> decoded = DecodePin(pin_id)) {
      if (const std::optional<PinRef> ref = resolve(*decoded)) {
        pending_pin_ = PendingPin{*ref, decoded->output};
        open_add_menu_ = true;
      }
    }
  }
  ed::EndCreate();
}

void NodeEditorWindow::HandleDelete() {
  // EndDelete only follows a BeginDelete that returned true.
  if (!ed::BeginDelete()) return;
  graph::Graph& graph = document_->graph();
  bool changed = false;

  // Links go last: deleting a node deletes its links too, and a reroute's links are needed to join it up again.
  // Each is kept with the nodes the canvas had it between: link ids are made again when a graph is read back (undo),
  // so the canvas can report an id that's another link's by now, such as one a reroute just joined up.
  struct DeletedLink {
    int id;
    int from;
    int to;
  };
  std::vector<DeletedLink> links;
  ed::LinkId link_id;
  ed::PinId start_id;
  ed::PinId end_id;
  while (ed::QueryDeletedLink(&link_id, &start_id, &end_id)) {
    if (!ed::AcceptDeletedItem()) continue;
    const std::optional<DecodedPin> start = DecodePin(start_id);
    const std::optional<DecodedPin> end = DecodePin(end_id);
    if (start && end) links.push_back({static_cast<int>(link_id.Get() - kLinkBase), start->node, end->node});
  }
  ed::NodeId node_id;
  while (ed::QueryDeletedNode(&node_id)) {
    const Node* node = graph.FindNode(static_cast<int>(node_id.Get()));
    if (node && IsRoutineEnd(*node)) {
      ed::RejectDeletedItem();
      SetStatus("A function or macro keeps its inputs and outputs");
      continue;
    }
    if (ed::AcceptDeletedItem()) {
      const int id = static_cast<int>(node_id.Get());
      // A reroute deleted on its own joins what came into it to what it went to, so the link stays.
      if (node && node->type == graph::kReroute) graph::DissolveReroute(graph, id);
      else graph.RemoveNode(id);
      if (selected_node_ == id) selected_node_ = 0;
      changed = true;
    }
  }
  // The deleted nodes' links are gone with them already; the links a reroute joined up again are new ones.
  for (const DeletedLink& deleted : links) {
    const auto found = std::find_if(graph.links().begin(), graph.links().end(), [&](const graph::Link& link) { return link.id == deleted.id; });
    if (found != graph.links().end() && found->from.node == deleted.from && found->to.node == deleted.to) graph.RemoveLink(deleted.id);
  }
  changed = changed || !links.empty();
  ed::EndDelete();
  if (changed) Edited();
}

void NodeEditorWindow::HandleClipboard() {
  using node_widgets::ClipboardAction;
  node_widgets::ClipboardRequest request = node_widgets::TakeClipboardRequest();
  graph::Graph& graph = document_->graph();
  // A function's or macro's inputs and outputs are never copied: its graph keeps one of each.
  std::erase_if(request.nodes, [&graph](int id) {
    const Node* node = graph.FindNode(id);
    return node && IsRoutineEnd(*node);
  });

  switch (request.action) {
    case ClipboardAction::kCopy:
    case ClipboardAction::kCut:
      if (request.nodes.empty()) break;
      ImGui::SetClipboardText(graph::WriteNodes(graph, request.nodes).c_str());
      if (request.action == ClipboardAction::kCut) {
        for (int id : request.nodes) graph.RemoveNode(id);
        if (std::find(request.nodes.begin(), request.nodes.end(), selected_node_) != request.nodes.end()) selected_node_ = 0;
        Edited();
      }
      break;
    case ClipboardAction::kPaste:
      // Between ed::Begin and ed::End, outside Suspend, the mouse is already in canvas units.
      if (const char* text = ImGui::GetClipboardText()) PasteNodes(text, ImGui::GetMousePos().x, ImGui::GetMousePos().y);
      break;
    case ClipboardAction::kDuplicate: {
      // A copy beside the original, leaving the clipboard alone.
      float x = std::numeric_limits<float>::max();
      float y = std::numeric_limits<float>::max();
      for (int id : request.nodes) {
        if (const Node* node = graph.FindNode(id)) {
          x = std::min(x, node->x);
          y = std::min(y, node->y);
        }
      }
      if (!request.nodes.empty()) PasteNodes(graph::WriteNodes(graph, request.nodes), x + kDuplicateOffset, y + kDuplicateOffset);
      break;
    }
    case ClipboardAction::kNone: break;
  }
}

void NodeEditorWindow::PasteNodes(const std::string& text, float x, float y) {
  graph::Graph copied;
  if (!graph::ReadNodes(text, copied)) return;
  graph::Graph& graph = document_->graph();

  float left = std::numeric_limits<float>::max();
  float top = std::numeric_limits<float>::max();
  for (const Node& node : copied.nodes()) {
    left = std::min(left, node.x);
    top = std::min(top, node.y);
  }

  // The copies get new ids, and their links are made again between them.
  std::map<int, int> new_ids;
  std::vector<int> pasted;
  for (const Node& node : copied.nodes()) {
    if (IsRoutineEnd(node)) continue;
    Node& added = graph.AddNode(node.type, x + node.x - left, y + node.y - top);
    added.owner = owner_;
    added.settings = node.settings;
    added.values = node.values;
    new_ids[node.id] = added.id;
    pasted.push_back(added.id);
    ed::SetNodePosition(NodeId(added.id), ImVec2(added.x, added.y));
  }
  for (const graph::Link& link : copied.links()) {
    const auto from = new_ids.find(link.from.node);
    const auto to = new_ids.find(link.to.node);
    if (from != new_ids.end() && to != new_ids.end()) graph::Connect(graph, {from->second, link.from.pin}, {to->second, link.to.pin});
  }
  node_widgets::SelectNodes(pasted);
  selected_node_ = pasted.size() == 1 ? pasted.front() : 0;
  focus_ = Focus::kNode;
  Edited();
}

void NodeEditorWindow::DrawChoiceMenu() {
  if (!ImGui::BeginPopup(kChoiceMenu)) return;
  graph::Graph& graph = document_->graph();
  Node* node = graph.FindNode(choice_node_);
  const NodePins pins = node ? graph::PinsOf(*node, graph) : NodePins{};
  const Pin* pin = node ? graph::FindPin(pins.inputs, choice_pin_) : nullptr;
  if (!pin || !pin->choices) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  const auto stored = node->values.find(pin->name);
  std::string current;
  TextValue(stored != node->values.end() ? stored->second : pin->initial, current);

  const float font = ImGui::GetFontSize();
  const bool appearing = ImGui::IsWindowAppearing();
  const graph::ChoiceList& list = ChoicesOf(*pin->choices, appearing);
  if (appearing) ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(font * kChoiceListWidth);
  node_widgets::InputString("##search", choice_filter_, 0, "Search, or type your own");
  const auto pick = [&](const std::string& value) {
    node->values[pin->name] = value;
    Edited();
    ImGui::CloseCurrentPopup();
  };
  ImGui::BeginChild("##choices", ImVec2(font * kChoiceListWidth, font * kChoiceListHeight));
  // What's typed, when it isn't one of the values: the list is a help, not a limit.
  const bool listed = std::any_of(list.begin(), list.end(), [this](const auto& choice) { return choice.second == choice_filter_; });
  if (!choice_filter_.empty() && !listed && ImGui::Selectable(("Use \"" + choice_filter_ + "\"").c_str())) pick(choice_filter_);
  for (const auto& [label, value] : list) {
    if (!choice_filter_.empty() && !node_widgets::ContainsIgnoringCase(label, choice_filter_) && !node_widgets::ContainsIgnoringCase(value, choice_filter_)) continue;
    const bool selected = value == current;
    if (ImGui::Selectable((label + "##" + value).c_str(), selected)) pick(value);
    if (selected && appearing) ImGui::SetScrollHereY();
  }
  if (list.empty() && choice_filter_.empty()) ImGui::TextDisabled("Nothing to pick from yet: type a value.");
  ImGui::EndChild();
  ImGui::EndPopup();
}

const graph::ChoiceList& NodeEditorWindow::ChoicesOf(const graph::ChoiceSource& source, bool refresh) {
  if (!source.live) return source.fixed;
  auto cached = live_choices_.find(&source);
  if (cached == live_choices_.end()) {
    cached = live_choices_.emplace(&source, source.live()).first;
  } else if (refresh) {
    cached->second = source.live();
  }
  return cached->second;
}

void NodeEditorWindow::DrawAddMenu() {
  if (!ImGui::BeginPopup(kAddMenu)) return;
  const graph::Graph& graph = document_->graph();

  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
  node_widgets::InputString("##search", add_filter_, 0, "Search nodes");

  // Every entry: a node type, with the settings that pick a variable for Get and Set, or a function to call.
  struct Entry {
    std::string label;
    std::string category;
    std::string type;
    Settings settings;
    const std::string* description;
    const std::string* keywords;
  };
  std::vector<Entry> entries;
  const auto offer = [&](const NodeDesc& desc, std::string label, std::string category, Settings settings, const std::string* description) {
    // Dragged from a pin: only nodes with a pin that fits it.
    if (pending_pin_) {
      const NodePins pins = graph::PinsOf(Prototype(desc, settings), graph);
      const Node* from = graph.FindNode(pending_pin_->ref.node);
      if (!from) return;
      const NodePins from_pins = graph::PinsOf(*from, graph);
      const Pin* pending = graph::FindPin(pending_pin_->output ? from_pins.outputs : from_pins.inputs, pending_pin_->ref.pin);
      if (!pending) return;
      const PinType pending_type = pending_pin_->output ? graph::OutputType(graph, pending_pin_->ref) : pending->type;
      const std::vector<Pin>& side = pending_pin_->output ? pins.inputs : pins.outputs;
      const bool fits = std::any_of(side.begin(), side.end(), [&](const Pin& pin) { return pending_pin_->output ? MayFit(pending_type, pin.type) : MayFit(pin.type, pending_type); });
      if (!fits) return;
    }
    entries.push_back({std::move(label), std::move(category), desc.id, std::move(settings), description, &desc.keywords});
  };
  for (const std::unique_ptr<NodeDesc>& desc : graph::Library::Get().nodes()) {
    // Nodes without a category are the editor's own to place: a function's inputs, outputs and calls.
    if (desc->category.empty()) continue;
    // Events start the event graph's chains; a function or macro starts at its inputs.
    if (owner_ != 0 && desc->role == NodeRole::kEvent) continue;
    if (const graph::SettingDesc* member = MemberSetting(*desc)) {
      if (member->kind == graph::SettingKind::kVariable) {
        for (const graph::Variable& variable : graph.variables()) {
          if (graph::AcceptsVariable(*member, variable.type)) offer(*desc, MemberLabel(*desc, *member, variable.name), desc->category, {{member->name, variable.name}}, &desc->description);
        }
      } else {
        for (const graph::Dispatcher& dispatcher : graph.dispatchers()) offer(*desc, MemberLabel(*desc, *member, dispatcher.name), desc->category, {{member->name, dispatcher.name}}, &desc->description);
      }
      // A node that may pick none is offered as itself too: a custom event without params.
      if (!member->optional) continue;
    }
    offer(*desc, desc->title, desc->category, {}, &desc->description);
  }
  if (const NodeDesc* call = graph::Library::Get().Find(graph::kRoutineCall)) {
    for (const Routine& routine : graph.routines()) {
      // A macro is written out where it's used, so it can't use itself.
      if (routine.kind == RoutineKind::kMacro && routine.id == owner_) continue;
      const Settings settings = {{std::string(graph::kRoutineSetting), std::to_string(routine.id)}, {std::string(graph::kRoutineNameSetting), routine.name}};
      offer(*call, routine.name, routine.kind == RoutineKind::kMacro ? "Macros" : "Functions", settings, routine.comment.empty() ? &call->description : &routine.comment);
    }
  }

  const Entry* chosen = nullptr;
  const auto item = [&](const Entry& entry, const std::string& label) {
    const bool picked = ImGui::MenuItem(label.c_str());
    if (ImGui::IsItemHovered() && !entry.description->empty()) ImGui::SetTooltip("%s", entry.description->c_str());
    return picked;
  };

  if (!add_filter_.empty()) {
    // A flat list of matches; Enter takes the first.
    bool first = true;
    for (const Entry& entry : entries) {
      const bool matches = node_widgets::ContainsIgnoringCase(entry.label, add_filter_) || node_widgets::ContainsIgnoringCase(entry.category, add_filter_) || node_widgets::ContainsIgnoringCase(*entry.keywords, add_filter_);
      if (!matches) continue;
      if (item(entry, entry.label + "##" + entry.type + entry.category) || (first && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
        chosen = &entry;
        break;
      }
      first = false;
    }
    if (first) ImGui::TextDisabled("Nothing matches");
  } else {
    // Categories nest at their slashes: Game/tip/Functions.
    const std::function<void(const std::string&)> level = [&](const std::string& prefix) {
      std::vector<std::string> children;
      for (const Entry& entry : entries) {
        if (entry.category.size() <= prefix.size() || entry.category.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string rest = entry.category.substr(prefix.size());
        const std::string child = rest.substr(0, rest.find('/'));
        if (std::find(children.begin(), children.end(), child) == children.end()) children.push_back(child);
      }
      for (const std::string& child : children) {
        if (!ImGui::BeginMenu(child.c_str())) continue;
        level(prefix + child + "/");
        ImGui::EndMenu();
      }
      const std::string own = prefix.empty() ? std::string() : prefix.substr(0, prefix.size() - 1);
      for (const Entry& entry : entries) {
        if (!prefix.empty() && entry.category == own && item(entry, entry.label + "##" + entry.type + entry.category)) chosen = &entry;
      }
    };
    level({});
    if (entries.empty()) ImGui::TextDisabled("No node fits that pin");
  }

  if (chosen) {
    AddNode(chosen->type, chosen->settings);
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void NodeEditorWindow::AddNode(const std::string& type, const Settings& settings) {
  graph::Graph& graph = document_->graph();
  const NodeDesc* desc = graph::Library::Get().Find(type);
  if (!desc) return;
  Node& node = graph.AddNode(type, add_x_, add_y_);
  node.owner = owner_;
  node.settings = Prototype(*desc, settings).settings;
  const int id = node.id;
  ed::SetNodePosition(NodeId(id), ImVec2(add_x_, add_y_));

  // Opened from a dragged link: connect through the first pin that fits.
  if (pending_pin_) {
    const NodePins pins = graph::PinsOf(*graph.FindNode(id), graph);
    for (const Pin& pin : pending_pin_->output ? pins.inputs : pins.outputs) {
      const PinRef ref{id, pin.name};
      if (pending_pin_->output ? graph::Connect(graph, pending_pin_->ref, ref) : graph::Connect(graph, ref, pending_pin_->ref)) break;
    }
    pending_pin_.reset();
  }

  // The new node is selected, so its details show at once.
  ed::ClearSelection();
  ed::SelectNode(NodeId(id));
  selected_node_ = id;
  focus_ = Focus::kNode;
  Edited();
}

void NodeEditorWindow::AcceptDrop(const ImVec2& at) {
  graph::Graph& graph = document_->graph();
  const ImGuiPayload* variable = ImGui::AcceptDragDropPayload(kVariablePayload);
  const ImGuiPayload* routine = variable ? nullptr : ImGui::AcceptDragDropPayload(kRoutinePayload);
  const ImGuiPayload* dispatcher = variable || routine ? nullptr : ImGui::AcceptDragDropPayload(kDispatcherPayload);
  if (!variable && !routine && !dispatcher) return;
  add_x_ = at.x;
  add_y_ = at.y;
  pending_pin_.reset();

  if (routine) {
    int id = 0;
    std::memcpy(&id, routine->Data, sizeof(id));
    const Routine* found = graph.FindRoutine(id);
    if (!found) return;
    if (found->kind == RoutineKind::kMacro && found->id == owner_) {
      SetStatus("A macro can't use itself", true);
      return;
    }
    AddNode(std::string(graph::kRoutineCall), {{std::string(graph::kRoutineSetting), std::to_string(id)}, {std::string(graph::kRoutineNameSetting), found->name}});
    return;
  }

  if (dispatcher) {
    dropped_member_.assign(static_cast<const char*>(dispatcher->Data), static_cast<size_t>(dispatcher->DataSize));
    dropped_dispatcher_ = true;
    open_drop_menu_ = true;
    return;
  }

  // As in a blueprint: Ctrl drops a Get, Alt a Set, and otherwise it asks.
  const std::string name(static_cast<const char*>(variable->Data), static_cast<size_t>(variable->DataSize));
  const ImGuiIO& io = ImGui::GetIO();
  if (io.KeyCtrl) AddNode("var.get", {{"Variable", name}});
  else if (io.KeyAlt) AddNode("var.set", {{"Variable", name}});
  else {
    dropped_member_ = name;
    dropped_dispatcher_ = false;
    open_drop_menu_ = true;
  }
}

void NodeEditorWindow::DrawDropMenu() {
  if (!ImGui::BeginPopup(kDropMenu)) return;
  const std::string name = dropped_member_;
  const graph::Graph& graph = document_->graph();
  const graph::Variable* variable = dropped_dispatcher_ ? nullptr : graph.FindVariable(name);
  if (!variable && !(dropped_dispatcher_ && graph.FindDispatcher(name))) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  ImGui::TextDisabled("%s", name.c_str());
  ImGui::Separator();
  // Each node that can pick it: Get and Set, a Bool's If, a number's Increment; Call, Bind, a custom event taking
  // the dispatcher's params.
  const auto offer = [&](const char* type) {
    const NodeDesc* desc = graph::Library::Get().Find(type);
    const graph::SettingDesc* setting = desc ? MemberSetting(*desc) : nullptr;
    if (!setting || (variable && !graph::AcceptsVariable(*setting, variable->type))) return;
    const char* shortcut = std::strcmp(type, "var.get") == 0 ? "Ctrl" : std::strcmp(type, "var.set") == 0 ? "Alt" : nullptr;
    if (ImGui::MenuItem(MemberLabel(*desc, *setting, name).c_str(), shortcut)) AddNode(type, {{setting->name, name}});
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", desc->description.c_str());
  };
  if (dropped_dispatcher_) {
    for (const char* type : kDispatcherNodes) offer(type);
  } else {
    for (const char* type : kVariableNodes) offer(type);
  }
  ImGui::EndPopup();
}

void NodeEditorWindow::DrawNodeMenu() {
  if (!ImGui::BeginPopup(kNodeMenu)) return;
  graph::Graph& graph = document_->graph();
  const Node* node = graph.FindNode(context_node_);
  if (!node) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  const int id = node->id;
  ImGui::TextDisabled("%s", graph::TitleOf(*node).c_str());
  ImGui::Separator();

  // A variable's nodes turn into each other: a Bool's Get into its own If, with True and False.
  const graph::Variable* variable = graph.FindVariable(node->Setting("Variable"));
  const std::string type = node->type;
  if (type == "var.get" && variable && variable->type == PinKind::kBool && ImGui::MenuItem("Convert to Branch")) ConvertNode(id, "var.branch");
  if (type == "var.get" && ImGui::MenuItem("Convert to Set")) ConvertNode(id, "var.set");
  if ((type == "var.set" || type == "var.branch") && ImGui::MenuItem("Convert to Get")) ConvertNode(id, "var.get");

  // A use of a function or macro opens its graph.
  const Routine* routine = type == graph::kRoutineCall ? graph::RoutineOf(*node, graph) : nullptr;
  if (routine && ImGui::MenuItem(routine->kind == RoutineKind::kMacro ? "Open Macro" : "Open Function")) {
    selected_routine_ = routine->id;
    focus_ = Focus::kRoutine;
    ShowGraph(routine->id);
  }

  if (graph.FindNode(id) && !IsRoutineEnd(*graph.FindNode(id))) {
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
      const Node* original = graph.FindNode(id);
      const int ids[] = {id};
      PasteNodes(graph::WriteNodes(graph, ids), original->x + kDuplicateOffset, original->y + kDuplicateOffset);
    }
    if (ImGui::MenuItem("Delete", "Del")) {
      // A reroute joins its link up again, as with the Delete key.
      if (type == graph::kReroute) graph::DissolveReroute(graph, id);
      else graph.RemoveNode(id);
      if (selected_node_ == id) selected_node_ = 0;
      Edited();
    }
  } else if (graph.FindNode(id)) {
    ImGui::TextDisabled("A function or macro keeps its inputs and outputs");
  }
  ImGui::EndPopup();
}

void NodeEditorWindow::ConvertNode(int id, const std::string& type) {
  graph::Graph& graph = document_->graph();
  Node* node = graph.FindNode(id);
  if (!node || !graph::Library::Get().Find(type)) return;
  node->type = type;
  node->values.clear();
  // Links to pins the new type doesn't have go; the exec input stays where both have one.
  graph::RemoveDanglingLinks(graph);
  selected_node_ = id;
  focus_ = Focus::kNode;
  Edited();
}

void NodeEditorWindow::SyncFromCanvas() {
  // Positions are kept in the graph. A drag is one undo step, taken when the mouse lets go.
  for (Node& node : document_->graph().nodes()) {
    if (node.owner != owner_) continue;
    const ImVec2 position = ed::GetNodePosition(NodeId(node.id));
    if (position.x != node.x || position.y != node.y) {
      node.x = position.x;
      node.y = position.y;
      moving_ = true;
    }
  }
  if (moving_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    moving_ = false;
    Edited();
  }

  // Selecting a node shows it in the details.
  ed::NodeId selected[1];
  const int node = ed::GetSelectedNodes(selected, 1) > 0 ? static_cast<int>(selected[0].Get()) : 0;
  if (node != selected_node_) {
    selected_node_ = node;
    if (node) focus_ = Focus::kNode;
    else if (focus_ == Focus::kNode) focus_ = Focus::kNone;
  }
}

void NodeEditorWindow::DrawLua() {
  if (lua_view_ != document_->compiled().lua) lua_view_ = document_->compiled().lua;
  if (glass::SmallButton("Copy", glass::kAccent)) ImGui::SetClipboardText(lua_view_.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("What the graph saves as scripts/%s.graph.lua", document_->name().c_str());
  node_widgets::InputStringMultiline("##lua", lua_view_, ImGui::GetContentRegionAvail(), ImGuiInputTextFlags_ReadOnly);
}

// Right -------------------------------------------------------------------------------------------------

void NodeEditorWindow::DrawDetails() {
  if (!document_) {
    ImGui::TextWrapped("Graphs are saved in the mod's graphs folder, each with the Lua script it makes in scripts. The script is an ordinary Lua mod.");
    return;
  }
  switch (focus_) {
    case Focus::kNode: DrawNodeDetails(); break;
    case Focus::kVariable: DrawVariableDetails(); break;
    case Focus::kRoutine: DrawRoutineDetails(); break;
    case Focus::kDispatcher: DrawDispatcherDetails(); break;
    case Focus::kNone:
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("Select a node, function, macro, variable or event dispatcher to edit it here.");
      ImGui::PopStyleColor();
      break;
  }
  DrawProblems();
}

void NodeEditorWindow::DrawNodeDetails() {
  Node* node = document_->graph().FindNode(selected_node_);
  const NodeDesc* desc = node ? graph::FindDesc(*node) : nullptr;
  if (!node) return;
  ImGui::SeparatorText(graph::TitleOf(*node).c_str());
  if (!desc) {
    ImGui::TextColored(kErrorColor, "There's no %s node; the game may not declare it any more.", node->type.c_str());
    return;
  }
  const Routine* routine = IsRoutineNode(*node) ? graph::RoutineOf(*node, document_->graph()) : nullptr;
  ImGui::TextWrapped("%s", routine && !routine->comment.empty() ? routine->comment.c_str() : desc->description.c_str());
  ImGui::TextDisabled("n%d in the Lua", node->id);
  if (routine && glass::Button(routine->kind == RoutineKind::kMacro ? "Edit Macro" : "Edit Function", glass::kAccent)) {
    selected_routine_ = routine->id;
    focus_ = Focus::kRoutine;
    if (node->type == graph::kRoutineCall) ShowGraph(routine->id);
    return;
  }

  for (const graph::SettingDesc& setting : desc->settings) {
    if (setting.shown && !setting.shown(*node)) continue;
    ImGui::PushID(setting.name.c_str());
    std::string value = graph::SettingOf(*node, setting.name);
    switch (setting.kind) {
      case graph::SettingKind::kList: DrawListSetting(*node, setting); break;
      case graph::SettingKind::kScript: DrawScriptSetting(*node, setting); break;
      case graph::SettingKind::kCode:
        ImGui::TextUnformatted(setting.name.c_str());
        if (node_widgets::InputStringMultiline("##code", value, ImVec2(-FLT_MIN, ImGui::GetFontSize() * 16.0f), ImGuiInputTextFlags_AllowTabInput)) {
          node->settings[setting.name] = value;
          Edited(false);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) Edited();
        break;
      case graph::SettingKind::kAddress:
      case graph::SettingKind::kText:
        if (node_widgets::InputString(setting.name.c_str(), value, 0, setting.kind == graph::SettingKind::kAddress ? "0x82000000" : nullptr)) {
          node->settings[setting.name] = value;
          Edited(false);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) Edited();
        break;
      case graph::SettingKind::kChoice:
      case graph::SettingKind::kVariable:
      case graph::SettingKind::kDispatcher: {
        std::vector<std::string> choices = setting.choices;
        if (setting.kind == graph::SettingKind::kVariable) {
          choices.clear();
          for (const graph::Variable& variable : document_->graph().variables()) {
            if (graph::AcceptsVariable(setting, variable.type)) choices.push_back(variable.name);
          }
        }
        if (setting.kind == graph::SettingKind::kDispatcher) {
          choices.clear();
          for (const graph::Dispatcher& dispatcher : document_->graph().dispatchers()) choices.push_back(dispatcher.name);
        }
        if (ImGui::BeginCombo(setting.name.c_str(), value.empty() ? "None" : value.c_str())) {
          if (setting.optional && ImGui::Selectable("None", value.empty()) && !value.empty()) {
            node->settings[setting.name].clear();
            graph::RemoveDanglingLinks(document_->graph());
            Edited();
          }
          for (const std::string& choice : choices) {
            if (ImGui::Selectable(choice.c_str(), choice == value) && choice != value) {
              node->settings[setting.name] = choice;
              // Pins that came from the old pick go, with their links.
              graph::RemoveDanglingLinks(document_->graph());
              Edited();
            }
          }
          ImGui::EndCombo();
        }
        break;
      }
    }
    if (!setting.tooltip.empty()) ImGui::SetItemTooltip("%s", setting.tooltip.c_str());
    ImGui::PopID();
  }
}

void NodeEditorWindow::DrawScriptSetting(Node& node, const graph::SettingDesc& setting) {
  std::string value = graph::SettingOf(node, setting.name);
  if (node_widgets::InputString(setting.name.c_str(), value, 0, "main.lua")) {
    node.settings[setting.name] = value;
    Edited(false);
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) Edited();
  if (value.empty()) return;

  // The file's code, read again when the node names another one; saved on its own, apart from the graph.
  const std::filesystem::path file = document_->mod_dir() / "scripts" / value;
  if (file != script_file_) {
    script_file_ = file;
    script_text_.clear();
    std::ifstream in(file, std::ios::binary);
    script_exists_ = static_cast<bool>(in);
    if (in) script_text_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    script_saved_ = script_text_;
  }
  ImGui::SeparatorText("Code");
  const bool changed = script_text_ != script_saved_;
  ImGui::BeginDisabled(!changed);
  if (glass::SmallButton("Save Script", glass::kAccent)) {
    std::error_code code;
    std::filesystem::create_directories(file.parent_path(), code);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (out) out << script_text_;
    if (out) {
      script_saved_ = script_text_;
      script_exists_ = true;
      SetStatus("Saved scripts/" + value + "; Save and Reload runs it");
    } else {
      SetStatus("Couldn't write " + file.string(), true);
    }
  }
  ImGui::SameLine();
  if (glass::SmallButton("Revert")) script_text_ = script_saved_;
  ImGui::EndDisabled();
  ImGui::SameLine();
  const std::string state = !script_exists_ ? "No such file yet: saving makes it" : changed ? "Unsaved changes" : "scripts/" + value;
  ImGui::TextDisabled("%s", state.c_str());
  node_widgets::InputStringMultiline("##code", script_text_, ImGui::GetContentRegionAvail(), ImGuiInputTextFlags_AllowTabInput);
}

void NodeEditorWindow::DrawListSetting(Node& node, const graph::SettingDesc& setting) {
  graph::Graph& graph = document_->graph();
  std::vector<std::string> entries = graph::ListOf(node, setting.name);
  const float font = ImGui::GetFontSize();
  ImGui::SeparatorText(setting.name.c_str());

  int removed = -1;
  bool changed = false;
  for (size_t index = 0; index < entries.size(); ++index) {
    ImGui::PushID(static_cast<int>(index));
    std::string text = entries[index];
    ImGui::SetNextItemWidth(font * 10.0f);
    if (node_widgets::InputString("##entry", text, ImGuiInputTextFlags_EnterReturnsTrue) && text != entries[index]) {
      // An entry is a pin, so it can't share a name with another of the node's pins.
      const NodePins pins = graph::PinsOf(node, graph);
      const bool taken = graph::FindPin(pins.inputs, text) || graph::FindPin(pins.outputs, text);
      if (text.empty() || text.find('\n') != std::string::npos) {
        SetStatus("Give it a name", true);
      } else if (taken) {
        SetStatus("The node has a pin named " + text + " already", true);
      } else {
        // Its pin's links and typed value follow.
        graph.RenamePin({node.id, entries[index]}, true, text);
        graph.RenamePin({node.id, entries[index]}, false, text);
        entries[index] = text;
        changed = true;
      }
    }
    ImGui::SetItemTooltip("Press Enter to rename");
    ImGui::SameLine();
    if (glass::SmallButton("x", glass::kDanger)) removed = static_cast<int>(index);
    ImGui::SetItemTooltip("Remove it, with its pin's links");
    ImGui::PopID();
  }
  if (removed >= 0) {
    entries.erase(entries.begin() + removed);
    changed = true;
  }

  const std::string add = "Add " + (setting.item.empty() ? std::string("Entry") : setting.item);
  if (glass::SmallButton(add.c_str(), glass::kCreate)) {
    // The next number after the largest when they're all numbers, as a switch on an Int's cases are; else a name.
    long long next = 0;
    bool numbers = !entries.empty();
    for (const std::string& entry : entries) {
      char* end = nullptr;
      const long long number = std::strtoll(entry.c_str(), &end, 0);
      if (!end || *end != '\0') numbers = false;
      else next = std::max(next, number + 1);
    }
    std::string name = numbers || entries.empty() ? std::to_string(next) : setting.item + std::to_string(entries.size());
    for (int suffix = 1; std::find(entries.begin(), entries.end(), name) != entries.end(); ++suffix) name = setting.item + std::to_string(entries.size() + suffix);
    entries.push_back(name);
    changed = true;
  }

  if (!changed) return;
  node.settings[setting.name] = graph::JoinList(entries);
  graph::RemoveDanglingLinks(graph);
  Edited();
}

void NodeEditorWindow::DrawVariableDetails() {
  graph::Graph& graph = document_->graph();
  graph::Variable* variable = graph.FindVariable(selected_variable_);
  if (!variable) return;
  ImGui::SeparatorText("Variable");

  std::string name = variable->name;
  if (node_widgets::InputString("Name", name, ImGuiInputTextFlags_EnterReturnsTrue) && name != variable->name) {
    std::string reason;
    if (!CheckName(graph, name, &reason)) {
      SetStatus(reason, true);
    } else {
      // The nodes using it follow.
      graph::RenameVariable(graph, variable->name, name);
      selected_variable_ = name;
      Edited();
    }
  }
  ImGui::SetItemTooltip("Press Enter to rename. The nodes using it follow");

  if (ImGui::BeginCombo("Type", KindLabel(variable->type))) {
    for (PinKind kind : kVariableKinds) {
      if (ImGui::Selectable(KindLabel(kind), kind == variable->type) && kind != variable->type) {
        variable->type = kind;
        variable->initial = graph::DefaultValue(kind);
        Edited();
      }
    }
    ImGui::EndCombo();
  }

  // The value it starts with, as the script loads.
  switch (variable->type) {
    case PinKind::kBool: {
      bool checked = variable->initial.is_boolean() && variable->initial.get<bool>();
      if (ImGui::Checkbox("Starts as", &checked)) {
        variable->initial = checked;
        Edited();
      }
      break;
    }
    case PinKind::kFloat: {
      double number = variable->initial.is_number() ? variable->initial.get<double>() : 0.0;
      if (ImGui::InputDouble("Starts as", &number, 0.0, 0.0, "%.6g")) {
        variable->initial = number;
        Edited(false);
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) Edited();
      break;
    }
    case PinKind::kAny: ImGui::TextDisabled("Starts as nil"); break;
    default: {
      std::string text;
      TextValue(variable->initial, text);
      if (node_widgets::InputString("Starts as", text)) {
        variable->initial = text;
        Edited(false);
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) Edited();
    }
  }

  if (node_widgets::InputStringMultiline("Comment", variable->comment, ImVec2(0.0f, ImGui::GetTextLineHeight() * 3.0f))) Edited(false);
  if (ImGui::IsItemDeactivatedAfterEdit()) Edited();

  ImGui::Spacing();
  if (glass::Button("Delete Variable", glass::kDanger)) {
    // The nodes using it go with it.
    graph::DeleteVariable(graph, variable->name);
    selected_variable_.clear();
    focus_ = Focus::kNone;
    Edited();
  }
}

void NodeEditorWindow::DrawRoutineDetails() {
  graph::Graph& graph = document_->graph();
  Routine* routine = graph.FindRoutine(selected_routine_);
  if (!routine) return;
  const bool macro = routine->kind == RoutineKind::kMacro;
  ImGui::PushStyleColor(ImGuiCol_Text, RoutineColor(routine->kind));
  ImGui::SeparatorText(macro ? "Macro" : "Function");
  ImGui::PopStyleColor();
  ImGui::TextWrapped("%s", macro ? "Written out in full wherever it's used, so its inputs and outputs can be exec pins, like Branch's." : "A graph of its own, called from the event graph and other functions. It becomes a Lua function.");

  std::string name = routine->name;
  if (node_widgets::InputString("Name", name, ImGuiInputTextFlags_EnterReturnsTrue) && name != routine->name) {
    std::string reason;
    if (!CheckName(graph, name, &reason)) {
      SetStatus(reason, true);
    } else {
      // The nodes using it follow.
      graph::RenameRoutine(graph, routine->id, name);
      Edited();
    }
  }
  ImGui::SetItemTooltip("Press Enter to rename. The nodes using it follow");

  if (!macro) {
    bool pure = routine->pure;
    if (ImGui::Checkbox("Pure", &pure)) {
      routine->pure = pure;
      // Its exec pins come or go, and their links with them.
      graph::RemoveDanglingLinks(graph);
      Edited();
    }
    ImGui::SetItemTooltip("No exec pins: worked out wherever its outputs are read, as Add is");
  }

  DrawParams(*routine, false);
  DrawParams(*routine, true);

  ImGui::Spacing();
  if (node_widgets::InputStringMultiline("Comment", routine->comment, ImVec2(0.0f, ImGui::GetTextLineHeight() * 3.0f))) Edited(false);
  if (ImGui::IsItemDeactivatedAfterEdit()) Edited();

  ImGui::Spacing();
  if (owner_ != routine->id && glass::Button(macro ? "Open Macro" : "Open Function", glass::kAccent)) ShowGraph(routine->id);
  if (owner_ != routine->id) ImGui::SameLine();
  if (glass::Button(macro ? "Delete Macro" : "Delete Function", glass::kDanger)) {
    // Its graph and the nodes using it go with it.
    const int id = routine->id;
    graph::DeleteRoutine(graph, id);
    if (owner_ == id) ShowGraph(0);
    focus_ = Focus::kNone;
    Edited();
  }
}

void NodeEditorWindow::DrawParams(Routine& routine, bool outputs) {
  graph::Graph& graph = document_->graph();
  const bool function = routine.kind == RoutineKind::kFunction;
  const int id = routine.id;
  const auto refuse = [function](const std::string& name) -> std::string {
    if (function && (name == graph::kExecIn || name == graph::kExecOut)) return name + " is kept for the function's own exec pins";
    return {};
  };
  // The pins of its inputs node or outputs node, and of its calls, follow with their links.
  const auto renamed = [&graph, id, outputs](const std::string& from, const std::string& to) { graph::RenameRoutineParam(graph, id, outputs, from, to); };
  DrawParamList(outputs ? "Outputs" : "Inputs", outputs ? routine.outputs : routine.inputs, graph::ParamKinds(routine.kind), outputs ? "Add Output" : "Add Input", outputs ? "Result" : "Value", refuse, renamed);
}

void NodeEditorWindow::DrawParamList(const char* title, std::vector<graph::Param>& params, const std::vector<PinKind>& kinds, const char* add_label, const std::string& base, const std::function<std::string(const std::string& name)>& refuse, const std::function<void(const std::string& from, const std::string& to)>& renamed) {
  graph::Graph& graph = document_->graph();
  const float font = ImGui::GetFontSize();
  ImGui::SeparatorText(title);
  ImGui::PushID(title);

  int removed = -1;
  for (size_t index = 0; index < params.size(); ++index) {
    graph::Param& param = params[index];
    ImGui::PushID(static_cast<int>(index));
    Swatch(PinColor(param.type));

    std::string name = param.name;
    ImGui::SetNextItemWidth(font * 7.0f);
    if (node_widgets::InputString("##name", name, ImGuiInputTextFlags_EnterReturnsTrue) && name != param.name) {
      const bool taken = std::any_of(params.begin(), params.end(), [&](const graph::Param& other) { return other.name == name; });
      const std::string refused = name.empty() ? "Give it a name" : taken ? "There's one named " + name + " already" : refuse(name);
      if (!refused.empty()) {
        SetStatus(refused, true);
      } else {
        renamed(param.name, name);
        param.name = name;
        Edited();
      }
    }
    ImGui::SetItemTooltip("Press Enter to rename");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(font * 5.0f);
    if (ImGui::BeginCombo("##type", KindLabel(param.type))) {
      for (PinKind kind : kinds) {
        if (ImGui::Selectable(KindLabel(kind), kind == param.type) && kind != param.type) {
          param.type = kind;
          Edited();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (glass::SmallButton("x", glass::kDanger)) removed = static_cast<int>(index);
    ImGui::SetItemTooltip("Remove it");
    ImGui::PopID();
  }
  if (removed >= 0) {
    params.erase(params.begin() + removed);
    graph::RemoveDanglingLinks(graph);
    Edited();
  }

  if (glass::SmallButton(add_label, glass::kCreate)) {
    params.push_back({UniqueParamName(params, base), PinKind::kFloat});
    Edited();
  }
  ImGui::PopID();
}

void NodeEditorWindow::DrawDispatcherDetails() {
  graph::Graph& graph = document_->graph();
  graph::Dispatcher* dispatcher = graph.FindDispatcher(selected_dispatcher_);
  if (!dispatcher) return;
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(PinColor(PinKind::kDelegate)));
  ImGui::SeparatorText("Event Dispatcher");
  ImGui::PopStyleColor();
  ImGui::TextWrapped("Events bound to it with Bind Event run when it's called, with its params. Drag it onto the graph to call it or bind to it.");

  std::string name = dispatcher->name;
  if (node_widgets::InputString("Name", name, ImGuiInputTextFlags_EnterReturnsTrue) && name != dispatcher->name) {
    std::string reason;
    if (!CheckName(graph, name, &reason)) {
      SetStatus(reason, true);
    } else {
      // The nodes using it follow.
      graph::RenameDispatcher(graph, dispatcher->name, name);
      selected_dispatcher_ = name;
      Edited();
    }
  }
  ImGui::SetItemTooltip("Press Enter to rename. The nodes using it follow");

  // Call's inputs and the outputs of the custom events taking its params follow, with their links.
  const std::string current = dispatcher->name;
  const auto renamed = [&graph, current](const std::string& from, const std::string& to) { graph::RenameDispatcherParam(graph, current, from, to); };
  DrawParamList("Params", dispatcher->params, graph::ParamKinds(RoutineKind::kFunction), "Add Param", "Value", [](const std::string&) { return std::string(); }, renamed);

  ImGui::Spacing();
  if (node_widgets::InputStringMultiline("Comment", dispatcher->comment, ImVec2(0.0f, ImGui::GetTextLineHeight() * 3.0f))) Edited(false);
  if (ImGui::IsItemDeactivatedAfterEdit()) Edited();

  ImGui::Spacing();
  if (glass::Button("Delete Event Dispatcher", glass::kDanger)) {
    // The nodes using it go with it; custom events taking its params keep going without them.
    graph::DeleteDispatcher(graph, dispatcher->name);
    selected_dispatcher_.clear();
    focus_ = Focus::kNone;
    Edited();
  }
}

void NodeEditorWindow::DrawProblems() {
  const std::vector<graph::Problem>& problems = document_->compiled().problems;
  if (!problems.empty()) {
    ImGui::SeparatorText("Problems");
    for (size_t i = 0; i < problems.size(); ++i) {
      const graph::Problem& problem = problems[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::PushStyleColor(ImGuiCol_Text, problem.warning ? kWarningColor : kErrorColor);
      const Node* node = document_->graph().FindNode(problem.node);
      const std::string text = node ? graph::TitleOf(*node) + ": " + problem.message : problem.message;
      ImGui::TextWrapped("%s", text.c_str());
      ImGui::PopStyleColor();
      if (node) {
        ImGui::SetItemTooltip("Click to find it on the canvas");
        // In whichever graph it's in: the event graph or a function's or macro's.
        if (ImGui::IsItemClicked()) {
          ShowGraph(node->owner);
          navigate_to_ = node->id;
        }
      }
      ImGui::PopID();
    }
  }

  // What happened when the scripts last ran.
  for (const lua::ScriptMod& loaded : lua::Loaded()) {
    if (loaded.name != mod_folder_ || loaded.error.empty()) continue;
    ImGui::SeparatorText("Last run");
    ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
    ImGui::TextWrapped("%s", loaded.error.c_str());
    ImGui::PopStyleColor();
  }
}

// Mods and graphs ----------------------------------------------------------------------------------------

const ModInfo* NodeEditorWindow::Mod() const { return mod_folder_.empty() ? nullptr : model_.mods().Find(mod_folder_); }

void NodeEditorWindow::OpenMod(const std::string& folder) {
  mod_folder_ = folder;
  Last() = {folder, {}};
  document_.reset();
  RefreshGraphs();
  if (!graphs_.empty()) OpenGraph(graphs_.front());
}

void NodeEditorWindow::OpenGraph(const std::string& name) {
  const ModInfo* mod = Mod();
  if (!mod) return;
  auto document = std::make_unique<graph::Document>(mod->dir, name);
  std::string error;
  if (!document->Load(&error)) {
    SetStatus("Couldn't open " + name + ": " + error, true);
    return;
  }
  document_ = std::move(document);
  Last() = {mod_folder_, name};
  owner_ = 0;
  focus_ = Focus::kNone;
  selected_node_ = 0;
  selected_variable_.clear();
  selected_routine_ = 0;
  selected_dispatcher_.clear();
  pending_pin_.reset();
  ResetCanvas();
}

void NodeEditorWindow::ResetCanvas() {
  // A canvas per graph, so each opens fresh, fitted to its nodes.
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
  moving_ = false;
}

void NodeEditorWindow::Save(bool reload) {
  if (!document_) return;
  std::string error;
  if (!document_->Save(&error)) {
    SetStatus(error, true);
    return;
  }
  RefreshGraphs();
  const ModInfo* mod = Mod();
  if (!reload) {
    SetStatus("Saved " + document_->name());
    return;
  }
  model_.ReloadScripts();
  if (mod && !mod->enabled) SetStatus("Saved. " + mod->name + " is off, so its scripts don't run; turn it on in the mods menu", true);
  else if (!document_->compiled().ok()) SetStatus("Saved and reloaded, but this graph has problems, so it doesn't run", true);
  else SetStatus("Saved and reloaded");
}

void NodeEditorWindow::CreateMod(const std::string& name) {
  if (!graph::IsFileName(name)) {
    SetStatus("A mod's name can have letters, digits, spaces, _ and -", true);
    return;
  }
  const std::filesystem::path dir = model_.mods().root() / name;
  std::error_code code;
  if (std::filesystem::exists(dir, code)) {
    SetStatus("There's a mod folder named " + name + " already", true);
    return;
  }
  std::filesystem::create_directories(dir / "scripts", code);
  if (code) {
    SetStatus("Couldn't make " + dir.string() + ": " + code.message(), true);
    return;
  }
  std::ofstream(dir / "mod.toml") << "name = \"" << name << "\"\nversion = \"1.0.0\"\ndescription = \"Made with the node editor\"\n";
  // Enabled at the end of the load order, and the menus rescan.
  SetModEnabled(name, true);
  model_.Poll();
  OpenMod(name);
  CreateGraph("Main");
}

void NodeEditorWindow::CreateGraph(const std::string& name) {
  const ModInfo* mod = Mod();
  if (!mod) return;
  if (!graph::IsFileName(name)) {
    SetStatus("A graph's name can have letters, digits, spaces, _ and -", true);
    return;
  }
  if (std::find(graphs_.begin(), graphs_.end(), name) != graphs_.end()) {
    SetStatus("There's a graph named " + name + " already", true);
    return;
  }
  document_ = std::make_unique<graph::Document>(mod->dir, name);
  Last() = {mod_folder_, name};
  owner_ = 0;
  focus_ = Focus::kNone;
  selected_node_ = 0;
  selected_variable_.clear();
  ResetCanvas();
  Save(false);
}

void NodeEditorWindow::RenameGraph(const std::string& name) {
  if (!document_ || name == document_->name()) return;
  if (!graph::IsFileName(name)) {
    SetStatus("A graph's name can have letters, digits, spaces, _ and -", true);
    return;
  }
  // Saved first, so what's renamed is what's on screen.
  std::string error;
  if (!document_->Save(&error) || !graph::RenameGraph(document_->mod_dir(), document_->name(), name, &error)) {
    SetStatus(error, true);
    return;
  }
  RefreshGraphs();
  OpenGraph(name);
  SetStatus("Renamed to " + name);
}

void NodeEditorWindow::DeleteGraph() {
  if (!document_) return;
  std::string error;
  const std::string name = document_->name();
  if (!graph::DeleteGraph(document_->mod_dir(), name, &error)) {
    SetStatus(error, true);
    return;
  }
  document_.reset();
  RefreshGraphs();
  if (!graphs_.empty()) OpenGraph(graphs_.front());
  SetStatus("Deleted " + name);
}

void NodeEditorWindow::RefreshGraphs() {
  const ModInfo* mod = Mod();
  graphs_ = mod ? graph::ListGraphs(mod->dir) : std::vector<std::string>{};
}

void NodeEditorWindow::AfterSaving(std::function<void()> action) {
  if (!document_ || !document_->dirty()) {
    action();
    return;
  }
  after_saving_ = std::move(action);
  open_save_prompt_ = true;
}

void NodeEditorWindow::AskFor(Ask ask, std::string initial) {
  ask_ = ask;
  ask_text_ = std::move(initial);
  open_ask_ = true;
}

void NodeEditorWindow::DrawPopups() {
  // Opened here, at the window's level, so they open the same from the toolbar, the sidebar or the canvas.
  if (open_ask_) {
    open_ask_ = false;
    ImGui::OpenPopup(kAskPopup);
  }
  if (open_save_prompt_) {
    open_save_prompt_ = false;
    ImGui::OpenPopup(kSavePopup);
  }

  const char* titles[] = {"", "New Mod", "New Graph", "Rename Graph", "Delete Graph"};
  const std::string ask_title = std::string(titles[static_cast<int>(ask_)]) + kAskPopup;
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 24.0f, 0.0f));
  if (ImGui::BeginPopupModal(ask_title.c_str(), nullptr, ImGuiWindowFlags_NoResize)) {
    bool done = false;
    if (ask_ == Ask::kDeleteGraph) {
      ImGui::TextWrapped("Delete %s and its script? This can't be undone.", ask_text_.c_str());
      if (glass::Button("Delete", glass::kDanger)) {
        DeleteGraph();
        done = true;
      }
    } else {
      if (ask_ == Ask::kNewMod) ImGui::TextWrapped("A folder in the mods folder, with a graph to start.");
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      const bool entered = node_widgets::InputString("Name", ask_text_, ImGuiInputTextFlags_EnterReturnsTrue);
      if (glass::Button("OK", glass::kAccent) || entered) {
        const std::string name = ask_text_;
        if (ask_ == Ask::kNewMod) AfterSaving([this, name] { CreateMod(name); });
        if (ask_ == Ask::kNewGraph) AfterSaving([this, name] { CreateGraph(name); });
        if (ask_ == Ask::kRenameGraph) RenameGraph(name);
        done = true;
      }
    }
    ImGui::SameLine();
    if (glass::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done = true;
    if (done) {
      ask_ = Ask::kNone;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (ImGui::BeginPopupModal(kSavePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Save the changes to %s first?", document_ ? document_->name().c_str() : "the graph");
    std::function<void()> next;
    if (glass::Button("Save", glass::kAccent)) {
      Save(false);
      if (document_ && !document_->dirty()) next = std::move(after_saving_);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (glass::Button("Don't Save", glass::kDanger)) {
      next = std::move(after_saving_);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (glass::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      after_saving_ = {};
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    // After EndPopup, as it may close the window.
    if (next) next();
  }
}

void NodeEditorWindow::Edited(bool settled) {
  if (!document_) return;
  document_->Edited(settled);
}

void NodeEditorWindow::Undo() {
  if (!document_ || !document_->Undo()) return;
  AfterHistory();
}

void NodeEditorWindow::Redo() {
  if (!document_ || !document_->Redo()) return;
  AfterHistory();
}

void NodeEditorWindow::AfterHistory() {
  // The graph was replaced: the canvas places its nodes again, and what was shown or selected may be gone.
  graph::Graph& graph = document_->graph();
  place_nodes_ = true;
  if (owner_ != 0 && !graph.FindRoutine(owner_)) ShowGraph(0);
  if (!graph.FindNode(selected_node_)) selected_node_ = 0;
  if (!graph.FindVariable(selected_variable_)) selected_variable_.clear();
  if (!graph.FindRoutine(selected_routine_)) selected_routine_ = 0;
  if (!graph.FindDispatcher(selected_dispatcher_)) selected_dispatcher_.clear();
}

void NodeEditorWindow::SetStatus(std::string text, bool error) {
  status_ = std::move(text);
  status_error_ = error;
  status_time_ = ImGui::GetTime();
}

}  // namespace rexfabric::ui
