# Adding RexFabric to a game

## CMake

```cmake
include(path/to/RexFabric/CMakeLists.txt)
rexglue_setup_target(my_game)
rexfabric_attach(my_game)               # or with any of WITH_GUIDE, WITH_TEXTURES, WITH_NODE_EDITOR
```

`WITH_GUIDE` puts the mods menu in RexLiveless's Guide; attach RexLiveless to the target first. Without it the menu is plain ImGui windows. `WITH_TEXTURES` adds texture packs for a game whose renderer uploads textures itself; see [game_side.md](game_side.md). `WITH_NODE_EDITOR` adds the node editor, which makes Lua mods from node graphs in game; see [node_editor.md](node_editor.md).

With RexGlass attached to the same target, before or after RexFabric, the Mods and Browse Mods windows and the node editor are glass once the game installs RexGlass. Nothing else is needed; without RexGlass they're plain ImGui windows. With the Guide, the menu is RexLiveless's, which is glass with RexGlass too.

Add a template folder to the manifest, which RexFabric fills with the codegen templates its hooks need:

```toml
[entrypoint]
template_dir = "generated/templates"
```

RexFabric reads the stock templates from the SDK source tree: set `REXSDK_DIR`, or `REXFABRIC_SDK_TEMPLATES` to `<rexglue-sdk>/resources/templates`.

RexFabric brings Lua 5.4.8 (compiled as C++), miniz and nlohmann json in `thirdparty`. On Windows it links WinHTTP; elsewhere it loads the system's libcurl when Thunderstore is first used.

## The app

```cpp
void OnPreSetup(rex::RuntimeConfig& config) override {
  rexfabric::Setup({.thunderstore = "my-community", .menu_key = "F5", .frame_function = 0x8266FBB0});
}
void OnPostSetup() override {
  DeclareModApi();          // the game's own API, see game_api.md
  rexfabric::Start();
}
void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override { rexfabric::CreateMenus(drawer, immediate_drawer()); }
bool OnWindowCloseRequested() override { rexfabric::Stop(); return true; }
void OnShutdown() override { rexfabric::Stop(); }
```

Closing the window exits the process without reaching `OnShutdown`, so `Stop` is called on the close request too; it only runs once. With the Guide, call `CreateMenus` after `rexglue_guide::Install`.

## Config

| Member | Is |
|---|---|
| `thunderstore` | The game's Thunderstore community, as in `thunderstore.io/c/<community>`. Empty: no browser. |
| `mods_dir` | Where mods live. Empty: `mods` beside the executable. |
| `menu_key` | The key that opens the mods menu, such as `"F5"`. It becomes the rebindable `bind_mods`. Empty: no key; the game calls `ShowMenu` itself. |
| `node_editor_key` | The key that opens the node editor, such as `"F6"`, with `WITH_NODE_EDITOR`. It becomes the rebindable `bind_node_editor`. Empty: no key; the mods menu opens it. |
| `frame_function` | A guest function the game runs once a frame, such as its present. RexFabric hooks it to fire the frame event. 0: the game calls `rexfabric::Tick` once a frame. |
| `native_mods`, `lua_mods` | Turn either kind of mod off. |

These are the game's decisions, so they're set in code rather than as cvars.

## Mods on disk

Each mod is a folder in the mods directory. What's in it decides what it is: `.asi` and library files are native mods, `scripts/*.lua` is a Lua mod, and a folder can be both. `mod.toml` (`name`, `description`, `version`, `author`) and Thunderstore's `manifest.json` name it in the menu, and `icon.png` is its picture.

The load order lives in `mods.json` beside the mods:

```json
{ "load_order": ["UnlimitedGardenSpace", "FreeCam"], "disabled": ["Test"] }
```

A folder the file doesn't mention is new, and joins the end of the load order enabled. The menu rewrites the file as mods are turned on, off and moved.

## The mods menu

The menu lists the mods in two groups, enabled in load order and available, and turns them on and off and moves them. When the game has a Thunderstore community it also browses it: search, sort, categories, and install. Installing puts a package in `mods/<package name>`, with any dependencies that aren't there yet, and enables it.

Mods load at startup, so changes need a restart; the menu offers one, which starts the game again with the same arguments. Lua mods can also reload in place with Reload Scripts.

With `WITH_GUIDE`, the menu is a Mods blade in the Guide and a window of the same rows for the menu key. Without it, it's a Mods window with a Browse Mods window beside it.

With `WITH_NODE_EDITOR`, the menu also opens the node editor: a Node Editor button in the Mods window, or a Node Editor action in the Guide. `rexfabric::MenuShown()` is true while either the menu or the editor is open, so a game that checks it before giving mods input covers both.

### The Guide's rows

The Guide is worked with a pad, so its menu is laid out for that:

| Page | Rows |
|---|---|
| Mods | Browse Mods, Reload Scripts, Open Folder and, with the node editor, Node Editor side by side, then the mods as two lists side by side, as on a pack screen: Available on the left, Enabled on the right in load order, each with its icon, version and author. |
| A mod | Its picture, version, author and description, Enabled, Open Folder. |
| Browse Mods | Search, Filter (sort, category, deprecated; its row shows what's set), the packages with icons and whether each is installed, then Previous Page and Next Page. |
| A package | Its picture, author, downloads, version and date, and description; Manage (the mod's own page) once installed; one install row that reads Install, then what it's doing (Checking, Downloading), then Update with the new version or Reinstall; View on Thunderstore; and what it requires, marked when already installed. |

Once a change needs a restart, "Restart to Apply Changes" shows above the actions on every page. It opens a page of its own with Restart Now and Not Now, so the press meant for the first row of a page never restarts the game. An install that fails shows on the Mods page too, with Dismiss, whichever page the player had gone to meanwhile.

In the two lists, up and down move through a list and left and right go between them. A opens a mod's page, X enables or disables it (the selection stays put, so a run of mods goes across with repeated presses), and Y picks it up: up and down then move it through the load order, left and right carry it across, and any face button puts it down. The mouse gets arrows on the mod under it, and the wheel scrolls the list it's over. The legend shows what X and Y do.

Rows that come and go never move the selection off the row it's on, and Previous Page and Next Page are always both there (greyed at the ends), so paging keeps the selection where it is.
