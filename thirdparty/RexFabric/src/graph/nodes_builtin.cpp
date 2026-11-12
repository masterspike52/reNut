// The nodes every game has: events, flow, the rex API (hooks, registers, memory, input, cvars, switches,
// logging) and plain logic and math. Each one's Lua is what a mod would write by hand; lua_mods.md is the API
// they stand for.

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include <rexfabric/game_api.h>

#include "lua_compiler.h"
#include "node_library.h"

namespace rexfabric::graph {
namespace {

// The key names the game's input understands.
const std::vector<std::string>& KeyNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> keys;
    for (int i = 1; i <= 12; ++i) keys.push_back("F" + std::to_string(i));
    for (char c = 'A'; c <= 'Z'; ++c) keys.push_back(std::string(1, c));
    for (char c = '0'; c <= '9'; ++c) keys.push_back(std::string(1, c));
    for (const char* key : {"Space", "Return", "Escape", "Tab", "Backspace", "Shift", "Control", "Alt", "Up", "Down", "Left", "Right", "Insert", "Delete", "Home", "End", "PageUp", "PageDown", "Backtick", "Minus", "Plus", "Comma", "Period", "Semicolon", "Slash", "Backslash", "LBracket", "RBracket", "Quote"}) keys.push_back(key);
    for (int i = 0; i <= 9; ++i) keys.push_back("Numpad" + std::to_string(i));
    for (const char* key : {"NumpadEnter", "NumpadPlus", "NumpadMinus", "NumpadStar", "NumpadSlash"}) keys.push_back(key);
    return keys;
  }();
  return names;
}

const std::vector<std::string>& RegisterNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> registers;
    for (int i = 0; i < 32; ++i) registers.push_back("r" + std::to_string(i));
    for (int i = 0; i < 32; ++i) registers.push_back("f" + std::to_string(i));
    registers.push_back("lr");
    registers.push_back("ctr");
    return registers;
  }();
  return names;
}

std::vector<std::string> Counts(int first, int last) {
  std::vector<std::string> counts;
  for (int i = first; i <= last; ++i) counts.push_back(std::to_string(i));
  return counts;
}

int CountOf(const Node& node, std::string_view setting) {
  const std::string text = SettingOf(node, setting);
  return text.empty() ? 0 : std::stoi(text);
}

// What a register holds: f registers floats, the rest integers.
PinKind RegisterKind(std::string_view name) { return !name.empty() && name[0] == 'f' ? PinKind::kFloat : PinKind::kInt; }

// What rex.mem's reader of this type gives back.
PinKind MemoryKind(std::string_view type) {
  if (type == "f32" || type == "f64") return PinKind::kFloat;
  if (type == "string" || type == "wstring") return PinKind::kString;
  return PinKind::kInt;
}

PinKind CvarKind(std::string_view type) {
  if (type == "boolean") return PinKind::kBool;
  if (type == "number") return PinKind::kFloat;
  if (type == "string") return PinKind::kString;
  return PinKind::kInt;
}

// Swaps the pin named `name` for one of `kind`, keeping its place.
void Retype(std::vector<Pin>& pins, std::string_view name, PinKind kind) {
  for (Pin& pin : pins) {
    if (pin.name == name) pin = DataPin(name, kind);
  }
}

std::string Joined(const std::vector<std::string>& parts) {
  std::string joined;
  for (size_t i = 0; i < parts.size(); ++i) joined += (i ? ", " : "") + parts[i];
  return joined;
}

std::string Quoted(Emitter& emit, std::string_view setting) { return LuaQuote(emit.Setting(setting)); }

std::string Titled(const Node& node, std::string_view prefix, std::string_view setting, std::string_view suffix = {}) {
  const std::string value = SettingOf(node, setting);
  return std::string(prefix) + (value.empty() ? "?" : value) + std::string(suffix);
}

// A text setting naming a cvar or switch: empty is an error.
std::string NamedSetting(Emitter& emit, std::string_view setting) {
  if (emit.Setting(setting).empty()) emit.Error("Needs a name");
  return Quoted(emit, setting);
}

// Pure nodes ----------------------------------------------------------------------------------------------

NodeDesc Pure(std::string id, std::string title, std::string category, std::string description, std::vector<Pin> inputs, Pin output, std::function<std::string(Emitter&)> expression) {
  NodeDesc desc{.id = std::move(id), .title = std::move(title), .category = std::move(category), .description = std::move(description), .role = NodeRole::kPure};
  desc.pins = {std::move(inputs), {std::move(output)}};
  desc.expression = [expression = std::move(expression)](Emitter& emit, std::string_view) { return expression(emit); };
  return desc;
}

// A and B into Result, as `(A op B)`. Searching for the operator finds it.
NodeDesc Binary(std::string id, std::string title, std::string category, std::string description, PinKind in, PinKind out, std::string op) {
  NodeDesc desc = Pure(std::move(id), std::move(title), std::move(category), std::move(description), {DataPin("A", in), DataPin("B", in)}, DataPin("Result", out), [op](Emitter& emit) { return "(" + emit.Input("A") + " " + op + " " + emit.Input("B") + ")"; });
  desc.keywords = op;
  return desc;
}

// A node found by searching for `keywords` as well as its title.
NodeDesc Keyed(NodeDesc desc, std::string keywords) {
  desc.keywords = std::move(keywords);
  return desc;
}

// Value into Result, as `function(Value)`.
NodeDesc Unary(std::string id, std::string title, std::string category, std::string description, PinKind in, PinKind out, std::string function) {
  return Pure(std::move(id), std::move(title), std::move(category), std::move(description), {DataPin("Value", in)}, DataPin("Result", out), [function = std::move(function)](Emitter& emit) { return function + "(" + emit.Input("Value") + ")"; });
}

// Events -----------------------------------------------------------------------------------------------

// rex.on(name, function(params) chain end)
void EmitListener(Emitter& emit, const std::string& event, const std::vector<std::string>& params) {
  emit.Line("rex.on(" + event + ", function(" + Joined(params) + ")");
  emit.Open();
  emit.Chain(kExecOut);
  emit.Close();
  emit.Line("end)");
}

void AddEvents(Library& library) {
  const std::string category = "Events";

  library.Add({.id = "event.start", .title = "On Start", .category = category,
      .description = "Runs once as the scripts load, before the game's first frame. Set things up here: settings, variables. The game can't be called yet.",
      .role = NodeRole::kEvent, .delegate = true, .pins = {{}, {ExecPin(kExecOut)}},
      .emit = [](Emitter& emit) {
        emit.Line("do");
        emit.Open();
        emit.Chain(kExecOut);
        emit.Close();
        emit.Line("end");
      }});

  library.Add({.id = "event.frame", .title = "On Frame", .category = category,
      .description = "Runs every frame, with the seconds since the last one.",
      .role = NodeRole::kEvent, .delegate = true, .pins = {{}, {ExecPin(kExecOut), DataPin("Seconds", PinKind::kFloat)}},
      .emit = [](Emitter& emit) { EmitListener(emit, "\"frame\"", {emit.Param("Seconds")}); }});

  library.Add({.id = "event.hook_before", .title = "Before Function", .category = category,
      .description = "Runs each time the game calls the function at Address, before it runs. Registers are its arguments: r3 is the first. Skip Original stops the function itself from running.",
      .role = NodeRole::kEvent, .pins = {{}, {ExecPin(kExecOut), DataPin("Registers", PinKind::kRegisters)}},
      .settings = {{.name = "Address", .kind = SettingKind::kAddress, .tooltip = "The guest function, like 0x82558D28"}},
      .title_of = [](const Node& node) { return Titled(node, "Before ", "Address"); },
      .emit = [](Emitter& emit) {
        emit.CheckHookable("Address");
        emit.Line("rex.hook(" + emit.Address("Address") + ", function(" + emit.Param("Registers") + ")");
        emit.Open();
        emit.Chain(kExecOut);
        if (emit.HasEventLocal("skip")) emit.Line("if " + emit.EventLocal("skip") + " then return false end");
        emit.Close();
        emit.Line("end)");
      }});

  library.Add({.id = "event.hook_after", .title = "After Function", .category = category,
      .description = "Runs each time the function at Address returns. r3 in Registers is what it returned, and can be changed.",
      .role = NodeRole::kEvent, .pins = {{}, {ExecPin(kExecOut), DataPin("Registers", PinKind::kRegisters)}},
      .settings = {{.name = "Address", .kind = SettingKind::kAddress, .tooltip = "The guest function, like 0x8232C048"}},
      .title_of = [](const Node& node) { return Titled(node, "After ", "Address"); },
      .emit = [](Emitter& emit) {
        emit.CheckHookable("Address");
        emit.Line("rex.hook_after(" + emit.Address("Address") + ", function(" + emit.Param("Registers") + ")");
        emit.Open();
        emit.Chain(kExecOut);
        emit.Close();
        emit.Line("end)");
      }});

  library.Add({.id = "event.midasm", .title = "Midasm Site", .category = category,
      .description = "Claims a midasm site the game compiled in. Registers are the ones the site lends, by name. At a branch site, Take Branch takes the branch.",
      .role = NodeRole::kEvent, .pins = {{}, {ExecPin(kExecOut), DataPin("Registers", PinKind::kRegisters)}},
      .settings = {{.name = "Address", .kind = SettingKind::kAddress, .tooltip = "The site's address from the game's midasm config"}},
      .title_of = [](const Node& node) { return Titled(node, "Midasm ", "Address"); },
      .emit = [](Emitter& emit) {
        emit.Line("rex.midasm(" + emit.Address("Address") + ", function(" + emit.Param("Registers") + ")");
        emit.Open();
        emit.Chain(kExecOut);
        if (emit.HasEventLocal("take")) emit.Line("return " + emit.EventLocal("take"));
        emit.Close();
        emit.Line("end)");
      }});

  library.Add({.id = "event.key", .title = "Key Pressed", .category = category,
      .description = "Runs once when the key goes down, while the game window has focus and no menu wants the keyboard.",
      .role = NodeRole::kEvent, .delegate = true, .pins = {{}, {ExecPin(kExecOut)}},
      .settings = {{.name = "Key", .kind = SettingKind::kChoice, .initial = "F9", .choices = KeyNames()}},
      .title_of = [](const Node& node) { return Titled(node, "Key ", "Key", " Pressed"); },
      .emit = [](Emitter& emit) {
        // rex.input.pressed tracks one edge per key, so a second node on the same key would never see a press.
        for (const Node& other : emit.graph().nodes()) {
          if (other.id != emit.node().id && other.type == emit.node().type && SettingOf(other, "Key") == emit.Setting("Key")) emit.Warn("Another Key Pressed node waits for " + emit.Setting("Key") + "; only one of them sees each press");
        }
        emit.Line("rex.on(\"frame\", function()");
        emit.Open();
        emit.Line("if rex.input.pressed(" + Quoted(emit, "Key") + ") and rex.input.accepts() then");
        emit.Indent();
        emit.Chain(kExecOut);
        emit.Dedent();
        emit.Line("end");
        emit.Close();
        emit.Line("end)");
      }});

  library.Add({.id = "event.setting_changed", .title = "Setting Changed", .category = category,
      .description = "Runs on the frame after the cvar named Setting changes, with its new value.",
      .role = NodeRole::kEvent, .delegate = true, .pins = {{}, {ExecPin(kExecOut), DataPin("Value", PinKind::kAny)}},
      .settings = {{.name = "Setting", .kind = SettingKind::kText, .tooltip = "The cvar's name"}},
      .title_of = [](const Node& node) { return Titled(node, "", "Setting", " Changed"); },
      .emit = [](Emitter& emit) {
        emit.Line("rex.cvar.on_change(" + NamedSetting(emit, "Setting") + ", function(_, " + emit.Param("Value") + ")");
        emit.Open();
        emit.Chain(kExecOut);
        emit.Close();
        emit.Line("end)");
      }});
}

