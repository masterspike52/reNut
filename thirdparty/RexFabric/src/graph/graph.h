// A node graph: what the node editor edits and the Lua compiler reads.
//
// Pure data. Which pins a node has, and which links make sense, is the node library's business
// (node_library.h); this only keeps the nodes, the links between them and the graph's variables.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace rexfabric::graph {

// What flows along a link. Exec links carry the order things run in; the rest carry values.
enum class PinKind : uint8_t {
  kExec,
  kBool,
  kInt,
  kFloat,
  kNumber,     // an int or a float, whichever the node's number inputs are: Add of two ints is an int
  kString,
  kObject,     // an object of one of the game's types
  kRegisters,  // a hook's or a midasm site's registers
  kDelegate,   // an event's chain, handed to something that runs it later: a timer, an event dispatcher
  kAny,        // whatever Lua value comes in
};

const char* PinKindName(PinKind kind);
bool ParsePinKind(std::string_view name, PinKind& kind);

struct Node {
  int id = 0;
  // The graph the node is in: 0 for the event graph, otherwise the id of the function or macro whose graph it is.
  int owner = 0;
  // A node type from the library: "flow.branch", "game.call:tip.version".
  std::string type;
  float x = 0.0f;
  float y = 0.0f;
  // Choices made on the node rather than through a pin: an address, a key, a register.
  std::map<std::string, std::string, std::less<>> settings;
  // Values typed into unlinked inputs, by pin name. A missing one is the pin's default.
  std::map<std::string, nlohmann::json, std::less<>> values;

  std::string Setting(std::string_view name) const;
};

struct PinRef {
  int node = 0;
  std::string pin;
  bool operator==(const PinRef&) const = default;
};

// From an output pin to an input pin. Pins are named, so links survive a node type gaining pins.
struct Link {
  int id = 0;
  PinRef from;
  PinRef to;
};

// A value the whole graph shares, kept between events. It compiles to a local at the top of the script.
struct Variable {
  std::string name;
  PinKind type = PinKind::kFloat;
  nlohmann::json initial;
  std::string comment;
};

// A function or a macro: a graph of its own, used from other graphs through call nodes. Its nodes are in the same
// Graph as everything else, owned by its id; an entry node gives its inputs and an exit node takes its outputs.
//
// A function compiles to a Lua function, called where it's used; one marked pure has no exec pins and is worked out
// where its outputs are read. A macro is written out in full wherever it's used, so its inputs and outputs can be
// exec pins too: a macro can be flow of its own, like Branch.
enum class RoutineKind : uint8_t { kFunction, kMacro };

struct Param {
  std::string name;
  PinKind type = PinKind::kFloat;
};

// A list of events to run, as Unreal's event dispatchers are: Bind adds an event to it, Call runs every event bound to
// it, with its params. It compiles to a table at the top of the script, named after it.
struct Dispatcher {
  std::string name;
  std::vector<Param> params;
  std::string comment;
};

struct Routine {
  int id = 0;  // at least 1; the owner of its nodes
  RoutineKind kind = RoutineKind::kFunction;
  std::string name;
  std::vector<Param> inputs;
  std::vector<Param> outputs;
  bool pure = false;  // functions only
  std::string comment;
};

class Graph {
 public:
  Node& AddNode(std::string type, float x, float y);
  // Keeps the node's own id, as loaded from a file.
  Node& InsertNode(Node node);
  bool RemoveNode(int id);

  // Exec outputs and data inputs hold one link, so a link already at that end is replaced. Exec inputs and data
  // outputs take any number.
  void AddLink(const PinRef& from, const PinRef& to, bool exec);
  // Adds a link as stored in a file, replacing nothing.
  void InsertLink(const PinRef& from, const PinRef& to);
  void RemoveLink(int id);
  // Drops every link at this pin.
  void RemoveLinksAt(const PinRef& pin, bool output);
  // A pin of a node now goes by `name`: its links follow, and so does a value typed into it.
  void RenamePin(const PinRef& pin, bool output, const std::string& name);

  std::vector<Node>& nodes() { return nodes_; }
  const std::vector<Node>& nodes() const { return nodes_; }
  const std::vector<Link>& links() const { return links_; }
  std::vector<Variable>& variables() { return variables_; }
  const std::vector<Variable>& variables() const { return variables_; }

  // A routine with a new id, and no nodes yet (node_library.h's CreateRoutine adds its entry and exit).
  Routine& AddRoutine(RoutineKind kind, std::string name);
  // Keeps the routine's own id, as loaded from a file.
  Routine& InsertRoutine(Routine routine);
  // Removes the routine and every node in its graph.
  void RemoveRoutine(int id);
  std::vector<Dispatcher>& dispatchers() { return dispatchers_; }
  const std::vector<Dispatcher>& dispatchers() const { return dispatchers_; }
  Dispatcher* FindDispatcher(std::string_view name);
  const Dispatcher* FindDispatcher(std::string_view name) const;
  // "Done", "Done1": a dispatcher name nothing uses yet.
  std::string UniqueDispatcherName(std::string_view base) const;
  // Variables, functions, macros and dispatchers share one set of names, as each is a Lua name in the script.
  bool NameTaken(std::string_view name) const;

  std::vector<Routine>& routines() { return routines_; }
  const std::vector<Routine>& routines() const { return routines_; }
  Routine* FindRoutine(int id);
  const Routine* FindRoutine(int id) const;
  // "Function", "Function1": a routine name nothing uses yet.
  std::string UniqueRoutineName(std::string_view base) const;

  Node* FindNode(int id);
  const Node* FindNode(int id) const;
  Variable* FindVariable(std::string_view name);
  const Variable* FindVariable(std::string_view name) const;

  bool HasLinkAt(const PinRef& pin, bool output) const;
  // The one link into a data input, or null.
  const Link* LinkInto(const PinRef& input) const;
  // Every link out of an output.
  std::vector<const Link*> LinksOutOf(const PinRef& output) const;

  // "Speed", "Speed1", "Speed2": a variable name nothing uses yet.
  std::string UniqueVariableName(std::string_view base) const;

 private:
  std::vector<Node> nodes_;
  std::vector<Link> links_;
  std::vector<Variable> variables_;
  std::vector<Routine> routines_;
  std::vector<Dispatcher> dispatchers_;
  int next_node_ = 1;
  int next_link_ = 1;
  int next_routine_ = 1;
};

}  // namespace rexfabric::graph
