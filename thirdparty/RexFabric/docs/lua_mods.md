# Writing a Lua mod

Lua mods run in one Lua 5.4 VM the game shares between them. The API is the one the earlier script runtime in the SDK fork had, name for name, so mods written for it run unchanged.

## Layout

```
mods/
  MyMod/
    scripts/
      main.lua
      lib/util.lua      -- require("lib.util")
    mod.toml            -- optional: name, description, version, author
    icon.png            -- optional, shown in the mods menu
```

Every `.lua` directly in `scripts` runs when the game starts, in file name order. Subfolders are only reached through `require`, which looks inside the mod's own `scripts` folder. Mods run in load order, so a later mod's hooks run after an earlier one's.

Errors are logged with a traceback and the mod's name; one failing file doesn't stop the others.

## Hooks

```lua
rex.hook(0x82558D28, function(ctx)
  if disabled then
    ctx.r3 = 0
    return false        -- returning exactly false skips the original function
  end
end)

rex.hook_after(0x8232C048, function(ctx)
  rex.info("returned", ctx.r3)
end)
```

Where the game names its functions (see [game_api.md](game_api.md)), a name works wherever an address does: `rex.hook("obj.objInstIsDerivedFrom", fn)`. An editor opened on the mods folder completes the names.

`rex.hook` runs before the function and `rex.hook_after` after it. Every before hook on a function runs, even after one returns false; the original runs only if none did. Both return an id for `rex.unhook(id)`. `rex.hookable(address)` says whether an address can be hooked, and `rex.hooks()` lists every Lua hook as `{id, address, phase, owner}`.

`ctx` is the guest's registers during the call:

| Field | Is |
|---|---|
| `r0` to `r31` | integer registers, read and written as 32 bit |
| `f0` to `f31` | float registers |
| `lr`, `ctr` | link and count registers |
| `address` | the hooked function |
| `ctx:u64(n)`, `ctx:set_u64(n, v)`, `ctx:s32(n)`, `ctx:s64(n)`, `ctx:f32(n)` | other views of a register |

`ctx` is only valid inside the hook that got it.

## Guest memory

`rex.mem.u8`, `u16`, `u32`, `u64`, `s8` to `s64`, `f32`, `f64` read an address, and `set_u8` to `set_f64` write one, converting from and to the guest's big endian. `rex.mem.string(addr [, max])`, `set_string`, `wstring` (UTF-16, returned as UTF-8), `bytes(addr, count)` and `set_bytes` cover strings and raw bytes.

## Calling the game

`rex.call(address, ...)` calls a guest function with up to eight integer arguments and returns its r3. It has to run on a guest thread: inside a hook, or in a frame event when the game fires frames from its present.

## Midasm sites

A game can compile a stub into the middle of a function. A mod claims it by its address:

```lua
rex.midasm(0x824DC3AC, function() return true end)      -- at a branch: true takes it
rex.midasm(0x823DA320, function(m) m.r3 = 1 end)        -- the registers the stub lent, by name
```

`rex.unmidasm(id)` releases a claim and `rex.midasms()` lists them. A site nobody claims runs the game's own instruction.

## Switches

`rex.switch.set(name, value)` sets a switch the game's compiled-in code reads; true and false become 1 and 0. `rex.switch.get(name)` and `rex.switch.list()` read them. Switches Lua set go back to 0 when scripts reload.

## Cvars

```lua
rex.cvar.boolean("MyFlag", true, "Category", "What it does")   -- also int32, int64, uint32, uint64, number, string
local on = rex.cvar.get("MyFlag")
rex.cvar.set("MyFlag", false)
rex.cvar.on_change("MyFlag", function(name, value) end)
```

A cvar a mod defines shows in the settings and is saved like any other. `rex.cvar.exists(name)` and `rex.cvar.list([category])` look them up. Change callbacks run on the next frame.

## Events

```lua
rex.on("frame", function(seconds) end)              -- every frame, with the seconds since the last
rex.on("tip.garden_loaded", function(garden) end)   -- events the game declares
```

## Timers

```lua
local id = rex.every(0.5, function() rex.info("tick") end)  -- every half second, until cancelled
rex.after(3, function() rex.cancel(id) end)                 -- once, three seconds from now
```

`rex.after(seconds, fn)` runs `fn` once, `rex.every(seconds, fn)` again and again; both return an id for `rex.cancel(id)`, which is fine with a timer that already ran. Timers run on the frame thread at most once a frame, on the first frame their time has come, so a repeating timer faster than the frame rate runs once a frame, and none run before the game's first frame. A timer whose function fails is logged and, if it repeats, keeps going. Reloading the scripts drops every timer.

## Waiting

A function `rex.run` runs is a coroutine: it may wait for time to pass or for an event, and goes on by itself when the
wait ends, as Unreal's latent nodes do. Everything else in it runs as any script does.

```lua
rex.run(function()
  local garden = rex.wait_for("tip.garden_loaded")      -- until the event fires; gives its arguments
  rex.wait(2)                                           -- two seconds
  local sign = rex.wait_for("tip.signpost_chosen", function(sign) return sign.tag == 2176 end)
  local nothing = rex.wait_for("tip.hour_passed", nil, 5)  -- nil after five seconds without one
end)
```