// An event that only runs when something runs it: a timer, a dispatcher it's bound to. Its params are a dispatcher's
// when it picks one, so it can be bound to that dispatcher and get what Call gives.
void AddCustomEvent(Library& library) {
  library.Add({.id = "event.custom", .title = "Custom Event", .category = "Events",
      .description = "Runs when a timer or an event dispatcher runs it. Link its top right pin to Set Timer by Event, or to a dispatcher's Bind Event. With Params From set, it has that dispatcher's params.",
      .role = NodeRole::kEvent, .delegate = true, .pins = {{}, {ExecPin(kExecOut)}},
      .settings = {{.name = "Name", .kind = SettingKind::kText, .initial = "CustomEvent", .tooltip = "What the event is called on the canvas"},
                   {.name = "Params From", .kind = SettingKind::kDispatcher, .tooltip = "A dispatcher whose params the event takes, to be bound to it", .optional = true}},
      .shape = [](const Node& node, const Graph& graph, NodePins& pins) {
        if (const Dispatcher* dispatcher = graph.FindDispatcher(SettingOf(node, "Params From"))) {
          for (const Param& param : dispatcher->params) pins.outputs.push_back(DataPin(param.name, param.type));
        }
      },
      .title_of = [](const Node& node) { return Titled(node, "", "Name"); },
      // Nothing registers it: its chain is its delegate function, which the compiler writes when it's taken.
      .emit = [](Emitter& emit) {
        if (!emit.Linked(kDelegatePin)) emit.Warn("Nothing runs this event: link its top right pin to Set Timer by Event or a dispatcher's Bind Event");
      }});
}

// Flow -------------------------------------------------------------------------------------------------

void AddFlow(Library& library) {
  const std::string category = "Flow";

  // Placed by double clicking a link rather than from the add menu, so it has no category.
  Pin reroute_in{.name = std::string(kRerouteIn), .type = {PinKind::kAny}, .wildcard = true};
  Pin reroute_out{.name = std::string(kRerouteOut), .type = {PinKind::kAny}, .wildcard = true};
  library.Add({.id = std::string(kReroute), .title = "Reroute", .category = {},
      .description = "A point a link passes through, to lead it around things. Double click a link to add one.",
      .pins = {{reroute_in}, {reroute_out}},
      .shape = [](const Node& node, const Graph& graph, NodePins& pins) {
        // Wildcards until something links to it; then both pins are the type of what passes through.
        const bool linked = graph.HasLinkAt({node.id, std::string(kRerouteIn)}, false) || graph.HasLinkAt({node.id, std::string(kRerouteOut)}, true);
        if (!linked) return;
        const PinType type = RerouteType(graph, node);
        for (Pin* pin : {&pins.inputs.front(), &pins.outputs.front()}) {
          pin->type = type;
          pin->wildcard = false;
        }
      },
      .emit = [](Emitter& emit) { emit.Chain(kRerouteOut); },
      .expression = [](Emitter& emit, std::string_view) { return emit.Input(kRerouteIn); },
      .role_of = [](const Node& node, const Graph& graph) { return RerouteType(graph, node).kind == PinKind::kExec ? NodeRole::kAction : NodeRole::kPure; }});

  library.Add({.id = "flow.branch", .title = "Branch", .category = category,
      .description = "Goes on through True or False.",
      .pins = {{ExecPin(), DataPin("Condition", PinKind::kBool, true)}, {ExecPin("True"), ExecPin("False")}},
      .emit = [](Emitter& emit) {
        emit.Line("if " + emit.Input("Condition") + " then");
        emit.Indent();
        emit.Chain("True");
        emit.Dedent();
        if (emit.Linked("False")) {
          emit.Line("else");
          emit.Indent();
          emit.Chain("False");
          emit.Dedent();
        }
        emit.Line("end");
      }});

  library.Add({.id = "flow.sequence", .title = "Sequence", .category = category,
      .description = "Runs each output's chain in turn.",
      .pins = {{ExecPin()}, {}},
      .settings = {{.name = "Outputs", .kind = SettingKind::kChoice, .initial = "2", .choices = Counts(2, 8)}},
      .shape = [](const Node& node, const Graph&, NodePins& pins) {
        for (int i = 1; i <= CountOf(node, "Outputs"); ++i) pins.outputs.push_back(ExecPin("Then " + std::to_string(i)));
      },
      .emit = [](Emitter& emit) {
        for (int i = 1; i <= CountOf(emit.node(), "Outputs"); ++i) emit.Chain("Then " + std::to_string(i));
      }});

  library.Add({.id = "flow.for", .title = "For Loop", .category = category,
      .description = "Runs Body once for each Index from First to Last, then goes on through Completed. Index only exists in Body's chain.",
      .pins = {{ExecPin(), DataPin("First", PinKind::kInt, "1"), DataPin("Last", PinKind::kInt, "10")}, {ExecPin("Body"), DataPin("Index", PinKind::kInt), ExecPin("Completed")}},
      .emit = [](Emitter& emit) {
        emit.Line("for " + emit.Param("Index") + " = " + emit.Input("First") + ", " + emit.Input("Last") + " do");
        emit.Indent();
        emit.Chain("Body");
        emit.Dedent();
        emit.Line("end");
        emit.Chain("Completed");
      }});

  library.Add({.id = "flow.do_once", .title = "Do Once", .category = category,
      .description = "Goes on the first time it's reached, and not again until Reset is reached.",
      .pins = {{ExecPin(), ExecPin("Reset")}, {ExecPin(kExecOut)}},
      .emit = [](Emitter& emit) {
        const std::string done = emit.State("done", "false");
        if (emit.entered() == "Reset") {
          emit.Line(done + " = false");
          emit.EndChain();
          return;
        }
        emit.Line("if not " + done + " then");
        emit.Indent();
        emit.Line(done + " = true");
        emit.Chain(kExecOut);
        emit.Dedent();
        emit.Line("end");
      }});

  library.Add({.id = "flow.flip_flop", .title = "Flip Flop", .category = category,
      .description = "Goes on through A, then B the next time, then A again.",
      .pins = {{ExecPin()}, {ExecPin("A"), ExecPin("B")}},
      .emit = [](Emitter& emit) {
        const std::string a = emit.State("a", "true");
        emit.Line("if " + a + " then");
        emit.Indent();
        emit.Line(a + " = false");
        emit.Chain("A");
        emit.Dedent();
        emit.Line("else");
        emit.Indent();
        emit.Line(a + " = true");
        emit.Chain("B");
        emit.Dedent();
        emit.Line("end");
      }});
}

// Hooks and the guest ------------------------------------------------------------------------------------

