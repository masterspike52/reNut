# The game's modding API

Every game has its own things worth giving mods: a garden, a player, an inventory. RexFabric knows nothing about any of them. The game declares them once, in C++, through `include/rexfabric/game_api.h`, and every kind of mod gets them in its own language: Lua as tables and objects, native mods by name through `RexFabricApi`, and later C# the same way.

Declare everything before `rexfabric::Start`, from one thread. `OnPostSetup`, just before `Start`, is a good place.

## Namespaces

```cpp
rexfabric::Namespace game("tip");
```

Everything the game declares lives in a namespace, which is a global table in Lua. Names inside it may be dotted, which nests them: `"garden.money"` is `tip.garden.money`. The name `rex` belongs to RexFabric.

## Functions

```cpp
game.Function("version", [] { return std::string("1.2"); });
game.Function("garden.money", [] { return ReadMoney(); });
game.Function("garden.give", [](int64_t amount, bool notify) { Give(amount, notify); });
```

Any callable works. Its argument and return types decide how mods call it:

| C++ | Lua | Native |
|---|---|---|
| `bool` | boolean | `REXFABRIC_VALUE_BOOL`, in `i` |
| any integer | integer | `REXFABRIC_VALUE_INT`, in `i` |
| `float`, `double` | number | `REXFABRIC_VALUE_FLOAT`, in `f` |
| `std::string`, `std::string_view`, `const char*` | string | `REXFABRIC_VALUE_STRING`, in `s` |
| `rexfabric::Object` | an object of the game's types, or nil | `REXFABRIC_VALUE_OBJECT`, in `address` and `object_type` |
| `void` (result only) | nothing | `REXFABRIC_VALUE_NONE` |

Functions run on the thread the mod calls from: a guest thread inside a hook, or whichever thread fired an event. If the game code they touch cares, the function has to handle that. An exception thrown from one becomes a Lua error, or a failed `call_function` for a native mod.

## Constants and enums

```cpp
game.Constant("max_pinatas", int64_t(64));
game.Enum("Item", {{"Apple", 1}, {"Turnip", 2}});
```

In Lua these are `tip.max_pinatas` and `tip.Item.Apple`.

## Types

A type describes a guest struct by its fields, so mods read and write it by name instead of by offset:

```cpp
rexfabric::TypeDef& garden = game.Type("Garden", 0x8000);
rexfabric::TypeDef& pinata = game.Type("Pinata", 0x3A0);
pinata.Field("species", 0x10, rexfabric::kU32)
      .Field("happiness", 0x1F0, rexfabric::kF32)
      .Field("garden", 0x20, rexfabric::Pointer(garden))
      .Field("name", 0x40, rexfabric::Chars(32))
      .Array("needs", 0x100, rexfabric::kU8, 16)
      .Method("feed", [](rexfabric::Object self, int64_t item) { Feed(self.address, item); });
```

| Field type | Is |
|---|---|
| `kBool8`, `kBool32` | a boolean in one or four bytes |
| `kS8` to `kU64` | integers |
| `kF32`, `kF64` | floats |
| `Pointer(type)` | a 32 bit pointer to another type; reads as that object, or nil when null |
| `Struct(type)` | another type held inline |
| `kCString` | a 32 bit pointer to a string; read only |
| `Chars(n)` | n characters held inline, NUL padded |

`Array(name, offset, type, count, stride)` is `count` of a field type, `stride` bytes apart (0: packed). A method takes the object as its first argument. Values are big endian in guest memory, and RexFabric converts them.

In Lua, `tip.Pinata(address)` makes an object; fields read and write memory, methods are called with `:`, arrays index from 1, and `.address` and `.type` are always there. Native mods use `find_type`, `type_field` and `type_method`.

### Objects that come and go

A mod may hold an object after the game destroyed it, and its address then points at freed memory. A type whose objects come and go is held by handle instead:

