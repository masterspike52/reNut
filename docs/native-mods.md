# Native mods and RexFabric's C/C++ API

Most reNut mods are Lua ([rexfabric-api.md](rexfabric-api.md), [renut-api.md](renut-api.md)).
RexFabric can also load **native mods**: DLLs written in C or C++ against a small C interface,
`rexfabric/api.h`. This page covers both sides of RexFabric's native API:

- [Native (DLL) mods](#native-dll-mods): what they can do, how they load, how to build one.
- [For reNut developers: the host side](#for-renut-developers-the-host-side): the C++ that reNut
  itself uses to give mods its `renut` table, its events and its hook points.

## Contents

- [Native (DLL) mods](#native-dll-mods)
  - [Are they supported in reNut?](#are-they-supported-in-renut)
  - [How they load](#how-they-load)
  - [Building one](#building-one)
  - [A minimal mod](#a-minimal-mod)
  - [The API table](#the-api-table)
  - [Values crossing the API](#values-crossing-the-api)
  - [Threads](#threads)
  - [What a native mod can't do](#what-a-native-mod-cant-do)
- [For reNut developers: the host side](#for-renut-developers-the-host-side)
  - [Starting RexFabric](#starting-rexfabric)
  - [Declaring the game's API: `rexfabric::Namespace`](#declaring-the-games-api-rexfabricnamespace)
  - [Events](#events)
  - [Keeping mods' hooks on functions reNut hooks: `project_hooks.h`](#keeping-mods-hooks-on-functions-renut-hooks-project_hooksh)
  - [Hooks from the game's own C++: `HookGuest`](#hooks-from-the-games-own-c-hookguest)
  - [Switches and midasm sites](#switches-and-midasm-sites)
  - [Input for `rex.input`](#input-for-rexinput)
  - [Other host calls](#other-host-calls)

---

## Native (DLL) mods

### Are they supported in reNut?

Yes. reNut leaves RexFabric's native loader on, so an enabled mod's native files load at start.
What they can reach is smaller than Lua's, though: no on-screen drawing, no way to add Lua
functions, and reNut's own C++ hooks win over theirs the same way they win over Lua's (see
[below](#what-a-native-mod-cant-do)). Use one when you need native speed in a hook, or code that
already exists in C/C++; for everything else Lua is easier.

### How they load

- **Files.** Every `.asi` and `.dll` (`.asi` and `.so` on Linux) directly in an enabled mod's
  folder, or in its `plugins` subfolder, sorted by path. **Every** such file is loaded, so don't
  put helper DLLs there that aren't mods.
- **When.** As RexFabric starts: after the game's memory exists, before the game runs, and before
  any Lua script. Mods load in load order (`mods.json`, see [mods-and-scripts.md](mods-and-scripts.md)).
- **Init.** RexFabric calls the exported `RexFabricModInit(api, mod)`.
  - Return nonzero to stay loaded.
  - Return `0` to decline: everything the mod attached during init is removed, the library is
    unloaded and the log says `[<mod>/<file>] declined to load`.
  - A library without `RexFabricModInit` still loads (`loaded (no RexFabricModInit, so it sets
    itself up)`), so a plain `.asi` can do its own setup in `DllMain`.
- **Shutdown.** When reNut closes, RexFabric removes each native mod's hooks, listeners and claims,
  then calls its exported `RexFabricModShutdown()` if it has one, last loaded first. Libraries are
  left mapped (a game thread may still be returning through a hook).
- **No reload.** Reload Scripts only reruns Lua; native mods load once per run.
- **Its name** in the log is `<mod folder>/<file name>`, e.g. `[my_mod/my_mod.asi] loaded`.
- **Dependencies.** Windows looks for DLLs your mod links to beside `renut.exe`, not in your mod
  folder: link the C runtime statically or avoid extra DLLs.

### Building one

A native mod needs nothing but RexFabric's `include` folder (`thirdparty/RexFabric/include` in the
reNut source), and only `rexfabric/api.h` from it, which is plain C. Build a 64-bit shared library
that exports `RexFabricModInit`. RexFabric ships a CMake template in
`thirdparty/RexFabric/examples/native_template`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_mod LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(REXFABRIC_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../include" CACHE PATH "RexFabric's include folder")

add_library(my_mod MODULE my_mod.cpp)
target_include_directories(my_mod PRIVATE ${REXFABRIC_INCLUDE})
set_target_properties(my_mod PROPERTIES PREFIX "" SUFFIX ".asi")
```

```
cmake -B build -DREXFABRIC_INCLUDE=<reNut source>/thirdparty/RexFabric/include
cmake --build build
```

Copy `my_mod.asi` into `mods/<YourMod>/`, next to a `mod.toml`, and enable the mod.

**Compatibility.** `RexFabricApi` only grows: new members go at the end and raise
`REXFABRIC_API_VERSION` (now `1`). Check `api->version` (and `api->size`) before using a member
added after the version you built against; a mod built for an older version keeps working.

### A minimal mod

It logs every change to a game counter (reNut runs mods' hooks on `counterAdd`, see
[rexfabric-api.md](rexfabric-api.md#renuts-own-hooks-win)) and every world the player enters.

```c
#include <stdio.h>
#include <rexfabric/api.h>

static const RexFabricApi* api;
static RexFabricMod* self;

/* counterAdd(profile, counter, amount, silent). */
#define COUNTER_ADD 0x824AC1C0u

static void on_counter_add(RexFabricCall* call, void* user) {
  (void)user;
  unsigned counter = (unsigned)api->get_gpr(call, 4);
  int amount = (int)(int32_t)api->get_gpr(call, 5);
  api->call_next(call);  /* run the rest of the chain and the game's function */
  char line[96];
  snprintf(line, sizeof line, "counter %u changed by %d", counter, amount);
  api->log(self, REXFABRIC_LOG_INFO, line);
}

/* renut.world_enter fires with (number, scene, name). */
static void on_world_enter(const RexFabricValue* args, uint32_t count, void* user) {
  (void)user;
  if (count < 3 || args[2].type != REXFABRIC_VALUE_STRING) return;
  char line[128];
  snprintf(line, sizeof line, "entered %s (world %lld)", args[2].s, (long long)args[0].i);
  api->log(self, REXFABRIC_LOG_INFO, line);
}

REXFABRIC_MOD_EXPORT int RexFabricModInit(const RexFabricApi* host, RexFabricMod* mod) {
  if (host->version < 1) return 0;
  api = host;
  self = mod;
  if (!api->hook(self, COUNTER_ADD, on_counter_add, NULL)) {
    api->log(self, REXFABRIC_LOG_ERROR, "counterAdd isn't hookable");
    return 0;
  }
  api->listen(self, "renut.world_enter", on_world_enter, NULL);
  return 1;
}
```

In a `.cpp` file, put `extern "C"` before `REXFABRIC_MOD_EXPORT`.

### The API table

`RexFabricModInit` gets a `const RexFabricApi*`. Every function takes and returns plain C types.
Ids are `uint64_t`; `0` means none or failure. "Bool" results are `int`, nonzero for true.

**Logging**

| Member | What |
|---|---|
| `log(mod, level, message)` | A line in reNut's log, tagged with the mod: `REXFABRIC_LOG_DEBUG`, `_INFO`, `_WARN`, `_ERROR`. |

**Hooks** (on guest functions, by address)

| Member | What |
|---|---|
| `is_hookable(address)` | Whether a recompiled function starts there. |
| `hook(mod, address, fn, user)` | Adds `fn(call, user)` to the function's hook chain; returns a hook id, `0` (and a log warning) when the address isn't hookable. Hooks on one function run in the order they were added. |
| `unhook(id)` | Removes a hook. |
| `call_next(call)` | Inside a hook: runs the rest of the chain and then the game's function. **Don't call it to replace the function**; set `r3` yourself then. |
| `call_address(call)` | The hooked function's address. |
| `get_gpr(call, n)`, `set_gpr(call, n, v)` | Integer register `rN` (0 to 31), all 64 bits. Arguments are in `r3`..`r10`, the result in `r3`. |
| `get_fpr(call, n)`, `set_fpr(call, n, v)` | Float register `fN` as a double. Float arguments in `f1`..`f13`, the result in `f1`. |
| `call_guest(call, address, args, count)` | Calls the guest function at `address` with up to 8 integer arguments and returns its `r3`. Only inside a hook (it runs on the hooked call's thread and stack); the hooked call's registers are put back after. Logs a warning and returns `0` for an address with no function. |

The `call` pointer is valid until your hook returns. As with Lua: a hook on a function reNut
replaces itself is accepted but never runs, except on the functions listed in
[rexfabric-api.md](rexfabric-api.md#renuts-own-hooks-win). Native mods load before Lua, so on a
function both hook, the native hook runs around the Lua hooks.

**Guest memory** (guest virtual addresses; reads and writes convert from and to big-endian)

| Member | What |
|---|---|
| `guest_pointer(address)` | The host pointer for a guest address. The data there is big-endian. |
| `read_u8/u16/u32/u64/f32/f64(address)` | Reads. |
| `write_u8/u16/u32/u64/f32/f64(address, value)` | Writes. |

Nothing checks addresses: a bad one crashes reNut.

**The game's API** (what reNut declares, by full name)

| Member | What |
|---|---|
| `find_function(name)` | A declared function, such as `"renut.game._world"`, or null. |
| `call_function(fn, args, count, result)` | Calls it. Arguments are converted to what it declares (an integer passes for a float or an object, a bool for an int). Returns `0` when `count` or an argument doesn't fit, or when the function throws (logged). `result` may be null; a string result stays valid until this thread's next call. |
| `find_type(name)` | A declared type (`"renut.Part"`, `"renut.Vehicle"`, `"renut.Character"`), or null. |
| `type_size(type)` | Its size in bytes. |
| `type_field(type, name, &field)` | A declared field's offset, kind, array count and stride, length and target type. `0` when there's no such field. |
| `type_method(type, name)` | A declared method; call it with `call_function`, the object first. |
| `find_interface(name, min_version)` | A C table the game published for native mods. **reNut publishes none**, so this returns null. |

Only functions reNut declares **in C++** are found. Much of the `renut` table is Lua written on top
of those (the documented `renut.parts`, `renut.vehicle`, ...), and those Lua functions aren't
reachable from C. The C++ ones mostly have underscored names (`renut.parts._define`); they're
reNut's internal layer and may change between versions. Many of them only make sense on the game
thread, so call them from a hook or an event.

```c
const RexFabricFunction* name_of = api->find_function("renut.game._world_name");
RexFabricValue arg = {0};
arg.type = REXFABRIC_VALUE_INT;
arg.i = 3;
RexFabricValue result = {0};
if (name_of && api->call_function(name_of, &arg, 1, &result) && result.type == REXFABRIC_VALUE_STRING)
  api->log(self, REXFABRIC_LOG_INFO, result.s);
```

**Events**

| Member | What |
|---|---|
| `listen(mod, event, fn, user)` | Calls `fn(args, count, user)` each time `event` fires. `"frame"` (one FLOAT: seconds since the last frame) and the game's events, `"renut.<name>"` (their arguments are in [renut-api.md](renut-api.md)). Returns a listener id. |
| `unlisten(id)` | Removes a listener. |

Arguments are valid during the call. Listeners run in the order they were added.

**Midasm sites**

| Member | What |
|---|---|
| `claim_midasm(mod, address, fn, user)` | Claims a compiled-in midasm site; `fn(site, user)` runs when the game reaches it, and returning nonzero asks for its patch. Every claim on a site runs, in claim order. |
| `release_midasm(id)` | Releases a claim. |
| `midasm_address(site)` | The site's address. |
| `midasm_register(site, "r3")` | A register the site lends, as a `RexFabricRegister*` (`u64`, `u32`, `s32`, `f64`), or null. |

reNut compiles in no claimable sites yet, so claims never run.

**Switches and cvars**

| Member | What |
|---|---|
| `get_switch(name)`, `set_switch(name, value)` | Named `int32_t` flags, `0` until set. Shared with Lua's `rex.switch`. reNut's own code reads none. |
| `get_cvar(name, buffer, size)` | A setting's value as text, NUL-terminated, at most `size` bytes; returns the full length, or `-1` when there's no such setting. Names as in [Lua](rexfabric-api.md#settings-rexcvar). |
| `set_cvar(name, value)` | Sets a setting from text. Nonzero on success. |

### Values crossing the API

`RexFabricValue` carries one value of the game's API:

| `type` | Field used |
|---|---|
| `REXFABRIC_VALUE_NONE` | nothing |
| `REXFABRIC_VALUE_BOOL` | `i` (0 or 1) |
| `REXFABRIC_VALUE_INT` | `i` |
| `REXFABRIC_VALUE_FLOAT` | `f` |
| `REXFABRIC_VALUE_STRING` | `s` (UTF-8, NUL-terminated) |
| `REXFABRIC_VALUE_OBJECT` | `address`, and `object_type` (null when untyped) |

Field kinds (`REXFABRIC_FIELD_*`) describe how a type's fields sit in guest memory: integers and
floats of each size, `BOOL8`/`BOOL32`, `POINTER` (a 32-bit pointer to `target`), `STRUCT` (a
`target` inline), `CSTRING` (a pointer to a string) and `CHARS` (`length` characters inline).

### Threads

- A hook runs on the game thread making the call. Native hooks take **no lock**: the same hook can
  run on several threads at once, so guard shared state yourself.
- Lua's lock doesn't cover native code; a native hook can run while Lua runs on another thread.
- Event listeners run on the thread that fires the event. In reNut, `frame` and the `renut` events
  fire on the game's update thread.

### What a native mod can't do

- **Add functions to Lua.** The C API has no way to declare functions, types or events; only the
  host (reNut) can, in C++. To talk to a Lua mod, share a cvar (`get_cvar`/`set_cvar` and
  `rex.cvar`) or a switch (`get_switch`/`set_switch` and `rex.switch`).
- **Fire events.** Natives can listen, not fire.
- **Draw on screen.** There's no drawing in the C API; Lua's `rex.draw` / `rex.screen_text` can.
- **Call guest functions outside a hook.** `call_guest` needs a hooked call.
- **Hook functions reNut replaces**, except the ones reNut keeps hookable.

---

## For reNut developers: the host side

This part is for working on reNut itself (or another game built on RexFabric). Mods never see it.
It's the C++ in `rexfabric/rexfabric.h`, `game_api.h`, `project_hooks.h`, `guest_hooks.h`,
`switches.h`, `midasm.h` and `host_input.h`, compiled into `renut.exe`.

### Starting RexFabric

reNut attaches RexFabric in its `CMakeLists.txt` with
`rexfabric_attach(renut WITH_GUIDE WITH_NODE_EDITOR)` (the mods menu in the Guide, and the node
editor), and drives it from `src/renut_app.h`:

| Call | Where in reNut | What |
|---|---|---|
| `rexfabric::Configure(settings)` | `OnConfigure` | Adds RexFabric's log category and old key names. |
| `rexfabric::Setup({.mods_dir = ..., .menu_key = "F9", .node_editor_key = "F10"})` | `OnInit` | The mods folder (reNut's `renut_mods_folder`), the keys (rebindable as `ui.bind.mods` and `ui.bind.node_editor`). No Thunderstore community, so no mod browser. No `frame_function`: reNut fires the frame event itself. |
| the `Declare()` functions, then `rexfabric::CreateMenus(ui, images)` | `OnInit` | Declares the `renut` namespace (below) and makes the menus and the overlay. Declarations must come before `Start`. |
| `rexfabric::Start()` | `OnTitleLaunch` | Loads the enabled mods: native ones, then Lua. Guest memory exists, the guest hasn't run. |
| `rexfabric::Tick()` | reNut's hook on `rex_appMainDraw` | Fires the `frame` event after each drawn frame, on the game thread. |
| `rexfabric::Stop()` | window closing, and `OnShutdown` | Stops Lua and native mods and removes every hook. Runs once. |

### Declaring the game's API: `rexfabric::Namespace`

A game gives its mods functions, types, constants, enums and events by declaring them once in
C++. Lua then sees them as tables and objects ([rexfabric-api.md](rexfabric-api.md#game-namespaces-as-lua)),
native mods look them up by name, and the node editor makes nodes from them. reNut does all of this
in one namespace, `renut`, from many files (`part_api.cpp`, `custom_worlds.cpp`, `custom_npcs.cpp`,
`menu_api.cpp`, `sound_api.cpp`, `mod_data.cpp`, `net_api.cpp`, `renut_lua.cpp`, ...): every
`rexfabric::Namespace renut("renut")` refers to the same namespace.

```cpp
#include <rexfabric/game_api.h>

void Declare() {
  rexfabric::Namespace renut("renut");

  // Functions: any callable taking and returning bool, integers, floats, std::string,
  // std::string_view, const char* or rexfabric::Object. Names are relative to the namespace.
  renut.Function("game._world", []() -> int64_t { return CurrentWorld(); });
  renut.Function("game._world_name", [](int64_t number) { return WorldName(uint32_t(number)); });

  renut.Constant("max_players", int64_t(4));
  renut.Enum("Team", {{"Red", 0}, {"Blue", 1}});

  // A type: its fields read and write guest memory from Lua (obj.field), methods are obj:name().
  rexfabric::TypeDef& part = renut.Type("Part", 0x400);
  part.Field("health", 0x1F0, rexfabric::kF32);
  part.Method("aid", [](rexfabric::Object self) -> int64_t { return PartAid(self.address); });

  // Lua run once the declarations are globals, before any mod: friendlier wrappers.
  renut.Lua(R"LUA( function renut.world_name() return renut.game._world_name(renut.game._world()) end )LUA");
}
```

| Builder | What |
|---|---|
| `Function(name, callable)` | A function mods call. It runs on the caller's thread: a game thread in a hook or event. Throwing `std::exception` becomes a Lua error naming the function. |
| `Constant(name, value)` | A value (`rexfabric::Value`: bool, int64, double, string or Object). |
| `Enum(name, {{label, value}, ...})` | A table of named integers. Node editor inputs typed with it pick from its labels. |
| `Type(name, size)` | A guest struct; asking again for the same name returns the same `TypeDef`. |
| `TypeDef::Field(name, offset, type)` | A field: `kBool8`, `kBool32`, `kS8`...`kU64`, `kF32`, `kF64`, `kCString`, `Pointer(type)`, `Struct(type)`, `Chars(length)`. |
| `TypeDef::Array(name, offset, type, count, stride)` | `count` elements, `stride` bytes apart (`0`: packed). Lua indexes them from 1. |
| `TypeDef::Method(name, callable)` | A function whose first argument is the object. |
| `Event(name)` | Declares an event and returns a `rexfabric::Event` to fire it (below). |
| `Lua(source)` | Lua run after every namespace's declarations, before any mod. reNut's documented `renut.parts`, `renut.vehicle`, ... are written this way. |
| `Category(prefix, title)` | The node editor's add menu lists declarations whose names start with `prefix` under `title` (`Category("parts.", "Parts")`). The longest prefix wins; the rest go under `Game/renut`. |
| `Args(name, {"part:Part", "amount", "->Vehicle"})` | Names a function's or an event's pins for the node editor, in order. `name:Type` makes an integer argument an object pin of that type (`Type?`: may be left unlinked, passing 0); `->Type` types the result; an argument typed with an enum or a `Choices` list is picked from it. An event with `Args` gets exactly that many outputs (otherwise three Any outputs). |
| `Choices(name, {{label, value}, ...})` / `Choices(name, function)` | A list a typed argument (`"flag:crates.Flag"`) is picked from in the node editor; the function form is worked out each time the list is shown. |

The namespace `rex` is RexFabric's own and refused. Strings passed in from Lua keep their full
length (zero bytes included).

`PublishInterface(name, version, table)` lets a game hand native mods a C table of its own,
found with `find_interface`. reNut publishes none.

### Events

```cpp
static std::optional<rexfabric::Event> world_enter;
world_enter = renut.Event("world_enter");          // full name "renut.world_enter"
...
world_enter->Fire(int64_t(number), int64_t(scene), std::string(name));
```

`Fire` passes its arguments as `Value`s to every listener, Lua and native, on the firing thread,
in the order they started listening. With nobody listening it costs one load. Lua listeners run
under the VM lock. reNut fires its events through a helper in `part_api.cpp` (`Fire`) that also
marks "inside a renut event" for its own API.

### Keeping mods' hooks on functions reNut hooks: `project_hooks.h`

Every recompiled function checks its RexFabric hook slot on entry, which is how `rex.hook` works
without patching anything. A function reNut defines itself with `REX_HOOK` / `REX_HOOK_RAW`
replaces the generated one, slot check and all, so mods' hooks on it never run. Two macros keep
them running:

```cpp
#include <rexfabric/project_hooks.h>

// Mods' hooks run around the game's own function, inside reNut's hook.
REXFABRIC_HOOKABLE(sub_821BD590);
REX_HOOK_RAW(sub_821BD590) {
  const uint32_t man = ctx.r3.u32;
  REXFABRIC_ORIGINAL(sub_821BD590)(ctx, base);   // instead of __imp__sub_821BD590(ctx, base)
  AfterTick(man);
}

// Mods' hooks run around all of reNut's hook: a before-hook returning false skips it too.
REXFABRIC_HOOK_RAW(sub_824AC1C0) {
  ...
  __imp__sub_824AC1C0(ctx, base);
  ...
}
```

`REXFABRIC_HOOKABLE` goes once at file scope before `REXFABRIC_ORIGINAL`. reNut uses these on
`0x821BD590`, `0x824FDC38`, `0x82503568`, `0x8236A570`, `0x824E8838` (`ORIGINAL`) and
`0x824AC1C0` (`HOOK_RAW`); the list for modders is in
[rexfabric-api.md](rexfabric-api.md#renuts-own-hooks-win). Add a function there whenever mods
should keep hooking something reNut takes over.

### Hooks from the game's own C++: `HookGuest`

`rexfabric/guest_hooks.h` lets the game's C++ join a function's hook chain at run time, with the
registers at hand, instead of replacing it at build time:

```cpp
rexfabric::HookGuest(0x821F0B18, [](rexfabric::GuestCall& call) {
  BeforeView(call.ctx(), call.base());
  call.Next();                       // the rest of the chain and the function
  AfterView(call.ctx(), call.base());
});
```

It returns a hook id (`0` when the address isn't hookable); `UnhookGuest(id)` removes it. A hook
added after the mods loaded runs after their hooks on that function. reNut doesn't use it today.

### Switches and midasm sites

For patches that can only be compiled in, a mod decides whether they apply:

```cpp
// A switch, read where the patch is (one relaxed load).
bool Space1_hook() {
  static const rexfabric::Switch unlocked("UnlimitedGardenSpace");
  return unlocked.on();
}

// A midasm site mods can claim, from a [[midasm_hook]] entry naming this stub.
void Space3_hook(PPCRegister& r3) { rexfabric::Midasm<0x823DA320>({{"r3", &r3}}); }
bool Space1_branch() { return rexfabric::Midasm<0x824DC3AC>(); }
```

Switches start at `0`, meaning "behave like the unmodded game". An unclaimed site answers `false`
and writes nothing. reNut has neither yet: its `[[midasm_hook]]` entries in
`config/renut_hooks.toml` are its own patches, not sites for mods.

### Input for `rex.input`

How a game samples the keyboard and mouse is its own business, so it installs a sampler and
`rex.input` reads through it. reNut's is `InstallModInput()` in
`src/renut_engine/game/keyboard_input.cpp`:

| `HostInput` member | reNut's |
|---|---|
| `resolve_key(name)` | The SDK input system's key-name parser (the same as the `renut_key_*` binds). |
| `key_down(code)` | The window's key state, kept from its key events. Mouse buttons as keys only while the mouse camera has captured the mouse. |
| `mouse_button_down(n)` | The five mouse buttons, captured or not. |
| `consume_mouse_delta()` | Mouse motion in physical pixels since the last call. |
| `consume_wheel()` | The overlay's wheel since the last call, 120 a notch. |
| `accepts_input()` | No reNut overlay window wants the cursor, and the Guide is closed. |
| `set_guest_input_held(held)` | Player 1's pad entries reach the game empty while held. |
| `flush()` | Drops mouse motion and wheel. |

Until a game installs one, `rex.input` reports no input.

### Other host calls

| Call | What |
|---|---|
| `InstalledMods()` | The mods folder as it is now (folder, name, version, author, enabled), enabled ones in load order. reNut gives it to crash reports. |
| `EnabledModDirs()`, `ModsDir()`, `ModsRevision()` | For loading mod content (bundles, parts, sounds): the enabled mods' folders in load order, and a counter that goes up when mods are turned on or off, moved or renamed. |
| `SetModEnabled(folder, on)`, `RenameMod(from, to)`, `SeedLoadOrder(list)` | Change the load order as the menu does. |
| `ModSettings()` | Every setting Lua mods added (`mods.<name>`, category, mod). reNut's settings window (F4) shows the ones whose category starts with `Nuts&Bolts/`. |
| `ShowMenu`, `ToggleMenu`, `MenuShown`, `ShowNodeEditor`, `NodeEditorShown` | The mods menu and node editor windows. |
| `Api()`, `Host()` | The native API table and the host's own mod handle, for the game's own use. |