void AddGuest(Library& library) {
  const std::string category = "Hooks";

  library.Add({.id = "hook.skip", .title = "Skip Original", .category = category,
      .description = "The hooked function doesn't run this time. The rest of the chain still does. Only in a Before Function chain.",
      .pins = {{ExecPin()}, {ExecPin(kExecOut)}},
      .emit = [](Emitter& emit) {
        if (emit.event().type != "event.hook_before") emit.Error("Skip Original only works in a Before Function event's chain");
        else emit.Line(emit.EventLocal("skip") + " = true");
      }});

  library.Add({.id = "midasm.take", .title = "Take Branch", .category = category,
      .description = "At a midasm branch site, takes the branch. Only in a Midasm Site chain.",
      .pins = {{ExecPin()}, {ExecPin(kExecOut)}},
      .emit = [](Emitter& emit) {
        if (emit.event().type != "event.midasm") emit.Error("Take Branch only works in a Midasm Site event's chain");
        else emit.Line(emit.EventLocal("take") + " = true");
      }});

  const SettingDesc register_setting{.name = "Register", .kind = SettingKind::kChoice, .initial = "r3", .choices = RegisterNames()};

  library.Add({.id = "reg.get", .title = "Get Register", .category = category,
      .description = "Reads a register. r registers, lr and ctr are integers, f registers floats.",
      .role = NodeRole::kPure, .pins = {{DataPin("Registers", PinKind::kRegisters)}, {DataPin("Value", PinKind::kInt)}},
      .settings = {register_setting},
      .shape = [](const Node& node, const Graph&, NodePins& pins) { Retype(pins.outputs, "Value", RegisterKind(SettingOf(node, "Register"))); },
      .title_of = [](const Node& node) { return Titled(node, "Get ", "Register"); },
      .expression = [](Emitter& emit, std::string_view) { return emit.Input("Registers") + "." + emit.Setting("Register"); }});

  library.Add({.id = "reg.set", .title = "Set Register", .category = category,
      .description = "Writes a register. Before a function, that changes its arguments; after it, what it returns.",
      .pins = {{ExecPin(), DataPin("Registers", PinKind::kRegisters), DataPin("Value", PinKind::kInt)}, {ExecPin(kExecOut)}},
      .settings = {register_setting},
      .shape = [](const Node& node, const Graph&, NodePins& pins) { Retype(pins.inputs, "Value", RegisterKind(SettingOf(node, "Register"))); },
      .title_of = [](const Node& node) { return Titled(node, "Set ", "Register"); },
      .emit = [](Emitter& emit) { emit.Line(emit.Input("Registers") + "." + emit.Setting("Register") + " = " + emit.Input("Value")); }});

  library.Add({.id = "guest.call", .title = "Call Function", .category = category,
      .description = "Calls a guest function with integer arguments and gives back what it returned in r3. Only from a hook or a frame event, on a guest thread.",
      .pins = {{ExecPin(), DataPin("Address", PinKind::kInt, "")}, {ExecPin(kExecOut), DataPin("Result", PinKind::kInt)}},
      .settings = {{.name = "Arguments", .kind = SettingKind::kChoice, .initial = "0", .choices = Counts(0, 8)}},
      .shape = [](const Node& node, const Graph&, NodePins& pins) {
        for (int i = 1; i <= CountOf(node, "Arguments"); ++i) pins.inputs.push_back(DataPin("Arg " + std::to_string(i), PinKind::kInt));
      },
      .emit = [](Emitter& emit) {
        if (emit.event().type == "event.start") emit.Error("The game can't be called from On Start; call it from a hook or a frame event");
        std::vector<std::string> args = {emit.Input("Address")};
        for (int i = 1; i <= CountOf(emit.node(), "Arguments"); ++i) args.push_back(emit.Input("Arg " + std::to_string(i)));
        const std::string call = "rex.call(" + Joined(args) + ")";
        emit.Line(emit.Linked("Result") ? emit.Output("Result") + " = " + call : call);
      }});
}

// Memory -----------------------------------------------------------------------------------------------

void AddMemory(Library& library) {
  const std::string category = "Memory";
  const std::vector<std::string> readable = {"u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64", "f32", "f64", "string", "wstring"};
  const std::vector<std::string> writable = {"u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64", "f32", "f64", "string"};

  library.Add({.id = "mem.read", .title = "Read Memory", .category = category,
      .description = "Reads guest memory at Address as Type, from the guest's big endian. wstring reads UTF-16 as text.",
      .role = NodeRole::kPure, .pins = {{DataPin("Address", PinKind::kInt, "")}, {DataPin("Value", PinKind::kInt)}},
      .settings = {{.name = "Type", .kind = SettingKind::kChoice, .initial = "u32", .choices = readable}},
      .shape = [](const Node& node, const Graph&, NodePins& pins) { Retype(pins.outputs, "Value", MemoryKind(SettingOf(node, "Type"))); },
      .title_of = [](const Node& node) { return Titled(node, "Read ", "Type"); },
      .expression = [](Emitter& emit, std::string_view) { return "rex.mem." + emit.Setting("Type") + "(" + emit.Input("Address") + ")"; }});

  library.Add({.id = "mem.write", .title = "Write Memory", .category = category,
      .description = "Writes Value to guest memory at Address as Type, in the guest's big endian.",
      .pins = {{ExecPin(), DataPin("Address", PinKind::kInt, ""), DataPin("Value", PinKind::kInt)}, {ExecPin(kExecOut)}},
      .settings = {{.name = "Type", .kind = SettingKind::kChoice, .initial = "u32", .choices = writable}},
      .shape = [](const Node& node, const Graph&, NodePins& pins) { Retype(pins.inputs, "Value", MemoryKind(SettingOf(node, "Type"))); },
      .title_of = [](const Node& node) { return Titled(node, "Write ", "Type"); },
      .emit = [](Emitter& emit) { emit.Line("rex.mem.set_" + emit.Setting("Type") + "(" + emit.Input("Address") + ", " + emit.Input("Value") + ")"); }});
}

// Variables ----------------------------------------------------------------------------------------------

PinKind VariableKind(const Node& node, const Graph& graph) {
  const Variable* variable = graph.FindVariable(SettingOf(node, "Variable"));
  return variable ? variable->type : PinKind::kAny;
}

// The variable's name, or an error when the graph has no such variable.
std::string VariableName(Emitter& emit) {
  const std::string name = emit.Setting("Variable");
  if (!emit.graph().FindVariable(name)) {
    emit.Error(name.empty() ? "Pick a variable" : "There's no variable named " + name);
    return "nil";
  }
  return name;
}

void AddVariables(Library& library) {
  const std::string category = "Variables";
  const SettingDesc variable{.name = "Variable", .kind = SettingKind::kVariable};
  SettingDesc boolean = variable;
  boolean.kinds = {PinKind::kBool};
  SettingDesc number = variable;
  number.kinds = {PinKind::kInt, PinKind::kFloat};

  // A Get is just the variable's name and its value, on one row.
  library.Add({.id = "var.get", .title = "Get", .category = category,
      .description = "A variable's value. Variables keep their values between events.",
      .role = NodeRole::kPure, .compact = true, .pins = {{}, {DataPin("Value", PinKind::kAny)}},
      .settings = {variable},
      .shape = [](const Node& node, const Graph& graph, NodePins& pins) { Retype(pins.outputs, "Value", VariableKind(node, graph)); },
      .title_of = [](const Node& node) { return Titled(node, "", "Variable"); },
      .expression = [](Emitter& emit, std::string_view) { return VariableName(emit); }});

  // A Bool variable as its own branch: a Get and a Branch in one node.
  library.Add({.id = "var.branch", .title = "If", .category = category,
      .description = "Goes on through True or False, as the Bool variable is.",
      .pins = {{ExecPin()}, {ExecPin("True"), ExecPin("False")}},
      .settings = {boolean},
      .title_of = [](const Node& node) { return Titled(node, "If ", "Variable"); },
      .emit = [](Emitter& emit) {
        const std::string name = VariableName(emit);
        const Variable* variable = emit.graph().FindVariable(emit.Setting("Variable"));
        if (variable && variable->type != PinKind::kBool) emit.Error(variable->name + " isn't a Bool");
        emit.Line("if " + name + " then");
        emit.Indent();
        emit.Chain("True");
        emit.Dedent();
        if (emit.Linked("False")) {
          emit.Line("else");
          emit.Indent();
          emit.Chain("False");
          emit.Dedent();
        }
        emit.Line("end");
      }});

  // Count = Count + 1, as `Count++` would be.
  const auto step = [&](std::string id, std::string title, std::string keywords, std::string op) {
    library.Add({.id = std::move(id), .title = title, .category = category,
        .description = title + "s a number variable by 1 and gives its new value.",
        .keywords = std::move(keywords),
        .pins = {{ExecPin()}, {ExecPin(kExecOut), DataPin("Value", PinKind::kNumber)}},
        .settings = {number},
        .shape = [](const Node& node, const Graph& graph, NodePins& pins) { Retype(pins.outputs, "Value", VariableKind(node, graph)); },
        .title_of = [title](const Node& node) { return Titled(node, title + " ", "Variable"); },
        .emit = [op](Emitter& emit) {
          const std::string name = VariableName(emit);
          if (name == "nil") return;
          const Variable* variable = emit.graph().FindVariable(name);
          if (variable->type != PinKind::kInt && variable->type != PinKind::kFloat) emit.Error(name + " isn't a number");
          emit.Line(name + " = " + name + " " + op + " 1");
          if (emit.Linked("Value")) emit.Line(emit.Output("Value") + " = " + name);
        }});
  };
  step("var.increment", "Increment", "++ +=", "+");
  step("var.decrement", "Decrement", "-- -=", "-");

  library.Add({.id = "var.set", .title = "Set", .category = category,
      .description = "Gives a variable a new value.",
      .pins = {{ExecPin(), DataPin("Value", PinKind::kAny)}, {ExecPin(kExecOut)}},
      .settings = {variable},
      .shape = [](const Node& node, const Graph& graph, NodePins& pins) { Retype(pins.inputs, "Value", VariableKind(node, graph)); },
      .title_of = [](const Node& node) { return Titled(node, "Set ", "Variable"); },
      .emit = [](Emitter& emit) {
        const std::string name = VariableName(emit);
        if (name != "nil") emit.Line(name + " = " + emit.Input("Value"));
      }});
}

// Functions and macros ---------------------------------------------------------------------------------

Pin ParamPin(const Param& param) { return param.type == PinKind::kExec ? ExecPin(param.name) : DataPin(param.name, param.type); }

// The routine's name, kept on its nodes (entry, calls) for their titles.
std::string RoutineName(const Node& node, std::string_view fallback) {
  const std::string name = SettingOf(node, kRoutineNameSetting);
  return name.empty() ? std::string(fallback) : name;
}

