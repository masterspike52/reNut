# reNut modding documentation

reNut, the PC version of Banjo-Kazooie: Nuts & Bolts, loads mods from the `mods` folder next to `renut.exe`.
A mod can add vehicle parts and vehicles, whole worlds with doors in Showdown Town, characters with
challenges, menus and HUDs, online features, and change how the game behaves, without touching the
game's own files.

Mods are written in Lua against two APIs:

- **`renut`**: reNut's own API for the game: parts, vehicles, worlds, characters, challenges, the HUD,
  sound, the camera, saves, online messages and more.
- **`rex`**: RexFabric's API, the modding framework under reNut: hooks into the game's code, its memory,
  calling its functions, input, settings, logging and drawing on screen.

**NutTool** makes the content Lua can't: your own models, textures, animations, parts, worlds,
characters and player models, and it writes the mod (and its Lua) for you.

## Where to start

| You want to... | Read |
|---|---|
| Make your first mod | [Getting started](getting-started.md) |
| Understand how mods load and run | [Mods and scripts](mods-and-scripts.md) |
| Look up a `renut` function | [The renut API](renut-api.md) |
| Look up a `rex` function | [The RexFabric API](rexfabric-api.md) |
| Make mods from nodes, in game | [The node editor](node-editor.md) |
| Use your own models, worlds and characters | [NutTool](nuttool/README.md) |

## Tutorials

| Tutorial | What it covers |
|---|---|
| [Parts and vehicles](tutorials/parts-and-vehicles.md) | New parts and blocks, behaviour, forces, weapons and projectiles, aiming, bones, every vehicle's events, spawning vehicles. |
| [Worlds and doors](tutorials/worlds-and-doors.md) | Worlds of your own, doors in Showdown Town, travelling between worlds, trigger zones, music, materials, water, fog. The Hyrule mod as the example. |
| [Characters and challenges](tutorials/characters-and-challenges.md) | Your own characters with text, voices and challenges, enemies, the game's characters, challenges made in Lua, rewards, crates and objects. |
| [HUD, menus and input](tutorials/hud-menus-and-input.md) | On-screen text, bars and markers, message boxes, the challenge panel, pick-lists, rows in the game's menus, keyboard, mouse and controller, the camera, sound. |
| [Online multiplayer](tutorials/online-multiplayer.md) | Messages between players with `renut.net`, designing a networked mod, and how the Online Co-op mod works. |
| [Hooks and memory](tutorials/hooks-and-memory.md) | Working with the game's code directly: finding functions, hooks, calling game functions, reading and writing memory. |

## References

| Page | What it covers |
|---|---|
| [renut-api.md](renut-api.md) | Every `renut` function, object, option and event, and when each can be called. |
| [rexfabric-api.md](rexfabric-api.md) | Every `rex` function: logging, timers, events, hooks and the register context, `rex.call`, memory, input and key names, cvars, drawing, switches, midasm. |
| [mods-and-scripts.md](mods-and-scripts.md) | The mods folder, `mod.toml`, load order, the mods menu, keys, how scripts run, `require`, errors and the log, reloading, threads, data, settings, sharing between mods, `.nbpatch` files. |
| [node-editor.md](node-editor.md) | RexFabric's in-game node editor: nodes, graphs and how they become a mod's Lua. |
| [native-mods.md](native-mods.md) | Native (DLL) mods and RexFabric's C API, and the host side for reNut developers. |
| [internals.md](internals.md) | For advanced modders: game addresses and structures, the challenge manager, functions reNut replaces, the underscore functions. |
| [effects.md](effects.md) | Every particle effect in the game, with its id, for `renut.fx.play`. |
| `lua_api/renut.lua` (next to `renut.exe`) | The `renut` API's definitions for editor autocomplete; see [Mods and scripts](mods-and-scripts.md). |

## NutTool

| Page | What it covers |
|---|---|
| [README](nuttool/README.md) | What NutTool is, its two programs, setting it up. |
| [GUI guide](nuttool/gui-guide.md) | The window, the asset browser, tabs, every menu, settings. |
| [Custom models](nuttool/custom-models.md) | Preparing models in Blender: formats, scale and axes, materials, shape keys, bones, collision. |
| [Animations](nuttool/animations.md) | Replacing animations, the timeline format, the Blender add-on. |
| [Parts and vehicles](nuttool/parts-and-vehicles.md) | New parts and blocks from your models, ready-made vehicles. |
| [Worlds](nuttool/worlds.md) | Worlds from your own models, their materials, water and doors. |
| [Characters](nuttool/characters.md) | Characters, enemies, lines, voices and challenges. |
| [Player models](nuttool/player-models.md) | Replacing Banjo's model. |
| [Textures and text](nuttool/textures-and-text.md) | Textures, menus, text tables, scripts, fonts, audio and video. |
| [Script graphs](nuttool/script-graphs.md) | NutTool's script graphs: a mod's Lua from nodes. |
| [Command line](nuttool/cli.md) | Every `nuttool.exe` command, and NutTool's own Lua scripting. |
| [Packaging and sharing](nuttool/packaging-and-sharing.md) | Mod folders and `.nbpatch` files, installing, testing, sharing. |
| [Troubleshooting](nuttool/troubleshooting.md) | Undoing changes, common problems, FAQ. |

## Keys in reNut

| Key | What |
|---|---|
| **F9** | The mods menu: turn mods on and off, order them, reload scripts. |
| **F10** | The node editor. |
| **F4** | reNut's settings (a mod's settings are under "Mod: <name>"). |
| **F8** | All settings. |
| **F2** | reNut's log window. Mods' `print` output and errors go to `logs/renut_<n>.log`. |
| **G** | The Guide menu (its Mods entry opens the mods menu too). |
