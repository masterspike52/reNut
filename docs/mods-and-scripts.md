# How mods and scripts work

This is the reference for how reNut finds, loads and runs mods. If you've never made a mod, start
with [getting-started.md](getting-started.md); come back here when you want the details.

The functions themselves are in [renut-api.md](renut-api.md) (the `renut` table: the game) and
[rexfabric-api.md](rexfabric-api.md) (the `rex` table: RexFabric, the modding framework).

## Contents

- [The mods folder](#the-mods-folder)
- [A mod's folder](#a-mods-folder)
- [mod.toml](#modtoml)
- [Load order: mods.json](#load-order-modsjson)
- [The mods menu](#the-mods-menu)
- [Keys](#keys)
- [What happens at startup](#what-happens-at-startup)
- [How scripts run](#how-scripts-run)
- [Events](#events)
- [Timers and waiting](#timers-and-waiting)
- [Calls that wait for the game](#calls-that-wait-for-the-game)
- [require and modules](#require-and-modules)
- [One Lua state for every mod](#one-lua-state-for-every-mod)
- [Errors and the log](#errors-and-the-log)
- [Reload Scripts](#reload-scripts)
- [Threads](#threads)
- [Keeping data: renut.data](#keeping-data-renutdata)
- [Mod settings: renut.settings and cvars](#mod-settings-renutsettings-and-cvars)
- [Sharing values between mods: renut.mods](#sharing-values-between-mods-renutmods)
- [.nbpatch files](#nbpatch-files)
- [Game files, models and other content](#game-files-models-and-other-content)
- [Native plugins](#native-plugins)
- [Autocomplete in VS Code](#autocomplete-in-vs-code)

## The mods folder

Mods live in the `mods` folder in reNut's folder, next to `renut.exe`. reNut makes it if it's
missing. On Linux with the AppImage, reNut's writable folder is `$XDG_DATA_HOME/renut` (by default
`~/.local/share/renut`), so mods go in `~/.local/share/renut/mods`.

Two settings in **F4 > Mods** control it (saved in `renut.toml` under `[renut]`, applied at the next
start):

| Setting | Default | What it does |
|---|---|---|
| `renut_mods` (**Mods**) | `true` | Turns on what reNut applies from the mods folder at startup: `.nbpatch` files, model bundles, character packs and mod vehicles. Lua and native mods load either way. |
| `renut_mods_folder` (**Mods folder**) | `mods` | Where the mods folder is: a full path, or one relative to reNut's folder. |

What's in the mods folder:

```
mods/
  mods.json            the load order (see below)
  some_mod/            one folder per mod
  another_mod/
  something.nbpatch    loose NB Studio patches (see .nbpatch files)
  .cache/              reNut's built .nbpatch files
  .merged/             links reNut rebuilds each start
  .characters/         character packs from NutTool
  .renut_vehicles      which mod vehicles were already put in the save
```

Every folder is a mod, except folders whose name starts with `.` (tool state) or ends with
`.installing` (a half-finished install). Leave the dot files and folders alone.

## A mod's folder

```
mods/my_mod/
  mod.toml               name, description, version, author
  icon.png               the picture in the mods menu
  scripts/
    main.lua             every .lua directly in scripts/ runs at startup
    my_mod/
      util.lua           subfolders are for require: require("my_mod.util")
  graphs/
    Main.json            a node editor graph...
  (scripts/Main.graph.lua)  ...and the script it compiles to, which is what runs
  bundles/Bundle/4f/<id> model bundles, loaded with the game's common bundle
  bundles/Bundle/50/<id>
  worlds/Bundle/4f/<id>  bundles for the mod's worlds, loaded by their scenes
  worlds/Bundle/50/<id>
  vehicles/*.blueprint   vehicles put in the player's garage once
  sounds, music          any files your scripts play (.ogg, .mp3, .flac, .wav)
  *.nbpatch              NB Studio patches
  *.dll, *.asi           native plugins (also in plugins/)
```

Every file type reNut or RexFabric reads in a mod's folder:

| File | Read by | When | More |
|---|---|---|---|
| `mod.toml` | RexFabric | whenever the mod list is read | [mod.toml](#modtoml) |
| `manifest.json` | RexFabric | as `mod.toml`, used for anything `mod.toml` leaves blank | [mod.toml](#modtoml) |
| `icon.png` | the mods menu | when the menu shows the mod | [mod.toml](#modtoml) |
| `scripts/*.lua` | RexFabric | at startup and on Reload Scripts | [How scripts run](#how-scripts-run) |
| `scripts/**/*.lua` (in subfolders) | `require` | when a script requires them | [require and modules](#require-and-modules) |
| `graphs/*.json` | the node editor | when you open the graph | [node-editor.md](node-editor.md) |
| `bundles/Bundle/4f\|50/<id>` | reNut | at startup | [Game files](#game-files-models-and-other-content) |
| `worlds/Bundle/4f\|50/<id>` | reNut | at startup | [tutorials/worlds-and-doors.md](tutorials/worlds-and-doors.md) |
| `vehicles/*.blueprint` | reNut | at startup, and by `renut.vehicle.spawn` | [tutorials/parts-and-vehicles.md](tutorials/parts-and-vehicles.md) |
| sound files | `renut.sound`, `renut.npcs` | when a script plays them | [renut-api.md](renut-api.md) |
| `*.nbpatch` | reNut | at startup | [.nbpatch files](#nbpatch-files) |
| `*.dll`, `*.asi` (here or in `plugins/`) | RexFabric | at startup | [native-mods.md](native-mods.md) |

Files your own scripts read (JSON, data tables and so on) can go anywhere in the folder; see
[require and modules](#require-and-modules) for how a script finds its own folder.

**The folder name is the mod's id.** It's used in the log, in `mods.json`, by `renut.mods`, as the
name of the mod's saved data and in its settings' names. Use letters, digits and `_` (`renut.data`
only accepts letters, digits, spaces, `_`, `-` and `.`), and keep it the same from version to
version.

## mod.toml

```toml
name = "Hyrule"
description = "Hyrule Field through a stone arch in Showdown Town."
version = "3"
author = "reNut"
```

| Key | Shown | If missing |
|---|---|---|
| `name` | the mods menu's lists and the mod's page | the folder's name |
| `description` | the mod's page | nothing |
| `version` | next to the name (`v3`) and in crash reports | nothing |
| `author` | next to the version (`by reNut`) | nothing |

All four are text, and every one is optional; the file itself is optional. No other keys are read.
A blank value counts as missing.

If the folder also has a Thunderstore-style `manifest.json`, its `name`, `description` and
`version_number` are used for whatever `mod.toml` doesn't set.

A broken `mod.toml` (a TOML syntax error) is reported once in the log as
`<folder>: unreadable mod.toml: ...`, and the mod shows under its folder name.

`icon.png` in the mod's folder is its picture in the mods menu. Make it square (256 x 256 works
well).

## Load order: mods.json

`mods/mods.json` lists the mods that are on, in the order they load, and the ones that are off:

```json
{
  "disabled": ["old_test_mod"],
  "load_order": [
    "renut_coop",
    "renut_hyrule"
  ]
}
```

- Names are folder names; upper and lower case don't matter.
- A folder that isn't in either list is new: it's turned **on** and joins the end of the load order
  (new folders in alphabetical order). reNut writes the file at every start, so from then on it's
  listed.
- Names in the file with no folder are dropped.
- The mods menu writes this file when you change anything there. You can also edit it by hand while
  reNut is closed.

The load order decides:

- the order mods' scripts run (and so the order their event listeners are called in),
- which mod wins when two serve the same file (`.nbpatch` files, bundles): the first one,
- whether a value another mod shares with `renut.mods.export` is there yet when your script starts.

## The mods menu

Press **F9** (or open the Guide with **G** or the controller's Guide button, and choose **Mods**).

At the top are the actions:

- **Reload Scripts**: stops every Lua mod and runs the scripts again. See
  [Reload Scripts](#reload-scripts).
- **Open Folder**: opens the mods folder.
- **Node Editor**: opens the node editor ([node-editor.md](node-editor.md)).

Below them are two lists side by side: **Available** (off) and **Enabled** (on, in load order, first
at the top).

| To | Controller (and keys that play as one) | Mouse |
|---|---|---|
| Move between the lists and mods | D-pad or stick | point |
| Turn a mod on or off | **X** (Enable / Disable) | the arrow on the mod pointing to the other list |
| Change the load order | **Y** to pick up, up/down to move, **Y** to put down | the up and down arrows on an enabled mod |
| Open a mod's page | **A** | click it |

A mod's page shows its icon, version, author and description, an **Enabled** switch and **Open
Folder**. Turning a mod on puts it last in the load order.

Mods load at startup, so after any change the menu shows **Restart to Apply Changes**. Choosing it
offers **Restart Now** (reNut closes and starts again with the same options) and **Not Now**. If the
change only affects Lua scripts, **Reload Scripts** applies it without a restart: it runs the scripts
of the mods that are enabled in the menu right now.

There's no in-game mod browser in reNut; mods are shared as zips (see
[getting-started.md](getting-started.md#14-share-your-mod)).

## Keys

| Key | Opens | Change it |
|---|---|---|
| **F1** | FPS overlay | fixed |
| **F2** | reNut's log window (reNut's own messages, not mods') | fixed |
| **F4** | reNut Settings, with a **Mod: &lt;folder&gt;** tab for each mod with settings | fixed |
| **F5** | Game Flags (player 1's flags and counters) | fixed |
| **F8** | Every setting (the SDK's settings window) | `ui.bind.settings` |
| **F9** | Mods menu | `ui.bind.mods` |
| **F10** | Node editor | `ui.bind.node_editor` |
| **G** | The Guide (it has the Mods menu too) | `renut_key_guide` (F4 > Input) |

To change F8, F9 or F10, set the key name in `renut.toml` (next to `renut.exe`) while reNut is
closed, or in the F8 window:

```toml
[ui.bind]
settings = 'F8'
mods = 'F9'
node_editor = 'F10'
```

The keyboard also plays the game (by default W A S D, Space, E, Q, R, F, C, V, Shift, Control, Tab,
Escape, the arrow keys and the mouse buttons; see F4 > Input). Pick hotkeys for your mod that don't
clash with these.

## What happens at startup

When the game launches (after **PLAY** in the launcher), in this order:

1. reNut applies its part of the mods folder: loose `.nbpatch` files, then each enabled mod's
   `.nbpatch` files in load order, then the mods' `bundles/` and `worlds/` bundles, character packs,
   and mod vehicles.
2. RexFabric reads the mods folder, writes `mods.json`, and logs `N of M mods enabled in <folder>`.
3. Native plugins of the enabled mods load.
4. The Lua scripts run: mod by mod in load order, and in each mod every `.lua` file directly in
   `scripts/`, in file-name order. After each mod the log says `[<folder>] N of M scripts ran`, and at
   the end `Lua 5.4: N script mods`.
5. Vehicle parts added with `renut.parts.add` are fixed for this run.
6. The game itself starts.

So your scripts run **before the game does**: at the top of a script there's no world, no player and
no vehicle yet. The top level is for setting things up (adding parts, worlds, menu entries, settings,
zones) and registering event listeners; the work happens in the events.

## How scripts run

A script is a normal Lua 5.4 file. Its top level runs once, then it's done; what keeps your mod alive
afterwards are the functions you handed to events and timers.

```lua
-- Top level: runs once, at startup and on each Reload Scripts.
local count = 0

renut.game.on("world_enter", function(number, name)
  -- Runs every time a world loads, for as long as the game runs.
  count = count + 1
  print("worlds entered: " .. count .. " (now " .. name .. ")")
end)
```

What's available:

- The standard Lua 5.4 libraries: `string`, `table`, `math`, `utf8`, `coroutine`, `io`, `os`,
  `debug`, and `require` (Lua files only: no native C modules).
- `print(...)`, which writes to the log with your mod's name in front.
- `renut` (the game) and `rex` (RexFabric).

Run order:

- Mods in load order; inside a mod, the files directly in `scripts/` by file name (`a_setup.lua`
  before `main.lua`). If the order inside your mod matters, use one main file that `require`s the
  rest.
- A file that fails partway has already run everything above the error. The mod's other files still
  run.

## Events

Most of a mod is functions called by events. There are two families.

**RexFabric's frame event** runs once a frame, everywhere: on the title screen, in menus, in worlds,
while paused. It gets the seconds since the last frame.

```lua
rex.on("frame", function(dt)
  if rex.input.pressed("F6") and rex.input.accepts() then
    rex.screen_text("F6!", 2)
  end
end)
```

**reNut's events** are part of the game's own update, so game functions work in them at once:

```lua
renut.game.on("tick", function() end)                  -- each game update, in a world, not paused
renut.game.on("world_enter", function(number, name, scene) end)
renut.game.on("world_leave", function(number, name, scene) end)
renut.vehicle.on("tick", function(vehicle, dt) end)
renut.zones.on("enter", function(zone, object, what) end)
```

There are many more (parts, vehicles, characters, objects, challenges, the HUD, saving): each
namespace's `.on` is listed in [renut-api.md](renut-api.md).

Every `.on` (and `rex.on`) returns a handle:

```lua
local handle = renut.game.on("tick", function() end)
handle:off()            -- stop it
print(handle:active())  -- false now
renut.off(handle)       -- the same as handle:off(); nil is fine

-- Only the first time:
renut.once(renut.game, "world_enter", function(number, name) print("first world: " .. name) end)
```

Listeners are called in the order they were added, so earlier mods in the load order hear an event
first.

## Timers and waiting

There are two kinds of timer:

| | `renut.after` / `renut.every` | `rex.after` / `rex.every` |
|---|---|---|
| Counts | game time: game updates in a world, not while paused | real time, frame by frame, everywhere |
| Returns | a handle: `h:off()` | a number: `rex.cancel(id)` |
| Clock | `renut.time()`: game seconds since the scripts loaded | `rex.now()`: real seconds (only differences mean anything) |

```lua
local h = renut.every(5, function() print("5 game seconds passed") end)
renut.after(30, function() h:off() end)

local id = rex.every(1, function() rex.screen_text("tick", 0.5) end)
rex.after(10, function() rex.cancel(id) end)
```

A timer runs at most once per check (each frame for `rex` timers, each game update for `renut`
timers). A repeating timer that fell behind carries on from now instead of running several times to
catch up.

**Waiting inside a callback.** Every event callback and every timer function runs as a coroutine, so
it can pause:

```lua
renut.game.on("world_enter", function(number, name)
  renut.wait(2)        -- 2 seconds of game time
  renut.hud.message("Welcome to " .. name, 3)
  renut.wait(5)
  renut.hud.message("Still here?", 3)
end)
```

`renut.wait(seconds)` counts game time and only works inside a callback (an event's or a timer's).
At the top level of a script it raises an error. While one call is paused, the event can fire again
and start a fresh call.

You can use Lua's `coroutine` library for your own coroutines as usual; `renut.wait` doesn't work
inside them (wrap the work in a `renut.after` instead).

## Calls that wait for the game

Many `renut` functions need the game to be in the middle of its update: anything that reads or
changes vehicles, characters, the HUD, the world. The definitions in `lua_api/renut.lua` mark them
*Event.* Where you call one decides what happens:

| Called from | What happens |
|---|---|
| A reNut event (`renut.game.on`, `renut.vehicle.on`, `renut.parts.on`, `renut.zones.on`, `renut.after`...) | It runs at once. |
| `rex.on("frame")`, `rex.after`, `rex.every`, or another callback outside the game's update | Your callback pauses at that call until the game's next update, then carries on with the real result. |
| The top level of a script, or your own coroutine | The call is queued for the game's next update and returns `nil` straight away. |

The game's update only runs in a world, so on the title screen a paused call waits until you're in
one. For anything that should happen often, put it straight in `renut.game.on("tick", ...)` instead
of relying on the wait.

## require and modules

`require` finds Lua files in the calling mod's `scripts/` folder:

```lua
-- scripts/main.lua
local util = require("my_mod.util")   -- scripts/my_mod/util.lua or scripts/my_mod/util/init.lua
util.hello()
```

```lua
-- scripts/my_mod/util.lua
local M = {}
function M.hello() print("hello from util") end
return M
```

Rules:

- **Put modules in a subfolder.** Every `.lua` directly in `scripts/` runs on its own at startup, so
  a module there would run twice (once as a script, once when required).
- **Name the subfolder after your mod.** Every mod shares one Lua state, so module names are shared
  too: if two mods `require("util")`, the second gets the first one's module. `require("my_mod.util")`
  can't clash.
- **Call `require` at the top level** of your scripts (or in modules they load). Where `require`
  looks is set to each script's folder as that script starts, so a `require` inside an event
  callback, later on, looks in whichever mod's folder ran last.
- Native C modules can't be loaded.

A script can find its own mod folder from its file name. File names in Lua use `/`:

```lua
-- From a file directly in scripts/.
local MOD_DIR = debug.getinfo(1, "S").source:match("^@(.*)/scripts/[^/]+$")
local file = io.open(MOD_DIR .. "/data/levels.txt", "r")
```

`renut.mods.this()` gives your mod's folder name.

## One Lua state for every mod

All Lua mods run in one Lua state. That means:

- **Globals are shared.** A global variable or function one mod makes is visible to (and can be
  overwritten by) every other mod. Make everything `local`.
- **Don't replace functions in `renut` or `rex`.** Assigning `renut.hud.message = ...` changes it for
  every mod.
- **`package.loaded` is shared**, which is why module names need your mod's name (see above).
- To share on purpose, use [`renut.mods`](#sharing-values-between-mods-renutmods), not globals.

Functions that keep things per mod (`renut.data`, `renut.settings`, `renut.mods.export`,
`renut.hud.text` keys) find out which mod is calling from the file the call is in: a file under
`mods/<folder>/scripts/`.

## Errors and the log

reNut writes a log file per run: `logs/renut_<number>.log` in reNut's folder (the highest number is
the latest run). Lua mods write to it under the `rexfabric` category:

```
2026-11-12 02:55:40.034 I rexfabric  t22844 2 of 2 mods enabled in F:\renut\mods
2026-11-12 02:55:40.040 I rexfabric  t22844 [renut_coop] 1 of 1 scripts ran
2026-11-12 02:55:40.041 I rexfabric  t22844 [renut_hyrule] 1 of 1 scripts ran
2026-11-12 02:55:40.041 I rexfabric  t22844 Lua 5.4: 2 script mods
```

The letter after the time is the level: `I` info, `W` warning, `E` error, `D` debug.

Writing to the log:

```lua
print("a", 1, true)        -- info, values joined by spaces
rex.info("same as print")
rex.warn("something odd")  -- warning
rex.error("something broke")
rex.debug("details")       -- debug
```

Each line starts with your mod's folder in brackets, `[my_mod]`.

**The F2 window shows reNut's own messages only**, not mods'. Use the log file, or put text on the
screen while you play: `rex.screen_text(text, seconds)` shows a line in the top right corner (see
[rexfabric-api.md](rexfabric-api.md) for its colour, size and key options).

**Errors never stop the game.** They go to the log as errors:

- A syntax error or a runtime error while a file loads:
  `[my_mod] main.lua: <path>/main.lua:12: <message>` plus a stack trace, and the mod's
  `N of M scripts ran` count is lower. Everything the file did before the error stays.
- An error inside an event callback: the event's name (`frame`, `renut.game_tick`, ...), then the
  message with the file and line and a stack trace. It's logged each time it happens, and the listener
  stays registered.
- An error in a `rex.after` / `rex.every` function: `a timer failed: ...`.
- A function given the wrong arguments says what it expected, for example
  `renut.zones.add{name = ..., at = ..., radius or size = ...}`, or Lua's own
  `bad argument #1 to ...`.

To catch an error yourself, use `pcall`:

```lua
local ok, problem = pcall(renut.npcs.spawn, "showdowntown", "my_npc")
if not ok then rex.warn("couldn't spawn: " .. tostring(problem)) end
```

## Reload Scripts

**Reload Scripts** in the mods menu (F9) stops every Lua mod and starts them again, using the mods
enabled in the menu at that moment.

What a reload does:

1. Removes every Lua mod's hooks, midasm handlers and event listeners, and sets `rex.switch` values
   the scripts changed back to 0.
2. Closes the Lua state: every variable, table, timer, paused callback, `renut.mods` export and
   required module is gone.
3. Makes a new Lua state and runs every enabled mod's scripts again, in load order, from the top.
   `renut.time()` starts again from 0.

What a reload keeps:

- Settings (`renut.settings`, `rex.cvar`) and their current values.
- `renut.data` (it's on disk).
- Things already made in the game: trigger zones, spawned characters and vehicles, worlds and doors,
  menu entries, sounds playing, text drawn on the screen with no time limit. Most are keyed by a name,
  so adding them again under the same name replaces them.

What needs a restart (the menu's **Restart to Apply Changes**):

- New or changed vehicle parts (`renut.parts.add`): parts are fixed when the game starts.
- `.nbpatch` files, bundles, character packs, mod vehicles.
- Native plugins.
- A mod turned off: reload stops running its scripts, but anything it already changed in the game
  stays until a restart.

## Threads

- One Lua state, one lock: only one piece of Lua runs at a time, whichever thread asks.
- `rex.on("frame")` runs on the game's thread, after it draws each frame.
- reNut's events and `renut.after` / `renut.every` run on the game's thread, in its update.
- Hooks (`rex.hook`, see [tutorials/hooks-and-memory.md](tutorials/hooks-and-memory.md)) run on
  whichever game thread called the hooked function.
- Lua can't start threads of its own.

Because your Lua runs inside the game's frame, **a slow callback slows the game down** and an endless
loop freezes it. Don't loop waiting for something; use an event, a timer or `renut.wait`. Don't call
`os.execute` or read large files every frame.

## Keeping data: renut.data

`renut.data` keeps values between runs, per mod. Values are numbers, text, `true`/`false`, and tables
of those (nested up to 32 deep). Setting a key to `nil` removes it.

```lua
local runs = renut.data.get("runs", 0) + 1   -- the second argument is the default
renut.data.set("runs", runs)
print("This mod has run " .. runs .. " times")

renut.data.set("best", {time = 61.4, vehicle = "Truck"})
for _, key in ipairs(renut.data.keys()) do print(key) end
```

For data that belongs to one save (one player profile), use the `save_` versions. They need a signed
in profile:

```lua
renut.game.on("world_enter", function()
  local visits = renut.data.save_get("visits", 0) + 1
  renut.data.save_set("visits", visits)
end)
-- renut.data.save_keys() lists them.
```

Where and how:

- Each mod has a file: `mod_data/<folder>.json` in reNut's folder. Delete it to start over.
- Every `set` writes the file at once, so don't call it every frame; keep the value in a variable
  and save it now and then (on `world_leave`, on `renut.game.on("save", ...)`, on a timer).
- Keys are 1 to 256 characters; an encoded value is at most 1 MiB.
- Tables come back as plain tables: metatables (like a `renut.vec`'s) aren't kept.
- Call `renut.data` from a file directly in your mod's `scripts/` folder. Called from a module in a
  subfolder, it can't tell which mod is calling and raises an error; have the module take the values
  as arguments, or give it a function from your main script.

## Mod settings: renut.settings and cvars

`renut.settings` adds settings the player can change in **F4**, under a tab called
**Mod: &lt;folder&gt;**:

```lua
local speed = renut.settings.add{key = "speed", default = 5, description = "How fast the thing goes."}
renut.settings.add{key = "show_timer", default = true, description = "Show the timer on screen."}

print(renut.settings.get("speed"))
renut.settings.set("show_timer", false)
renut.settings.on_change("speed", function(value) print("speed is now " .. value) end)
```

- `add{key, default, description}` makes the setting if it's new and returns its current value. The
  default's type sets the setting's type: `true`/`false`, a whole number (`5`), a decimal (`5.0`), or
  text.
- The full name is `<folder>_<key>`, lower case, with anything but letters, digits and `_` turned
  into `_` (so `my_mod` + `speed` is `my_mod_speed`). That's what shows in F4, and in `renut.toml`
  under `[mods]`.
- The player saves their changes with **Save to config** in F4. `renut.settings.set` changes the
  value for this run; it's only written to `renut.toml` if the config is saved afterwards. For
  values your mod keeps on its own, use `renut.data`.
- `on_change` functions run on the next frame after a change.
- Settings stay through Reload Scripts with their values; adding one again just returns its value.

Underneath, these are RexFabric cvars. `rex.cvar` reaches every setting, reNut's and the SDK's too:

```lua
print(rex.cvar.get("renut_time_of_day"))          -- by name
rex.cvar.set("renut_time_of_day", "Night")
local mine = rex.cvar.boolean("my_mod_debug", false, "Scripts", "Draw debug shapes.")
```

A cvar made with `rex.cvar.boolean` / `int32` / `number` / `string` and the rest goes under
`[mods]`, in the category you give it. Only categories starting with `Nuts&Bolts/` show in F4; the
others show in F8, which lists every setting. `renut.settings` picks a category for you, so prefer it.
The full `rex.cvar` list is in [rexfabric-api.md](rexfabric-api.md).

## Sharing values between mods: renut.mods

```lua
-- In mods/coins_api/scripts/main.lua: share a table of functions.
local coins = 0
renut.mods.export({
  add = function(n) coins = coins + n end,
  count = function() return coins end,
})
```

```lua
-- In another mod: use it.
renut.mods.when("coins_api", function(api)
  api.add(5)
  print("coins: " .. api.count())
end)
```

| Function | What it does |
|---|---|
| `renut.mods.list()` | The enabled mods' folder names, in load order. |
| `renut.mods.enabled(name)` | Whether a mod is enabled. |
| `renut.mods.this()` | The calling mod's folder name. |
| `renut.mods.export(value)` | Shares a value (usually a table) under your mod's folder name. Exporting again replaces it. |
| `renut.mods.import(name)` | That mod's value, or `nil` if it hasn't exported (yet). |
| `renut.mods.when(name, fn)` | Calls `fn(value)` once that mod exports, straight away if it already has. |

`import` at the top level only sees mods earlier in the load order. `when` works in either order, so
prefer it. If the other mod might not be installed, check `renut.mods.enabled(name)` first. Exports
are cleared by Reload Scripts and made again as the scripts run.

## .nbpatch files

`.nbpatch` files are patches made with NB Studio: a zip with a `patch.json` and the game files it
changes or adds (as differences from the originals, or whole new files). reNut applies them at
startup, without touching your game files: each patched file is built once into `mods/.cache/` and
served to the game in place of the original. Cache files no mod uses any more are deleted.

Where they go and their order:

1. Loose in the `mods` folder: they load first, in file-name order.
2. In a mod's folder: after the loose ones, mod by mod in load order, each mod's in file-name order.
   These follow the mods menu: a disabled mod's patches don't load.

A patch applies whole or not at all. It's skipped when:

- its format is newer than reNut understands,
- a patch with the same id already loaded,
- a patch it requires hasn't loaded before it,
- it conflicts with one that loaded (either side may list the other),
- it changes a file an earlier patch already changes (the first one wins),
- a game file it changes isn't the version it was made for, or a file it adds already exists.

reNut doesn't log why a patch was skipped, so if one seems to do nothing, check those. Paths starting
with `update/` patch the title update's files. Changes to the game's executable inside a patch are
ignored (reNut runs a recompiled game), and so are world edits; vehicle parts in a patch are added.

The **Mods** setting in F4 (`renut_mods`) turns patches off. Changes take a restart.

## Game files, models and other content

reNut doesn't replace game files dropped loose into a mod's folder. Mod content reaches the game
these ways:

- **`.nbpatch` files** change or add game files (above).
- **`bundles/Bundle/4f/<id>` and `bundles/Bundle/50/<id>`** (the id is 6 hex digits): new bundles,
  served as game files and loaded with the game's common bundle, so your parts' and characters'
  models are there in every world. An id the game already uses, or another mod already served, is
  skipped. NutTool makes these ([nuttool/README.md](nuttool/README.md),
  [tutorials/parts-and-vehicles.md](tutorials/parts-and-vehicles.md)).
- **`worlds/Bundle/...`**: the same, but only loaded when one of your worlds' scenes asks for them
  ([tutorials/worlds-and-doors.md](tutorials/worlds-and-doors.md)).
- **`vehicles/*.blueprint`**: each is copied into the player's save once, like a downloaded vehicle.
  reNut remembers which in `mods/.renut_vehicles`, so one the player deletes stays deleted. Scripts
  can also spawn them: `renut.vehicle.spawn("<folder>: <name>")`.
- **Sound and music files** (`.ogg`, `.mp3`, `.flac`, `.wav`) anywhere in your folder, played with
  `renut.sound.play("sounds/boing.wav")`, a path inside your mod. As with `renut.data`, call it from a
  file directly in `scripts/` (or give a full path).
- **Character packs** in `mods/.characters/`, made by NutTool
  ([tutorials/characters-and-challenges.md](tutorials/characters-and-challenges.md)).

Bundles, vehicles and character packs follow the **Mods** setting and take a restart.

## Native plugins

A mod can also ship C++ plugins: `.dll` and `.asi` files (on Linux `.so`, on macOS `.dylib`) in its
folder or in its `plugins/` folder. They load at startup, mod by mod in load order, before the Lua
scripts; changes take a restart. See [native-mods.md](native-mods.md).

## Autocomplete in VS Code

reNut ships LuaCATS definitions of the `renut` table in `lua_api/renut.lua`, next to `renut.exe`.
They give you autocomplete, parameter hints and the description of every function. The node editor
reads the same files.

1. Install [VS Code](https://code.visualstudio.com/) and the **Lua** extension by sumneko
   (`sumneko.lua`).
2. Open your mod's folder in VS Code (**File > Open Folder**).
3. Make a `.luarc.json` in your mod's folder:

```json
{
  "runtime.version": "Lua 5.4",
  "workspace.library": ["../../lua_api"],
  "diagnostics.globals": ["rex"]
}
```

- `workspace.library` points at the `lua_api` folder. `../../lua_api` works when your mod is in
  `mods/` next to `renut.exe`; otherwise write the full path, for example
  `"C:/Games/renut/lua_api"`.
- `diagnostics.globals` stops the extension warning about `rex`, which isn't in `lua_api`.
- On Linux with the AppImage, `lua_api` is inside the AppImage; copy `renut.lua` from reNut's source
  (`src/renut_engine/lua_api/renut.lua`) to a folder of your own and point at that.

Functions marked *Event.* in the hints are the ones that need the game's update (see
[Calls that wait for the game](#calls-that-wait-for-the-game)).

## See also

- [getting-started.md](getting-started.md): your first mod, step by step.
- [renut-api.md](renut-api.md), [rexfabric-api.md](rexfabric-api.md): every function.
- [node-editor.md](node-editor.md): making mods from node graphs.
- [native-mods.md](native-mods.md): C++ plugins.
- [internals.md](internals.md): how reNut and RexFabric fit together.
- Tutorials: [parts and vehicles](tutorials/parts-and-vehicles.md),
  [worlds and doors](tutorials/worlds-and-doors.md),
  [characters and challenges](tutorials/characters-and-challenges.md),
  [HUD, menus and input](tutorials/hud-menus-and-input.md),
  [online multiplayer](tutorials/online-multiplayer.md),
  [hooks and memory](tutorials/hooks-and-memory.md).