// A function with exec pins, or a macro with an exec input, is reached by a chain; the rest are worked out where read.
NodeRole CallRole(const Node& node, const Graph& graph) {
  const Routine* routine = RoutineOf(node, graph);
  if (!routine) return NodeRole::kAction;
  if (routine->kind == RoutineKind::kFunction) return routine->pure ? NodeRole::kPure : NodeRole::kAction;
  const bool exec = std::any_of(routine->inputs.begin(), routine->inputs.end(), [](const Param& param) { return param.type == PinKind::kExec; });
  return exec ? NodeRole::kAction : NodeRole::kPure;
}

// `name(args)` for a call to a function.
std::string FunctionCall(Emitter& emit, const Routine& routine) {
  std::vector<std::string> args;
  for (const Param& param : routine.inputs) args.push_back(emit.Input(param.name));
  return routine.name + "(" + Joined(args) + ")";
}

// The routine nodes are drawn and added by the editor and the compiler knows them; the add menu doesn't list them.
void AddRoutines(Library& library) {
  library.Add({.id = std::string(kRoutineEntry), .title = "Inputs", .category = {},
      .description = "Where the function or macro starts: its inputs, given by whoever uses it.",
      .role = NodeRole::kEvent,
      .shape = [](const Node& node, const Graph& graph, NodePins& pins) {
        const Routine* routine = RoutineOf(node, graph);
        if (!routine) return;
        if (routine->kind == RoutineKind::kFunction && !routine->pure) pins.outputs.push_back(ExecPin(kExecOut));
        for (const Param& param : routine->inputs) pins.outputs.push_back(ParamPin(param));
      },
      .title_of = [](const Node& node) { return RoutineName(node, "Inputs"); }});

  library.Add({.id = std::string(kRoutineExit), .title = "Outputs", .category = {},
      .description = "Where the function or macro ends: its outputs, given back to whoever used it.",
      .shape = [](const Node& node, const Graph& graph, NodePins& pins) {
        const Routine* routine = RoutineOf(node, graph);
        if (!routine) return;
        if (routine->kind == RoutineKind::kFunction && !routine->pure) pins.inputs.push_back(ExecPin());
        for (const Param& param : routine->outputs) pins.inputs.push_back(ParamPin(param));
      },
      // A function's: return its outputs. A macro's is followed by the compiler into the graph using it.
      .emit = [](Emitter& emit) {
        const Routine* routine = RoutineOf(emit.node(), emit.graph());
        if (!routine) return;
        std::vector<std::string> values;
        for (const Param& param : routine->outputs) values.push_back(emit.Input(param.name));
        emit.Line(values.empty() ? std::string("do return end") : "do return " + Joined(values) + " end");
      }});

  library.Add({.id = std::string(kRoutineCall), .title = "Call", .category = {},
      .description = "Runs a function or macro of the graph's own.",
      .shape = [](const Node& node, const Graph& graph, NodePins& pins) {
        const Routine* routine = RoutineOf(node, graph);
        if (!routine) return;
        const bool exec = routine->kind == RoutineKind::kFunction && !routine->pure;
        if (exec) pins.inputs.push_back(ExecPin());
        for (const Param& param : routine->inputs) pins.inputs.push_back(ParamPin(param));
        if (exec) pins.outputs.push_back(ExecPin(kExecOut));
        for (const Param& param : routine->outputs) pins.outputs.push_back(ParamPin(param));
      },
      .title_of = [](const Node& node) { return RoutineName(node, "Call"); },
      // A function's call; a macro's is written out in full by the compiler instead.
      .emit = [](Emitter& emit) {
        const Routine* routine = RoutineOf(emit.node(), emit.graph());
        if (!routine) {
          emit.Error("Calls a function the graph doesn't have any more");
          return;
        }
        const std::string call = FunctionCall(emit, *routine);
        const bool used = std::any_of(routine->outputs.begin(), routine->outputs.end(), [&](const Param& param) { return emit.Linked(param.name); });
        if (!used) {
          emit.Line(call);
          return;
        }
        std::vector<std::string> results;
        for (const Param& param : routine->outputs) results.push_back(emit.Output(param.name));
        emit.Line(Joined(results) + " = " + call);
      },
      .expression = [](Emitter& emit, std::string_view output) -> std::string {
        const Routine* routine = RoutineOf(emit.node(), emit.graph());
        if (!routine) {
          emit.Error("Calls a function the graph doesn't have any more");
          return "nil";
        }
        const std::string call = FunctionCall(emit, *routine);
        if (routine->outputs.size() <= 1) return call;
        const auto found = std::find_if(routine->outputs.begin(), routine->outputs.end(), [&](const Param& param) { return param.name == output; });
        return "(select(" + std::to_string(found - routine->outputs.begin() + 1) + ", " + call + "))";
      },
      .role_of = CallRole});
}

// Switch and Select ------------------------------------------------------------------------------------

// What a Switch or Select picks by: an Int, a String, a Bool (Select only), or one of the game's enums by its full name.
std::vector<std::string> PickChoices(bool with_bool) {
  std::vector<std::string> choices = {"Int", "String"};
  if (with_bool) choices.push_back("Bool");
  for (const std::string& name : EnumNames()) choices.push_back(name);
  return choices;
}

// A node picking by an Int or a String has entries added in the details panel; by a Bool or an enum, its own.
bool PicksByList(const Node& node, std::string_view setting) {
  const std::string by = SettingOf(node, setting);
  return by == "Int" || by == "String";
}

// The values a Switch's cases or a Select's options are, in order: the list's, a Bool's, an enum's.
std::vector<std::string> PickValues(const Node& node, std::string_view by_setting, std::string_view list_setting) {
  const std::string by = SettingOf(node, by_setting);
  if (by == "Bool") return {"False", "True"};
  if (const EnumDef* enumeration = FindEnum(by)) {
    std::vector<std::string> values;
    for (const auto& [name, number] : enumeration->values) values.push_back(name);
    return values;
  }
  return ListOf(node, list_setting);
}

// A case's or an option's value as Lua: 3, "Hello", true, tip.Species.Whirlm.
std::string PickKey(Emitter& emit, std::string_view by, const std::string& value) {
  if (by == "String") return LuaQuote(value);
  if (by == "Bool") return value == "True" ? "true" : "false";
  if (FindEnum(by)) return LuaPath(by) + LuaMember(value);
  std::string literal;
  if (IntegerText(value, literal)) return literal;
  emit.Error(value + " isn't a whole number, so it can't be a case of an Int");
  return "nil";
}

PinKind PickKind(std::string_view by) { return by == "String" ? PinKind::kString : by == "Bool" ? PinKind::kBool : PinKind::kInt; }

// The type a Select gives: what comes into its options, or else what its value goes to; Any until it's linked.
PinType SelectType(const Node& node, const Graph& graph) {
  // A Select linked to another Select asks it in turn, which could ask back; a few steps are plenty.
  static thread_local int depth = 0;
  if (depth > 8) return {PinKind::kAny};
  ++depth;
  PinType type{PinKind::kAny};
  for (const std::string& option : PickValues(node, "Index", "Options")) {
    if (const Link* link = graph.LinkInto({node.id, option})) {
      type = OutputType(graph, link->from);
      if (type.kind != PinKind::kAny) break;
    }
  }
  if (type.kind == PinKind::kAny) {
    for (const Link* link : graph.LinksOutOf({node.id, "Return Value"})) {
      const Node* target = graph.FindNode(link->to.node);
      const NodePins pins = target ? PinsOf(*target, graph) : NodePins{};
      if (const Pin* pin = FindPin(pins.inputs, link->to.pin); pin && pin->type.kind != PinKind::kAny) {
        type = pin->type;
        break;
      }
    }
  }
  --depth;
  return type;
}

void AddPicks(Library& library) {
  // As Unreal's Switch on Int, Switch on String and Switch on an enum.
  library.Add({.id = "flow.switch", .title = "Switch", .category = "Flow",
      .description = "Goes on through the case Selection matches, or Default when it matches none. On an Int or a String, add cases in the details panel; on an enum, its values are the cases.",
      .keywords = "case select branch on",
      .pins = {{ExecPin()}, {}},
      .settings = {{.name = "On", .kind = SettingKind::kChoice, .initial = "Int", .choices = PickChoices(false), .tooltip = "What Selection is: an Int, a String or one of the game's enums"},
                   {.name = "Cases", .kind = SettingKind::kList, .initial = "0\n1", .item = "Case", .shown = [](const Node& node) { return PicksByList(node, "On"); }}},
      .shape = [](const Node& node, const Graph&, NodePins& pins) {
        pins.inputs.push_back(DataPin("Selection", PickKind(SettingOf(node, "On"))));
        for (const std::string& value : PickValues(node, "On", "Cases")) pins.outputs.push_back(ExecPin(value));
        pins.outputs.push_back(ExecPin("Default"));
      },
      .title_of = [](const Node& node) { return Titled(node, "Switch On ", "On"); },
      .emit = [](Emitter& emit) {
        const std::string by = emit.Setting("On");
        const std::vector<std::string> cases = PickValues(emit.node(), "On", "Cases");
        if (cases.empty()) {
          emit.Chain("Default");
          return;
        }
        // Worked out once, then compared with each case in turn.
        const std::string selection = emit.Output("Selection");
        emit.Line(selection + " = " + emit.Input("Selection"));
        for (size_t i = 0; i < cases.size(); ++i) {
          emit.Line((i == 0 ? "if " : "elseif ") + selection + " == " + PickKey(emit, by, cases[i]) + " then");
          emit.Indent();
          emit.Chain(cases[i]);
          emit.Dedent();
        }
        if (emit.Linked("Default")) {
          emit.Line("else");
          emit.Indent();
          emit.Chain("Default");
          emit.Dedent();
        }
        emit.Line("end");
      }});

  // As Unreal's Select: the option Index picks, every option worked out, each of the type it's linked to.
  library.Add({.id = "logic.select", .title = "Select", .category = "Logic",
      .description = "The option Index picks, or nil when it picks none. On an Int or a String, add options in the details panel; on a Bool or an enum, its values are the options. The options take the type of what's linked to them or to the result.",
      .keywords = "pick choose ternary option",
      .role = NodeRole::kPure, .pins = {{}, {DataPin("Return Value", PinKind::kAny)}},
      .settings = {{.name = "Index", .kind = SettingKind::kChoice, .initial = "Int", .choices = PickChoices(true), .tooltip = "What Index is: an Int, a String, a Bool or one of the game's enums"},
                   {.name = "Options", .kind = SettingKind::kList, .initial = "0\n1", .item = "Option", .shown = [](const Node& node) { return PicksByList(node, "Index"); }}},
      .shape = [](const Node& node, const Graph& graph, NodePins& pins) {
        pins.inputs.push_back(DataPin("Index", PickKind(SettingOf(node, "Index"))));
        const PinType type = SelectType(node, graph);
        for (const std::string& option : PickValues(node, "Index", "Options")) {
          Pin pin = DataPin(option, type.kind);
          pin.type = type;
          pins.inputs.push_back(std::move(pin));
        }
        pins.outputs.front().type = type;
      },
      .expression = [](Emitter& emit, std::string_view) -> std::string {
        const std::string by = emit.Setting("Index");
        std::vector<std::string> entries;
        for (const std::string& option : PickValues(emit.node(), "Index", "Options")) entries.push_back("[" + PickKey(emit, by, option) + "] = " + emit.Input(option));
        return "({" + Joined(entries) + "})[" + emit.Input("Index") + "]";
      }});
}

