# Roadmap

What RexFabric does today, and what comes next, roughly in order.

## Done

| Piece | Notes |
|---|---|
| Hook slots through codegen fragments | Every recompiled function is hookable, no SDK patch |
| Template composition | Any module can add codegen fragments |
| Hook core | Chains, ordering, ownership, safe publishing |
| Native mods | `.asi`, `.dll`, `.so`, `.dylib` with a versioned C API |
| Lua mods | The old script runtime's API, unchanged, on the hook core |
| Midasm sites and switches | Compiled-in stubs mods claim, from Lua and native mods |
| The game's API | Functions, types, enums, constants and events a game declares for every front end |
| Mod list | Load order and enabled state in `mods.json`, names from `mod.toml` and `manifest.json` |
| Thunderstore | Listing, search, categories, install with dependencies, set up per game in code |
| Mods menu | Plain ImGui windows, or a Guide blade and window with RexLiveless, laid out for a pad (verified in the full Guide) |
| The game's side | Guest hooks for the game's own C++, the content API, texture packs, taking over an old mod list (used by retip) |
| Node editor | Lua mods from node graphs, in game, with the game's API as nodes; graphs compile to plain Lua scripts |
| Content | Classes, enums, structs, data tables, languages, textures, avatars with the game's components, the content browser and its editors (docs/content_and_classes.md) |
| Map edits | Assets changing the game's worlds as they're entered (spawns, and the game's own kinds of edit), edited in the world itself with markers and gizmos over the game's picture |
| Materials | Assets of the game's material types, edited with a live preview drawn by the game's renderer, worn live by whatever the game draws with them |
| Graph materials | Materials made of node graphs lit by the game's shading models, compiled to HLSL for the game's shader, in an editor after Unreal's (docs/materials.md) |
| Meshes | 3D models imported as binary glTF, which the game reads and draws (content_and_classes.md, Meshes) |
| Worlds | Mods' worlds built on the game's, with map edits of their own, entered where the game would enter the base (content_and_classes.md, Worlds) |
| Waiting and the console | `rex.run`, `rex.wait` and `rex.wait_for` in Lua coroutines; a Lua console running a line in the mods' VM |
| Packets | Kinds of ground mods add as variants of a game's own, with their texts, icon, look and unlock, made and edited in the content browser and listed for the game with its own (content_and_classes.md, Packets) |
| Saved with the world | Avatars spawned saved kept in the game's save of their world and spawned again when it loads, each mod's own `rex.world_data()`, and parts the game keeps of its own (`DeclareWorldSavePart`); what a mod that's gone had is kept for when it's back, never failing the load |
| Mod channel | Messages by named topic between the boxes of a co-op game, over a transport the game gives on its own network stream (docs/mod_channel.md); used by retip to keep mod packets in step |

## Next

**Updates and uninstall.** A package's own page already offers Update when Thunderstore has a newer version than the one installed. Still to come: saying so in the lists without opening each package, updating everything at once, and removing a mod from the menu.

**Mods in co-op.** Lua bindings for the mod channel (`rex.channel`), and sharing the mod list before a join, so a guest can fetch from Thunderstore what the host runs.

**Content mods.** Texture and audio replacement from mod folders, likely as an API the game's renderer and audio call, since what can be replaced is the game's business.

**Live reload for native mods,** once it's possible to know no guest thread is inside one.

**More from the node editor.** Comment boxes to group nodes, a Delay node that waits frames, functions made from a group of nodes, and graph variables shown as settings in the game's menus.

**C#.** A .NET front end on the same game API, if modders ask for it.

**r2modman.** A launch option for the mods folder, so mod managers can start the game with their own profile.
