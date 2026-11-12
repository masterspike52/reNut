// The node editor: makes Lua mods out of node graphs, in game. Built when the project attaches RexFabric
// WITH_NODE_EDITOR.
//
// It picks a mod and one of its graphs, edits the graph on a canvas, and saves it with the Lua it compiles to
// (graph_document.h), which Save and Reload then runs. Laid out like a blueprint editor: the mod's graphs, the
// graph's functions, macros and variables on the left, the canvas and the Lua it makes in the middle, the selected
// node, function, macro or variable and the graph's problems on the right. With RexGlass the whole window is the
// canvas's drifting light, with the panels around the canvas glass over it, as the nodes are.
//
// The canvas shows one graph at a time: the event graph, or the graph of a function or macro, opened from the
// sidebar or from a node using it. On the canvas the wheel zooms, a right drag pans, a right click adds a node or,
// on a node, shows what can be done with it, and dragging a link into empty space adds a node connected to it.
// Double clicking a link adds a reroute there, a point to drag the link around things by; deleting a reroute joins
// its link up again. A variable dragged in from the sidebar asks whether to get or set it (Ctrl drops a Get, Alt a Set); a function or
// macro dragged in is called; an event dispatcher dragged in asks whether to call it, bind to it or unbind from it.
//
// Events that can be run by a timer or a dispatcher have a square delegate pin in their title's top right corner, as
// in Unreal: linked to Set Timer by Event or a dispatcher's Bind Event, the event's chain runs when they say. Delete removes the selection; Ctrl+C, Ctrl+X, Ctrl+V and Ctrl+D copy, cut, paste and
// duplicate. Ctrl+S saves, Ctrl+Z and Ctrl+Y undo and redo. Unlinked inputs are typed into on the node itself.

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <rex/ui/ui_system.h>

#include "../graph/graph_document.h"
#include "menu_model.h"
#include "node_graph_style.h"

struct ImVec2;

namespace ax::NodeEditor {
struct EditorContext;
}

namespace rexfabric::ui {

class NodeEditorWindow : public rex::ui::DialogBase {
 public:
  // `closed` runs as the window goes.
  NodeEditorWindow(MenuModel& model, std::function<void()> closed);
  ~NodeEditorWindow() override;

  // Closes the window, once unsaved changes are saved or let go.
  void Hide();

  bool Draw(ImGuiIO& io) override;

 private:
  void OnDraw(ImGuiIO& io);

  enum class Focus { kNone, kNode, kVariable, kRoutine, kDispatcher };
  using Settings = decltype(graph::Node::settings);
  // The names the window asks for in a popup.
  enum class Ask { kNone, kNewMod, kNewGraph, kRenameGraph, kDeleteGraph };

  // A pin a link was dragged from into empty space; the node added next connects to it.
  struct PendingPin {
    graph::PinRef ref;
    bool output = false;
  };

  // Top
  void DrawToolbar();
  void DrawModPicker();
  void Shortcuts();

  // Left
  void DrawGraphs();
  void DrawRoutines(graph::RoutineKind kind);
  void DrawVariables();
  void DrawDispatchers();

  // Middle
  void DrawBreadcrumb();
  // Shows the event graph (0) or a function's or macro's graph on the canvas.
  void ShowGraph(int owner);
  void DrawCanvas();
  // Fits the graph shown to the view, zoomed out when it's big but never in past its size.
  void FitView();
  void DrawNode(graph::Node& node);
  void DrawCompactNode(graph::Node& node, const graph::NodePins& pins);
  void DrawReroute(graph::Node& node);
  // A double clicked link gets a reroute where it was clicked.
  void HandleDoubleClick();
  void DrawInlineValue(graph::Node& node, const graph::Pin& pin);
  // The list an input with choices (Pin::choices) is picked from, with a search box.
  void DrawChoiceMenu();
  // An input's choices: a live list as it was when last opened (worked out the first time it's needed).
  const graph::ChoiceList& ChoicesOf(const graph::ChoiceSource& source, bool refresh = false);
  void HandleCreate();
  void HandleDelete();
  void HandleClipboard();
  void PasteNodes(const std::string& text, float x, float y);
  void DrawAddMenu();
  void AddNode(const std::string& type, const Settings& settings);
  // A drop from the sidebar onto the canvas, at `at` in canvas units.
  void AcceptDrop(const ImVec2& at);
  void DrawDropMenu();
  void DrawNodeMenu();
  // Makes a node another type, keeping its settings: a Bool's Get into its If.
  void ConvertNode(int node, const std::string& type);
  void SyncFromCanvas();
  void DrawLua();