// Time: delays and timers -------------------------------------------------------------------------------

void AddTimers(Library& library) {
  const std::string category = "Time";

  // As Unreal's Delay: running it again while it waits does nothing.
  library.Add({.id = "time.delay", .title = "Delay", .category = category,
      .description = "Goes on through Completed once Duration seconds have passed. Reached again while it waits, it does nothing.",
      .keywords = "wait sleep later",
      .pins = {{ExecPin(), DataPin("Duration", PinKind::kFloat, 1.0)}, {ExecPin("Completed")}},
      .emit = [](Emitter& emit) {
        const std::string waiting = emit.State("Waiting", "false");
        emit.Line("if not " + waiting + " then");
        emit.Indent();
        emit.Line(waiting + " = true");
        emit.Line("rex.after(" + emit.Input("Duration") + ", function()");
        emit.Indent();
        emit.Line(waiting + " = false");
        emit.Chain("Completed");
        emit.Dedent();
        emit.Line("end)");
        emit.Dedent();
        emit.Line("end");
      }});

  library.Add({.id = "time.set_timer", .title = "Set Timer by Event", .category = category,
      .description = "Runs the event linked to Event once Time seconds have passed, or every Time seconds when Looping. Handle stops it with Clear Timer.",
      .keywords = "timer every repeat interval",
      .pins = {{ExecPin(), DelegatePin(), DataPin("Time", PinKind::kFloat, 1.0), DataPin("Looping", PinKind::kBool, false)}, {ExecPin(kExecOut), DataPin("Handle", PinKind::kInt)}},
      .emit = [](Emitter& emit) {
        const std::string event = emit.Input(kDelegatePin);
        const std::string time = emit.Input("Time");
        // A Looping typed in picks the function; one that's linked picks it as the script runs.
        const std::string looping = emit.Input("Looping");
        const std::string start = looping == "true" ? "rex.every" : looping == "false" ? "rex.after" : "(" + looping + " and rex.every or rex.after)";
        const std::string call = start + "(" + time + ", " + event + ")";
        emit.Line(emit.Linked("Handle") ? emit.Output("Handle") + " = " + call : call);
      }});

  library.Add({.id = "time.clear_timer", .title = "Clear Timer", .category = category,
      .description = "Stops a timer Set Timer by Event started. One that already ran is fine.",
      .keywords = "stop cancel",
      .pins = {{ExecPin(), DataPin("Handle", PinKind::kInt)}, {ExecPin(kExecOut)}},
      .emit = [](Emitter& emit) { emit.Line("rex.cancel(" + emit.Input("Handle") + ")"); }});
}

// Event dispatchers ---------------------------------------------------------------------------------------

// The dispatcher's table, or an error when the graph has no such dispatcher.
std::string DispatcherName(Emitter& emit) {
  const std::string name = emit.Setting("Dispatcher");
  if (!emit.graph().FindDispatcher(name)) {
    emit.Error(name.empty() ? "Pick an event dispatcher" : "There's no event dispatcher named " + name);
    return {};
  }
  return name;
}

constexpr std::string_view kBindHelper = "local function rf_bind(list, run)\n  for _, bound in ipairs(list) do if bound == run then return end end\n  list[#list + 1] = run\nend";
constexpr std::string_view kUnbindHelper = "local function rf_unbind(list, run)\n  for i, bound in ipairs(list) do if bound == run then table.remove(list, i) return end end\nend";

void AddDispatchers(Library& library) {
  const std::string category = "Event Dispatchers";
  const SettingDesc dispatcher{.name = "Dispatcher", .kind = SettingKind::kDispatcher};

  library.Add({.id = "dispatch.call", .title = "Call", .category = category,
      .description = "Runs every event bound to the dispatcher, in the order they were bound, with these params.",
      .keywords = "broadcast fire dispatch",
      .pins = {{ExecPin()}, {ExecPin(kExecOut)}},
      .settings = {dispatcher},
      .shape = [](const Node& node, const Graph& graph, NodePins& pins) {
        if (const Dispatcher* found = graph.FindDispatcher(SettingOf(node, "Dispatcher"))) {
          for (const Param& param : found->params) pins.inputs.push_back(DataPin(param.name, param.type));
        }
      },
      .title_of = [](const Node& node) { return Titled(node, "Call ", "Dispatcher"); },
      .emit = [](Emitter& emit) {
        const std::string name = DispatcherName(emit);
        if (name.empty()) return;
        std::vector<std::string> args;
        for (const Param& param : emit.graph().FindDispatcher(name)->params) args.push_back(emit.Input(param.name));
        // Over a copy, so an event unbinding itself as it runs doesn't make the next one skipped.
        emit.Line("for _, run in ipairs({table.unpack(" + name + ")}) do run(" + Joined(args) + ") end");
      }});

  library.Add({.id = "dispatch.bind", .title = "Bind Event to", .category = category,
      .description = "Binds the event linked to Event to the dispatcher, so Call runs it. Bound already, it stays bound once.",
      .keywords = "subscribe listen add",
      .pins = {{ExecPin(), DelegatePin()}, {ExecPin(kExecOut)}},
      .settings = {dispatcher},
      .title_of = [](const Node& node) { return Titled(node, "Bind Event To ", "Dispatcher"); },
      .emit = [](Emitter& emit) {
        const std::string name = DispatcherName(emit);
        if (name.empty()) return;
        emit.Helper("rf_bind", kBindHelper);
        emit.Line("rf_bind(" + name + ", " + emit.Input(kDelegatePin) + ")");
      }});

  library.Add({.id = "dispatch.unbind", .title = "Unbind Event from", .category = category,
      .description = "Unbinds the event linked to Event from the dispatcher.",
      .keywords = "unsubscribe remove",
      .pins = {{ExecPin(), DelegatePin()}, {ExecPin(kExecOut)}},
      .settings = {dispatcher},
      .title_of = [](const Node& node) { return Titled(node, "Unbind Event From ", "Dispatcher"); },
      .emit = [](Emitter& emit) {
        const std::string name = DispatcherName(emit);
        if (name.empty()) return;
        emit.Helper("rf_unbind", kUnbindHelper);
        emit.Line("rf_unbind(" + name + ", " + emit.Input(kDelegatePin) + ")");
      }});

  library.Add({.id = "dispatch.unbind_all", .title = "Unbind All from", .category = category,
      .description = "Unbinds every event from the dispatcher.",
      .keywords = "clear",
      .pins = {{ExecPin()}, {ExecPin(kExecOut)}},
      .settings = {dispatcher},
      .title_of = [](const Node& node) { return Titled(node, "Unbind All From ", "Dispatcher"); },
      .emit = [](Emitter& emit) {
        const std::string name = DispatcherName(emit);
        if (!name.empty()) emit.Line(name + " = {}");
      }});
}

// Math, logic and text ---------------------------------------------------------------------------------

