# The node editor

The node editor makes Lua mods out of node graphs, inside the running game, much like Unreal's
Blueprints. You lay out events, actions and values as nodes, link them, and the editor writes the
Lua for you. The result is an ordinary Lua script in your mod's `scripts` folder, so a mod made in
the editor ships and runs like any hand-written one: players don't need the graph, only the script.

Everything a node can do is something a script could do by hand. The `rex` calls the nodes write
are documented in [rexfabric-api.md](rexfabric-api.md), the `renut` ones in
[renut-api.md](renut-api.md). How a mod folder and its scripts load is in
[mods-and-scripts.md](mods-and-scripts.md).

## Contents

- [Opening and closing it](#opening-and-closing-it)
- [The window](#the-window)
- [Working on the canvas](#working-on-the-canvas)
- [Pins, types and typed-in values](#pins-types-and-typed-in-values)
- [The nodes](#the-nodes)
- [Variables, functions, macros and event dispatchers](#variables-functions-macros-and-event-dispatchers)
- [Saving: graphs and the Lua they compile to](#saving-graphs-and-the-lua-they-compile-to)
- [Problems and warnings](#problems-and-warnings)
- [Tutorial: a press counter](#tutorial-a-press-counter)

## Opening and closing it

- **F10** opens and closes it. The key is the rebindable setting `ui.bind.node_editor`.
- Or press **F9** for the mods menu and pick **Node Editor** in its action bar. The mods menu
  steps aside, since the editor wants the mouse.
- Closing the window with unsaved changes asks first: **Save**, **Don't Save** or **Cancel**. The
  same question comes up when you switch to another mod or graph.
- While it's open it holds the game's input, so typing and clicking in it doesn't play the game.
- It reopens on the mod and graph you had open last (for as long as reNut runs). The first time, it
  opens the first mod that has graphs.

## The window

**Toolbar (top):**

| Item | What |
|---|---|
| **Mod** | Picks the mod to work on. Every mod in the mods folder is listed, disabled ones marked `(off)`. **New Mod...** makes one (below). |
| **Save** | Writes the graph and its script (**Ctrl+S**). |
| **Save and Reload** | Saves, then runs Reload Scripts: every enabled mod's Lua runs again, this one included. |
| **Undo**, **Redo** | **Ctrl+Z**, **Ctrl+Y** (or **Ctrl+Shift+Z**). Up to 200 steps per graph. |
| Status | The last action's result, or a count: "No problems", "N warnings" or "N problems; it won't run until they're fixed". |

**New Mod...** asks for a name (letters, digits, spaces, `_` and `-`), then makes
`mods/<name>/` with a `scripts` folder and a `mod.toml`
(`name`, `version = "1.0.0"`, `description = "Made with the node editor"`), enables the mod at the
end of the load order, and starts a graph named **Main** in it.

**Left sidebar**, one section each, with a **+** to add:

| Section | What |
|---|---|
| **Graphs** | The mod's graphs. Each is a script of its own. Click one to open it; under the open one, **Event Graph** shows its main canvas. Right click the open graph to **Rename...** or **Delete...** it. |
| **Functions** | The graph's functions: graphs of their own, called from other graphs. |
| **Macros** | The graph's macros: nodes written out wherever they're used. |
| **Variables** | Values the whole graph shares, kept between events. |
| **Event Dispatchers** | Lists of events that run together when the dispatcher is called. |

Variables, functions, macros and dispatchers can be dragged onto the canvas, and right clicked to
delete (the nodes using them go too).

**Middle:** two tabs. **Graph** is the canvas, with a breadcrumb (**Event Graph** > the function
or macro shown). **Lua** shows the script the graph compiles to, live, with a **Copy** button.

**Right (details):** what's selected. For a node: its settings and its inputs' values. For a
variable, function, macro or dispatcher: its name, type, inputs and outputs, comment and a delete
button. Below that, **Problems** (click one to jump to its node) and **Last run**, the error this
mod's scripts gave the last time they ran.

## Working on the canvas

| To | Do |
|---|---|
| Zoom | Mouse wheel. |
| Pan | Drag with the right mouse button. |
| Add a node | Right click empty canvas: the add menu. Type to search (titles, categories and keywords such as `+`, `<`, `++`); **Enter** takes the first match. |
| Add a node already linked | Drag from a pin into empty space and let go: the add menu only lists nodes with a pin that fits, and links the new node to it. |
| Link two pins | Drag from one to the other. An exec output and a data input hold one link, so a new one replaces the old. |
| Act on a node | Right click it: **Duplicate**, **Delete**, **Open Function / Open Macro**, and for variables **Convert to Branch / Set / Get**. |
| Reroute a link | Double click the link to add a reroute point there, then drag the point. Deleting a reroute joins its link up again. |
| Delete | Select, then **Delete**. |
| Copy, cut, paste, duplicate | **Ctrl+C**, **Ctrl+X**, **Ctrl+V**, **Ctrl+D**. The clipboard is plain text (JSON), so nodes paste between graphs. |
| Use a variable | Drag it from the sidebar: it asks **Get** or **Set**. Hold **Ctrl** while dropping for a Get, **Alt** for a Set. |
| Call a function or macro | Drag it from the sidebar, or pick it from the add menu's **Functions** / **Macros**. |
| Use a dispatcher | Drag it from the sidebar: it asks Call, Bind or Unbind. |
| Type a value | Click an unlinked input on the node and type, or pick from its list. |

## Pins, types and typed-in values

**Exec** pins (the arrows) carry the order things run in; the rest carry values.

| Type | Holds | Typed in as |
|---|---|---|
| Exec | The order things run in. | |
| Bool | `true` / `false`. | A checkbox. |
| Int | A whole number. | `42`, `-3`, `0x82E1B280`. |
| Float | A number. | A number field. |
| Number | An Int or a Float, whichever goes in: Add of two Ints gives an Int. | `3` is an Int, `3.0` or `1e3` a Float. |
| String | Text. | Text, quoted for you. |
| Object | One of the game's types (`renut.Part`, `renut.Vec`...). | Can't be typed: link one. |
| Registers | A hook's or a midasm site's registers (`ctx`). | Can't be typed. |
| Event (delegate) | An event's chain, handed to a timer or a dispatcher (the square pin in an event's top right corner). | Can't be typed. |
| Any | Whatever Lua value comes in. | Blank or `nil` is `nil`, `true`/`false` and numbers are themselves, anything else is text. |

**Fitting:** an Int goes into a Float; anything goes into and comes out of Any; an object goes
into an object pin of its type (or of any type). Links can't make a loop through data.

**Inputs with choices** (keys, registers, the game's enums and lists such as challenge or counter
names) open a list with a search box. **Use "..."** takes what you typed even when it isn't listed.

**Pure nodes** (no exec pins, such as Add or Get) are worked out wherever their outputs are read.
A pure node read twice runs twice, as in a Blueprint.

## The nodes

The add menu nests by category. Nodes come from four places: the built-in nodes, the game's
declared API, the game's Lua API definitions, and your graph's own functions and macros.

### Built-in nodes

**Events** start chains. Events marked with a delegate pin can also be run by a timer or a dispatcher.

| Node | Writes | What |
|---|---|---|
| On Start (delegate) | `do ... end` | Runs once as the scripts load. Set up settings and variables here. The game can't be called yet. |
| On Frame (delegate) | `rex.on("frame", ...)` | Every frame, with **Seconds** since the last one. |
| Before Function | `rex.hook(address, ...)` | Each time the game calls the function at **Address**, before it runs. **Registers** are its arguments. |
| After Function | `rex.hook_after(address, ...)` | Each time it returns; `r3` in **Registers** is its result and can be changed. |
| Midasm Site | `rex.midasm(address, ...)` | A compiled-in midasm site (reNut has none claimable yet). |
| Key Pressed (delegate) | `rex.input.pressed` + `accepts` in a frame listener | Once per press of **Key**, while no menu wants the keyboard. Only one Key Pressed node per key sees each press. Defaults to F9, which is reNut's mods menu key: pick another. Avoid the digit keys `0`-`9`: they're misread ([why](rexfabric-api.md#keyboard-and-mouse-rexinput)). |
| Setting Changed (delegate) | `rex.cvar.on_change` | The frame after the cvar named **Setting** changes, with its **Value**. |
| Custom Event (delegate) | a function | Runs only when a timer or dispatcher runs it. **Params From** gives it a dispatcher's params. |

**Flow:** Branch, Sequence (2 to 8 outputs), For Loop (First..Last), Do Once (with Reset), Flip
Flop, Switch (on an Int, a String or one of the game's enums; cases added in the details panel
for Int and String), and Reroute (from double clicking a link).

**Hooks:** Skip Original (only in a Before Function chain: the function doesn't run this time),
Take Branch (only in a Midasm Site chain), Get Register and Set Register (`r0`-`r31`, `f0`-`f31`,
`lr`, `ctr`), Call Function (`rex.call` with 0 to 8 integer arguments; not from On Start).

**Memory:** Read Memory and Write Memory, with **Type** `u8`...`u64`, `s8`...`s64`, `f32`, `f64`,
`string` (and `wstring` for reading).

**Variables:** Get, Set, If (a Bool variable as a branch), Increment and Decrement (number
variables). The add menu lists one entry per variable.

**Time:** Now (`rex.now()`), Delay (goes on through **Completed** after **Duration**; reached
again while waiting, it does nothing), Set Timer by Event (runs a linked event after **Time**, or
every **Time** when **Looping**; gives a **Handle**), Clear Timer.

**Event Dispatchers:** Call, Bind Event to, Unbind Event from, Unbind All from.

**Math:** Add, Subtract, Multiply, Divide (always a Float), Floor Divide, Modulo, Min, Max, Clamp,
Lerp, Negate, Increment, Decrement, Abs, Floor, Ceil, Round, To Float, Square Root, Sin, Cos,
Random Float, Random Integer. **Math/Bits:** Bitwise And, Or, Xor, Not, Shift Left, Shift Right.

**Logic:** And, Or, Not, Equal, Not Equal, Less, Less Or Equal, Greater, Greater Or Equal,
Or Default (`Value or Default`), Is Set (not `nil`), Select (picks one of its options by an Int,
String, Bool or enum index).

**Text:** Join, To Text, Hex (eight hex digits), Length, Contains, Starts With, Match (a Lua pattern).

**Tables:** Make Array (Size items of a type), Get Item, Set Item, Add Item, Remove Item, Length,
Contains Item, For Each Item.

**Log:** Log (`rex.info`/`warn`/`error`/`debug` with a message and an optional value), Print
String (`rex.screen_text` on screen and/or `rex.info` in the log, with Duration, Font Size, a Key
that replaces the line with the same key, and a colour).

**Input:** Key Down, Mouse Button Down (Left, Right, Middle, Back, Forward), Input Allowed
(`rex.input.accepts()`), Read Mouse Motion, Read Wheel, Hold Game Input.

**Settings:** Get Setting, Set Setting, Add Setting (`rex.cvar.<type>` with Name, Type, Category
and Description; category `Mods` unless you change it), Get Switch, Set Switch.

**Mod:** **Script File** shows one of the mod's own Lua files (`scripts/<File>`) on the canvas.
The file runs on its own like every script of the mod; select the node to read and edit its code
in the details panel (**Save Script**, **Revert**). It can't point at a graph's own `.graph.lua`.

**Lua:** for what nodes don't cover.

| Node | What |
|---|---|
| Lua Chunk | Lua written at the top of the script, before every event: local helper functions and tables the other nodes' Lua uses. |
| Lua Code | Statements run where the chain reaches it. `$A` to `$D` in the code are its four inputs. |
| Lua Expression | A value from an expression, `$A` to `$D` being its inputs (`$A * 2 + $B`). |

### Game nodes (what reNut declares in C++)

Made from everything reNut declares in its `renut` namespace (see
[native-mods.md](native-mods.md#for-renut-developers-the-host-side)): a call node per function, a
node per constant and enum value, **Make**, **Get** and **Set** nodes per type and field, a node
per method, and an **On ...** event node per event. Names lose their leading underscores
(`parts._define` shows as `define`).

They're grouped by the categories reNut gives them: **Parts**, **Vehicles**, **Characters**,
**Worlds**, **Effects**, **Camera**, **Mouse**, **Physics**, **World look**, **Objects**, **Pad**,
**Game State**, **Challenges**, **Crates**, **HUD**, **Zones**, **Materials**, **Shape Keys**,
**Menus**, **Data**, **Sound**, **Online**, **Mods**. Anything else is under **Game/renut/**
(Functions, Constants, Enums, Events). **Game > Address Of** gives any object's address.

These are the raw functions underneath the `renut` table. Where an **API** node does the same
thing, prefer it: it's the documented API.

### API nodes (from `lua_api/renut.lua`)

reNut ships its Lua API's definitions (LuaLS `---@meta` files) in `lua_api` next to `renut.exe`,
and the editor turns every `.lua` file there into nodes, under **API/renut/...** (`API/renut/parts`,
`API/renut/vehicle`, and methods under their class, such as `API/renut/Vec`):

| Definition | Becomes |
|---|---|
| A function | A call node: an input per parameter, an output per return value. |
| A function marked `---@nodiscard` | A pure node instead (unless it takes a callback). |
| A parameter that's a table of fields | One input per field. An optional field you leave unlinked and untyped is left out, so the function's own default applies. |
| A callback parameter (`fun(...)`) | An exec output whose chain is the callback's body, with the callback's parameters as outputs. |
| An event overload (`renut.parts.on("damage", fn)`) | An event node, **On damage**, with the callback's parameters as outputs. |
| A method (`Vec:length`) | A node with a **Self** input. |
| `---@operator` lines | Operator nodes: `Vec + Vec`, `Negate Vec`... |
| A class with fields and methods | A **Break** node giving its fields (`Break Vec`: x, y, z). |
| A parameter typed as a set of strings | An input picked from that list. |

The definitions are read once, the first time the editor builds its node list in a run, so a
changed `renut.lua` needs a restart.

### Your graph's functions and macros

The add menu's **Functions** and **Macros** list the graph's own (see below).

## Variables, functions, macros and event dispatchers

**Variables** (+ in the sidebar) have a name, a type (Bool, Int, Float, String or Any), a value
they start as (Any starts as `nil`) and a comment. Each becomes a `local` at the top of the script,
so it keeps its value between events until the scripts reload. Names must be Lua names that hide
nothing the script needs (not `rex`, `renut` or a Lua keyword), and variables, functions, macros
and dispatchers share one set of names.

**Functions** are graphs of their own, opened from the sidebar or a call node. A function starts
at its **Inputs** node and ends at its **Outputs** node; inputs and outputs (Bool, Int, Float,
String, Any) are set in the details panel. A **pure** function has no exec pins and is worked out
wherever its outputs are read. Functions compile to Lua functions declared before the events, so
they can call each other.

**Macros** are written out in full wherever they're used, so their inputs and outputs can be exec
pins too: a macro can be flow of its own, like Branch. A new macro starts with an exec **In** and
**Out**. A macro can't use itself.

Event nodes only run in the event graph, not inside functions or macros.

**Event dispatchers** are lists of events, like Unreal's. **Bind Event to** adds the event linked
to its **Event** pin (an event's delegate pin, or a Custom Event's), **Call** runs every bound
event in the order they were bound, with the dispatcher's params, and **Unbind** removes them.
Binding the same event twice keeps it once. A dispatcher compiles to a `local` table.

**Timers:** link an event's delegate pin to **Set Timer by Event**'s **Event** input. The event's
chain then runs after **Time** seconds (or every **Time** seconds when **Looping**) as well as when
the event itself happens. Keep the **Handle** to stop it with **Clear Timer**. Timers are
RexFabric's (`rex.after` / `rex.every`) and run on frames.

## Saving: graphs and the Lua they compile to

Each graph is two files in the mod's folder:

```
mods/<Mod>/graphs/<Name>.json         the graph: what the editor opens
mods/<Mod>/scripts/<Name>.graph.lua   its script: what runs
```

- **Save** writes both. **Save and Reload** also reruns every enabled mod's Lua, so the change
  takes effect at once. A mod that's turned off saves fine, but its scripts don't run; the status
  line says so.
- The script runs like any other `.lua` directly in `scripts`: at load, in file-name order with
  the mod's other scripts. You can add hand-written scripts beside it, or show them on the canvas
  with a **Script File** node.
- **Don't edit `.graph.lua` by hand**: saving the graph again replaces it. Change the graph.
- Renaming a graph renames both files; deleting it deletes both.
- To share a mod, the `scripts` folder is enough. Keep `graphs/` if you (or others) want to keep
  editing it in the editor.
- A graph name can have letters, digits, spaces, `_` and `-`.

**What the compiler writes.** Each event becomes the call that registers it (`rex.on`, `rex.hook`,
`rex.hook_after`, `rex.midasm`, `rex.cvar.on_change`, a `renut` event) and its chain becomes the
body of the function it registers, in link order; branches and loops nest as `if` and `for`
blocks. Variables, dispatchers and the state some nodes keep (Do Once, Flip Flop, Delay) are
locals at the top. Events go out top to bottom as laid out on the canvas (then left to right), so
the On Start highest up runs first. A typical script:

```lua
-- Made with the RexFabric node editor from graphs/Main.json.
-- Saving the graph again replaces this file, so change the graph rather than this file.

local Count = 0

-- Before 0x8266FBB0
rex.hook(0x8266FBB0, function(n1_Registers)
  Count = (Count + 1)
  if (Count > 100) then
    rex.info("swaps", Count)
  end
end)
```

Local names start with `n` and the node's id (`m` for nodes inside a macro), so they change as you
add nodes; nothing outside the script should depend on them.

**The graph file** is JSON (version 1): the variables, nodes (id, type, position, settings, typed
values, and `owner` for nodes in a function or macro), links (from a node's output pin to another
node's input pin, by pin name), routines (functions and macros) and dispatchers. A node type the
editor doesn't know any more (the game stopped declaring it) loads as it is and shows as a problem.

## Problems and warnings

The editor compiles the graph after every edit and lists what's wrong in the details panel.
**Errors** stop the graph from running: saving a graph with errors writes a script that only logs
`graphs/<Name>.json has problems, so it doesn't run; open it in the node editor to see them`, so a
half-made mod never runs. **Warnings** don't.

Common ones:

| Message | Means |
|---|---|
| `Never runs: no event's chain leads here` (warning) | An action isn't linked to any event's chain. |
| `0x... isn't the start of a recompiled function, so the hook never runs` (warning) | A Before/After Function address isn't a function's start. Fix it: when the script runs, `rex.hook` raises an error there and the rest of the script doesn't run. |
| `Needs an address`, `... isn't an address; write it in hex` | The Address setting is empty or not a number. It's read as hex, with or without `0x`. |
| `Skip Original only works in a Before Function event's chain` | It's in another event's chain. |
| `The game can't be called from On Start; ...` | Call Function in On Start. Use a hook or On Frame. |
| `Another Key Pressed node waits for F6; ...` (warning) | Two nodes share one key, so only one sees each press. |
| `Nothing runs this event: ...` (warning) | A Custom Event whose delegate pin isn't linked. |
| `Events only run in the event graph` | An event node inside a function or macro. |
| `There's no <type> node; the game may not declare it any more` | A node type the editor doesn't know. |
| `Needs a name`, `Pick a variable`, `There's no variable named ...` | A setting left empty or pointing at something deleted. |

The compiler can't tell whether reNut replaces a function itself: a Before/After Function on one
of those compiles fine and never runs. See
[rexfabric-api.md](rexfabric-api.md#renuts-own-hooks-win).

## Tutorial: a press counter

A small mod: every press of **F6** counts up and shows the count in the screen's top right corner.

1. **Open the editor.** Start reNut and press **F10** (or **F9**, then **Node Editor**).
2. **Make the mod.** In the toolbar's **Mod** list pick **New Mod...**, type `PressCounter`, press
   **OK**. The editor makes `mods/PressCounter`, enables it, and opens a new graph, **Main**.
3. **Add a variable.** Press **+** next to **Variables**. In the details panel, name it `Presses`
   (press **Enter**), set **Type** to **Int**, and leave it starting as `0`.
4. **Add the event.** Right click the canvas and pick **Events > Key Pressed**. In the details
   panel, set **Key** to `F6` (the default, F9, opens reNut's mods menu).
5. **Count.** Drag from the Key Pressed node's exec output (its arrow) into empty space and let go.
   In the add menu, pick **Variables > Increment Presses**. The two nodes are linked.
6. **Make the text.** Right click the canvas and add **Text > Join**. Click its **A** input and
   type `Presses: `. Link Increment's **Value** output to Join's **B** input.
7. **Show it.** Drag from Increment's exec output into empty space and pick **Log > Print String**.
   Link Join's **Result** to Print String's **In String**. On the node, type `presses` into **Key**,
   so each press replaces the line instead of adding one.
8. **Check.** The toolbar should say **No problems**. Open the **Lua** tab to see the script,
   something like this:

   ```lua
   local Presses = 0

   -- Key F6 Pressed
   rex.on("frame", function()
     local n3_Value, n5_Text
     if rex.input.pressed("F6") and rex.input.accepts() then
       Presses = Presses + 1
       n3_Value = Presses
       n5_Text = tostring((tostring("Presses: ") .. tostring(n3_Value)))
       rex.screen_text(n5_Text, 2.0, 0x00A8FF, 0.0, "presses")
       rex.info(n5_Text)
     end
   end)
   ```

9. **Run it.** Press **Save and Reload**, close the editor (F10) and press **F6** in game:
   `Presses: 1`, `Presses: 2`, ... appear in the top right corner and in the log (F2).

**Going further:**

- Let the player turn the on-screen text off: add an **On Start** event leading to **Settings >
  Add Setting** (Name `presscounter_show`, Type `boolean`, Default on, Category
  `Nuts&Bolts/Press Counter`); it appears in a tab of reNut's settings (F4). Then link a
  **Get Setting** (Name `presscounter_show`) to Print String's **Print to Screen** input.
- React to the game instead of a key: an **After Function** event on `0x82207320` (the level's
  update) runs once a frame while a level is loaded; **API > renut > ...** event nodes such as the
  parts' **On damage** run on game events.
- Reset the count after five seconds of no presses: link the Key Pressed chain on to a **Delay**
  and **Set Presses** to `0` from its **Completed** output.

For hooks and memory by hand, see [tutorials/hooks-and-memory.md](tutorials/hooks-and-memory.md).