  // Right
  void DrawDetails();
  void DrawNodeDetails();
  void DrawVariableDetails();
  void DrawRoutineDetails();
  void DrawParams(graph::Routine& routine, bool outputs);
  void DrawDispatcherDetails();
  // A kList setting's entries: a switch's cases, a select's options. They're pins of the node, so renaming one takes
  // its links along.
  void DrawListSetting(graph::Node& node, const graph::SettingDesc& setting);
  // A kScript setting: the file's name, and its code to read, edit and save apart from the graph.
  void DrawScriptSetting(graph::Node& node, const graph::SettingDesc& setting);
  // A list of params, each with its name, type and a remove button, and a button adding one. `refuse` says why a new
  // name can't be taken, or nothing; `renamed` makes the pins using a param follow its new name.
  void DrawParamList(const char* title, std::vector<graph::Param>& params, const std::vector<graph::PinKind>& kinds, const char* add_label, const std::string& base, const std::function<std::string(const std::string& name)>& refuse, const std::function<void(const std::string& from, const std::string& to)>& renamed);
  void DrawProblems();

  // Mods and graphs
  const ModInfo* Mod() const;
  void OpenMod(const std::string& folder);
  void OpenGraph(const std::string& name);
  void Save(bool reload);
  void CreateMod(const std::string& name);
  void CreateGraph(const std::string& name);
  void RenameGraph(const std::string& name);
  void DeleteGraph();
  void RefreshGraphs();

  // Runs `action` now, or once the open graph's unsaved changes are saved or let go.
  void AfterSaving(std::function<void()> action);
  void DrawPopups();
  void AskFor(Ask ask, std::string initial);

  // After an edit to the graph. See graph::Document::Edited.
  void Edited(bool settled = true);
  void Undo();
  void Redo();
  void AfterHistory();
  void SetStatus(std::string text, bool error = false);
  void ResetCanvas();

  MenuModel& model_;
  std::function<void()> closed_;
  // The UI drops the window on the next Draw.
  bool closing_ = false;

  std::string mod_folder_;
  std::vector<std::string> graphs_;
  std::unique_ptr<graph::Document> document_;
  ax::NodeEditor::EditorContext* canvas_ = nullptr;
  NodeGraphStyle style_;

  // The graph on the canvas: 0 for the event graph, or a function's or macro's id.
  int owner_ = 0;

  Focus focus_ = Focus::kNone;
  int selected_node_ = 0;
  std::string selected_variable_;
  int selected_routine_ = 0;
  std::string selected_dispatcher_;
  std::set<int> problem_nodes_;
  int navigate_to_ = 0;
  // What the pin under the mouse is, shown once the canvas is suspended.
  std::string pin_tooltip_;

  bool place_nodes_ = true;
  bool fit_view_ = true;
  bool moving_ = false;

  bool open_add_menu_ = false;
  float add_x_ = 0.0f;
  float add_y_ = 0.0f;
  std::string add_filter_;
  std::optional<PendingPin> pending_pin_;

  // The variable or dispatcher dropped on the canvas, asking what to make of it, and the node right clicked.
  std::string dropped_member_;
  bool dropped_dispatcher_ = false;
  bool open_drop_menu_ = false;
  int context_node_ = 0;
  bool open_node_menu_ = false;

  // The input whose choices are being picked from.
  int choice_node_ = 0;
  std::string choice_pin_;
  std::string choice_filter_;
  bool open_choice_menu_ = false;
  std::map<const graph::ChoiceSource*, graph::ChoiceList> live_choices_;

  Ask ask_ = Ask::kNone;
  bool open_ask_ = false;
  std::string ask_text_;
  std::function<void()> after_saving_;
  bool open_save_prompt_ = false;

  std::string status_;
  bool status_error_ = false;
  double status_time_ = 0.0;
  std::string lua_view_;

  // The script file a Script File node shows (DrawScriptSetting): its code as edited and as last read or saved.
  std::filesystem::path script_file_;
  std::string script_text_;
  std::string script_saved_;
  bool script_exists_ = false;
};

}  // namespace rexfabric::ui