void AddMath(Library& library) {
  const std::string math = "Math";
  const PinKind number = PinKind::kNumber;
  const PinKind integer = PinKind::kInt;
  const PinKind real = PinKind::kFloat;

  library.Add(Binary("math.add", "Add", math, "A + B. Integers in give an integer out.", number, number, "+"));
  library.Add(Binary("math.subtract", "Subtract", math, "A - B.", number, number, "-"));
  library.Add(Binary("math.multiply", "Multiply", math, "A * B.", number, number, "*"));
  library.Add(Binary("math.divide", "Divide", math, "A / B, always a float.", number, real, "/"));
  library.Add(Binary("math.floor_divide", "Floor Divide", math, "A // B: the division rounded down.", number, number, "//"));
  library.Add(Binary("math.modulo", "Modulo", math, "A % B: what's left after dividing.", number, number, "%"));
  library.Add(Pure("math.min", "Min", math, "The smaller of A and B.", {DataPin("A", number), DataPin("B", number)}, DataPin("Result", number), [](Emitter& emit) { return "math.min(" + emit.Input("A") + ", " + emit.Input("B") + ")"; }));
  library.Add(Pure("math.max", "Max", math, "The larger of A and B.", {DataPin("A", number), DataPin("B", number)}, DataPin("Result", number), [](Emitter& emit) { return "math.max(" + emit.Input("A") + ", " + emit.Input("B") + ")"; }));
  library.Add(Pure("math.clamp", "Clamp", math, "Value kept between Min and Max.", {DataPin("Value", number), DataPin("Min", number), DataPin("Max", number, "1")}, DataPin("Result", number), [](Emitter& emit) { return "math.max(" + emit.Input("Min") + ", math.min(" + emit.Input("Max") + ", " + emit.Input("Value") + "))"; }));
  library.Add(Pure("math.lerp", "Lerp", math, "From A at Alpha 0 to B at Alpha 1.", {DataPin("A", real), DataPin("B", real, 1.0), DataPin("Alpha", real, 0.5)}, DataPin("Result", real), [](Emitter& emit) { const std::string a = emit.Input("A"); return "(" + a + " + (" + emit.Input("B") + " - " + a + ") * " + emit.Input("Alpha") + ")"; }));
  library.Add(Unary("math.negate", "Negate", math, "-Value.", number, number, "-"));
  library.Add(Keyed(Pure("math.increment", "Increment", math, "Value + 1.", {DataPin("Value", number)}, DataPin("Result", number), [](Emitter& emit) { return "(" + emit.Input("Value") + " + 1)"; }), "++ +1"));
  library.Add(Keyed(Pure("math.decrement", "Decrement", math, "Value - 1.", {DataPin("Value", number)}, DataPin("Result", number), [](Emitter& emit) { return "(" + emit.Input("Value") + " - 1)"; }), "-- -1"));
  library.Add(Unary("math.abs", "Abs", math, "Value without its sign.", number, number, "math.abs"));
  library.Add(Unary("math.floor", "Floor", math, "Value rounded down to an integer.", number, integer, "math.floor"));
  library.Add(Unary("math.ceil", "Ceil", math, "Value rounded up to an integer.", number, integer, "math.ceil"));
  library.Add(Pure("math.round", "Round", math, "Value rounded to the nearest integer.", {DataPin("Value", number)}, DataPin("Result", integer), [](Emitter& emit) { return "math.floor(" + emit.Input("Value") + " + 0.5)"; }));
  library.Add(Pure("math.to_float", "To Float", math, "An integer as a float.", {DataPin("Value", number)}, DataPin("Result", real), [](Emitter& emit) { return "(" + emit.Input("Value") + " + 0.0)"; }));
  library.Add(Unary("math.sqrt", "Square Root", math, "The square root of Value.", real, real, "math.sqrt"));
  library.Add(Unary("math.sin", "Sin", math, "The sine of Value, in radians.", real, real, "math.sin"));
  library.Add(Unary("math.cos", "Cos", math, "The cosine of Value, in radians.", real, real, "math.cos"));
  library.Add(Pure("math.random", "Random Float", math, "A new random float from 0 up to 1 each time it's read.", {}, DataPin("Result", real), [](Emitter&) { return std::string("math.random()"); }));
  library.Add(Pure("math.random_int", "Random Integer", math, "A new random integer from Min to Max each time it's read.", {DataPin("Min", integer, "1"), DataPin("Max", integer, "6")}, DataPin("Result", integer), [](Emitter& emit) { return "math.random(" + emit.Input("Min") + ", " + emit.Input("Max") + ")"; }));

  const std::string bits = "Math/Bits";
  library.Add(Binary("bits.and", "Bitwise And", bits, "The bits set in both A and B.", integer, integer, "&"));
  library.Add(Binary("bits.or", "Bitwise Or", bits, "The bits set in A or B.", integer, integer, "|"));
  library.Add(Binary("bits.xor", "Bitwise Xor", bits, "The bits set in A or B but not both.", integer, integer, "~"));
  library.Add(Binary("bits.left", "Shift Left", bits, "A's bits moved B places up.", integer, integer, "<<"));
  library.Add(Binary("bits.right", "Shift Right", bits, "A's bits moved B places down.", integer, integer, ">>"));
  library.Add(Keyed(Unary("bits.not", "Bitwise Not", bits, "Every bit of Value flipped.", integer, integer, "~"), "~"));
}

void AddLogic(Library& library) {
  const std::string logic = "Logic";
  const PinKind boolean = PinKind::kBool;
  const PinKind number = PinKind::kNumber;

  library.Add(Binary("logic.and", "And", logic, "True when A and B both are.", boolean, boolean, "and"));
  library.Add(Binary("logic.or", "Or", logic, "True when A or B is.", boolean, boolean, "or"));
  library.Add(Keyed(Pure("logic.not", "Not", logic, "True when Value is false.", {DataPin("Value", boolean)}, DataPin("Result", boolean), [](Emitter& emit) { return "(not " + emit.Input("Value") + ")"; }), "! ~"));
  library.Add(Binary("logic.equal", "Equal", logic, "True when A and B are the same.", PinKind::kAny, boolean, "=="));
  library.Add(Binary("logic.not_equal", "Not Equal", logic, "True when A and B differ.", PinKind::kAny, boolean, "~="));
  library.Add(Binary("logic.less", "Less", logic, "A < B.", number, boolean, "<"));
  library.Add(Binary("logic.less_equal", "Less Or Equal", logic, "A <= B.", number, boolean, "<="));
  library.Add(Binary("logic.greater", "Greater", logic, "A > B.", number, boolean, ">"));
  library.Add(Binary("logic.greater_equal", "Greater Or Equal", logic, "A >= B.", number, boolean, ">="));
  library.Add(Keyed(Pure("logic.or_default", "Or Default", logic, "Value, or Default when Value is nil or false.", {DataPin("Value", PinKind::kAny), DataPin("Default", PinKind::kAny, "0")}, DataPin("Result", PinKind::kAny), [](Emitter& emit) { return "(" + emit.Input("Value") + " or " + emit.Input("Default") + ")"; }), "or else fallback coalesce nil"));
  library.Add(Pure("logic.is_set", "Is Set", logic, "True when Value is anything but nil: an object that exists, a cvar that's there.", {DataPin("Value", PinKind::kAny)}, DataPin("Result", boolean), [](Emitter& emit) { return "(" + emit.Input("Value") + " ~= nil)"; }));
}

void AddText(Library& library) {
  const std::string text = "Text";
  const PinKind string = PinKind::kString;

  library.Add(Keyed(Pure("text.join", "Join", text, "A followed by B, each turned into text.", {DataPin("A", PinKind::kAny), DataPin("B", PinKind::kAny)}, DataPin("Result", string), [](Emitter& emit) { return "(tostring(" + emit.Input("A") + ") .. tostring(" + emit.Input("B") + "))"; }), ".. concat"));
  library.Add(Unary("text.to_text", "To Text", text, "Any value as text.", PinKind::kAny, string, "tostring"));
  library.Add(Pure("text.hex", "Hex", text, "An integer as eight hex digits, like 82558D28.", {DataPin("Value", PinKind::kInt)}, DataPin("Result", string), [](Emitter& emit) { return "string.format(\"%08X\", " + emit.Input("Value") + ")"; }));
  library.Add(Pure("text.length", "Length", text, "How many bytes the text has.", {DataPin("Text", string)}, DataPin("Result", PinKind::kInt), [](Emitter& emit) { return "#" + emit.Input("Text"); }));
  library.Add(Pure("text.contains", "Contains", text, "True when Part is somewhere in Text.", {DataPin("Text", string), DataPin("Part", string)}, DataPin("Result", PinKind::kBool), [](Emitter& emit) { return "(string.find(" + emit.Input("Text") + ", " + emit.Input("Part") + ", 1, true) ~= nil)"; }));
  library.Add(Keyed(Pure("text.starts_with", "Starts With", text, "True when Text begins with Start.", {DataPin("Text", string), DataPin("Start", string)}, DataPin("Result", PinKind::kBool), [](Emitter& emit) { const std::string start = emit.Input("Start"); return "(string.sub(" + emit.Input("Text") + ", 1, #" + start + ") == " + start + ")"; }), "prefix begins"));
  library.Add(Keyed(Pure("text.match", "Match", text, "What Pattern (a Lua pattern) finds in Text: its first capture, or the whole match when it has none. Nil when it doesn't match.", {DataPin("Text", string), DataPin("Pattern", string, "^(.+)$")}, DataPin("Capture", string), [](Emitter& emit) { return "string.match(" + emit.Input("Text") + ", " + emit.Input("Pattern") + ")"; }), "pattern regex find capture"));
}

// Tables: arrays and maps ---------------------------------------------------------------------------------

const std::vector<std::string>& ItemTypes() {
  static const std::vector<std::string> types = {"Any", "String", "Int", "Float", "Bool"};
  return types;
}

PinKind ItemKind(std::string_view type) {
  if (type == "String") return PinKind::kString;
  if (type == "Int") return PinKind::kInt;
  if (type == "Float") return PinKind::kFloat;
  if (type == "Bool") return PinKind::kBool;
  return PinKind::kAny;
}

// A Make Array's item count: its Size setting as a whole number, 0 when it isn't one.
int ArraySize(const Node& node) {
  const std::string text = SettingOf(node, "Size");
  int size = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return 0;
    size = std::min(size * 10 + (c - '0'), 4096);
  }
  return size;
}

std::string ItemPin(int index) { return "[" + std::to_string(index) + "]"; }

