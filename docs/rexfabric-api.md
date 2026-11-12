# RexFabric Lua API: the `rex` table

RexFabric is the mod framework under reNut. It finds the mods, runs their Lua, hooks the game's
functions and draws the mods menu, the node editor and the on-screen overlay. `rex` is its Lua
table: everything a mod needs that isn't specific to Banjo-Kazooie: Nuts & Bolts. The game's own
table, `renut` (parts, vehicles, worlds, characters, menus, sound, data, online), is documented in
[renut-api.md](renut-api.md).

New to mods? Start with [getting-started.md](getting-started.md). How a mod folder, `mod.toml`,
`mods.json`, `require`, errors and **Reload Scripts** work is in
[mods-and-scripts.md](mods-and-scripts.md). A walk-through of hooks and memory is in
[tutorials/hooks-and-memory.md](tutorials/hooks-and-memory.md); useful game addresses are in
[internals.md](internals.md).

## Contents

- [Conventions](#conventions)
- [The Lua environment](#the-lua-environment)
- [Threads and the VM lock](#threads-and-the-vm-lock)
- [Logging: `print` and `rex.log`](#logging-print-and-rexlog)
- [Time and version: `rex.now`, `rex.version`](#time-and-version-rexnow-rexversion)
- [Timers: `rex.after`, `rex.every`, `rex.cancel`](#timers-rexafter-rexevery-rexcancel)
- [Events: `rex.on`](#events-rexon)
- [Hooks: `rex.hook` and friends](#hooks-rexhook-and-friends)
- [The register context: `ctx`](#the-register-context-ctx)
- [Calling game functions: `rex.call`](#calling-game-functions-rexcall)
- [Game memory: `rex.mem`](#game-memory-rexmem)
- [Keyboard and mouse: `rex.input`](#keyboard-and-mouse-rexinput)
- [Settings: `rex.cvar`](#settings-rexcvar)
- [Text on screen: `rex.screen_text`](#text-on-screen-rexscreen_text)
- [Drawing: `rex.draw`](#drawing-rexdraw)
- [Switches: `rex.switch`](#switches-rexswitch)
- [Midasm sites: `rex.midasm`](#midasm-sites-rexmidasm)
- [Game namespaces as Lua](#game-namespaces-as-lua)
- [What `rex` doesn't have](#what-rex-doesnt-have)
- [Summary table](#summary-table)

## Conventions

- **Addresses** are the game's own, from Title Update 3's `default.xex`: integers such as
  `0x82207320`. A recompiled function `sub_82207320` starts at `0x82207320`. An address must be an
  integer from `0` to `0xFFFFFFFF`, or the call raises `guest address ... out of range`.
- **Integers.** Arguments documented as integers go through Lua's integer check: `2` and `2.0`
  are fine, `2.5` raises `number has no integer representation`.
- **Errors.** The errors listed below are ordinary Lua errors: they end the function that called
  the `rex` function unless you catch them with `pcall`.
- **"When"** in the tables: *any time* means a script's top level, a hook, an event listener, a
  timer, anywhere. *Guest thread* means inside a hook, a midasm handler, a `frame` listener or a
  `renut` event (in reNut all of those run on game threads).

## The Lua environment

- **Lua 5.4** (5.4.8), compiled as C++. Every standard library is open: `string`, `table`,
  `math`, `utf8`, `io`, `os`, `debug`, `package`, `coroutine`. Scripts are not sandboxed.
- **One VM for every mod.** All enabled mods' scripts run in one Lua state, so globals are shared.
  Keep your variables `local` or in one table named after your mod.
- **`print`** is replaced: it writes to reNut's log, like `rex.info` (below).
- **`require`** finds `.lua` files in your mod's `scripts` folder (`?.lua` and `?/init.lua`);
  native Lua modules are off (`package.cpath` is empty). The search path is set to each file's
  folder while that file loads, so call `require` at your script's top level. Details in
  [mods-and-scripts.md](mods-and-scripts.md).
- **Globals RexFabric adds:** `rex`, and one global table per game namespace. In reNut that's
  `renut` ([renut-api.md](renut-api.md)). Its Lua conveniences run before any mod's script.

## Threads and the VM lock

The game runs on several threads, and hooks fire on whichever thread calls the hooked function.
RexFabric protects the single Lua state with **one lock** (a recursive mutex):

- Every way into Lua takes the lock first: hooks (before and after), event listeners (`rex.on`),
  midasm handlers, cvar watchers, timers (they run from the `frame` event), and Reload Scripts.
  So **no two Lua callbacks ever run at the same time**, and you never need locks in Lua.
- A hook's Lua runs under the lock, but **the hooked game function runs outside it**: a
  `rex.hook` callback runs, the lock is released, the game's function runs, then the lock is taken
  again for the `rex.hook_after` callbacks.
- While one thread runs Lua, any other thread that reaches a Lua hook **waits**. Keep callbacks
  short, especially on functions called often.
- The lock is recursive: a callback that calls `rex.call` can trigger more hooks on the same thread,
  and those run normally. Don't make a game function you call wait for another game thread that may
  itself be waiting to run a Lua hook.
- In reNut, the `frame` event, the `renut` events and the part callbacks all run on the game's
  update thread. Native mods (see [native-mods.md](native-mods.md)) don't take this lock.

## Logging: `print` and `rex.log`

Each line goes to reNut's log (and the in-game log window, **F2**), at the given level, tagged
with the RexFabric category and your mod's folder name: `[my_mod] hello`.

| Function | Level | Notes |
|---|---|---|
| `print(...)` | info | Values are turned to text with Lua's `tostring` rules and joined with spaces. |
| `rex.log(...)`, `rex.info(...)` | info | The same as `print`. |
| `rex.warn(...)` | warning | |
| `rex.error(...)` | error | Only writes a line. It does **not** raise a Lua error; use Lua's `error()` for that. |
| `rex.debug(...)` | debug | May be hidden at the log's default level. |

- **When:** any time.
- **The tag.** While a file loads, it's that file's mod. Inside a callback, it's the mod whose
  `scripts` folder holds the innermost Lua function on the call stack. When there's none (a callback
  RexFabric runs after yours already failed), the tag is `[lua]`.
- **Errors in callbacks** are caught and logged with a stack trace, tagged with your mod and what
  failed: `hook`, `midasm handler`, `cvar watcher`, or the event's name (`frame`,
  `renut.part_spawn`). The game carries on and the callback runs again next time.

```lua
print("loaded", 3, true)           -- [my_mod] loaded 3 true
rex.warn("no save loaded yet")
```

## Time and version: `rex.now`, `rex.version`

| Name | What |
|---|---|
| `rex.now()` | Seconds on a steady (monotonic) clock, as a float. Only differences between two calls mean anything. Any time. |
| `rex.version` | A string field: the ReXGlue SDK version reNut was built with. |

## Timers: `rex.after`, `rex.every`, `rex.cancel`

Functions run later, on the `frame` event's thread, as frames go by.

| Function | Returns | What |
|---|---|---|
| `rex.after(seconds, fn)` | timer id (integer) | Runs `fn()` once, `seconds` from now. |
| `rex.every(seconds, fn)` | timer id | Runs `fn()` every `seconds` until it's cancelled. A negative time counts as `0` (every frame). |
| `rex.cancel(id)` | nothing | Stops a timer. An id that already ran, isn't a timer, or is `nil` is fine. |

- **When:** any time. They need the `frame` event to fire, which in reNut is once per drawn frame
  on every screen (menus, loading and the garage included).
- A timer runs **at most once a frame**, on the first frame its time has come; a repeating one
  continues from then, rather than running several times to catch up. Timers due on the same
  frame run oldest first.
- `seconds` must be a number and `fn` a function, or they raise `a timer's time is a number of
  seconds` / `a timer runs a function`.
- An error in a timer is logged as `a timer failed: ...` and doesn't stop other timers. A
  repeating timer that fails keeps running.
- Timers live in the Lua VM: Reload Scripts drops them all.
- These are RexFabric's own timers. The `renut` table has its own `renut.after` / `renut.every`
  for game time ([renut-api.md](renut-api.md)).

```lua
local id = rex.every(0.5, function() rex.info("tick") end)
rex.after(3, function() rex.cancel(id) end)   -- six ticks, then stop
```

## Events: `rex.on`

```lua
rex.on(name, fn)
```

Runs `fn(...)` every time the event `name` fires, for as long as the scripts are loaded.

- **Returns** nothing. There's no way to remove one listener; Reload Scripts removes them all.
- **Order.** Listeners of one event run in the order they were added, all mods together, so mods
  earlier in the load order hear it first. An error in one is logged and doesn't stop the rest.
- **When:** any time. A name nothing fires is accepted and simply never runs.
- **Arguments** arrive as Lua values: numbers, strings, booleans, `nil`, and game objects as
  [object proxies](#game-namespaces-as-lua) (or `nil` for a null address).

| Event | Arguments | When it fires |
|---|---|---|
| `"frame"` | `seconds` since the last frame (`0` the first time) | Once per frame. In reNut: on the game thread, right after the game draws a frame, on every screen. |
| `"renut.<name>"` | depends on the event | The game's events, which reNut declares: `renut.part_spawn`, `renut.vehicle_tick`, `renut.world_enter`, `renut.game_tick`, `renut.net_message` and many more. The list and their arguments are in [renut-api.md](renut-api.md); most mods use the wrappers there (`renut.parts.on`, `renut.vehicle.on`, ...) instead of `rex.on`. |

```lua
local elapsed = 0
rex.on("frame", function(dt)
  elapsed = elapsed + dt
end)

rex.on("renut.world_enter", function(number, scene, name)
  print("entered", name)
end)
```

## Hooks: `rex.hook` and friends

A hook runs your function every time the game calls one of its functions. Lua hooks are
**before** and **after**, not "around":

| Function | Returns | What |
|---|---|---|
| `rex.hook(address, fn)` | hook id (integer) | `fn(ctx)` runs **before** the function at `address`. If `fn` returns exactly `false`, the game's function is skipped this time. |
| `rex.hook_after(address, fn)` | hook id | `fn(ctx)` runs **after** the function returns; its result is in `ctx.r3` (or `ctx.f1`) and can be changed. The return value of `fn` is ignored. |
| `rex.unhook(id)` | boolean | Removes one hook. `false` when there's no such id. |
| `rex.hooks()` | list | Every Lua hook, sorted by address, then by id: `{ id = ..., address = ..., phase = "pre" or "post", owner = "<mod folder>" }`. |
| `rex.hookable(address)` | boolean | Whether a recompiled function starts at `address`. |

- **When:** any time, including from inside a hook. `ctx` is described [below](#the-register-context-ctx).
- **Errors.** Hooking an address where no recompiled function starts raises
  `cannot hook 0x...: no recompiled function at this address`. `fn` must be a function.
- **Several hooks on one address** run in the order they were added. **Every** before-hook runs,
  even after one returned `false`; the function is skipped if any of them did. After-hooks run
  whether or not it was skipped.
- **Skipping a function.** The caller still reads its result, so set it yourself:
  `ctx.r3 = 0; return false`.
- **A hook that raises an error** is logged and counts as not returning `false`.
- **Native mods.** All Lua hooks on one address share one slot in that function's hook chain, at
  the place the first Lua hook was added. Native mods load before Lua, so a native hook on the same
  function runs around the Lua ones.
- **Reload Scripts** removes every Lua hook.

### Telling call sites apart: `ctx.lr`

In a before-hook, `ctx.lr` is the **return address**: reNut's generated code sets the link
register before each call, so it points just after the instruction that called the function. Use it
to react only to calls from one place:

```lua
-- Logs who calls a function, once per call site.
local seen = {}
rex.hook(0x82XXXXXX, function(ctx)
  if seen[ctx.lr] then return end
  seen[ctx.lr] = true
  print(string.format("called from %08X with r3 = %08X", ctx.lr, ctx.r3))
end)

-- Then, to react to one caller only (the address the log printed):
local FROM_THERE = 0x82YYYYYY
rex.hook(0x82XXXXXX, function(ctx)
  if ctx.lr ~= FROM_THERE then return end
  -- ...
end)
```

### reNut's own hooks win

reNut replaces many of the game's functions with its own C++ (its `REX_HOOK` / `REX_HOOK_RAW`
hooks). On those, **`rex.hookable` still returns `true` and `rex.hook` accepts the address, but
your hook never runs**, because reNut's replacement takes the place of the function, hook check
included. If a hook stays silent, search reNut's `src` folder for `sub_<address>` (or the
function's `rex_` name in `config/renut_funcs.toml`). The functions behind the `renut` events
(vehicle and part damage, spawning, and so on) are mostly among them: use the events instead.

The exceptions: on these functions reNut runs mods' `rex.hook` and `rex.hook_after` itself (with
RexFabric's `project_hooks.h`), so Lua hooks work there.

| Address | Function | Where mods' hooks run |
|---|---|---|
| `0x821BD590` | challengeManTick (once a frame while a level runs) | Around the game's own function, inside reNut's hook. |
| `0x824FDC38` | BeginChallenge (`challengeMan, &aid, flag`) | Around the game's own function. |
| `0x82503568` | challenge state 14: settles a finished challenge's outcome (`challengeMan`) | Around the game's own function. |
| `0x8236A570` | CreateObject (`class, objparams copy, net`) | Around the game's own function. |
| `0x824E8838` | gameSaveSave | Around the game's own function. |
| `0x824AC1C0` | counterAdd (`profile, counter, amount, silent`) | Around **all** of reNut's hook: a before-hook returning `false` skips reNut's work too. |

"Around the game's own function" means reNut's own work before and after it still happens even
when your before-hook skips the function. `ctx.address` is the address above in every case.

## The register context: `ctx`

A hook gets the PowerPC registers of the call. Arguments arrive in `r3`, `r4`, ... (integers and
pointers, in order) and `f1`, `f2`, ... (floats, numbered separately). A function returns in `r3`
or `f1`.

| Field / method | Read | Write |
|---|---|---|
| `ctx.r0` ... `ctx.r31` | The low 32 bits, unsigned. | Sets the register to the value's low 32 bits; the top 32 bits are cleared. |
| `ctx.f0` ... `ctx.f31` | The float register, as a Lua number (a double). | Sets it. |
| `ctx.lr` | The link register: in a before-hook, the return address of the call. | Sets it. |
| `ctx.ctr` | The count register (64 bits). | Sets it. |
| `ctx.address` | The hooked function's address. | Not writable (raises `unknown PPC context field 'address'`). |
| `ctx:u64(n)` | All 64 bits of `rN`. | |
| `ctx:set_u64(n, v)` | | Sets all 64 bits of `rN`. |
| `ctx:s32(n)`, `ctx:s64(n)` | `rN` as a signed 32- or 64-bit number. | |
| `ctx:f32(n)` | The low 32 bits of `fN` read as a single float. Float registers hold doubles, so this is rarely what you want: use `ctx.fN`. | |

- `n` is `0` to `31`; anything else raises `register index ... out of range [0, 31]`.
- Writing a field that doesn't exist raises `unknown PPC context field '...'`. Reading one gives `nil`.
- **A `ctx` is only valid during the callback it was passed to.** One kept in a variable and used
  later raises `PPC context used outside the hook that produced it`.
- In a before-hook, changing `r3`..`r10` / `f1`..`f13` changes the arguments the game function
  sees. In an after-hook, changing `r3` / `f1` changes what the caller gets.

```lua
-- Doubles the second argument before the game sees it.
rex.hook(0x82XXXXXX, function(ctx) ctx.r4 = ctx.r4 * 2 end)

-- Makes a function return 0 without running it.
rex.hook(0x82XXXXXX, function(ctx)
  ctx.r3 = 0
  return false
end)

-- Reads the result.
rex.hook_after(0x82XXXXXX, function(ctx) print("returned", ctx.r3) end)
```

## Calling game functions: `rex.call`

```lua
local r3 = rex.call(address, arg1, ..., arg8)
```

Calls the game function at `address` with up to eight integer arguments (in `r3` to `r10`) and
returns its `r3` as an unsigned 32-bit integer.

- **When:** on a guest thread: inside a hook, a midasm handler, a `frame` listener (reNut fires
  `frame` on the game thread) or a `renut` event. At a script's top level it raises
  `rex.call must run on a guest thread ...`.
- **Errors:** more than eight arguments, a non-integer argument, or an address with no function
  (`no guest function at 0x...`).
- Only integer arguments; float arguments (`f1`...) can't be passed and a float result can't be
  read. For those, hook the function instead.
- Inside a hook, the call runs on that hook's registers, with its stack frame below the hooked
  call's. Every register except the float status register is put back afterwards, so the hooked
  call isn't disturbed.
- A game function may expect things to exist that don't (a loaded level, a player). The safest
  place to call one is a hook on a function the game runs when it would call it itself.

```lua
-- Gives player 1 a jiggy while a level runs (counterAdd: profile, counter, amount, silent).
rex.hook_after(0x82207320, function()
  if not (rex.input.pressed("J") and rex.input.accepts()) then return end
  local profile = rex.mem.u32(0x82E1B280)
  if profile ~= 0 then rex.call(0x824AC1C0, profile, 61, 1, 0) end
end)
```

## Game memory: `rex.mem`

Reads and writes the game's memory by guest address. The Xbox 360 is big-endian; these convert
for you.

| Function | Returns / does |
|---|---|
| `rex.mem.u8(a)`, `u16`, `u32`, `u64` | Unsigned integers. (`u64` values above `2^63` come back negative: Lua integers are signed 64-bit.) |
| `rex.mem.s8(a)`, `s16`, `s32`, `s64` | Signed integers. |
| `rex.mem.f32(a)`, `f64` | Floats. |
| `rex.mem.set_u8(a, v)` ... `set_u64`, `set_s8` ... `set_s64` | Writes the integer `v`, cut to the size. |
| `rex.mem.set_f32(a, v)`, `set_f64(a, v)` | Writes the number `v`. |
| `rex.mem.string(a [, max])` | The bytes up to the first zero, at most `max` (default `4096`). |
| `rex.mem.set_string(a, s)` | Writes all of `s` **and** a zero byte after it. Nothing checks that it fits. |
| `rex.mem.wstring(a [, max])` | A zero-terminated UTF-16 (big-endian) string, as UTF-8. `max` counts characters (default `4096`). Only the basic plane. |
| `rex.mem.bytes(a, n)` | `n` raw bytes (`0` to 16 MiB) as a Lua string. |
| `rex.mem.set_bytes(a, s)` | Writes the bytes of `s`, zero bytes included, no terminator. |

- **When:** any time. RexFabric starts after the game's memory exists, so these work even at a
  script's top level (though the game hasn't set anything up yet). Before memory exists they raise
  `guest memory is not mapped yet`.
- **No bounds or validity check.** Reading or writing an address the game hasn't mapped (a stale
  or garbage pointer) can crash reNut rather than raise an error. Check pointers for `0` and only
  follow pointers you know are live: a profile, level or vehicle only exists at certain times.
- **Binary data.** Lua strings may hold zero bytes; `bytes` / `set_bytes` and the `renut`
  functions that take strings keep a string's full length.

```lua
local profile = rex.mem.u32(0x82E1B280)            -- player 1's profile, 0 without a save
if profile ~= 0 then
  local counters = rex.mem.u32(profile + 308)      -- the profile's game counters
  if counters ~= 0 then print("notes:", rex.mem.u32(counters + 360)) end
end
```

## Keyboard and mouse: `rex.input`

Reads the PC keyboard and mouse, whether or not reNut's keyboard controls (`renut_keyboard`) are
on.

| Function | Returns / does |
|---|---|
| `rex.input.down(key)` | `true` while the key is held. |
| `rex.input.pressed(key)` | `true` once per press: the first call after the key goes down, then `false` until it's released and pressed again. |
| `rex.input.accepts()` | `true` when nothing in front of the game wants the input: no reNut overlay window that takes the cursor is open (reNut's settings, for example) and the Guide (where the mods menu lives) is closed. |
| `rex.input.mouse_button(n)` | `true` while mouse button `n` is held: `0` left, `1` right, `2` middle, `3` and `4` the side buttons. Works whether or not the mouse camera has captured the mouse. |
| `rex.input.mouse_delta()` | Two integers `dx, dy`: the mouse motion in physical pixels since the last call (any mod's). Reading resets it. |
| `rex.input.wheel()` | The mouse wheel turned since the last call (any mod's): `120` per notch, up positive. Reading resets it. |
| `rex.input.hold_guest(on)` | `true`: the game stops seeing player 1's controller input (its pad entries reach the game empty), for a mod that drives something itself, such as the camera. `false` gives it back. |
| `rex.input.flush()` | Throws away mouse motion and wheel piled up so far. Key states and `pressed` edges stay. |
| `rex.input.available()` | Whether the game supplies input to mods (always `true` in reNut). |

- **When:** any time; usually in a `frame` listener.
- **`key`** is a key name (below) or a number, used as a key code as is. An unknown name is never down.
- **`pressed` uses itself up.** RexFabric keeps one "was it down" flag per key, shared by every
  mod. Call it **every frame** (a frame missed can miss a press) and from **one place** per key (two
  callers share one press). Check `accepts()` as well, so a hotkey doesn't fire while the player
  types in a window:

```lua
rex.on("frame", function()
  local pressed = rex.input.pressed("F6")   -- every frame, before any early return
  if pressed and rex.input.accepts() then print("F6!") end
end)
```

- **`hold_guest` isn't released by Reload Scripts.** Give the input back (`hold_guest(false)`)
  before reloading, or the game keeps ignoring player 1's controls until something releases it.
- **The digit keys don't work by name.** A string that looks like a number counts as that number,
  so `"1"` is key code 1 (not the 1 key). Use letters, function keys or the numpad (`"Numpad1"`).

### Key names

`rex.input` reads key names with the same parser as reNut's keyboard bind settings
(`renut_key_*`), so any name those accept works here. Spell them as below.

| Group | Names |
|---|---|
| Letters | `A` to `Z` |
| Function keys | `F1` to `F24` |
| Control | `Space`, `Return`, `Escape`, `Tab`, `Backspace`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown` |
| Arrows | `Up`, `Down`, `Left`, `Right` |
| Modifiers | `Shift`, `Control`, `Alt` |
| Symbols | `Backtick`, `Minus`, `Plus` (the =/+ key), `Comma`, `Period`, `Semicolon`, `Slash`, `Backslash`, `LBracket`, `RBracket`, `Quote` |
| Numpad | `Numpad0` to `Numpad9`, `NumpadEnter`, `NumpadPlus`, `NumpadMinus`, `NumpadStar`, `NumpadSlash` |
| Mouse | `LMB`, `RMB`, `MMB`: only while the mouse camera has captured the mouse. Use `mouse_button` otherwise. |
| Digits | `0` to `9` exist but are misread (see the note above). |

Keys reNut already uses still reach scripts: **F2** (log window), **F4** (reNut settings),
**F8** (all settings), **F9** (mods menu), **F10** (node editor) and, with `renut_keyboard` on,
the game's key binds. Pick keys that don't clash, or make yours a setting.

## Settings: `rex.cvar`

A cvar is a named setting. Scripts read and change reNut's own and the SDK's, and define their own.

| Function | Returns | What |
|---|---|---|
| `rex.cvar.get(name [, default])` | value | The cvar's value: boolean, integer, number or string. An unknown name returns `default` when a second argument is given (even `nil`), otherwise raises `unknown cvar '...'`. |
| `rex.cvar.set(name, value)` | nothing | Sets it. Booleans become `"true"`/`"false"`, numbers their text, then the setting parses it. Raises `could not set cvar '...' to '...'` for an unknown name or a value the setting rejects. |
| `rex.cvar.exists(name)` | boolean | Whether it's registered. |
| `rex.cvar.list([category])` | list of names | Without a category, every setting's full name. With one, the mod settings in that category, then the settings under a scope of that name. |
| `rex.cvar.on_change(name, fn)` | nothing | `fn(name, value)` runs after the cvar changes. See below. |
| `rex.cvar.boolean(name, default, category, description)` | value | Defines a boolean cvar and returns its value. |
| `rex.cvar.int32`, `int64`, `uint32`, `uint64` | value | The same for integers (`default` an integer, `0` when left out). |
| `rex.cvar.number` | value | The same for floats (`default` `0.0` when left out). |
| `rex.cvar.string` | value | The same for text (`default` `""` when left out). |

- **When:** any time; define cvars at your script's top level.
- **Names.** A name can be a setting's full name (`renut.renut_vsync`), its old flat name
  (`renut_vsync`), or the name a mod defined (`mymod_amount`). Cvar names are global: prefix yours
  with your mod's name.
- **Defining.** `category` defaults to `"Scripts"`, `description` to `""`. The setting is
  registered as `mods.<name>`, with `<name>` as a short alias, and takes its value from the config
  file if the file has one. A category starting with `Nuts&Bolts/` (such as
  `"Nuts&Bolts/My Mod"`) also puts it in a tab of reNut's own settings window (**F4**).
- **Already registered?** If the name is taken (by reNut, the SDK, another mod, or your script's
  previous run), the define changes nothing and returns the current value.
- **Lifetime.** A defined cvar stays registered until reNut closes. After Reload Scripts the define
  returns the value it has now, not the default. Raises `could not register cvar '...'` when the
  settings registry refuses the name.
- **`on_change`.** The watcher runs on the next `frame` event after the change, on the frame
  thread, after that frame's Lua `frame` listeners. Several changes in one frame make one call,
  with the value at that time. It works for reNut's cvars and scripts' alike, but **the cvar must
  exist when you call `on_change`**: define your own first. Watchers go with Reload Scripts.

```lua
local amount = rex.cvar.int32("mymod_amount", 1, "Nuts&Bolts/My Mod", "How many to give per press.")
rex.cvar.on_change("mymod_amount", function(_, value) amount = value end)

if rex.cvar.get("renut_keyboard", false) then print("keyboard controls are on") end
```

## Text on screen: `rex.screen_text`

```lua
rex.screen_text(text [, seconds [, rgb [, size [, key]]]])
```

Shows a line of text in the screen's **top right corner** for a while, newest at the top, like
Unreal's Print String. For debugging. The node editor's **Print String** node writes this call.

| Argument | Default | What |
|---|---|---|
| `text` | | Any value, turned to text with `tostring` rules. |
| `seconds` | `2` | How long it stays; it fades over its last 0.4 s. `0` or less shows nothing. |
| `rgb` | `0x00A8FF` (cyan) | The colour as `0xRRGGBB`. |
| `size` | `0` | Text height in pixels, up to 200. `0` is the default (1.6 times the UI's font). |
| `key` | none | A line with a key **replaces** the shown line with the same key, in place, so a value printed every frame is one line that changes. |

- **When:** any time, any thread. At most 40 lines show; older ones drop off.
- The text stops at its first zero byte.

```lua
rex.on("frame", function(dt)
  rex.screen_text(string.format("fps %.0f", 1 / math.max(dt, 1e-6)), 0.5, 0xFFFFFF, 0, "fps")
end)
```

## Drawing: `rex.draw`

Shapes anywhere on the screen, drawn by RexFabric's overlay over the game (under the
`screen_text` lines). Positions and sizes are **fractions of the screen** (`0` to `1`), so they
look the same at any resolution. Mod HUDs and debug drawing should go through this.

| Function | What |
|---|---|
| `rex.draw.text(text, x, y [, options])` | Text at `x, y`. Vertically centred on `y`. |
| `rex.draw.rect(x, y, width, height [, options])` | A rectangle from its top-left corner. `width` is a fraction of the screen's width, `height` of its height. |
| `rex.draw.line(x1, y1, x2, y2 [, options])` | A line. |
| `rex.draw.circle(x, y, radius [, options])` | A circle; `radius` is a fraction of the screen's **height**. |
| `rex.draw.remove(key)` | Removes the shape with that key. |
| `rex.draw.clear([prefix])` | Removes every shape whose key starts with `prefix`; every shape for `""` (the default). Doesn't touch `screen_text` lines. |

Options, in a table (all optional):

| Option | Default | Applies to | What |
|---|---|---|---|
| `key` | none | all | A shape with a key replaces the shape with the same key. |
| `seconds` | see below | all | How long it stays. |
| `rgb` | `0xFFFFFF` | all | Colour as an integer `0xRRGGBB` (anything else is white). |
| `alpha` | `1` | all | Opacity, `0` to `1`. |
| `thickness` | `2` | lines, outlines | Pixels, up to 64. |
| `filled` | `true` | rect, circle | `false` draws an outline. |
| `rounding` | `0` | rect | Corner radius, as a fraction of the screen's height. |
| `size` | `0.03` | text | Text height as a fraction of the screen's height (up to 200 px). |
| `align` | centred | text | `"left"`: the text starts at `x`; `"right"`: it ends at `x`; otherwise it's centred on `x`. |
| `background` | `false` | text | A dark box behind the text. |

**How long a shape stays:**

- `seconds` greater than `0`: that long.
- No `seconds` **and a `key`**: until you remove it, clear it, or draw a shape with the same key.
- No `seconds` and no key: a tenth of a second, so a shape drawn every frame (or every tick)
  stays up without piling up.

At most 4096 shapes are kept; the oldest go first. Positions must be finite numbers
(`not a finite number` otherwise); `options` must be a table or nothing.

- **When:** any time, any thread. Shapes appear from the next drawn frame.

```lua
-- A health bar that follows a value, redrawn each frame under one key.
rex.on("frame", function()
  local health = 0.7
  rex.draw.rect(0.05, 0.9, 0.3, 0.03, {key = "mymod.bar.bg", rgb = 0x202020, alpha = 0.6, rounding = 0.005})
  rex.draw.rect(0.05, 0.9, 0.3 * health, 0.03, {key = "mymod.bar.fg", rgb = 0x5CFF6E, rounding = 0.005})
  rex.draw.text("Health", 0.05, 0.88, {key = "mymod.bar.label", align = "left", size = 0.025})
end)

-- Later: rex.draw.clear("mymod.") removes all three.
```

## Switches: `rex.switch`

Named integer flags that code compiled into the game reads, so a mod can turn a compiled-in patch
on or off. **reNut's own code reads no switches yet**, so in reNut these only matter as a shared
value between mods (a native mod can read them with `get_switch`, see
[native-mods.md](native-mods.md)).

| Function | What |
|---|---|
| `rex.switch.get(name)` | Its value; `0` when nothing has set it. |
| `rex.switch.set(name, value)` | Sets it to an integer, or a boolean (`true` is `1`). |
| `rex.switch.list()` | A table of `name = value` for every switch. |

- **When:** any time.
- Switches start at `0`, which compiled-in code reads as "behave like the unmodded game".
  Switches Lua set go back to `0` when the scripts reload or stop, so a mod that was turned off
  stops applying.

## Midasm sites: `rex.midasm`

A midasm hook sits at one instruction partway through a function, rather than at its start. The
game has to compile a stub in at that address for mods to claim. **reNut compiles in no claimable
sites yet** (its own midasm hooks are its own), so in reNut a handler you attach never runs.

| Function | Returns | What |
|---|---|---|
| `rex.midasm(address, fn)` | claim id | Claims the site at `address`. `fn(frame)` runs each time the game reaches it. |
| `rex.unmidasm(id)` | boolean | Releases a claim; `false` when there's no such claim. |
| `rex.midasms()` | list | Lua's claims: `{ id = ..., address = ..., owner = "<mod folder>" }`. |

- `frame.address` is the site's address. `frame.r3`, `frame.f1`, ... read and write the registers
  the site's stub lends, by the names it gave them (`r` registers as 32-bit unsigned integers, `f`
  registers as numbers). A register the site didn't lend raises
  `midasm site 0x... did not expose register '...'`.
- Returning a true value asks for the site's patch (at a branch site, the branch is taken). Every
  claim on a site runs, in claim order; the patch applies if any asked for it. A handler that raises
  an error asks for nothing.
- A `frame` is only valid during the handler that got it.
- **When:** any time. A site with no stub behind it never calls, which looks the same as one the
  game hasn't reached yet. Reload Scripts releases every Lua claim.

```lua
rex.midasm(0x823DA320, function(m) m.r3 = 1 end)       -- force a register
rex.midasm(0x824DC3AC, function() return true end)     -- take a branch
```

## Game namespaces as Lua

What a game declares for its mods in C++ (with `rexfabric::Namespace`, see
[native-mods.md](native-mods.md#for-renut-developers-the-host-side)) appears in Lua as global
tables. reNut declares one namespace, `renut`; [renut-api.md](renut-api.md) documents what's in it.
How the pieces look in Lua:

| Declared | In Lua |
|---|---|
| A function `renut.game._world` | A function at that path: `renut.game._world()`. Arguments are converted to what it declares and raise an error when they don't fit. An error inside it raises `renut.game._world: <message>`. |
| A constant or enum | A value, or a table of `Name = number`. |
| A type `renut.Part` | A table with `name` and `size`; calling it, `renut.Part(address)`, makes an **object proxy**, or `nil` for address `0`. |
| An event `renut.part_spawn` | Something `rex.on("renut.part_spawn", fn)` hears. |

**Object proxies:**

- `obj.address` is its guest address; `obj.type` its type's full name (`nil` when untyped).
- Declared fields read and write guest memory: `obj.field`, `obj.field = v`. Array fields index
  from 1 (`obj.list[1]`, `#obj.list`). A field that doesn't exist raises `<type> has no field '...'`.
- Methods are called with `:`: `obj:method(...)`.
- `tostring(obj)` gives `renut.Part(0x...)`; two proxies are `==` when address and type match.
- Where a function takes an integer address, you can pass an object and its address is used.

Most of `renut` is Lua written on top of these (the friendlier `renut.parts`, `renut.vehicle`, ...).
Use those; the underscored functions (`renut.x._y`) are the raw layer underneath.

## What `rex` doesn't have

- **No HTTP or networking.** RexFabric's HTTP client only serves its Thunderstore browser, which
  reNut doesn't enable. Online play between reNut players goes through `renut.net`
  ([renut-api.md](renut-api.md)).
- **No removal of single `rex.on` listeners** and **no unload callback**; Reload Scripts drops
  everything (hooks, listeners, timers, claims, switches set from Lua, `pressed` edges) and runs
  the scripts again in a new Lua state. Cvars stay. See [mods-and-scripts.md](mods-and-scripts.md).
- **No save API in `rex`.** `renut.data` keeps a mod's data with the player's save
  ([renut-api.md](renut-api.md)).

## Summary table

| `rex.` | Arguments | Returns | When |
|---|---|---|---|
| `log`, `info`, `warn`, `error`, `debug` | values | | any time |
| `now` | | number | any time |
| `version` | (string field) | | |
| `after`, `every` | seconds, fn | timer id | any time |
| `cancel` | id | | any time |
| `on` | name, fn(...) | | any time |
| `hook`, `hook_after` | address, fn(ctx) | hook id | any time |
| `unhook` | id | boolean | any time |
| `hooks` | | list | any time |
| `hookable` | address | boolean | any time |
| `call` | address, up to 8 integers | integer | guest thread |
| `mem.*` | address, ... | value | once guest memory exists (from load time) |
| `input.down`, `input.pressed` | key | boolean | any time (`pressed`: every frame) |
| `input.accepts`, `input.available` | | boolean | any time |
| `input.mouse_button` | 0 to 4 | boolean | any time |
| `input.mouse_delta` | | dx, dy | any time |
| `input.wheel` | | integer | any time |
| `input.hold_guest` | boolean | | any time |
| `input.flush` | | | any time |
| `cvar.get` | name [, default] | value | any time |
| `cvar.set` | name, value | | any time |
| `cvar.exists` | name | boolean | any time |
| `cvar.list` | [category] | list | any time |
| `cvar.on_change` | name, fn(name, value) | | any time (after the cvar exists) |
| `cvar.boolean` ... `cvar.string` | name, default, category, description | value | any time (usually load time) |
| `screen_text` | text [, seconds, rgb, size, key] | | any time |
| `draw.text`, `rect`, `line`, `circle` | positions [, options] | | any time |
| `draw.remove`, `draw.clear` | key / [prefix] | | any time |
| `switch.get`, `switch.set`, `switch.list` | name [, value] | | any time (reNut reads none) |
| `midasm`, `unmidasm`, `midasms` | address, fn / id | | any time (reNut has no claimable sites) |
