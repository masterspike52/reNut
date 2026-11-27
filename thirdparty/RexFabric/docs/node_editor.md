# The node editor

The node editor makes Lua mods out of node graphs, inside the running game. A graph is laid out like a blueprint: events start chains of actions, and values flow between nodes along links. Saving a graph compiles it to an ordinary Lua script in the mod's `scripts` folder, so a mod made in the editor runs, and ships, like any other Lua mod. Save and Reload runs it at once.

Because it runs in the game, the editor knows everything the game declared for its mods (see [game_api.md](game_api.md)). Every function, type, field, method, enum, constant and event the game offers turns up as a node without anyone writing nodes for it, and hook addresses are checked against the functions that can really be hooked.

## Turning it on

```cmake
rexfabric_attach(my_game WITH_NODE_EDITOR)      # with WITH_GUIDE and WITH_TEXTURES as before, if wanted
```

```cpp
rexfabric::Setup({.menu_key = "F5", .node_editor_key = "F6", .frame_function = 0x8266FBB0});
```

`node_editor_key` is optional; it becomes the rebindable `bind_node_editor`. Without it the editor opens from the mods menu: a Node Editor button in the plain Mods window, or a Node Editor action in the Guide's Mods blade, which closes the Guide to make way for the mouse. The game can also call `rexfabric::ShowNodeEditor()`.

While the editor is open, the game's own input is held, as with the mods menu, and `rexfabric::MenuShown()` is true, so a game that checks it before handing input to mods keeps mod hotkeys quiet while someone types.

The editor brings imgui-node-editor (MIT) in `thirdparty/imgui-node-editor`, at the commit Toon pins (27bcace). It builds against the SDK's ImGui; since ImGui lives in the runtime library, which exports its functions but not its data, RexFabric builds it with `GImGui` read through `ImGui::GetCurrentContext()`.