void AddTables(Library& library) {
  const std::string category = "Tables";
  const auto type_setting = [](std::string name, std::string tooltip) {
    return SettingDesc{.name = std::move(name), .kind = SettingKind::kChoice, .initial = "Any", .choices = ItemTypes(), .tooltip = std::move(tooltip)};
  };

  // As Unreal's Make Array: an input for each item, typed in or linked.
  library.Add({.id = "table.make_array", .title = "Make Array", .category = category,
      .description = "A Lua array of its items, in order from [1]. Set how many items it has with Size, and what they are with Of.",
      .keywords = "list table literal {}",
      .role = NodeRole::kPure, .pins = {{}, {DataPin("Array", PinKind::kAny)}},
      .settings = {type_setting("Of", "What the items are"),
                   {.name = "Size", .kind = SettingKind::kText, .initial = "3", .tooltip = "How many items: a whole number"}},
      .shape = [](const Node& node, const Graph&, NodePins& pins) {
        const PinKind kind = ItemKind(SettingOf(node, "Of"));
        for (int i = 1; i <= ArraySize(node); ++i) pins.inputs.push_back(DataPin(ItemPin(i), kind));
      },
      .title_of = [](const Node& node) { return "Make Array (" + std::to_string(ArraySize(node)) + ")"; },
      .expression = [](Emitter& emit, std::string_view) -> std::string {
        std::vector<std::string> items;
        for (int i = 1; i <= ArraySize(emit.node()); ++i) items.push_back(emit.Input(ItemPin(i)));
        return "{" + Joined(items) + "}";
      }});

  library.Add(Keyed(Pure("table.get", "Get Item", category, "The value at Key: an array's item by its index from 1, or a map's by its key. Nil when there's none.",
      {DataPin("Table", PinKind::kAny), DataPin("Key", PinKind::kAny, "1")}, DataPin("Value", PinKind::kAny),
      [](Emitter& emit) { return "(" + emit.Input("Table") + ")[" + emit.Input("Key") + "]"; }), "index lookup find []"));
  library.Add(Keyed(Pure("table.length", "Length", category, "How many items an array has.",
      {DataPin("Array", PinKind::kAny)}, DataPin("Length", PinKind::kInt),
      [](Emitter& emit) { return "#" + emit.Input("Array"); }), "count size #"));
  library.Add(Keyed(Pure("table.contains", "Contains Item", category, "True when Item is one of the array's items.",
      {DataPin("Array", PinKind::kAny), DataPin("Item", PinKind::kAny)}, DataPin("Result", PinKind::kBool),
      [](Emitter& emit) {
        return "(function(array, item) for _, value in ipairs(array) do if value == item then return true end end return false end)(" + emit.Input("Array") + ", " + emit.Input("Item") + ")";
      }), "has find includes"));

  library.Add({.id = "table.set", .title = "Set Item", .category = category,
      .description = "Sets the value at Key: an array's item by its index, or a map's by its key. Nil removes a map's.",
      .keywords = "assign put []=",
      .pins = {{ExecPin(), DataPin("Table", PinKind::kAny), DataPin("Key", PinKind::kAny, "1"), DataPin("Value", PinKind::kAny)}, {ExecPin(kExecOut)}},
      .emit = [](Emitter& emit) {
        emit.Line("(" + emit.Input("Table") + ")[" + emit.Input("Key") + "] = " + emit.Input("Value"));
        emit.Chain(kExecOut);
      }});
  library.Add({.id = "table.add", .title = "Add Item", .category = category,
      .description = "Puts Item at the end of the array.",
      .keywords = "append push insert",
      .pins = {{ExecPin(), DataPin("Array", PinKind::kAny), DataPin("Item", PinKind::kAny)}, {ExecPin(kExecOut)}},
      .emit = [](Emitter& emit) {
        emit.Line("table.insert(" + emit.Input("Array") + ", " + emit.Input("Item") + ")");
        emit.Chain(kExecOut);
      }});
  library.Add({.id = "table.remove", .title = "Remove Item", .category = category,
      .description = "Takes out the array's item at Index, moving the ones after it down.",
      .keywords = "delete pop",
      .pins = {{ExecPin(), DataPin("Array", PinKind::kAny), DataPin("Index", PinKind::kInt, "1")}, {ExecPin(kExecOut)}},
      .emit = [](Emitter& emit) {
        emit.Line("table.remove(" + emit.Input("Array") + ", " + emit.Input("Index") + ")");
        emit.Chain(kExecOut);
      }});

  library.Add({.id = "table.for_each", .title = "For Each Item", .category = category,
      .description = "Runs Body once for each of the array's items in order, then goes on through Completed. Index and Item only exist in Body's chain.",
      .keywords = "loop ipairs foreach array",
      .pins = {{ExecPin(), DataPin("Array", PinKind::kAny)}, {ExecPin("Body"), DataPin("Index", PinKind::kInt), DataPin("Item", PinKind::kAny), ExecPin("Completed")}},
      .emit = [](Emitter& emit) {
        emit.Line("for " + emit.Param("Index") + ", " + emit.Param("Item") + " in ipairs(" + emit.Input("Array") + ") do");
        emit.Indent();
        emit.Chain("Body");
        emit.Dedent();
        emit.Line("end");
        emit.Chain("Completed");
      }});
}

// The host: logging, time, input, cvars and switches -------------------------------------------------------

