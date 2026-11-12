# Getting started with Lua mods

This tutorial takes you from nothing to a small working mod for reNut. You'll make a mod folder, print
to the log, show a message in the game, add hotkeys, refill your vehicle's fuel, place a trigger zone,
reload your script without restarting, and share the result.

You need:

- reNut, set up so the game runs.
- A text editor. Any will do; [VS Code](#13-optional-autocomplete-in-vs-code) can autocomplete the
  `renut` functions for you.
- A little Lua: variables, functions, `if`, tables. Nothing fancy.

Every mod uses two Lua tables:

- `renut` is the game: vehicles, parts, the player, the HUD, worlds, events. Every function is in
  [renut-api.md](renut-api.md).
- `rex` is RexFabric, the modding framework under reNut: keys, timers, settings, drawing on the screen,
  hooks and memory. Every function is in [rexfabric-api.md](rexfabric-api.md).

How mods work in detail (load order, errors, threads, settings and so on) is in
[mods-and-scripts.md](mods-and-scripts.md). This page only shows you enough to get going.

## Contents

1. [Find the mods folder](#1-find-the-mods-folder)
2. [Make your mod's folder](#2-make-your-mods-folder)
3. [Write mod.toml](#3-write-modtoml)
4. [Your first script](#4-your-first-script)
5. [Start reNut and read the log](#5-start-renut-and-read-the-log)
6. [Show a message in the game](#6-show-a-message-in-the-game)
7. [Add a hotkey](#7-add-a-hotkey)
8. [Reload without restarting](#8-reload-without-restarting)
9. [A real change: refill your fuel](#9-a-real-change-refill-your-fuel)
10. [A trigger zone](#10-a-trigger-zone)
11. [Turn mods on and off, and order them](#11-turn-mods-on-and-off-and-order-them)
12. [When something goes wrong](#12-when-something-goes-wrong)
13. [Optional: autocomplete in VS Code](#13-optional-autocomplete-in-vs-code)
14. [Share your mod](#14-share-your-mod)
15. [The whole script](#15-the-whole-script)
16. [Where next](#16-where-next)

## 1. Find the mods folder

1. Open reNut's folder: the one with `renut.exe` in it.
2. Open the `mods` folder inside it. reNut makes it the first time it starts a game, so if it isn't
   there yet, start reNut once, or make the folder yourself.

Other ways to get there:

- In the game, press **F9** to open the mods menu, then choose **Open Folder**.
- On Linux with the AppImage, the folder is `~/.local/share/renut/mods` (or
  `$XDG_DATA_HOME/renut/mods` when that's set).

## 2. Make your mod's folder

1. Inside `mods`, make a folder called `my_first_mod`.
2. Inside `my_first_mod`, make a folder called `scripts`.

You'll end up with this:

```
mods/
  my_first_mod/
    mod.toml
    scripts/
      main.lua
```

The folder's name is your mod's id. It shows in the log, in the load order file and in your mod's
saved data and settings, so pick it once and keep it. Stick to lower case letters, digits and `_`.

## 3. Write mod.toml

Make a file called `mod.toml` in `my_first_mod` with this in it:

```toml
name = "My First Mod"
description = "Messages, hotkeys, a fuel refill and a trigger zone."
version = "1.0"
author = "Your Name"
```

This is what the mods menu shows. Every line is optional: with no `name`, the menu shows the
folder's name.

If you want a picture in the menu, put a square `icon.png` next to `mod.toml`.

## 4. Your first script

Make a file called `main.lua` in `my_first_mod/scripts` with one line:

```lua
print("Hello from my first mod!")
```

Every `.lua` file directly inside `scripts` runs once when the game starts. `print` writes a line to
reNut's log.

## 5. Start reNut and read the log

1. Start reNut and play as usual. A new mod folder is turned on by itself.
2. Close reNut again (or keep it running and use another window).
3. Open the `logs` folder in reNut's folder and open the newest `renut_<number>.log` (the highest
   number is the latest run).
4. Search for `my_first_mod`. You should find two lines like these:

```
2026-11-12 14:02:11.315 I rexfabric  t22844 [my_first_mod] Hello from my first mod!
2026-11-12 14:02:11.316 I rexfabric  t22844 [my_first_mod] 1 of 1 scripts ran
```

Every line from your mod starts with your folder's name in square brackets. `1 of 1 scripts ran`
means your file loaded without an error.

> **Note:** the **F2** window in the game is reNut's own log. It doesn't show your mod's lines; the
> log file does. For quick checks while playing, draw on the screen instead (see the tip in
> [step 7](#7-add-a-hotkey)).

## 6. Show a message in the game

Printing to the log is useful, but let's show something in the game. Add this to the end of
`main.lua`:

```lua
-- Each time a world finishes loading, greet the player.
renut.game.on("world_enter", function(number, name)
  renut.wait(1)  -- give the world a second to settle
  renut.hud.message("Welcome to " .. name .. "!", 3)
end)
```

What's happening:

- `renut.game.on("world_enter", fn)` calls your function every time a world loads. It gets the
  world's number and name.
- `renut.wait(1)` pauses your function for one second of game time, then it carries on. It only works
  inside callbacks like this one.
- `renut.hud.message(text, seconds)` shows the game's own message box for that many seconds.

Restart reNut and go into any world (Showdown Town counts). After a second you get a message box.

## 7. Add a hotkey

Now a key that does something. Add this to the end of `main.lua`:

```lua
-- F6: say hello.
rex.on("frame", function()
  if rex.input.pressed("F6") and rex.input.accepts() then
    print("F6 was pressed")
    renut.hud.message("Hello from my first mod!", 2)
  end
end)
```

What's happening:

- `rex.on("frame", fn)` calls your function once every frame, everywhere: in menus, in worlds, while
  paused.
- `rex.input.pressed("F6")` is `true` once when the key goes down, then `false` until you let go and
  press it again. Keys are named like `"F6"`, `"J"`, `"Space"`, `"Return"`, `"Up"`.
- `rex.input.accepts()` is `false` while a menu or window has the keyboard (the mods menu, the
  settings, the Guide), so typing there doesn't trigger your mod.
- `renut.hud.message` needs a world to show the box in. The `frame` event isn't part of the game's
  own update, so reNut holds your function at that line until the game's next update, then shows the
  message. If you press F6 on the title screen, the message waits until you're in a world.

Avoid keys the game already uses. By default the keyboard plays the game with W A S D, Space, E, Q,
R, F, C, V, G, Shift, Control, Tab, Escape and the arrow keys, and reNut's windows use F1, F2, F4, F5,
F8, F9 and F10. The full list is in [mods-and-scripts.md](mods-and-scripts.md#keys).

> **Tip:** `rex.screen_text("some text", 3)` puts a line of text in the top right corner of the screen
> for 3 seconds, anywhere and at any time. It's the quickest way to see a value while you play.

## 8. Reload without restarting

You don't need to restart reNut each time you change a script.

1. Save `main.lua`.
2. In the game, press **F9** to open the mods menu.
3. Choose **Reload Scripts** at the top.
4. Close the menu (F9 again).

Every Lua mod is stopped and every script runs again from the top, so your changes apply straight
away. Try it: change the F6 message text, reload, press F6.

Some things do need a restart: new vehicle parts, `.nbpatch` files, model bundles and native plugins.
[mods-and-scripts.md](mods-and-scripts.md#reload-scripts) lists what a reload redoes and what it
doesn't.

## 9. A real change: refill your fuel

Time to change the game. This key fills the fuel of the vehicle you're driving. Add it to the end of
`main.lua`:

```lua
-- J: fill the fuel of the vehicle Banjo is driving.
renut.game.on("tick", function()
  if not (rex.input.pressed("J") and rex.input.accepts()) then return end

  local vehicle = renut.player.vehicle()
  if not vehicle then
    renut.hud.message("Get in a vehicle first!", 2)
    return
  end
  if vehicle:max_fuel() == 0 then
    renut.hud.message("This vehicle has no fuel tank.", 2)
    return
  end

  vehicle:set_fuel(vehicle:max_fuel())
  renut.hud.message("Fuel refilled!", 2)
end)
```

What's happening:

- `renut.game.on("tick", fn)` calls your function once per game update while you're in a world. It
  doesn't run on the title screen or while the game is paused. Because it's part of the game's own
  update, game functions run straight away here.
- `renut.player.vehicle()` is the vehicle Banjo is in, or `nil` on foot.
- `vehicle:max_fuel()` is how much fuel its tanks hold (0 with no fuel tank), and
  `vehicle:set_fuel(x)` sets it.

Reload Scripts (F9), build or pick a vehicle with a fuel tank, drive until the fuel drops, and press
**J**.

The vehicle object can do much more: `set_ammo`, `repair`, `set_invulnerable`, `push` and so on. See
[renut-api.md](renut-api.md) and [tutorials/parts-and-vehicles.md](tutorials/parts-and-vehicles.md).

## 10. A trigger zone

A trigger zone is an invisible sphere or box; reNut tells you when Banjo (or his vehicle, or other
characters) goes in or out. Add this to the end of `main.lua`:

```lua
-- K: put a zone where Banjo stands. Going in or out of it shows a message.
local ZONE = "my_first_mod_spot"

renut.game.on("tick", function()
  if rex.input.pressed("K") and rex.input.accepts() then
    renut.zones.add{name = ZONE, at = renut.player.position(), radius = 4, watch = {"banjo", "vehicle"}}
    print("zone placed")
  end
end)

renut.zones.on("enter", function(zone)
  if zone == ZONE then renut.hud.message("You found the secret spot!", 2) end
end)

renut.zones.on("exit", function(zone)
  if zone == ZONE then rex.screen_text("You left the secret spot", 2) end
end)
```

What's happening:

- `renut.zones.add{...}` makes a zone: a `name`, where it is (`at`), its `radius` in game units, and
  what it `watch`es. Adding a zone with a name that already exists replaces that zone instead of
  making a second one, so pressing K again moves it.
- Zone names are shared by every mod, so start yours with your mod's name.
- `renut.zones.on("enter", fn)` and `renut.zones.on("exit", fn)` get the zone's name (then the object
  and what it was: `"banjo"`, `"vehicle"` or `"character"`).

Reload Scripts, go into a world and press **K**. You're standing in the new zone, so you get the
message at once. Walk out and you see the screen text; walk back in and the message shows again.

Zones made like this stay until reNut closes. To place one at a fixed spot instead, give `at` a
position: `at = renut.vec(12.5, 3, -40)`. To find Banjo's coordinates, press **F4**, open the
**Debug** tab and turn on **Debug player position**.

## 11. Turn mods on and off, and order them

Press **F9** for the mods menu (it's also in the Guide, the **G** key or the controller's Guide
button, as **Mods**). It has two lists:

- **Available**: mods that are turned off.
- **Enabled**: mods that are on, in load order (top loads first).

With a controller (or the keys that play as one):

1. Select a mod and press **X** to move it to the other list (Enable or Disable).
2. To change the order, select an enabled mod, press **Y** to pick it up, move it up or down, and press
   **Y** again to put it down.
3. Press **A** on a mod to open its page: its icon, version, author, description, an **Enabled**
   switch and **Open Folder**.

With the mouse, click a mod to open its page. Point at a mod and arrow buttons appear on it: one
moves it to the other list, and on enabled mods the up and down arrows move it in the load order.

After a change, a **Restart to Apply Changes** row shows up. Choose it, then **Restart Now** to
restart reNut (or **Not Now** to keep playing). A mod that's only Lua scripts can also be picked up by
**Reload Scripts** without a restart.

The order is saved in `mods/mods.json`. More on that in
[mods-and-scripts.md](mods-and-scripts.md#load-order-modsjson).

## 12. When something goes wrong

When a script has a mistake, the game carries on and the error goes to the log.

1. Open the newest `logs/renut_<number>.log`.
2. Search for `my_first_mod`, or for ` E rexfabric` (the error lines).

A mistake while the file loads (a typo, calling something that doesn't exist) looks like this:

```
... E rexfabric  t22844 [my_first_mod] main.lua: F:/renut/mods/my_first_mod/scripts/main.lua:12: attempt to call a nil value (field 'mesage')
stack traceback:
        ...
... I rexfabric  t22844 [my_first_mod] 0 of 1 scripts ran
```

It tells you the file and the line (`main.lua:12`) and what went wrong. Everything above that line in
the file already ran; everything after it didn't. Fix it, save, and Reload Scripts.

A mistake inside one of your callbacks (a `tick` or `frame` function, say) shows up each time that
callback runs, with the event's name and the same file and line. Your callback stays registered, so
once you fix it and reload, it works again.

Common mistakes:

- **The file isn't directly in `scripts`.** Only `.lua` files directly inside `scripts` run.
- **The mod is turned off.** Check the mods menu (F9).
- **A typo in a name**, like `renut.hud.mesage`. The error says `attempt to call a nil value`.
- **Forgetting `local`.** Every mod shares one Lua state, so a global you make is visible to every
  other mod. Make your variables and functions `local`.
- **Expecting F2 to show your prints.** F2 is reNut's own log; your lines are in the log file.

## 13. Optional: autocomplete in VS Code

reNut ships definitions of every `renut` function in `lua_api/renut.lua`, next to `renut.exe`. VS Code
can read them:

1. Install [VS Code](https://code.visualstudio.com/) and its **Lua** extension (by sumneko).
2. In VS Code, choose **File > Open Folder** and open your mod's folder (`mods/my_first_mod`).
3. Make a file called `.luarc.json` in your mod's folder with this in it:

```json
{
  "runtime.version": "Lua 5.4",
  "workspace.library": ["../../lua_api"],
  "diagnostics.globals": ["rex"]
}
```

Now typing `renut.` lists the functions, with what each one takes and does. The path
`../../lua_api` goes from your mod's folder up to reNut's folder; if your mods folder lives somewhere
else, write the full path to `lua_api` instead.

## 14. Share your mod

1. Bump `version` in `mod.toml`.
2. Make a zip of your mod's folder (the zip holds `my_first_mod/` with everything inside it).
3. Share the zip.

To install it, people extract the zip into their `mods` folder, so they get `mods/my_first_mod/`.
reNut turns a new folder on by itself the next time it starts.

Things to know:

- Ship what your mod uses: `mod.toml`, `icon.png`, `scripts/`, and if you have them `graphs/`,
  `bundles/`, `worlds/`, `vehicles/`, sound files and `.nbpatch` files.
- Don't ship `mods/mods.json`; it's each player's own load order.
- Keep the folder name the same in every version. Your mod's saved data and its settings are kept
  under that name.

## 15. The whole script

Here's `mods/my_first_mod/scripts/main.lua` with every step in it:

```lua
print("Hello from my first mod!")

-- Each time a world finishes loading, greet the player.
renut.game.on("world_enter", function(number, name)
  renut.wait(1)  -- give the world a second to settle
  renut.hud.message("Welcome to " .. name .. "!", 3)
end)

-- F6: say hello.
rex.on("frame", function()
  if rex.input.pressed("F6") and rex.input.accepts() then
    print("F6 was pressed")
    renut.hud.message("Hello from my first mod!", 2)
  end
end)

-- J: fill the fuel of the vehicle Banjo is driving.
renut.game.on("tick", function()
  if not (rex.input.pressed("J") and rex.input.accepts()) then return end

  local vehicle = renut.player.vehicle()
  if not vehicle then
    renut.hud.message("Get in a vehicle first!", 2)
    return
  end
  if vehicle:max_fuel() == 0 then
    renut.hud.message("This vehicle has no fuel tank.", 2)
    return
  end

  vehicle:set_fuel(vehicle:max_fuel())
  renut.hud.message("Fuel refilled!", 2)
end)

-- K: put a zone where Banjo stands. Going in or out of it shows a message.
local ZONE = "my_first_mod_spot"

renut.game.on("tick", function()
  if rex.input.pressed("K") and rex.input.accepts() then
    renut.zones.add{name = ZONE, at = renut.player.position(), radius = 4, watch = {"banjo", "vehicle"}}
    print("zone placed")
  end
end)

renut.zones.on("enter", function(zone)
  if zone == ZONE then renut.hud.message("You found the secret spot!", 2) end
end)

renut.zones.on("exit", function(zone)
  if zone == ZONE then rex.screen_text("You left the secret spot", 2) end
end)
```

## 16. Where next

- [mods-and-scripts.md](mods-and-scripts.md): how mods load and run, saving data
  (`renut.data`), settings in the F4 menu (`renut.settings`), mods talking to each other
  (`renut.mods`), and `.nbpatch` files.
- [renut-api.md](renut-api.md) and [rexfabric-api.md](rexfabric-api.md): every function.
- [node-editor.md](node-editor.md): make mods by connecting nodes instead of typing code (F10).
- Tutorials:
  - [Parts and vehicles](tutorials/parts-and-vehicles.md)
  - [Worlds and doors](tutorials/worlds-and-doors.md)
  - [Characters and challenges](tutorials/characters-and-challenges.md)
  - [HUD, menus and input](tutorials/hud-menus-and-input.md)
  - [Online multiplayer](tutorials/online-multiplayer.md)
  - [Hooks and memory](tutorials/hooks-and-memory.md)
- [native-mods.md](native-mods.md): mods in C++ (`.dll` plugins).
- [nuttool/README.md](nuttool/README.md): NutTool, for models, worlds and characters.
