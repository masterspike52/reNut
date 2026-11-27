# Writing a native mod

A native mod is a shared library built against `include/rexfabric/api.h` alone. It needs nothing from the game, the SDK or RexFabric's sources, so it keeps working across game and SDK updates for as long as the API version it asks for exists.

`examples/native_template` is a ready project to start from.

## Layout

Put the built library in a folder of its own under `mods` beside the game:

```
mods/
  MyMod/
    my_mod.asi
```

The library can also go in `mods/MyMod/plugins/`. Folders starting with `.` are ignored. Mods load in the load order the mods menu keeps (see [setup.md](setup.md)), and within a folder in file name order.

## Entry points

```cpp
extern "C" REXFABRIC_MOD_EXPORT int RexFabricModInit(const RexFabricApi* api, RexFabricMod* mod);
extern "C" REXFABRIC_MOD_EXPORT void RexFabricModShutdown();  // optional
```

`RexFabricModInit` runs once, after guest memory exists and before the game starts. Keep `api` and `mod`: every later call takes them. Return nonzero to stay loaded; returning zero removes any hooks, listeners and claims the mod added and unloads it.

Check `api->version` against the version you need. A newer host always has every member an older one had.

## Hooks

```cpp
RexFabricHookId id = api->hook(mod, 0x8266FBB0, OnSwap, user_data);
```

`address` is the guest address of a function's first instruction, as a disassembler shows it. `hook` returns 0 if the address isn't a recompiled function; `is_hookable` asks first. `unhook` removes one hook.

The hook runs on the guest thread making the call, every time:

```cpp
void OnSwap(RexFabricCall* call, void* user) {
  uint32_t device = (uint32_t)api->get_gpr(call, 3);  // arguments, before the call
  api->call_next(call);                               // the original function
  uint64_t result = api->get_gpr(call, 3);            // the result, after
}
```

Registers follow the PowerPC calling convention:

| Registers | Holds |
|---|---|
| r3 to r10 | integer and pointer arguments |
| r3 | integer result |
| f1 to f13 | float arguments |
| f1 | float result |
| r1 | stack pointer |

Change an argument with `set_gpr` before `call_next`, or the result after it. Leave `call_next` out to replace the function; then set r3 or f1 to what the caller should get.

Hooks run often. Keep them short, and don't block in them.

## Calling the game

Inside a hook, `call_guest(call, address, args, count)` calls another guest function with up to eight integer arguments and returns its r3. It works on a copy of the hooked call's registers, so the hooked call is unaffected.

## Guest memory

Guest addresses are 32 bit. `read_u32`, `write_f32` and the rest convert from and to the guest's big endian byte order. `guest_pointer` gives the host address for direct access, where the bytes are big endian.

## The game's API

A game can declare functions, types, constants and events for its mods (see [game_api.md](game_api.md)). Native mods find them by full name:

```cpp
const RexFabricFunction* money = api->find_function("tip.garden.money");
RexFabricValue result;
if (money && api->call_function(money, nullptr, 0, &result)) { /* result.i */ }

RexFabricField happiness;
const RexFabricType* pinata = api->find_type("tip.Pinata");
if (pinata && api->type_field(pinata, "happiness", &happiness)) {
  float value = api->read_f32(address + happiness.offset);
}
```

Arguments and results are `RexFabricValue`s: set `type` and the member it names (`i` for booleans and integers, `f`, `s`, or `address` and `object_type` for objects). An integer passes where a float or an object is expected. A string result stays valid until the same thread's next call.

A game can also publish a plain C table of its own for native mods, which is faster than calling by name: `api->find_interface("tip", 1)` returns it, or null when the game has no such table at that version or later.

## Events

```cpp
api->listen(mod, "frame", OnFrame, nullptr);          // every frame, args[0].f is the seconds since the last
api->listen(mod, "tip.garden_loaded", OnLoaded, nullptr);
```

`unlisten` stops one. Listeners run on the thread that fires the event.

## Midasm sites

A game can compile a stub into the middle of a function (see [game_api.md](game_api.md#midasm-sites)). A mod claims it by address:

```cpp
int OnSpace(RexFabricMidasm* site, void* user) {
  RexFabricRegister* r3 = api->midasm_register(site, "r3");  // null if the stub didn't lend r3
  if (r3) r3->u64 = 1;
  return 0;  // nonzero takes the branch, at a branch site
}
api->claim_midasm(mod, 0x823DA320, OnSpace, nullptr);
```

Every claim on a site runs, in claim order. `release_midasm` drops one.

## Switches and cvars

`set_switch` and `get_switch` reach the switches a game's compiled-in patches read. `get_cvar` and `set_cvar` read and change cvars as text.

## Logging

`api->log(mod, REXFABRIC_LOG_INFO, "text")` writes to the game's log under the `rexfabric` category, prefixed with the mod's folder and file name.

## Versioning

| Version | Added |
|---|---|
| 1 | Logging, hooks, registers, guest calls, guest memory, the game's API, events, midasm, switches, cvars |

Version 1 isn't released yet, so it can still change; from the first release on, the table only grows.