void AddHost(Library& library) {
  library.Add({.id = "log.write", .title = "Log", .category = "Log",
      .description = "Writes Message, and Value when it's given, to the game's log with the mod's name.",
      .pins = {{ExecPin(), DataPin("Message", PinKind::kAny), DataPin("Value", PinKind::kAny)}, {ExecPin(kExecOut)}},
      .settings = {{.name = "Level", .kind = SettingKind::kChoice, .initial = "info", .choices = {"info", "warn", "error", "debug"}}},
      .title_of = [](const Node& node) {
        const std::string level = SettingOf(node, "Level");
        return level == "info" ? std::string("Log") : level == "warn" ? std::string("Log Warning") : level == "error" ? std::string("Log Error") : std::string("Log Debug");
      },
      .emit = [](Emitter& emit) {
        const auto typed = emit.node().values.find("Value");
        const bool value = emit.Linked("Value") || (typed != emit.node().values.end() && typed->second.is_string() && !typed->second.get<std::string>().empty());
        emit.Line("rex." + emit.Setting("Level") + "(" + emit.Input("Message") + (value ? ", " + emit.Input("Value") : std::string()) + ")");
      }});

  // As Unreal's Print String: text on the screen for a while, for debugging, and in the log.
  const std::vector<std::pair<std::string, std::string>> colors = {{"Cyan", "0x00A8FF"}, {"White", "0xFFFFFF"}, {"Yellow", "0xFFD84A"}, {"Orange", "0xFF9A3C"}, {"Red", "0xFF5A50"}, {"Green", "0x5CFF6E"}, {"Pink", "0xFF6EC7"}};
  SettingDesc color{.name = "Color", .kind = SettingKind::kChoice, .initial = "Cyan", .tooltip = "The text's color on the screen"};
  for (const auto& [name, hex] : colors) color.choices.push_back(name);
  library.Add({.id = "log.print_string", .title = "Print String", .category = "Log",
      .description = "Shows In String in the screen's top right corner for Duration seconds, newest at the top, and writes it to the log. Font Size is in pixels (0: the default). A Key replaces the shown line with the same key instead of adding one, so a value printed every frame is one line that changes. For debugging.",
      .keywords = "debug screen text message print",
      .pins = {{ExecPin(), DataPin("In String", PinKind::kAny, "Hello"), DataPin("Duration", PinKind::kFloat, 2.0), DataPin("Font Size", PinKind::kFloat, 0.0), DataPin("Key", PinKind::kString, ""), DataPin("Print to Screen", PinKind::kBool, true), DataPin("Print to Log", PinKind::kBool, true)}, {ExecPin(kExecOut)}},
      .settings = {color},
      .emit = [colors](Emitter& emit) {
        const std::string screen = emit.Input("Print to Screen");
        const std::string log = emit.Input("Print to Log");
        if (screen == "false" && log == "false") return;
        std::string text = "tostring(" + emit.Input("In String") + ")";
        // Worked out once when it goes to both.
        if (screen != "false" && log != "false") {
          const std::string local = emit.Output("Text");
          emit.Line(local + " = " + text);
          text = local;
        }
        const auto found = std::find_if(colors.begin(), colors.end(), [&](const auto& entry) { return entry.first == emit.Setting("Color"); });
        const std::string hex = found != colors.end() ? found->second : colors.front().second;
        // A flag typed in decides now; one that's linked, as the script runs.
        const auto write = [&emit](const std::string& when, const std::string& line) {
          if (when == "true") emit.Line(line);
          else if (when != "false") emit.Line("if " + when + " then " + line + " end");
        };
        // An empty Key is none (PrintToScreen).
        write(screen, "rex.screen_text(" + text + ", " + emit.Input("Duration") + ", " + hex + ", " + emit.Input("Font Size") + ", " + emit.Input("Key") + ")");
        write(log, "rex.info(" + text + ")");
      }});

  library.Add(Pure("time.now", "Now", "Time", "A clock in seconds, for timing things.", {}, DataPin("Seconds", PinKind::kFloat), [](Emitter&) { return std::string("rex.now()"); }));

  const std::string input = "Input";
  library.Add({.id = "input.down", .title = "Key Down", .category = input,
      .description = "True while the key is held.",
      .role = NodeRole::kPure, .pins = {{}, {DataPin("Down", PinKind::kBool)}},
      .settings = {{.name = "Key", .kind = SettingKind::kChoice, .initial = "W", .choices = KeyNames()}},
      .title_of = [](const Node& node) { return Titled(node, "Key ", "Key", " Down"); },
      .expression = [](Emitter& emit, std::string_view) { return "rex.input.down(" + Quoted(emit, "Key") + ")"; }});

  const std::vector<std::string> buttons = {"Left", "Right", "Middle", "Back", "Forward"};
  library.Add({.id = "input.mouse_button", .title = "Mouse Button Down", .category = input,
      .description = "True while the mouse button is held.",
      .role = NodeRole::kPure, .pins = {{}, {DataPin("Down", PinKind::kBool)}},
      .settings = {{.name = "Button", .kind = SettingKind::kChoice, .initial = "Left", .choices = buttons}},
      .title_of = [](const Node& node) { return Titled(node, "Mouse ", "Button", " Down"); },
      .expression = [buttons](Emitter& emit, std::string_view) {
        const auto found = std::find(buttons.begin(), buttons.end(), emit.Setting("Button"));
        return "rex.input.mouse_button(" + std::to_string(found != buttons.end() ? found - buttons.begin() : 0) + ")";
      }});

  library.Add(Pure("input.accepts", "Input Allowed", input, "True while the game window has focus and no menu wants the input. Check it before acting on a held key.", {}, DataPin("Allowed", PinKind::kBool), [](Emitter&) { return std::string("rex.input.accepts()"); }));

  library.Add({.id = "input.mouse_motion", .title = "Read Mouse Motion", .category = input,
      .description = "How far the mouse moved since the last read.",
      .pins = {{ExecPin()}, {ExecPin(kExecOut), DataPin("X", PinKind::kFloat), DataPin("Y", PinKind::kFloat)}},
      .emit = [](Emitter& emit) {
        const std::string x = emit.Output("X");
        const std::string y = emit.Output("Y");
        emit.Line(x + ", " + y + " = rex.input.mouse_delta()");
      }});

  library.Add({.id = "input.wheel", .title = "Read Wheel", .category = input,
      .description = "How far the mouse wheel turned since the last read.",
      .pins = {{ExecPin()}, {ExecPin(kExecOut), DataPin("Amount", PinKind::kFloat)}},
      .emit = [](Emitter& emit) { emit.Line(emit.Output("Amount") + " = rex.input.wheel()"); }});

  library.Add({.id = "input.hold", .title = "Hold Game Input", .category = input,
      .description = "While held, the game doesn't see the keyboard and mouse, for a mod that drives the camera itself.",
      .pins = {{ExecPin(), DataPin("Held", PinKind::kBool, true)}, {ExecPin(kExecOut)}},
      .emit = [](Emitter& emit) { emit.Line("rex.input.hold_guest(" + emit.Input("Held") + ")"); }});

  const std::string settings = "Settings";
  const SettingDesc name{.name = "Name", .kind = SettingKind::kText, .tooltip = "The cvar's name"};

  library.Add({.id = "cvar.get", .title = "Get Setting", .category = settings,
      .description = "A cvar's value: one the game has, or one a mod added.",
      .role = NodeRole::kPure, .pins = {{}, {DataPin("Value", PinKind::kAny)}},
      .settings = {name},
      .title_of = [](const Node& node) { return Titled(node, "Get ", "Name"); },
      .expression = [](Emitter& emit, std::string_view) { return "rex.cvar.get(" + NamedSetting(emit, "Name") + ")"; }});

  library.Add({.id = "cvar.set", .title = "Set Setting", .category = settings,
      .description = "Changes a cvar.",
      .pins = {{ExecPin(), DataPin("Value", PinKind::kAny)}, {ExecPin(kExecOut)}},
      .settings = {name},
      .title_of = [](const Node& node) { return Titled(node, "Set ", "Name"); },
      .emit = [](Emitter& emit) { emit.Line("rex.cvar.set(" + NamedSetting(emit, "Name") + ", " + emit.Input("Value") + ")"); }});

  library.Add({.id = "cvar.define", .title = "Add Setting", .category = settings,
      .description = "Adds a cvar of the mod's own, which shows in the game's settings and is saved like any other. Usually from On Start.",
      .pins = {{ExecPin(), DataPin("Default", PinKind::kBool)}, {ExecPin(kExecOut)}},
      .settings = {name, {.name = "Type", .kind = SettingKind::kChoice, .initial = "boolean", .choices = {"boolean", "int32", "int64", "uint32", "uint64", "number", "string"}},
                   {.name = "Category", .kind = SettingKind::kText, .initial = "Mods"}, {.name = "Description", .kind = SettingKind::kText}},
      .shape = [](const Node& node, const Graph&, NodePins& pins) { Retype(pins.inputs, "Default", CvarKind(SettingOf(node, "Type"))); },
      .title_of = [](const Node& node) { return Titled(node, "Add Setting ", "Name"); },
      .emit = [](Emitter& emit) { emit.Line("rex.cvar." + emit.Setting("Type") + "(" + NamedSetting(emit, "Name") + ", " + emit.Input("Default") + ", " + Quoted(emit, "Category") + ", " + Quoted(emit, "Description") + ")"); }});

  const SettingDesc switch_name{.name = "Name", .kind = SettingKind::kText, .tooltip = "The switch's name, as the game's compiled-in code reads it"};
  library.Add({.id = "switch.get", .title = "Get Switch", .category = settings,
      .description = "A switch the game's compiled-in code reads.",
      .role = NodeRole::kPure, .pins = {{}, {DataPin("Value", PinKind::kInt)}},
      .settings = {switch_name},
      .title_of = [](const Node& node) { return Titled(node, "Get Switch ", "Name"); },
      .expression = [](Emitter& emit, std::string_view) { return "rex.switch.get(" + NamedSetting(emit, "Name") + ")"; }});

  library.Add({.id = "switch.set", .title = "Set Switch", .category = settings,
      .description = "Sets a switch the game's compiled-in code reads. Switches go back to 0 when the scripts reload.",
      .pins = {{ExecPin(), DataPin("Value", PinKind::kInt, "1")}, {ExecPin(kExecOut)}},
      .settings = {switch_name},
      .title_of = [](const Node& node) { return Titled(node, "Set Switch ", "Name"); },
      .emit = [](Emitter& emit) { emit.Line("rex.switch.set(" + NamedSetting(emit, "Name") + ", " + emit.Input("Value") + ")"); }});
}

// Mod -------------------------------------------------------------------------------------------------

// One of the mod's own Lua scripts, as a node: the file runs on its own like every script of the mod, so the graph
// shows it and edits its code without writing it into the graph's script again.
void AddScripts(Library& library) {
  library.Add({.id = "mod.script", .title = "Script File", .category = "Mod",
      .description = "One of the mod's Lua scripts (scripts/<File>). It runs on its own, as every script of the mod does. Select the node to read and edit its code in the details panel.",
      .role = NodeRole::kEvent, .pins = {{}, {}},
      .settings = {{.name = "File", .kind = SettingKind::kScript, .tooltip = "The script's file in the mod's scripts folder, like main.lua"}},
      .title_of = [](const Node& node) { return Titled(node, "Script ", "File"); },
      .emit = [](Emitter& emit) {
        const std::string file = emit.Setting("File");
        if (file.empty()) emit.Error("Needs a script file");
        if (file.ends_with(".graph.lua")) emit.Error("A graph's own script is written by the graph");
        emit.Line("-- scripts/" + file + " runs on its own");
      }});
}

// Lua as written, for what nodes don't say: helpers and tables the whole script shares, statements in a chain, and a
// value worked out from the node's inputs.
void AddLua(Library& library) {
  const std::string category = "Lua";
  library.Add({.id = "lua.chunk", .title = "Lua Chunk", .category = category,
      .description = "Lua the whole script shares, written at its top before every event: local helper functions and tables the other nodes' Lua uses.",
      .role = NodeRole::kEvent, .pins = {{}, {}},
      .settings = {{.name = "Name", .kind = SettingKind::kText, .initial = "Helpers", .tooltip = "What the chunk is called on the canvas"},
                   {.name = "Code", .kind = SettingKind::kCode}},
      .title_of = [](const Node& node) { return Titled(node, "", "Name"); },
      .emit = [](Emitter& emit) { emit.Helper("chunk:" + std::to_string(emit.node().id), emit.Setting("Code")); }});

  // $A to $D in the Lua are the inputs.
  auto withInputs = [](Emitter& emit, std::string code) {
    for (const char* pin : {"A", "B", "C", "D"}) {
      const std::string mark = std::string("$") + pin;
      if (code.find(mark) == std::string::npos) continue;
      const std::string value = emit.Input(pin);
      for (size_t at = code.find(mark); at != std::string::npos; at = code.find(mark, at + value.size())) code.replace(at, mark.size(), value);
    }
    return code;
  };
  const std::vector<Pin> inputs = {DataPin("A", PinKind::kAny), DataPin("B", PinKind::kAny), DataPin("C", PinKind::kAny), DataPin("D", PinKind::kAny)};

  std::vector<Pin> code_in = {ExecPin()};
  code_in.insert(code_in.end(), inputs.begin(), inputs.end());
  library.Add({.id = "lua.code", .title = "Lua Code", .category = category,
      .description = "Statements run where the chain reaches it. $A to $D are the inputs.",
      .pins = {code_in, {ExecPin(kExecOut)}},
      .settings = {{.name = "Code", .kind = SettingKind::kCode}},
      .emit = [withInputs](Emitter& emit) {
        std::istringstream lines(withInputs(emit, emit.Setting("Code")));
        for (std::string line; std::getline(lines, line);) emit.Line(line);
        emit.Chain(kExecOut);
      }});

  library.Add({.id = "lua.expression", .title = "Lua Expression", .category = category,
      .description = "A value worked out by a Lua expression. $A to $D are the inputs.",
      .role = NodeRole::kPure, .pins = {inputs, {DataPin("Value", PinKind::kAny)}},
      .settings = {{.name = "Expression", .kind = SettingKind::kText, .initial = "$A"}},
      .expression = [withInputs](Emitter& emit, std::string_view) { return "(" + withInputs(emit, emit.Setting("Expression")) + ")"; }});
}

}  // namespace

void AddBuiltinNodes(Library& library) {
  AddScripts(library);
  AddLua(library);
  AddEvents(library);
  AddCustomEvent(library);
  AddFlow(library);
  AddGuest(library);
  AddMemory(library);
  AddVariables(library);
  AddRoutines(library);
  AddPicks(library);
  AddTimers(library);
  AddDispatchers(library);
  AddMath(library);
  AddLogic(library);
  AddText(library);
  AddTables(library);
  AddHost(library);
}

}  // namespace rexfabric::graph
