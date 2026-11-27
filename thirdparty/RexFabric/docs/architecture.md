# How RexFabric works

RexFabric lets mods run code inside a recompiled game. It has three layers, and each one only talks to the ones below it.

| Layer | What it is | Where |
|---|---|---|
| Hook slots | One slot per recompiled function, checked on every call | `templates/`, `include/rexfabric/hook_slot.h` |
| Core | Hooks, events, midasm sites, switches, and the game's declared API | `src/hooks.*`, `src/events.*`, `src/midasm.cpp`, `src/switches.cpp`, `src/game_api.cpp` |
| Front ends | Ways to write mods: native and Lua today, C# planned | `src/native_loader.*`, `src/api.cpp`, `src/lua/` |

Beside them sit the mod list (`src/mods.*`), the Thunderstore client and the mod channel (`src/net/`), the menus (`src/ui/`) and the node editor (`src/graph/`, `src/ui/node_editor.*`).

The node editor isn't a front end of its own: it compiles graphs to Lua scripts, which the Lua front end runs like any other. See [node_editor.md](node_editor.md).

## Hook slots

In a rexglue project every guest function becomes a C++ function, defined through the `DEFINE_REX_FUNC` macro from the generated `<project>_pch.h`. Recompiled code calls other functions by that name, and indirect calls go through a table of the same names, so whatever that name runs sees every call.

RexFabric replaces the macro. Each function becomes a small shim:

```cpp
void sub_82271D10(PPCContext& ctx, uint8_t* base) {
  if (rexfabric_slot_sub_82271D10.chain.load(std::memory_order_relaxed)) [[unlikely]] return rexfabric::Dispatch(...);
  [[clang::musttail]] return __imp__sub_82271D10(ctx, base);
}
```

`__imp__sub_82271D10` is the recompiled body. An unhooked call costs one relaxed load and a predicted branch, then a jump to the body. The slots themselves, and a table of them by address, are generated into `<project>_init.cpp`.

The shim stays a weak symbol, so a project that defines a function itself with `REX_HOOK` still replaces it outright. RexFabric hooks on such a function never run, because the shim they hang off is gone.

## Getting the macro into codegen

Codegen reads one `template_dir` from the manifest, and every file there replaces a stock template whole. Copying whole templates would tie RexFabric to one SDK version, so it adds fragments to the stock templates instead, at anchor lines:

| Template | Fragment | Where |
|---|---|---|
| `codegen/pch_h` | `templates/pch_hooks.inja`, the hookable `DEFINE_REX_FUNC` | before `#ifndef DECLARE_REX_FUNC` |
| `codegen/init_cpp` | `templates/init_slots.inja`, the slots and the slot table | at the end |

The stock `DEFINE_REX_FUNC` sits inside `#ifndef DEFINE_REX_FUNC`, so defining it first is enough.

At configure time RexFabric reads the stock templates from the SDK source tree, applies every fragment, and writes the results into the project's `template_dir`. A file is only rewritten when its text changes, because codegen reruns whenever a template changes. If an SDK update removes an anchor, or makes it appear twice, configuring stops and names the fragment. See [templates.md](templates.md) for adding fragments from another module.

## Hook core

`Hooks` (`src/hooks.h`) maps guest addresses to slots and slots to hook chains.

A chain is an immutable list of hooks. Adding or removing a hook builds a new chain under a mutex and publishes it with one atomic store, so a guest thread that already loaded the old chain finishes on it. Old chains are never freed: nothing tracks when the last thread leaves one, and hooks change rarely enough that keeping them costs little.

A hooked call runs `Dispatch`, which walks the chain. Each hook receives the call and decides when to run the rest of it with `call_next`:

* before `call_next`, it sees and can change the arguments;
* after `call_next`, it sees and can change the result;
* without `call_next`, it replaces the function.

Hooks on one function run in the order they were added, the first outermost.

Every hook has an owner, a `RexFabricMod`. Unloading a mod removes all of its hooks at once.

## Native mods

A native mod is a shared library: `.asi` or `.dll` on Windows, `.asi` or `.so` on Linux, `.asi` or `.dylib` on macOS. RexFabric looks for them in each mod folder and in its `plugins` subfolder.

A mod exports `RexFabricModInit`, which receives the API table and the mod's own handle. It returns nonzero to stay loaded. `RexFabricModShutdown` is optional. A library without `RexFabricModInit` still loads, so an `.asi` that sets itself up in `DllMain` works too.

The API (`include/rexfabric/api.h`) is plain C with no SDK types. Registers are read by number through functions rather than by touching `PPCContext`, so an SDK change to the register layout never breaks a mod. The table only grows: new members go at the end with a version bump. See [native_mods.md](native_mods.md).

Native mods load in `rexfabric::Start`, which the project calls from `ReXApp::OnPostSetup`: guest memory exists by then, and the game hasn't started. `rexfabric::Stop` removes each mod's hooks and runs its shutdown, last loaded first. The project calls it from `OnWindowCloseRequested` as well as `OnShutdown`, because closing the window hard exits the process without reaching `OnShutdown`. The libraries stay mapped until the process exits, since a guest thread may still be returning through a hook.

## Lua mods

One Lua VM runs every Lua mod, behind a recursive mutex, since hooks fire on several guest threads. Lua hooks are before and after rather than around, as the old script runtime's were: one core hook per address runs every Lua before hook under the lock, then the original function outside it, then the after hooks. So a lock is never held across guest code the mod didn't ask for, and mods written for the old runtime behave exactly as they did. Lua is compiled as C++, so a Lua error unwinds C++ frames as an exception instead of jumping over their destructors. See [lua_mods.md](lua_mods.md).

## Events, midasm sites and switches

An event is a named list of listeners, published the same way as hook chains, so firing one takes no lock and costs one load with nobody listening. RexFabric fires `frame`; the game declares and fires its own.

A midasm site is a stub the game compiled into the middle of a function. It asks the mods that claimed its address, and an unclaimed one costs one load. A switch is a named integer the game's compiled-in code reads and mods set. Both are owned by the mod that claimed or set them, so unloading or reloading drops them.

## The game's API

Games declare functions, types, constants, enums and events in C++ (`include/rexfabric/game_api.h`). Functions are stored with their argument and result types, so each front end converts its own values to them: Lua builds tables and object proxies from the declarations, and native mods look them up by name. Nothing about any one game is in RexFabric. See [game_api.md](game_api.md).

## The mod channel

Messages between the boxes of a co-op game, by named topic, to the host, to every box or to one, with a handler per topic (`include/rexfabric/mod_channel.h`). RexFabric frames each message by its topic's hash; the game supplies the transport over its own network stream, so nothing about any one game's network is in RexFabric. See [mod_channel.md](mod_channel.md).

## Limits today

* Only the entrypoint module is hookable. Games with guest DLL modules get slots in each module, but only the executable's table is indexed.
* Hooks attach to whole functions. Partway through a function, only the midasm sites the game compiled in can be claimed.
* Native mods load once at startup. Unloading a running native mod needs a way to know no guest thread is inside it, which doesn't exist yet. Lua mods can reload.
