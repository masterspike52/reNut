// Every kind of node, and the rules for linking them.
//
// A node type is a NodeDesc: its pins, the choices made on it (settings), and how it becomes Lua. The built-in
// nodes (nodes_builtin.cpp) cover the rex API and plain logic; the game's nodes (nodes_game.cpp) are made from
// whatever the game declared with rexfabric::Namespace, so a game's functions, types and events show up without
// anyone writing nodes for them.
//
// To add a node, add a NodeDesc to one of those files. Its emit or expression writes the Lua (lua_compiler.h).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "graph.h"

namespace rexfabric {
struct EnumDef;
}

namespace rexfabric::graph {

class Emitter;

using ChoiceList = std::vector<std::pair<std::string, std::string>>;  // label, value as typed

// What an input is picked from: a list fixed when its node was made, or one the game works out when it's shown.
struct ChoiceSource {
  ChoiceList fixed;
  std::function<ChoiceList()> live;
  bool empty() const { return fixed.empty() && !live; }
};

struct PinType {
  PinKind kind = PinKind::kExec;
  // kObject: the game type's full name, "tip.Pinata". Empty is any object.
  std::string object;
  bool operator==(const PinType&) const = default;
};

// A pin as one node has it.
struct Pin {
  std::string name;
  PinType type;
  // An unlinked data input's value until one is typed: a bool for Bool, a number for Float, text for the rest.
  nlohmann::json initial;
  // Has to be linked. Objects and registers can't be typed in.
  bool required = false;
  // Takes whatever links to it first, exec or data, and is that type from then on: an unlinked reroute's pins.
  bool wildcard = false;
  // An unlinked input picked from these instead of typed in (an argument of a game enum or list), or any typed value.
  const ChoiceSource* choices = nullptr;
};

struct NodePins {
  std::vector<Pin> inputs;
  std::vector<Pin> outputs;
};

enum class SettingKind : uint8_t {
  kAddress,   // a guest address, "0x8266FBB0"
  kText,      // free text: a cvar's or a switch's name
  kChoice,    // one of `choices`: a key, a register, a type
  kVariable,    // one of the graph's variables
  kDispatcher,  // one of the graph's event dispatchers
  kList,        // entries added in the details panel, one a line: a switch's cases, a select's options
  kScript,      // a file in the mod's scripts folder, its code read and edited in the details panel
  kCode,        // Lua typed into the details panel, several lines
};

struct SettingDesc {
  std::string name;
  SettingKind kind = SettingKind::kText;
  std::string initial;
  std::vector<std::string> choices;
  std::string tooltip;
  // kVariable: the types of variable it can pick. Empty for any.
  std::vector<PinKind> kinds;
  // kVariable, kDispatcher: may pick none, leaving the setting empty.
  bool optional = false;
  // kList: what one entry is called, for the details panel's Add button: "Case".
  std::string item;
  // Whether the details panel shows the setting for this node; unset, always. A switch on an enum has no cases to add.
  std::function<bool(const Node& node)> shown;
};

// The setting lets the node pick a variable of this type.
bool AcceptsVariable(const SettingDesc& setting, PinKind kind);

// A kList setting's entries, and the setting's text for them.
std::vector<std::string> ListOf(const Node& node, std::string_view setting);
std::string JoinList(const std::vector<std::string>& entries);

enum class NodeRole : uint8_t {
  kEvent,   // starts a chain; no exec input
  kAction,  // runs when a chain reaches its exec input
  kPure,    // no exec pins; worked out wherever its outputs are read
};

struct NodeDesc {
  std::string id;  // stable: stored in graph files
  std::string title;
  // "Flow", or "Game/tip/Functions": slashes nest the add menu.
  std::string category;
  std::string description;
  // More to find the node by in the add menu's search: its operator, "<" or "++".
  std::string keywords;
  NodeRole role = NodeRole::kAction;
  // Drawn as one row: the title and the node's one output, without its label. A variable's Get.
  bool compact = false;
  // An event with a delegate pin in its title's top right corner (kDelegatePin), as in Unreal: linked to a timer or a
  // dispatcher's Bind, its chain runs when they say as well as when the event happens. Hooks have none, as their
  // chains need the registers of the call they run in.
  bool delegate = false;
  NodePins pins;
  std::vector<SettingDesc> settings;
  // Pins that depend on the node's settings or on the graph: a register's type, a variable's, a number of
  // arguments. Runs after `pins` are copied in.
  std::function<void(const Node& node, const Graph& graph, NodePins& pins)> shape;
  // The title with the node's settings in it, "Before 0x8266FBB0". Unset: `title`.
  std::function<std::string(const Node& node)> title_of;
  // Events and actions write Lua statements. Pure nodes give an expression for each output instead.
  std::function<void(Emitter& emit)> emit;
  std::function<std::string(Emitter& emit, std::string_view output)> expression;
  // The role for a node whose role depends on it: a call to a pure function is pure. Unset: `role`.
  std::function<NodeRole(const Node& node, const Graph& graph)> role_of;
};

// The exec pins most nodes have. Their labels aren't drawn.
inline constexpr std::string_view kExecIn = "Exec";
inline constexpr std::string_view kExecOut = "Then";
// The delegate output of an event that has one, the last of its outputs, and the delegate input of the nodes taking
// one: Set Timer by Event, Bind Event.
inline constexpr std::string_view kDelegatePin = "Event";

class Library {
 public:
  // The built-in nodes and the game's, made on first use, so after the game declared its API.
  static const Library& Get();