| Call | Does |
|---|---|
| `rex.run(fn, ...)` | Runs `fn(...)` now as a coroutine |
| `rex.wait(seconds)` | Waits, on the timers' clock (a frame at most, once its time has come) |
| `rex.wait_for(event, filter, timeout)` | Waits for the event's next firing whose arguments `filter` says yes to (any, without one), and gives its arguments; with `timeout`, gives nothing once that many seconds pass first |

Waiting anywhere else (an event handler, a hook, a timer not started by `rex.run`) is an error saying so, as there's
nothing to pause. A failure inside a waiting function is logged with its traceback, as a timer's is. Several functions
waiting for one event go on in the order they started waiting. Reloading the scripts drops every waiting function with
the timers (`src/lua/lua_timers.cpp`).

## World data

`rex.world_data()` gives the calling mod a table of its own that's kept in the save of the world the game is in, beside
its avatars saved with the world (content_and_classes.md, Saved with the world). In Trouble in Paradise that's the
garden: what a mod puts there comes back when that garden loads, and another garden has its own.

```lua
rex.on("tip.garden_loaded", function()
  local data = rex.world_data()
  data.visits = (data.visits or 0) + 1
  data.quests = data.quests or {}
  rex.info("visit", data.visits)
end)
```

* What's kept: booleans, numbers, text, and tables of them. A table whose keys are 1 to n is kept as a list; any other
  keeps its text keys. Functions, avatars, game objects and other keys are left out, with a warning in the log naming the
  mod.
* It's the same table for as long as the scripts run. A world loaded fills it again in place (emptying it for a world
  saved without it, such as a new garden), so a script may keep the table, and has the world's data by the time
  `tip.garden_loaded` fires. Between worlds it still holds the last one's.
* It's written when the game saves the world, not when it changes. A reload of the scripts keeps it: the next VM's
  `rex.world_data()` gives it back.
* A mod that's off or gone keeps its data in the world's save untouched until it's back.

`src/lua/lua_world_data.cpp`; the data goes in the world save's `mods` object, by mod folder.

## The console

The Lua console (Ctrl+Backtick in Trouble in Paradise, `ui.bind.lua_console`; Lua Console in the mods menu) runs a line
of Lua in the mods' VM and shows what it gave back. A line runs on the next frame, on the frame thread, where the
game's API works; it's tried as an expression first (`return` before it), as Lua's own interpreter does, so
`#tip.objects("entityAvatarPinataSignpost")` shows the count, and as statements when it isn't one (`x = 1`). Results
are shown joined, `(nothing)` for none; an error shows in red with its traceback. Up and Down go through the lines typed
before, and Escape closes it (the hotkey doesn't reach it while its line has the keyboard). Its globals are the mods'
globals, so a variable a line sets is there for the next; what it logs goes to the game's log as the mod `console`. It
may name every mod's content, as a developer's tool, where a mod names only its own and its dependencies' (`ContentMod`,
`src/lua/lua_console.cpp`, `src/ui/lua_console_window.cpp`). It's one of the editors: hidden with them by
`rexfabric.show_editors`.

## Input

`rex.input` reads the keyboard and mouse through the game:

| Call | Returns |
|---|---|
| `available()` | whether the game gave RexFabric input at all |
| `down(key)` | the key is held; keys are names like `"W"`, `"F9"`, `"Shift"` |
| `pressed(key)` | true once when the key goes down |
| `mouse_button(n)` | 0 left, 1 right, 2 middle, 3 and 4 the side buttons |
| `mouse_delta()` | motion since the last call, as dx, dy |
| `wheel()` | wheel motion since the last call |
| `accepts()` | the game window has focus and no menu wants the input |
| `hold_guest(held)` | holds the game's own input, for a mod that drives the camera |
| `flush()` | throws away piled up mouse motion |

Check `accepts()` before acting on a hotkey.

## The game's API

Whatever the game declares (see [game_api.md](game_api.md)) is a global table named after its namespace:

```lua
local p = tip.Pinata(ctx.r3)      -- nil for a null address
p.happiness = p.happiness + 1
p:feed(tip.Item.Apple)
rex.info(p, p.address, #p.needs, p.needs[1])
```

Objects of a type the game holds by handle (see [game_api.md](game_api.md)) can outlive what they stand for. `p.valid` is false once the game destroyed it, `p.address` is then nil, and any other field or method is an error saying it is gone. Keep such objects as long as you like, and check `valid` before using one later.

## Logging and time

`rex.screen_text(text, seconds, rgb)` shows text in the screen's top right corner for a while, newest at the top, for debugging, as Unreal's Print String does: two seconds and cyan (`0x00A8FF`) unless given. It's drawn by an overlay that only exists while there's text on the screen.

`rex.info`, `rex.warn`, `rex.error`, `rex.debug` and `print` write to the game's log, prefixed with the mod's name. `rex.now()` is a clock in seconds for timing. `rex.version` is the SDK's version.

## Reloading

The mods menu's Reload Scripts runs every enabled Lua mod again in a new VM. Hooks, claims, listeners and switches from before are dropped first; cvars the scripts defined keep their values.