A game with node graphs of its own (ReTiP's material editor) links the same copy with `rexfabric_link_imgui_node_editor(<target>)` rather than building another, as two copies in one program would be the same symbols twice. It works with or without `WITH_NODE_EDITOR`. Code written for an older imgui-node-editor may need `Config::SettingsFile` renamed to `SettingsName`.

## Files

```
mods/
  MyMod/
    graphs/
      Main.json             the graph, as the editor saves it
    scripts/
      Main.graph.lua        what it compiles to; runs like any other script of the mod
      helpers.lua           hand written scripts sit beside it as usual
    mod.toml
```

The script is rewritten each time the graph is saved, and says so on its first lines, so change the graph rather than the script. A mod can mix graphs and hand written scripts freely. To ship a mod, ship the folder; the `graphs` folder is only needed by whoever edits it further.

A graph with errors still saves, but its script then only logs that the graph has problems, rather than running half a mod.

New Mod (in the mod picker) makes a mod folder with a `mod.toml`, enables it at the end of the load order and starts a graph named Main in it.

## The window

| Part | Holds |
|---|---|
| Toolbar | The mod picker, Save, Save and Reload, Undo, Redo, and the graph's state: no problems, warnings, or how many errors |
| Left | The mod's graphs, with the open one's Event Graph under it, then the graph's functions, macros, variables and event dispatchers. Each section's round + adds one; right click a row to delete it (or, for a graph, rename it) |
| Middle | The canvas, with the graph it shows above it (Event Graph, or Event Graph > Function Name), and a Lua tab showing exactly what the graph saves as |
| Right | The selected node's settings, or the selected function, macro or variable, the graph's problems, and the error from the last time the mod's scripts ran |

| Control | Does |
|---|---|
| Right click | Add a node where the mouse is. Type to search; Enter takes the first match. Operators find their nodes too: `<`, `>=`, `++`, `--`, `..` |
| Right click a node | What can be done with it: a Bool's Get becomes its own branch (Convert to Branch), Get and Set turn into each other, a function's or macro's node opens its graph, Duplicate, Delete |
| Drag a variable from the sidebar | Ask whether to Get, Set, branch on (Bools) or Increment and Decrement (numbers) it where it's dropped. Ctrl drops a Get at once, Alt a Set |
| Drag a function or macro from the sidebar | Use it where it's dropped |
| Drag an event dispatcher from the sidebar | Ask whether to Call it, Bind an event to it, Unbind one or all, or add a Custom Event taking its params |
| Drag from an event's top right corner | The square delegate pin: link it to Set Timer by Event or Bind Event, and they run the event's chain |
| Drag from a pin into empty space | Add a node connected to that pin; the menu only offers nodes with a pin that fits |
| Drag between pins | Link them, if their types fit; a tooltip says why not when they don't |
| Double click a link | Add a reroute there: a dot the link passes through, dragged to lead the link around things. Its grips take more links, in on the left and out on the right. Deleting it joins its link up again |
| Wheel, right drag | Zoom, pan |
| Delete | Remove the selected nodes or links |
| Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+D | Copy, cut, paste and duplicate nodes. The clipboard is text, so nodes go between graphs and between games |
| Ctrl+S | Save |
| Ctrl+Z, Ctrl+Y (or Ctrl+Shift+Z) | Undo, redo. Typing into a field is one step once it stops |
| Click a problem | Show the graph its node is in, select the node and bring it into view |

Unlinked inputs are typed into on the node itself. Whole numbers are typed as text, so `0x82558D28` stays hex in the script. Nodes with errors get a red rim. A variable's Get is drawn compact, as one row: the variable's name and its value's pin.

With RexGlass in the game, the whole window is Toon's drifting aurora, the canvas and the panels around it alike. The sidebar and the details panel are glass panes over it, the buttons are pills of glass lit in what they do (blue to save, green to add, red to delete), and the nodes are glass panels tinted by kind, as in Toon. The window keeps RexGlass's theme while it's open even in a game that keeps its own look elsewhere. When the game's renderer hands RexGlass the screen as drawn so far (its `Config::screen`, as ReTiP does), each node refracts what's really under it: the aurora, the grid, wires running under it and other nodes. Without that, a node refracts a description of the canvas, its aurora and grid. Without RexGlass the nodes are flat tinted shapes over a shifting gradient, and the buttons are plain ones in the same colors.

## How a graph runs

Events start chains. An event node (On Frame, Before Function, Key Pressed) has an exec output, drawn as an arrow; following the exec links from it gives the actions that run when the event happens, in order. An action runs when its chain reaches it, and its own exec outputs say what runs next. Branch, Sequence, For Loop, Do Once and Flip Flop shape the chain.

Values flow along the other links, from outputs on the right of a node to inputs on the left. A pure node, one without exec pins (Add, Get Register, Read Memory), is worked out wherever its output is read, every time it's read, as in a blueprint. An action's outputs (Call Function's Result) are set when it runs, so they can only be read further along the same event's chain. An event's own outputs (On Frame's Seconds, a hook's Registers) only exist in that event's chain.

Variables belong to the whole graph and keep their values between events: a count of calls, a toggle a key flips. Each one becomes a local at the top of the script.

### Functions and macros

A graph can have functions and macros of its own, each with a graph of its own, opened from the sidebar. Each graph starts with an inputs node, named after it, whose outputs are its inputs, and an Outputs node, whose inputs are its outputs; the details panel adds, renames, retypes and removes them, and the nodes using it follow. Neither node can be deleted or copied. Events only go in the event graph.

| | Function | Macro |
|---|---|---|
| In the script | A Lua function, declared before the events so any graph can call it; functions can call each other or themselves | Written out in full wherever it's used, with its own locals for each use (`m12_n4_Value`, for the macro used by node 12) |
| Exec pins | One in and one out (Then), unless it's Pure | Any number, in and out, named as you like: a macro can be flow of its own, as Branch is |
| Data | Inputs are parameters, outputs are returned (`a, b = Name(x)`) | Inputs are whatever the node using it is given; outputs are worked out inside it |
| Pure | No exec pins: worked out wherever its outputs are read, like Add | A macro without an exec input is pure the same way |

A macro can't use itself, directly or through another macro, as it would be written out forever. A function whose chain ends before its Outputs node returns nothing.

```lua
local Twice

-- Twice
function Twice(n1_X)
  return (n1_X * 2)
end

-- On Start
do
  rex.info("twice", Twice(21))
end
```

### Timers, delegates and event dispatchers

As in Unreal, an event can be handed to something that runs it later. Events that can (On Start, On Frame, Key Pressed, Setting Changed, the game's events and Custom Event) have a square red delegate pin in their title's top right corner. Linked to a node's Event input, the event's chain runs when that node says, as well as when the event itself happens. Hooks and midasm sites have none, as their chains need the registers of the call they run in.

| Node | Does |
|---|---|
| Delay | Goes on through Completed once Duration seconds have passed. Reached again while it waits, it does nothing |
| Set Timer by Event | Runs the event linked to Event once after Time seconds, or every Time seconds when Looping. Its Handle stops it |
| Clear Timer | Stops the timer with that Handle |
| Custom Event | An event nothing in the game fires: it runs when a timer or a dispatcher runs it. With Params From set to a dispatcher, it has that dispatcher's params as outputs |
| Call | Runs every event bound to the dispatcher, in the order they were bound, with its params |
| Bind Event to | Binds the event linked to Event to the dispatcher. Bound already, it stays bound once |
| Unbind Event from, Unbind All from | Unbinds one event, or every one |

Event dispatchers are made in the sidebar like variables, each with params of its own, which Call takes and which the events bound to it get. A bound event gets them in order as its outputs: a custom event taking the dispatcher's params gets them by name. A Handle, or anything else one chain sets, reaches another chain through a variable, as in Unreal: set it where the timer starts, read it in the event the timer runs.

In the script, an event whose delegate pin is linked has its chain written once, as a function named after the node (`n8_Run`), before any event, and its own registration calls that function. A timer is `rex.after` or `rex.every` with it, and a dispatcher is a table of them named after the dispatcher:

```lua
local Ticked = {}
local n8_Run

-- Tick, which a timer or a dispatcher can run too
function n8_Run()
  Ticks = Ticks + 1
  for _, run in ipairs({table.unpack(Ticked)}) do run(Ticks) end
end

-- On Start
do
  TickTimer = rex.every(0.5, n8_Run)
end
```

Timers run on frames, so nothing set in On Start runs before the game's first frame. [lua_mods.md](lua_mods.md) has `rex.after`, `rex.every` and `rex.cancel`.

### Switch and Select

As Unreal's Switch and Select nodes. Both pick by an Int, a String or one of the game's enums, chosen in the details panel; Select can pick by a Bool too.

| Node | Does |
|---|---|
| Switch | Goes on through the case Selection matches, or Default when it matches none. A case without a link does nothing, rather than going to Default |
| Select | Gives the option Index picks, or nil when it picks none. Every option is worked out, as in Unreal |

On an Int or a String, the cases or options are added in the details panel: Add Case or Add Option adds one (the next number, when they're all numbers), each can be renamed with Enter, and x removes one. Each is a pin of the node, named by its value, so renaming one keeps its links and a typed value. On a Bool or an enum, the values are the cases or options, and the list isn't shown.

A Select's options take the type of what's linked into them, or else of what its value goes to; until then they're Any. So dragging off a Bool input and typing select gives a Select of Bools, with a checkbox for each option.

```lua
n14_Selection = n11_Count
if n14_Selection == 1 then
elseif n14_Selection == 4 then
  rex.cancel(TickTimer)
end

if ({[1] = false, [2] = true})[Mode] then
```

### Drawing

The Draw nodes draw over the game, as `rex.draw` does: Draw Text, Draw Rectangle, Draw Line and Draw Circle, and Remove
Drawing and Clear Drawings. Positions and sizes are fractions of the screen (0.5, 0.5 is its middle). Each shape takes
a Color (picked in the details panel), Alpha, Seconds and a Key. A shape lasts one frame unless Seconds says longer, so
draw it from On Frame to keep it on the screen; a Key replaces the shape drawn with the same key instead of adding one.

### Cast To

Every type the game declares has a Cast To node, as in Unreal: any value in, the type's object out (As Avatar for
`tip.Avatar`), and on through Valid or Not Valid. An object of another type, or an address, is one when an object of
the type is there; nil, any other value, or an object the game destroyed goes on through Not Valid. Put one in front
of anything that reads a variable another event sets: a frame that reads a hovered object before anything was
hovered, or after the object was destroyed, goes through Not Valid instead of failing.

```lua
n12_Value = Target
n12_As_Avatar = (type(n12_Value) == "userdata" or math.type(n12_Value) == "integer") and tip.Avatar(n12_Value) or nil
if n12_As_Avatar ~= nil and n12_As_Avatar.valid then
  ...
end
```

### Print String

As Unreal's: In String in the screen's top right corner for Duration seconds, newest at the top, in the Color picked in the details panel, and in the log. Print to Screen and Print to Log turn either off. It's for debugging while a mod is made; Log is for what a finished mod reports.

### Variables on the canvas

Variables belong to the whole graph and keep their values between events. Dragged onto the canvas, a variable becomes the node picked for it: Get (its value), Set, If (for a Bool: goes on through True or False as it is), or Increment and Decrement (for a number: `Count = Count + 1`, giving the new value). A Get of a Bool turns into its If from its right click menu.

Events go into the script from top to bottom as they're laid out on the canvas, so of two On Start nodes the higher runs first, and of two hooks on one function the higher is added first.

Here is a graph that counts calls to a function and, past a hundred, logs each call and stops the function from running, with the Lua it saves:

```lua
-- Made with the RexFabric node editor from graphs/Main.json.
-- Saving the graph again replaces this file, so change the graph rather than this file.

local Count = 0  -- calls so far

-- Before 0x8266FBB0
rex.hook(0x8266FBB0, function(n1_Registers)
  local n1_skip
  Count = (Count + 1)
  if (Count > 100) then
    rex.info("calls", Count)
    n1_skip = true
  end
  if n1_skip then return false end
end)
```

A node's own names in the script start with `n` and its id; the details panel shows each node's id, so the script and the canvas are easy to match up.

A reroute takes the type of what comes into it, or else of what it goes to, and is drawn in that color. One with no links at all takes the first link of any kind, exec or data.

### Types

| Pin | Color | Carries |
|---|---|---|
| Exec | White arrow | The order things run in |
| Bool | Red | true or false |
| Int | Teal | A whole number: addresses, counts, integer registers |
| Float | Green | A number with a fraction: float registers, seconds |
| Number | Pale green | Either. Math nodes have Number pins, and give an Int when everything going in is one, so `r3 + 0x10` is still an address and `r3 + 1.5` isn't |
| String | Pink | Text |
| Object | Blue | One of the game's objects, of the type the pin names |
| Registers | Orange | A hook's or a midasm site's registers |
| Any | Grey | Whatever comes in: a cvar's value, a game event's arguments |

Ints go into Float pins. Anything goes into an Any pin, and an Any output goes anywhere, as Lua won't know until it runs. Hovering a pin shows its name and type.

## Nodes

| Category | Nodes | Stand for |
|---|---|---|
| Events | On Start, On Frame, Before Function, After Function, Midasm Site, Key Pressed, Setting Changed, Custom Event | Running the chain at load, `rex.on("frame")`, `rex.hook`, `rex.hook_after`, `rex.midasm`, a frame listener checking `rex.input.pressed` and `accepts`, `rex.cvar.on_change` |
| Flow | Branch, Switch, Sequence, For Loop, Do Once, Flip Flop, and Reroute (from double clicking a link) | `if`, an `if` and `elseif` per case, chains in turn, `for`, a flag kept between runs, alternating outputs. A reroute writes nothing of its own: an exec chain runs on through it, and a value is read through it |
| Hooks | Skip Original, Take Branch, Get Register, Set Register, Call Function | Returning false from a before hook, returning true from a midasm site, `ctx.r3`, `ctx.r3 = v`, `rex.call` |
| Memory | Read Memory, Write Memory | `rex.mem.u32` and the rest, by the node's Type |
| Variables | Get, Set, If (Bools), Increment and Decrement (numbers), per variable | The graph's variables |
| Functions, Macros | One per function or macro of the graph's | Calling it, or writing it out |
| Math | Add, Subtract, Multiply, Divide, Floor Divide, Modulo, Min, Max, Clamp, Lerp, Negate, Increment, Decrement, Abs, Floor, Ceil, Round, To Float, Square Root, Sin, Cos, Random Float, Random Integer, and under Bits: And, Or, Xor, Not, Shift Left, Shift Right | Lua's operators and `math` |
| Logic | And, Or, Not, Equal, Not Equal, Less, Less Or Equal, Greater, Greater Or Equal, Is Set, Select | Comparisons, a test for nil, and a pick from a table of options |
| Text | Join, To Text, Hex, Length, Contains | `..`, `tostring`, `string.format("%08X")`, `#`, `string.find` |
| Log | Log, at info, warn, error or debug; Print String | `rex.info` and the rest; `rex.screen_text` and `rex.info` |
| Time | Now, Delay, Set Timer by Event, Clear Timer | `rex.now()`, `rex.after`, `rex.every`, `rex.cancel` |
| Event Dispatchers | Call, Bind Event to, Unbind Event from, Unbind All from, per dispatcher | The graph's event dispatchers |
| Input | Key Down, Mouse Button Down, Input Allowed, Read Mouse Motion, Read Wheel, Hold Game Input | `rex.input` |
| Settings | Get Setting, Set Setting, Add Setting, Get Switch, Set Switch | `rex.cvar` and `rex.switch` |
| Game | Address Of, then one folder per namespace | The game's API, below |

Each node's description shows when it's hovered in the add menu, and in the details panel when it's selected. [lua_mods.md](lua_mods.md) has the API they stand for.

A few rules the editor checks:

* Skip Original only works in a Before Function chain, and Take Branch in a Midasm Site chain.
* Call Function can't run from On Start, as the game can't be called before it runs; call it from a hook or a frame event.
* Two Key Pressed nodes on the same key get a warning, as `rex.input.pressed` tracks one press per key and only one of them would see it.
* A hook on an address that isn't the start of a recompiled function gets a warning, as it would never run.
* An action no event leads to gets a warning: it never runs. The nodes of a macro nothing uses don't.
* An event in a function's or macro's graph is an error: events only run in the event graph.
* Functions, macros and variables share one set of names, which have to be Lua names that hide nothing.
* If and Increment check their variable is a Bool, or a number.

### The game's nodes

For each namespace the game declared, under Game and the namespace's name:

| Declared | Nodes | Lua |
|---|---|---|
| A function | A node that calls it, with an input per parameter and a Result when it returns something | `tip.garden.money()` |
| A constant | A node giving its value | `tip.max_pinatas` |
| An enum | A node giving one of its values, picked on the node | `tip.Species.Whirlm` |
| A type | Make (an object from an address), and a Get and Set per field; arrays get an Index from 1 | `tip.Pinata(addr)`, `p.happiness`, `p.needs[i]` |
| A method | A node that calls it on Target | `p:feed(item)` |
| An event | An event node with three Any outputs, for whatever the game fires it with | `rex.on("tip.garden_loaded", ...)` |

Game functions and methods are actions, since the editor can't know which only read. Fields of a struct type and C string fields have no Set, as Lua can't assign them.

A graph names game nodes by what they stand for (`game.call:tip.garden.money`), so it keeps working as long as the game declares it. A graph opened where the game no longer declares something keeps the node, marked as unknown, so nothing is lost.

## Problems

Errors stop a graph from running: an input that needs a link, text that isn't a number, a value read outside the chain it exists in, a chain that runs back into itself (a For Loop is the way to repeat things), a variable named like a Lua keyword or a global the script needs. Warnings don't: an action that never runs, a hook that would never fire. The toolbar counts them, the problems list on the right names each with its node, and clicking one finds the node.

When the mod's scripts last ran into a Lua error, it shows under Last run, with the line in the `.graph.lua` file it happened on.

## Adding nodes

Every node type is a `NodeDesc` in `src/graph/nodes_builtin.cpp` (or, for nodes made from the game's declarations, `src/graph/nodes_game.cpp`). The description gives its pins and settings and how it becomes Lua; the editor, the file format and the compiler need nothing else.

```cpp
library.Add({.id = "text.upper", .title = "Upper Case", .category = "Text",
    .description = "Text in capitals.",
    .role = NodeRole::kPure, .pins = {{DataPin("Text", PinKind::kString)}, {DataPin("Result", PinKind::kString)}},
    .expression = [](Emitter& emit, std::string_view) { return "string.upper(" + emit.Input("Text") + ")"; }});
```

| Field | Is |
|---|---|
| `id` | Stored in graph files, so it never changes once shipped |
| `category` | Where the add menu puts it; slashes nest, `Math/Bits` |
| `role` | `kEvent` starts chains, `kAction` runs in one, `kPure` is an expression |
| `pins` | Inputs and outputs. `ExecPin()` and `ExecPin(kExecOut)` are the usual exec pins; `DataPin` and `ObjectPin` carry values |
| `settings` | Choices made in the details panel: an address, text, one of a list, a variable |
| `shape` | Changes the pins by the settings: Get Register's output is a Float for an f register |
| `title_of` | A title with the settings in it, "Before 0x8266FBB0" |
| `emit` | Writes an event's or an action's Lua through the `Emitter` |
| `expression` | Gives a pure node's Lua expression for one of its outputs |

The `Emitter` (`src/graph/lua_compiler.h`) is what `emit` and `expression` write with:

| Call | Does |
|---|---|
| `Input(pin)` | An input as a Lua expression: what its link brings, or the value typed into it |
| `Setting(name)`, `Address(name)` | A setting, or one as a hex address literal |
| `Line(text)`, `Indent()`, `Dedent()` | Writes the script |
| `Chain(output)` | Writes what an exec output leads to. An action's Then is followed by itself unless its emit did, or called `EndChain` |
| `Output(pin)` | An action's output, as a local of the event's function |
| `Param(pin)` | An event's output, as a parameter of its function |
| `Open()`, `Close()` | An event's function body, whose locals Close declares at its top |
| `State(name, initial)` | A local kept between runs, at the top of the script |
| `EventLocal(name)` | A local shared by an event's nodes, such as Skip Original's flag |
| `Error(text)`, `Warn(text)` | A problem with the node |

The pieces, for finding your way around:

| File | Is |
|---|---|
| `src/graph/graph.*` | The graph: nodes, links, variables. Plain data |
| `src/graph/node_library.*` | The node descriptions, pins and the linking rules |
| `src/graph/nodes_builtin.cpp`, `nodes_game.cpp` | The nodes |
| `src/graph/lua_compiler.*` | Graph to Lua |
| `src/graph/graph_file.*` | The JSON format, for files and the clipboard |
| `src/graph/graph_document.*` | A graph on disk, with its script and undo history |
| `src/ui/node_editor.*` | The window |
| `src/ui/node_graph_style.*`, `node_graph_widgets.*` | The canvas's look and pieces, ported from Toon's node graph editors |
| `src/ui/glass.h` | RexGlass when the game has it, plain ImGui otherwise |

Only the window needs ImGui; the rest builds and runs on its own, which is how the compiler is tested.