  void Add(NodeDesc desc);
  const NodeDesc* Find(std::string_view id) const;
  const std::vector<std::unique_ptr<NodeDesc>>& nodes() const { return nodes_; }

 private:
  std::vector<std::unique_ptr<NodeDesc>> nodes_;
  std::unordered_map<std::string, const NodeDesc*> by_id_;
};

void AddBuiltinNodes(Library& library);
void AddGameNodes(Library& library);
// From the game's Lua API definitions (LuaLS ---@meta files) in lua_api beside its executable (nodes_api.cpp).
void AddApiNodes(Library& library);

// Pins, for the node descriptions.
Pin ExecPin(std::string_view name = kExecIn);
Pin DataPin(std::string_view name, PinKind kind, nlohmann::json initial = nullptr);
Pin ObjectPin(std::string_view name, std::string_view type);
// A delegate input, which has to be linked to an event's delegate pin.
Pin DelegatePin(std::string_view name = kDelegatePin);
// What an unlinked input of this kind starts as.
nlohmann::json DefaultValue(PinKind kind);

const NodeDesc* FindDesc(const Node& node);
NodeRole RoleOf(const Node& node, const Graph& graph);
// The node's pins: its description's, shaped by its settings. None for a type the library doesn't know.
NodePins PinsOf(const Node& node, const Graph& graph);
const Pin* FindPin(const std::vector<Pin>& pins, std::string_view name);
// A setting's value, or its description's initial value when the node doesn't have it.
std::string SettingOf(const Node& node, std::string_view name);
std::string TitleOf(const Node& node);
// A name as a title: underscores as spaces, camel case split into words, every word capitalized.
// "set_emissive" is "Set Emissive", "RexLiveless" is "Rex Liveless"; dots and colons stay.
std::string TitleCase(std::string_view name);

// "tip.garden.money" as Lua reaches it: tip.garden.money, or tip["odd name"] for a part that isn't a name.
std::string LuaPath(std::string_view name);
// A field of a table: .name, or ["odd name"].
std::string LuaMember(std::string_view name);
// One of the enums the game declared, by its full name ("tip.Species"), or null.
const EnumDef* FindEnum(std::string_view name);
// Every enum the game declared, by full name.
std::vector<std::string> EnumNames();

// "Int", "Float", "tip.Pinata": a type as messages and tooltips name it.
std::string TypeName(const PinType& type);
// Typed number text with a fraction or an exponent, so a Number pin it's typed into is a float.
bool IsFloatText(std::string_view text);

// The type an output carries, with Number resolved to Int or Float from what goes into the node.
PinType OutputType(const Graph& graph, const PinRef& output);
// A value of `from` can go into an input of `to`. Ints go into floats; anything goes into and comes from Any.
bool Fits(const PinType& from, const PinType& to);

// From an output to an input of another node, with fitting types and no loop through data links. Sets `reason`
// when the answer is no.
bool CanConnect(const Graph& graph, const PinRef& from, const PinRef& to, std::string* reason = nullptr);
bool Connect(Graph& graph, const PinRef& from, const PinRef& to);
// Drops links to pins their nodes no longer have, after a setting took a pin away.
void RemoveDanglingLinks(Graph& graph);

// Functions and macros (graph.h's Routine). A routine's graph has one entry node, whose outputs are the routine's
// inputs, and one exit node, whose inputs are its outputs. Call nodes name the routine by id in this setting.
inline constexpr std::string_view kRoutineEntry = "routine.entry";
inline constexpr std::string_view kRoutineExit = "routine.exit";
inline constexpr std::string_view kRoutineCall = "routine.call";
inline constexpr std::string_view kRoutineSetting = "Routine";
// The routine's name, kept on its entry and its calls for their titles; RenameRoutine keeps it current.
inline constexpr std::string_view kRoutineNameSetting = "Name";

// The routine a call names, or whose entry or exit this is.
const Routine* RoutineOf(const Node& node, const Graph& graph);
const Node* EntryOf(const Graph& graph, int routine);
const Node* ExitOf(const Graph& graph, int routine);
// A routine with its entry and exit nodes.
Routine& CreateRoutine(Graph& graph, RoutineKind kind, std::string name);
// Removes a routine, its graph, and the nodes calling it.
void DeleteRoutine(Graph& graph, int id);
void RenameRoutine(Graph& graph, int id, const std::string& name);
// Reroutes: a point a link passes through, to lead it around things. Its two pins are the type of what comes in, or
// else of what it goes to; with no links at all they're wildcards, and link to anything. Exec chains run through it,
// and data is read through it.
inline constexpr std::string_view kReroute = "flow.reroute";
inline constexpr std::string_view kRerouteIn = "In";
inline constexpr std::string_view kRerouteOut = "Out";

PinType RerouteType(const Graph& graph, const Node& reroute);
// Splits a link with a reroute at (x, y). Returns the reroute's id, or 0 when there's no such link.
int InsertReroute(Graph& graph, int link, float x, float y);
// Removes a reroute and links what came into it straight to what it went to.
void DissolveReroute(Graph& graph, int reroute);

// The types a routine's inputs and outputs can have. Macros can have exec ones too.
std::vector<PinKind> ParamKinds(RoutineKind kind);
// An input (or, with `output`, an output) of a routine is renamed: the pins of its entry or exit and of its calls
// follow, with their links.
void RenameRoutineParam(Graph& graph, int routine, bool output, const std::string& from, const std::string& to);

// Variables and dispatchers: a node uses one when a setting of its description's picks it (Get, Set, If, Call, Bind).
bool UsesMember(const Node& node, SettingKind kind, std::string_view name);
bool UsesVariable(const Node& node, std::string_view name);
// Renames a variable and the nodes using it.
void RenameVariable(Graph& graph, const std::string& from, const std::string& to);
// Removes a variable and the nodes using it.
void DeleteVariable(Graph& graph, const std::string& name);
// Renames a dispatcher and the nodes using it.
void RenameDispatcher(Graph& graph, const std::string& from, const std::string& to);
// Removes a dispatcher and the nodes using it. Custom events that only took their params from it keep going.
void DeleteDispatcher(Graph& graph, const std::string& name);
// A dispatcher's param is renamed: the pins of its Call nodes and of the custom events taking its params follow.
void RenameDispatcherParam(Graph& graph, const std::string& dispatcher, const std::string& from, const std::string& to);

}  // namespace rexfabric::graph