```cpp
pinata.Held({.hold = [](uint32_t address) { return MakeHandle(address); },  // 0: no such object there
             .resolve = [](uint32_t handle) { return FindObject(handle); }});  // 0: gone
```

`hold` gives the handle of the object at an address, the same handle for the same object every time; `resolve` finds its address again. Mods then only ever hold handles. Each field read or write, and each method call, resolves first, so a gone object is an error (`tip.Pinata is gone`) rather than a read of freed memory, and `.valid` says whether it is still there. Inline structs and array elements of a held object keep its handle too. `tip.Pinata(address)` holds the address, so it gives nil where no such object is.

Either function may throw to refuse, with the reason as its message: a game that only allows its objects on its own thread, say. Lua gets the reason as an error.

For a held type, `Object::address` is the handle: the game hands mods `Object{type.Hold(address), &type}` and gets the address of what it is given with `type.Resolve(object.address)`. Native mods keep seeing addresses; RexFabric resolves held objects on their way out to one and holds addresses on their way back.

## Events

```cpp
static const rexfabric::Event loaded = game.Event("garden_loaded");
loaded.Fire(rexfabric::Object{address, &garden});
```

Mods listen by the full name, `tip.garden_loaded`. Firing an event nobody listens to costs one load, so it's fine on hot paths. Listeners run on the thread that fires.

RexFabric fires `frame` itself, with the seconds since the last frame, from the guest function named by `Config::frame_function`, or from `rexfabric::Tick`.

## Function names

A game can name its guest functions, so mods hook and call them by name rather than by an address that means nothing to
a reader:

```cpp
rexfabric::NameGuestFunction({.name = "garden.tick", .address = 0x8227A0C0, .signature = "void (*)(Garden *)", .source = "garden.cpp"});
```

`rex.hook`, `rex.hook_after`, `rex.hookable` and `rex.call` then take `"garden.tick"` wherever they take an address,
`rex.hooks()` gives each hook's `name`, and native mods look addresses up with `find_guest_function`. Name before
`Start`; a name or an address named twice is refused with a warning. The signature and source are only for the
reference and the editor definitions.

## Definitions for editors

At every start, before the Lua mods run, RexFabric writes Lua definitions into `<mods>/.api` for LuaLS (the Lua
extension of VS Code and other editors): `rex.lua` for RexFabric's own API, a file per namespace from its declarations,
`functions.lua` with every function name as `rex.FunctionName`, and `reference.md` with the same for reading. It also
writes `<mods>/.luarc.json` pointing at them when there is none. Opened on the mods folder, an editor completes and
checks `rex.*`, the game's namespaces and function names. The folder starts with a dot, so the mod list skips it.

## Native interfaces

For native mods that want speed, a game can publish a plain C table of its own and ship a header for it:

```cpp
static const TipModApi tip_api = {...};
rexfabric::PublishInterface("tip", 1, &tip_api);
```

A mod gets it with `api->find_interface("tip", 1)`. Versions only grow; a mod asks for the oldest version it can use.

## Midasm sites

Some patches sit partway through a function, where no hook reaches. The game declares the site in its codegen config, which makes codegen call a stub there, and builds the stub on `rexfabric/midasm.h`, so whether the patch applies is up to the mods:

```toml
[[midasm_hook]]
address = 0x824DC3AC
name = "Space1_hook"
jump_address_on_true = 0x824DC830

[[midasm_hook]]
address = 0x823DA320
name = "Space3_hook"
registers = ["r3"]
after_instruction = true
```

```cpp
bool Space1_hook() { return rexfabric::Midasm<0x824DC3AC>(); }
void Space3_hook(PPCRegister& r3) { rexfabric::Midasm<0x823DA320>({{"r3", &r3}}); }
```

Unclaimed, a site returns false and changes nothing, for the cost of one load. For a site run so often that calling into a mod is too slow, the stub can read a `rexfabric::Switch` instead, which a mod sets.

## Input

`rexfabric::SetHostInput` gives Lua's `rex.input` the game's keyboard and mouse; see `include/rexfabric/host_input.h`.
