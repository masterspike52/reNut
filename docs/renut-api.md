# The `renut` Lua API

`renut` is reNut's own Lua table for Banjo-Kazooie: Nuts & Bolts. Through it a mod adds vehicle parts,
reads and drives vehicles, spawns objects and characters, shows things on the HUD, adds worlds, doors and
characters of its own, plays sounds, keeps data, talks to other players online, and listens to what the
game does. It's a global in every mod script, next to RexFabric's [`rex`](rexfabric-api.md) table (hooks,
memory, input, drawing, logging).

This page lists every public function, object, option and event. For the mod folder, `mod.toml`, load
order and reloading, see [mods-and-scripts.md](mods-and-scripts.md); for worked examples, the tutorials:
[parts and vehicles](tutorials/parts-and-vehicles.md), [worlds and doors](tutorials/worlds-and-doors.md),
[characters and challenges](tutorials/characters-and-challenges.md),
[HUD, menus and input](tutorials/hud-menus-and-input.md), [online multiplayer](tutorials/online-multiplayer.md),
[hooks and memory](tutorials/hooks-and-memory.md).

## Contents

- [Conventions](#conventions)
  - [Positions, places, angles and colours](#positions-places-angles-and-colours)
  - [Addresses and objects](#addresses-and-objects)
  - [Asset ids and names](#asset-ids-and-names)
  - [Handles and `:off()`](#handles-and-off)
  - [When you can call what](#when-you-can-call-what)
  - [Errors](#errors)
- [Vectors: `renut.vec`](#vectors-renutvec)
- [Listeners and timers](#listeners-and-timers)
- [Parts: `renut.parts`](#parts-renutparts)
  - [`renut.parts.add`](#renutpartsadd)
  - [Part callbacks](#part-callbacks)
  - [Part objects](#part-objects)
  - [Weapons and projectiles](#weapons-and-projectiles)
  - [Turrets, axes, bones, shape keys and paint](#turrets-axes-bones-shape-keys-and-paint)
- [Vehicles: `renut.vehicle`](#vehicles-renutvehicle)
  - [Vehicle objects](#vehicle-objects)
- [Changing damage: `renut.set_amount`](#changing-damage-renutset_amount)
- [Effects: `renut.fx`](#effects-renutfx)
- [Game state: `renut.game`](#game-state-renutgame)
- [Crates: `renut.crates`](#crates-renutcrates)
- [Objects: `renut.objects`](#objects-renutobjects)
- [Characters: `renut.actors`](#characters-renutactors)
- [Trigger zones: `renut.zones`](#trigger-zones-renutzones)
- [The HUD: `renut.hud`](#the-hud-renuthud)
- [Debug drawing: `renut.debug`](#debug-drawing-renutdebug)
- [Sound: `renut.sound`](#sound-renutsound)
- [Menu rows: `renut.menu`](#menu-rows-renutmenu)
- [Online: `renut.net`](#online-renutnet)
- [Saved data: `renut.data`](#saved-data-renutdata)
- [The pad: `renut.pad`](#the-pad-renutpad)
- [Lua challenges: `renut.challenges`](#lua-challenges-renutchallenges)
- [Other mods: `renut.mods`](#other-mods-renutmods)
- [Settings: `renut.settings`](#settings-renutsettings)
- [The camera: `renut.camera`](#the-camera-renutcamera)
- [The mouse cursor: `renut.mouse`](#the-mouse-cursor-renutmouse)
- [The physics world and the look: `renut.world`](#the-physics-world-and-the-look-renutworld)
- [Mod worlds: `renut.worlds`](#mod-worlds-renutworlds)
- [Doors: `renut.doors`](#doors-renutdoors)
- [Mod characters: `renut.npcs`](#mod-characters-renutnpcs)
- [The player: `renut.player`](#the-player-renutplayer)
- [Shape keys: `renut.shapes`](#shape-keys-renutshapes)
- [Materials: `renut.materials`](#materials-renutmaterials)
- [Other values](#other-values)
- [Event reference](#event-reference)
  - [Events by `.on`](#events-by-on)
  - [Raw events for `rex.on`](#raw-events-for-rexon)

---

## Conventions

### Positions, places, angles and colours

- **Units.** Distances are game units (one garage grid cell is one unit), velocities units per second,
  times and `dt` seconds. `y` is up.
- **Vectors.** Positions, directions, velocities and forces are [`renut.vec`](#vectors-renutvec) values.
  Functions that take one read only `.x`, `.y` and `.z`, so a plain `{x = 1, y = 2, z = 3}` works as an
  argument. A few (marked) also take `{1, 2, 3}`.
- **Quaternions** are plain tables `{x = ..., y = ..., z = ..., w = ...}`.
- **Places.** Functions that spawn or move something take a *place*: a vector, or one of these strings.

  | Place | Where |
  |---|---|
  | `"banjo"` | Where Banjo (player 1) is. |
  | `"front"` | In front of Banjo as the camera sees him: 3 units out (6 while he's in a vehicle) along the line from the camera to him, at his height. A spawn there is also turned to face him (its `yaw` is added to that). |

  Each function says which place it uses when you give none. A place string needs Banjo in the running
  world, or it raises `Banjo isn't in this world to place it by`.
- **Angles.** Yaws of things in the world (spawns, `actors.turn`, `objects.turn`, doors, NPCs,
  `v:yaw()`) are **degrees** about the vertical axis. Turret aim, bone angles and angular velocity are
  **radians**. The camera's field of view is in degrees (vertical).
- **Colours** are `0xRRGGBB` integers, or tables `{r, g, b}` with each `0`-`1` (also read as
  `{x, y, z}` or `{1, 2, 3}` positions, so a vector works).
- **Integers.** Parameters that are whole numbers (counts, ids, flags' values) raise
  `number has no integer representation` for a value like `2.5`.

### Addresses and objects

Things in the game are found by their **address** in the game's memory, a plain Lua integer.

- **Parts** and **vehicles** come as Lua objects that hold the address: a part's is `part.instance`, a
  vehicle's `v.address`. See [Part objects](#part-objects) and [Vehicle objects](#vehicle-objects).
- **Objects and characters** (`renut.objects`, `renut.actors`, the player's avatar) are plain address
  integers.
- An object or address can outlive what it stands for: a vehicle that left the level, an object that was
  removed. Use what the events give you, and forget it when its `removed` / `on_destroy` / `world_leave`
  event comes. Most functions check an address is in the game's heap (`not a vehicle`, `not a part`,
  `not an object in this world`), but not that it's still the same thing.
- Addresses work with [`rex.mem`](rexfabric-api.md) for reading the game's memory yourself.

### Asset ids and names

The game names its assets by a 32-bit **asset id** (an *aid*): the asset type in the top byte, then a
hash of the asset's name. Functions that take an asset accept, unless they say otherwise:

| Form | Example |
|---|---|
| The name with its `aid_<type>_` prefix | `"aid_objparams_banjox_vehicleblock_gadgets_horn"` |
| The name without the prefix | `"banjox_vehicleblock_gadgets_horn"` |
| The id as hex text | `"0x1F164B76"` |

Some take only a short name and add the rest themselves (parts take `"gadgets_horn"`, projectiles
`"egg"`): each says so. Names are hashed exactly as given, so use the game's lower-case names. The
types that come up: `0x04` model, `0x19` script (a scene), `0x1F` objparams, `0x3D` challenge,
`0x43` dialog.

### Handles and `:off()`

Every `.on(...)`, `renut.after`, `renut.every`, `renut.once` and `rex.on` returns a **handle**:

| Method | What |
|---|---|
| `h:off()` | Stops it: its function isn't called again. |
| `h:active()` | Whether it's still on. |
| `h.event` | The raw event name (for `rex.on` and the `.on` functions built on it). |

`renut.off(h)` does the same as `h:off()` and accepts `nil`. Exceptions: `renut.challenges.on`'s handle
has `:off()` only; `renut.settings.on_change` and `renut.camera.follow` return nothing.

### When you can call what

Most functions work at any time and on any thread. Those marked **event** below make the game *do*
something (spawn, damage, move it the game's way, play an effect...) and need the game's own context,
which reNut lends only while one of its **game events** runs:

- a [part callback](#part-callbacks) (`on_tick`, `on_use`, ...);
- a listener of a game event: `renut.parts.on`, `renut.vehicle.on`, `renut.game.on`, `renut.objects.on`,
  `renut.actors.on`, `renut.zones.on`, `renut.hud.on`, `renut.crates.on`, `renut.player.on`, or
  `rex.on` on the same [raw events](#raw-events-for-rexon);
- a [timer](#listeners-and-timers) (`renut.after`, `renut.every`), which runs inside the game tick.

**Called anywhere else**, an event function doesn't fail: it waits for the next **game tick**.

- Inside a callback (any `rex.on` listener, including `rex.on("frame")`; the listeners of
  `renut.npcs.on` and `renut.net`; a `renut.menu` row's `on_select`; a `renut.hud.menu` function;
  `rex.after` / `rex.every`), the callback **pauses** at that call, and goes on at the next game tick
  with the call's real result.
- Anywhere else (a script's top level, a `rex.hook` function, a coroutine of your own), the call is
  **queued** for the next game tick and returns `nil` at once; its result is lost.

The game tick runs once a frame **while a level runs** (it doesn't in the front-end menus), so a call
made from the main menu waits until a level is up.

The events of `renut.npcs`, `renut.net`, `renut.menu` and `rex.on("frame")` are not game events: event
functions called in them wait for the tick as above.

Two more cases:

- `renut.player.play` raises `this works inside the renut events` instead of waiting (see
  [the player](#the-player-renutplayer)).
- `renut.parts.add` adds new parts only while the game starts (see [below](#renutpartsadd)).

### Errors

Wrong arguments raise Lua errors at the call. Errors from the C++ side name the internal function,
for example `renut.vehicle._get: the vehicle has no body right now (it is being rebuilt)`. Errors in
your callbacks are logged with a stack trace and the game carries on; how the log and error reporting
work is in [mods-and-scripts.md](mods-and-scripts.md).

Functions whose names start with `_` (`renut.vehicle._get`, `renut.hud._panel`, ...) are what the public
functions are built on. They aren't documented here and can change; [internals.md](internals.md) covers
them.

---

## Vectors: `renut.vec`

```lua
local p = renut.vec(x, y, z)  -- missing components are 0
```

| Operation | Result |
|---|---|
| `v.x`, `v.y`, `v.z` | The components (read and write). |
| `a + b`, `a - b`, `-a` | Vectors. |
| `a * s`, `s * a`, `a / s` | Scaled by a number. |
| `a == b` | Whether every component is equal. |
| `v:length()` | Its length. |
| `v:normalized()` | A unit vector the same way; the zero vector for a zero vector. |
| `a:dot(b)` | Dot product (`b` can be any `{x, y, z}`). |
| `a:cross(b)` | Cross product. |
| `tostring(v)` | `vec(1, 2, 3)` |

```lua
local ahead = v:position() + v:forward() * 10
local flat = renut.vec(ahead.x, 0, ahead.z):normalized()
```

---

## Listeners and timers

| Function | Returns | What |
|---|---|---|
| `renut.off(handle)` | | Stops a listener or timer; `nil` is fine. |
| `renut.once(space, event, fn)` | handle | Listens to `space.on(event, ...)` for the first time only: `renut.once(renut.game, "world_enter", fn)`. `space` is any `renut` table with an `.on`. |
| `renut.after(seconds, fn)` | handle | Calls `fn()` once, `seconds` of game time from now. |
| `renut.every(seconds, fn)` | handle | Calls `fn()` every `seconds` of game time (at least 0.001) until stopped. |
| `renut.time()` | number | Game time in seconds since the scripts loaded. |
| `renut.wait(seconds)` | | Pauses the callback it's called from for `seconds` of game time, then carries on. |

**Game time** counts the game's ticks: it doesn't move while the game is paused or outside a level, and
one tick adds at most 0.1 seconds. Timers run inside the game tick, so [event](#when-you-can-call-what)
functions work in them at once. `seconds` must be 0 or more (`a timer's seconds are 0 or more`).

`renut.wait` works only inside a callback that can pause: a listener added with `rex.on` or an `.on`
built on it (all of them but `renut.challenges.on`), a menu row's `on_select`, a timer's function, or a
`rex.after` / `rex.every` function. Elsewhere (a script's top level, a part callback such as `on_tick`,
a `renut.hud.choice` answer, a `renut.challenges` function) it raises
`renut.wait works inside a renut callback (an event's, a timer's)`. While one call of a listener is
paused, the next event starts a new call of it alongside.

```lua
renut.game.on("world_enter", function(number, name)
  renut.wait(2)
  renut.hud.message("Welcome to " .. name)
end)
```

reNut also makes RexFabric's `rex.after` and `rex.every` run their functions as callbacks that can pause
(see [rexfabric-api.md](rexfabric-api.md) for those timers; unlike `renut.after`, they aren't tied to
the game tick).

---

## Parts: `renut.parts`

| Function | Returns | When | What |
|---|---|---|---|
| `renut.parts.add(def)` | the id (lower-cased) | startup | Adds a vehicle part. [Below](#renutpartsadd). |
| `renut.parts.list()` | list | any | Every custom part loaded (from `.nbpatch` files and Lua): `{id, mod, name, base, lua}` each, `lua` being whether this Lua state added it. |
| `renut.parts.get(instance)` | part or `nil` | any | Your own part's object (its one table) by its address, once an event has shown it. |
| `renut.parts.wrap(instance)` | part or `nil` | any | A part object for any part address: your part's own table, or a new table for any other part. `nil` for `nil` or `0`. |
| `renut.parts.aid(name)` | integer | any | A shipped part's objparams asset id from its name: `"gadgets_horn"` (the `aid_objparams_` and `banjox_vehicleblock_` prefixes are optional). As `part:aid()` and crate contents give it. |
| `renut.parts.owned(part)` | `total, available` | any | How many of a part player 1 has in the garage, and how many of those aren't on a vehicle. `part` is a name (as `aid`) or an asset id. `0, 0` before the garage inventory is up. |
| `renut.parts.on(event, fn)` | handle | any | Listens to every part in the game: `"damage"`, `"break"`, `"use"`, `"release"`. See [Event reference](#events-by-on). |

### `renut.parts.add`

```lua
renut.parts.add{
  id = "mymod_booster",
  base = "gadgets_horn",
  name = "Booster",
  on_use = function(part)
    local v = part:vehicle()
    if v and v:use("fuel", 5) then v:impulse(v:forward() * v:mass() * 8) end
  end,
}
```

Call it from a script's top level. Parts are added while the game starts: the part is in the garage,
owned, and saved vehicles keep it by its id. When the scripts reload, calling it again for a part the
game already has only replaces its table (its callbacks and `params`); an id the game doesn't have yet
raises `parts are added as the game starts: restart reNut to add '<id>'`. A part that had no callbacks
when the game started gets no events after a reload either: restart for that too.

**Keys of `def`**

| Key | Type | Default | What |
|---|---|---|---|
| `id` | string | required | 1-40 characters of `a-z`, `0-9` and `_` (it's lower-cased first). Unique across every mod; start it with your mod's name. Don't change it once players have vehicles using it. |
| `base` | string | none | The shipped part it copies: `"gadgets_horn"`, `"weapon_eggturret"`, ... (the `aid_objparams_` and `banjox_vehicleblock_` prefixes are optional). The copy keeps the base's physics, model, attach points, class and behaviour. Without a base, the part is a [block of its own](#blocks-of-their-own). |
| `name` | string | the base's | Its name in the garage. |
| `count` | integer | 1 | How many the player owns, clamped to 1-99. |
| `model` | string | the base's | A model asset: a shipped part model or one in your mod's bundles (the `aid_model_` and `banjox_vehicleparts_` prefixes are optional). |
| `attach` | string | the base's | Its footprint and attach points (the `aid_avatarhavokdata_` and `banjox_vehicleblock_` prefixes are optional). |
| `type` | string | none | A garage type the part is a size of: every part naming the same type (1-40 characters) shares it. |
| `fields` | table | none | Values written over the copied record, by byte offset: `{[0x3FC] = {f32 = 140}, [0x130] = {str = "colour_orange"}}`. Types: `f32`, `u32`, `i32`, `u16`, `u8`, `str` (text with its ending zero). One type per offset; offsets under `0x10000`. Offsets can also be strings such as `"0x3FC"`. |
| `button` | `"A"`, `"B"`, `"X"` or `false` | the base's | Gives the part an action button starting on that button (players can change it in the garage); `false` takes the base's away. |
| `base_action` | boolean | `false` | `true` keeps the base's own button action (a horn honks, a weapon fires) as well as your `on_use`. |
| `params` | any | `{}` | Given to every instance as `part.params`. One table shared by all of them: don't write to it. |
| `on_...` | functions | none | The [callbacks](#part-callbacks). A part with at least one is a new kind of part with behaviour; one without is a variant of its base. |

#### Blocks of their own

A part without `base` is made from scratch: a plain block (class `vehicleBlock`) with its own record.
[NutTool](nuttool/README.md) makes its model and footprint. These keys are then used:

| Key | Type | Default | What |
|---|---|---|---|
| `model` | string | required | Its model, in your mod's bundles. |
| `attach` | string | required | Its footprint, in your mod's bundles. |
| `category` | string | required | The garage category it's listed in: `"body"`, `"gadget"`, `"seat"`, ... |
| `mass` | number | 10 | |
| `health` | number | 5 | |
| `paint` | string | `"colour_orange"` | Its colour before the player paints it (a `paint_colours` name). |

**Errors** (bad or taken id, missing `model` / `attach` / `category` for a block, a `type` over 40
characters, an unknown field type, a bad `button`, a part added twice) are raised at the call.

### Part callbacks

The `on_...` functions of a `renut.parts.add` table. Each gets your [part object](#part-objects) first.
They run on the game thread inside a game event, so [event](#when-you-can-call-what) functions work in
them at once (but `renut.wait` doesn't).

| Callback | Arguments | When |
|---|---|---|
| `on_spawn` | `part` | The part was made (in the garage or with a vehicle). |
| `on_attach` | `part` | It was built onto a vehicle. |
| `on_detach` | `part` | It came off its vehicle. `part:vehicle()` is still that vehicle during the call. |
| `on_tick` | `part, dt` | Every frame while it's on a vehicle. |
| `on_throttle` | `part, throttle, dt` | Every frame on a vehicle, after `on_tick`: the driver's throttle (`v:throttle()`, `-1` to `1`). |
| `on_physics` | `part, dt` | Every physics step while it's on a vehicle: the place for forces. |
| `on_use` / `on_button_down` | `part` | Its action button went down. Both run if both are given. |
| `on_release` / `on_button_up` | `part` | Its action button went up. |
| `on_driver_in` | `part` | A driver got in (the vehicle powers its parts on). |
| `on_driver_out` | `part` | The driver got out. |
| `on_damage` | `part, amount` | It's about to lose `amount` health. [`renut.set_amount`](#changing-damage-renutset_amount) changes the amount. |
| `on_break` | `part` | It's breaking off its vehicle. |
| `on_collide` | `part, strength, impact, other` | The frame's strongest hit on its vehicle landed on this part: `strength` a number, `impact` a vector (the hit's push), `other` the address of what it hit or `nil`. |
| `on_destroy` | `part` | The part is gone. Forget anything you keep about it. |

An error in a callback is logged as `[<mod>] <id>.<callback>: <message>`. After 20 errors, that part's
callbacks are switched off until the scripts reload.

### Part objects

A part object stands for one part on any vehicle. Your own parts (with callbacks) keep **one table** from
the first event that shows them until `on_destroy`, so you can keep state in it (`part.heat = 0`). Any
other part's object is a new table each time.

| Field | What |
|---|---|
| `part.instance` | Its address. |
| `part.id` | Your part's id; `nil` for shipped parts. |
| `part.params` | Its `params` (your parts; `nil` for shipped parts). |
| `part == other` | Whether both are the same part. |
| `tostring(part)` | `part(mymod_booster, 0x4A1B2C30)` (the class name for other parts). |

| Method | Returns | When | What |
|---|---|---|---|
| `part:def()` | table or `nil` | any | The table given to `renut.parts.add` (your parts only). |
| `part:vehicle()` | vehicle or `nil` | any | The [vehicle](#vehicle-objects) it's on. |
| `part:class()` | string | any | Its game class: `vehicleBlock`, `vehicleBlockEngine`, `vehicleBlockWheel`, `vehicleBlockWeapon`, ... |
| `part:is_a(class)` | boolean | any | Whether it's that class or derives from it. |
| `part:aid()` | integer | any | Its record's asset id. |
| `part:position()` | vector | any | Where it is in the world. |
| `part:cell()` | `x, y, z` | any | Its grid cell on the vehicle (integers). |
| `part:orientation()` | integer | any | Which way it's turned on the grid, `0`-`23`. |
| `part:health()`, `part:max_health()` | number | any | |
| `part:health_fraction()` | number | any | `0`-`1`. |
| `part:damage_level()` | integer | any | How damaged it looks, from `0` (fine). |
| `part:set_health(h)` | | any | Sets its health, clamped to `0`-max; the game works out its damage level again. |
| `part:repair()` | | any | Back to full health. |
| `part:damage(amount)` | boolean | event | Damages it the game's way; when that should break it off, it breaks off and this returns `true`. |
| `part:break_off()` | | event | Breaks it off its vehicle (the vehicle splits on its next update). |
| `part:press(control)`, `part:release(control)` | | event | Presses or lets go of its button as a driver would (fire a weapon, honk a horn). `control` defaults to `0`. Does nothing on a part that's off a vehicle. |

`part:damage`, `part:break_off` and `part:press` / `part:release` call the game's own functions
directly, so they don't fire the `damage`, `break` and `use` / `release` events.

### Weapons and projectiles

| Method | Returns | When | What |
|---|---|---|---|
| `part:fire(position, velocity, options)` | projectile address or `nil` | event | Launches one projectile from `position` at `velocity` (units per second). Uses no ammo: spend it with `v:use("ammo", n)` first. Without `options.projectile` the part must be a weapon, and fires its own projectile. `nil` when none is ready. |
| `part:can_fire(options)` | boolean | event | Whether `fire` with these options would launch one now. `options.projectile` is required. |
| `part:hold(held)` | | any | Holds (`true`, the default) or lets go of a weapon's trigger: it fires as the game's weapon does, with its rate, ammo and aim. Weapons only. |
| `part:ammo_per_shot()` | number | any | The ammo each of the weapon's own shots takes. Weapons only. |
| `part:muzzle()` | `position, direction` or `nil` | any | Where the barrel ends and which way it points as of the weapon's last tick (where its own shots start); `nil` before its first tick. Weapons only. |

`options` for `fire` and `can_fire` (all optional for `fire`; a quaternion table on its own is taken as
`rotation`):

| Option | Default | What |
|---|---|---|
| `projectile` | the weapon's own | A [`renut.projectiles`](#other-values) name (`"egg"`), another projectile's short name (`"name"` means `banjox_avatar_projectile_name`), a full `"aid_objparams_..."` name, or an asset id (`0x1F......`). Any part with callbacks can fire any projectile this way, whatever its base. |
| `rotation` | `{x = 0, y = 0, z = 0, w = 1}` | The projectile's rotation (a quaternion). |
| `max_flying` | 10 | How many of that projectile the part can have in the air at once, 1-30. Set by the first shot of each kind. |

- "The part's base has to be a weapon" applies to `fire` without `projectile`, `hold`, `ammo_per_shot`,
  `muzzle` and the aim functions below: other parts raise `the part isn't a weapon (its base has to be one)`.
- A weapon has no projectiles of its own until it has been on a vehicle for a tick: `fire` returns `nil`
  until then.
- With `projectile`, the part must have callbacks (`only parts added with renut.parts.add (with
  callbacks) fire a projectile of their choice`) and be on a vehicle (`the part isn't on a vehicle`). The
  first `fire` or `can_fire` of each kind makes that part's projectiles of that kind; they join the level
  over the next two ticks, and `fire` returns `nil` until then. Check `can_fire` before spending ammo.
- A projectile works only where its asset is loaded; elsewhere `fire` and `can_fire` raise
  `that projectile isn't loaded here (its objparams asset isn't in this level)`. Use `pcall` for kinds
  that may be missing.
- A part's projectiles of a chosen kind are let go when it leaves its vehicle or is destroyed (those in
  the air land first). Projectiles fired with `options.projectile` belong to no weapon.

```lua
on_use = function(part)
  local v = part:vehicle()
  local opts = {projectile = "grenade"}
  if part:can_fire(opts) and v:use("ammo", 1) then
    part:fire(part:position() + v:up() * 1.5, v:forward() * 40 + v:velocity(), opts)
  end
end,
```

### Turrets, axes, bones, shape keys and paint

| Method | Returns | When | What |
|---|---|---|---|
| `part:axes()` | `right, up, forward` | any | The part's own axes in the world (vectors). |
| `part:aim_at(direction, signs)` | | any | Turns a weapon's barrel to point along world `direction` (within its limits), and the gunner seat's animation with it. Call it every tick to hold the aim. `signs = {yaw_sign, pitch_sign, seat_yaw_sign}`, `-1` for any that turns the wrong way (default `1`). |
| `part:aim(yaw, pitch, seat_yaw_sign)` | | any | Sets the barrel's angles directly, in **radians** from the part's forward: yaw positive to the left (wrapped), pitch positive down (clamped to the weapon's limits). Weapons only. |
| `part:aim_angles()` | `yaw, pitch` | any | The barrel's angles now (radians). Weapons only. |
| `part:bone_count()` | integer | any | How many bones its model has (`0` without a model). |
| `part:bone(index, angle, axis)` | | event | Turns bone `index` by `angle` radians about `axis` (a vector in the bone's parent's space; default x) at its joint, in place of its pose. Call it every tick (`on_tick`) to hold it. Does nothing while the part is off a vehicle. |
| `part:shape(name)` | number or `nil` | any | The weight of its model's shape key `name` on this part; `nil` when the model has none of that name. |
| `part:set_shape(name, weight)` | | any | Sets that weight on this part only (`0` none, `1` all of it). Raises when the model has no such shape. [`renut.shapes.set`](#shape-keys-renutshapes) sets it for every part with the model. |
| `part:paint()` | `r, g, b` | any | Its paint colour, each `0`-`1`. |
| `part:set_paint(colour, effect)` | | event | Paints it (a [colour](#positions-places-angles-and-colours)); `effect` (default `false`) is passed on to the game's paint function as its effect flag. The new paint shows from the next frame. |

Bone indices are those NutTool's model export lists for the model. A bone the base moves itself (a
wheel's tyre, a turret's barrel) keeps being moved by it.

---

## Vehicles: `renut.vehicle`

| Function | Returns | When | What |
|---|---|---|---|
| `renut.vehicle.at(address)` | vehicle or `nil` | any | A [vehicle object](#vehicle-objects) for an address (`nil` for `nil` or `0`). |
| `renut.vehicle.list()` | list of vehicles | any | Every vehicle in the game now (from when it's made until it leaves its level). |
| `renut.vehicle.blueprints()` | list | any | What `spawn` can build: `{name, file}` each. `name` is `"<mod>: <vehicle>"` for each enabled mod's `vehicles/*.blueprint`, and `"save: <vehicle>"` for the vehicles in the save folder. |
| `renut.vehicle.spawn(blueprint, at, yaw)` | vehicle or `nil` | event | Builds a whole vehicle from a blueprint, as the garage does, at a [place](#positions-places-angles-and-colours) (default `"front"`), turned `yaw` degrees (default 0). **Experimental:** a spawned vehicle may not drive, and may fall apart. |
| `renut.vehicle.seat(v, actor, seat)` | | event | Seats a character in a vehicle on its next update. |
| `renut.vehicle.on(event, fn)` | handle | any | Listens to every vehicle. See [Event reference](#events-by-on). |

**`spawn`.** `blueprint` is a name from `blueprints()` (`"mymod: buggy"`), the part after the `: `
alone (`"buggy"`), or a blueprint file's path. The vehicle is put down on the ground under the place (a vector
place is never moved lower than given). Errors: `'<blueprint>' isn't a vehicle blueprint`,
`vehicles are spawned in a world`, and an asset message when a part's asset isn't loaded in this world.

**`seat`.** `v` is a vehicle object or address; `actor` a [character](#characters-renutactors) (address or
who-string, default `"banjo"`); `seat` is `"driver"` (default), `"passenger"`, `"other"`, or the game's
seat type `2`, `4` or `8`.

### Vehicle objects

A vehicle object has one field, `v.address`. Two objects for the same vehicle are `==`, and it prints as
`vehicle(0x...)`.

**Motion.** These read and write its physics body. While a vehicle rebuilds (after a part breaks off) it
has no body for a moment and they raise `the vehicle has no body right now (it is being rebuilt)`: check
`v:has_body()` first.

| Method | Returns | When | What |
|---|---|---|---|
| `v:has_body()` | boolean | any | Whether it has a physics body now. |
| `v:position()` / `v:set_position(p)` | vector / | any | Where it is; setting it moves it there at once (with its centre of mass). |
| `v:move(d)` | | any | Moves it by `d`. |
| `v:velocity()` / `v:set_velocity(vel)` | vector / | any | Units per second. Setting it doesn't wake a resting body. |
| `v:angular_velocity()` / `v:set_angular_velocity(w)` | vector / | any | Radians per second about each world axis. |
| `v:right()`, `v:up()`, `v:forward()` | vector | any | Its axes. |
| `v:speed()` | number | any | Its speed along its forward axis (negative going backwards). |
| `v:rotation()` / `v:set_rotation(q)` | quaternion / | any / event | Its rotation. Setting it turns it about its position and wakes it. |
| `v:yaw()` / `v:set_yaw(degrees)` | number / | any / event | Its heading in degrees; setting it stands it upright facing that way. |
| `v:mass()` | number | any | Its mass (`math.huge` for an immovable body). |
| `v:scale_mass(scale)` | | any | Multiplies its mass and inertia by `scale` (above 0). Each call multiplies again. |
| `v:push(force, dt, point)` | | event | Applies `force` for `dt` seconds (`dt` is required). With `point` (a world position) it acts there and can spin the vehicle. Wakes it. Call it each step from `on_physics` for a thrust. |
| `v:impulse(j, point)` | | event | A kick: changes its velocity by `j / mass` at once; with `point`, at that point (spinning it too). Wakes it. |
| `v:angular_impulse(j)` | | event | A spin kick. |
| `v:in_air(reach)` | boolean | event | Whether nothing is within `reach` units (default 1.5) below its lowest layer of parts. The rays pass through the vehicle itself. |

**Driver.**

| Method | Returns | When | What |
|---|---|---|---|
| `v:input()` | table | any | `{steer, stick_y, brake, accelerate, braking}`: `steer` and `stick_y` `-1`-`1`, `brake` and `accelerate` `0`-`1`, `braking` a boolean. A vehicle without wheels has its driver's pad read instead, which works only inside a game event (`0` elsewhere). |
| `v:throttle()` | number | any | `accelerate - brake`: `1` full throttle, `-1` full brake or reverse. |
| `v:steering()` | number | any | `-1` to `1`. |
| `v:driver()` | address or `nil` | event | The driver (a character's address). |

**Health.**

| Method | Returns | When | What |
|---|---|---|---|
| `v:health()`, `v:max_health()` | number | any | |
| `v:health_fraction()` | number | any | `0`-`1`. |
| `v:damage_level()` | integer | any | How damaged it looks, from `0`. |
| `v:set_health(h)` | | event | Sets it, clamped to `0`-max; `0` disables the vehicle. |
| `v:repair()` | | event | Full health, not disabled. |
| `v:damage(amount, part, attacker)` | | event | Damages it the game's way. `part` is a part object or `nil`, `attacker` an address or `nil`. |
| `v:disabled()` | boolean | any | Whether it's disabled. |
| `v:invulnerable()` / `v:set_invulnerable(on)` | boolean / | any | Whether it ignores damage; `on` defaults to `true`. |

`v:set_health` and `v:damage` call the game directly, so they don't fire the `health` and `damage`
events.

**Fuel, ammo and power.** A vehicle has three resources: `"fuel"`, `"ammo"` and `"power"` (or `0`, `1`,
`2`). Other values raise `a resource is "fuel", "ammo" or "power"`.

| Method | Returns | When | What |
|---|---|---|---|
| `v:resource(kind)`, `v:max_resource(kind)` | number | any | How much it has, and its most. |
| `v:set_resource(kind, value)` | | any | Sets it, clamped to `0`-most (a vehicle without tanks has a most of 0). |
| `v:use(kind, amount)` | boolean | event | Uses some up the game's way; whether there was enough. |
| `v:infinite(kind)` / `v:set_infinite(kind, on)` | boolean / | any | Whether it never runs out; `on` defaults to `true`. |
| `v:fuel()`, `v:max_fuel()`, `v:set_fuel(x)` | | any | The same for `"fuel"`. |
| `v:ammo()`, `v:max_ammo()`, `v:set_ammo(x)` | | any | The same for `"ammo"`. |

**Parts and the rest.**

| Method | Returns | When | What |
|---|---|---|---|
| `v:parts()` | list of parts | any | Every part on it. |
| `v:find_parts(class)` | list of parts | any | Those that are (or derive from) a class: `v:find_parts("vehicleBlockWheel")`. |
| `v:name()` / `v:set_name(name)` | string / | any | Its name (up to 63 characters). |
| `v:level()` | address | any | The level it's in. |
| `v:remove()` | | event | Removes it from the game. Raises `not a vehicle in the game` for an unknown one and `the player is in it` while player 1 drives it. |

```lua
renut.vehicle.on("tick", function(v, dt)
  if v:has_body() and v:position().y < -50 then
    v:set_position(v:position() + renut.vec(0, 80, 0))
    v:set_velocity(renut.vec())
  end
end)
```

---

## Changing damage: `renut.set_amount`

```lua
renut.set_amount(x)
```

Inside a damage event (a part's `on_damage`, `renut.parts.on("damage")`, `renut.vehicle.on("damage")`,
or their raw events), changes the damage about to be dealt; negative values become `0`, so `0` blocks
it. Every listener is given the original amount, and the last call wins. Elsewhere it raises
`renut.set_amount works inside a damage event`; NaN or infinity raises `not a finite amount`.

```lua
renut.vehicle.on("damage", function(v, amount)
  if v == renut.player.vehicle() then renut.set_amount(amount / 2) end
end)
```

---

## Effects: `renut.fx`

```lua
local played = renut.fx.play(effect, position, vehicle, seconds)
```

**Event.** Plays one of the game's particle effects.

| Argument | Default | What |
|---|---|---|
| `effect` | required | The effect: its id as a number (`0x4582A557`), its id as hex text, or its asset name (`"actor_banjo_jumpland"`). [`renut.fx.Effect`](#other-values) has every known name. |
| `position` | `"banjo"` | A vector or a [place](#positions-places-angles-and-colours). |
| `vehicle` | Banjo | A vehicle object whose level it plays in; without one, Banjo's level. |
| `seconds` | its own length | How long it plays. Give it for effects that would otherwise keep going. |

Returns `true` if it played, `false` if this level hasn't loaded that effect. Raises
`the game has no effect named '<name>'` for an unknown name and `no level with effects to play it in`
outside a level.

---

## Game state: `renut.game`

| Function | Returns | When | What |
|---|---|---|---|
| `renut.game.world()` | `number, name, scene` or `nil` | any | The running world: its number, its name for the game's own worlds (`""` otherwise) and its scene's asset id. `nil` outside a level. |
| `renut.game.paused()` | boolean | any | Whether the pause menu is up. |
| `renut.game.set_paused(paused)` | | event | Opens (`true`, the default) or closes the pause menu, as Start and Resume do. |
| `renut.game.time_scale()` | number | any | The game's speed (1 normal). |
| `renut.game.set_time_scale(scale)` | | any | Runs the game faster or slower: `0.05` to `4`, default `1`. |
| `renut.game.save()` | boolean | event | Saves the game as the game does; whether a save began. Raises `no save is loaded`. |
| `renut.game.flag(name)` | boolean | any | A game flag. |
| `renut.game.set_flag(name, on)` | | any | Sets a game flag (`on` defaults to `true`). |
| `renut.game.counter(name)` | integer | any | A game counter. |
| `renut.game.set_counter(name, value)` | | any | Sets a counter (a 32-bit integer). |
| `renut.game.add_counter(name, amount)` | | any | Adds a whole `amount` to a counter, quietly. |
| `renut.game.award(counter, amount, show)` | | event | Awards a counter the game's way (Jiggies, notes...): `amount` (default 1, at least 1), with the game's pop-up unless `show` is `false`. |
| `renut.game.challenge()` | asset id or `nil` | any | The challenge being played. |
| `renut.game.challenge_id(name)` | integer | any | A challenge's asset id from its name, *without* the `aid_challenge_` prefix. |
| `renut.game.challenge_start(challenge)` | | event | Starts one of the game's challenges by its asset (`"aid_challenge_..."`, the bare name, or hex text `"0x3D......"`; not an integer). It must be loaded in this world. |
| `renut.game.challenge_end(won)` | | event | Ends the challenge being played as won (`true`, the default) or lost, the game's way. |
| `renut.game.challenge_time(limit, elapsed)` | | event | Sets the running challenge's time limit and time so far, in seconds (`elapsed` defaults to 0; a `limit` of 0 makes it untimed). |
| `renut.game.challenge_hold(held)` | | any | Holds (`true`, the default) or lets go of the running challenge's timer. |
| `renut.game.on(event, fn)` | handle | any | `"world_enter"`, `"world_leave"`, `"challenge_start"`, `"challenge_finish"`, `"paused"`, `"resumed"`, `"tick"`, `"save"`, `"load"`. See [Event reference](#events-by-on). |

**Flag and counter names** are the game's own: `"gameFlag_Normal_..."`, `"gameFlag_Global_..."`,
`"gameFlag_Volatile_..."` for flags, `"gameCounter_BanjoX_..."` for counters, or a counter's short name
without that prefix (`"JiggiesBanked"`). The node editor lists them all ([node-editor.md](node-editor.md)).
They raise `no game flag or counter '<name>'`, `no save is loaded` before a save is loaded,
`'<name>' is a counter` / `is a flag` for the wrong kind, and in an online session whose host doesn't
allow it, `this online session's host doesn't allow changing the game`.

**Challenge errors:** `a challenge is running already`, `that challenge isn't loaded in this world`,
`a challenge is won or lost while it's being played`, `there's no challenge running`.

```lua
renut.game.on("challenge_finish", function(challenge, won)
  if won then renut.game.award("JiggiesBanked", 1) end
end)
```

---

## Crates: `renut.crates`

Crates are named by their game flag (`"gameFlag_Normal_ShowdownTown_CollectedCrate_01"`, ...).

| Function | Returns | When | What |
|---|---|---|---|
| `renut.crates.list()` | list of strings | event (first call) | Every crate's flag. The crates are read from the game the first time, which needs the game. |
| `renut.crates.contents(flag)` | list | event (first call) | What a crate gives: `{aid, count}` each (part asset ids, see `renut.parts.aid`). |
| `renut.crates.swap(flag, other)` | | any | From now on, crate `flag` gives what crate `other` holds. `other` `nil`, `""` or `flag` itself undoes it. The swap also applies to crates already collected, each time a level loads. |
| `renut.crates.give(flag, show)` | | event | Gives a crate as collecting it does; `show` (default `true`) shows its contents. |
| `renut.crates.in_world()` | list | any | The crates in the running world: `{flag, position, object}` each. |
| `renut.crates.spawn(flag, at)` | object or `nil` | event | Makes a crate at a place (default `"front"`), even one already collected. |
| `renut.crates.on("award", fn)` | handle | any | `fn(flag, contents)` before any crate is given (collected, bought, or given by a script); a `swap` made in it changes what this one gives. |

`spawn` needs reNut to have seen the game make a crate this session (Showdown Town places them):
before that it raises `reNut hasn't seen a crate yet`. Other errors: `no crate with the flag '...'`,
`a crate is named by its flag, gameFlag_...`, `crates are given in a world`.

---

## Objects: `renut.objects`

An object is anything the game has made in the running world, by its address. Its **kind** is the name
of what it was made from: its objTag without `objTag_BanjoX_` (else its class name), learned as the game
makes each kind this session. When two assets share a tag, the later one is `"<tag>@<AID>"`. Mods'
characters are `"<world>/<id>"`, game characters their character name.

| Function | Returns | When | What |
|---|---|---|---|
| `renut.objects.kinds()` | list | any | Every kind the game has made this session: `{name, class}` each. |
| `renut.objects.list()` | list | any | The objects in the running world: `{object, kind, position}` each (`kind` `""` when not known). |
| `renut.objects.spawn(kind, at, yaw)` | object or `nil` | event | Makes an object at a place (default `"front"`), turned `yaw` degrees (default 0). `kind` is a kind from `kinds()`, or an objparams asset (any [asset form](#asset-ids-and-names)). |
| `renut.objects.remove(object)` | | event | Removes it (and what it holds). |
| `renut.objects.kind(object)` | string | any | Its kind (`""` when not known). |
| `renut.objects.position(object)` | vector | any | Where it is. |
| `renut.objects.is_character(object)` | boolean | any | Whether it's a character in this world. |
| `renut.objects.move(object, at)` | | event | Moves it to a position (a vector or `{x, y, z}` / `{1, 2, 3}`; not a place string). |
| `renut.objects.turn(object, yaw, pitch, roll)` | | event | Sets its rotation in degrees (each defaults to 0). |
| `renut.objects.yaw(object)` | number | any | Its heading in degrees. |
| `renut.objects.on(event, fn)` | handle | any | `"spawn"`, `"removed"`, `"collected"`. See [Event reference](#events-by-on). |

**Spawning other worlds' things.** When what you spawn needs assets this world hasn't loaded, reNut loads
the world bundle that has them into this world (until you leave it). That takes memory; when there's no
room it raises `there's no memory left in this world for another world's bundles` or
`there's no room left for another world's models here`. An asset no bundle has raises
`it needs an asset ... (0x........): it may be another world's own version`. A kind the game hasn't made
this session that also isn't an objparams asset raises `the game hasn't made a '<kind>' this session`.

Functions that take an object raise `not an object in this world` for one that isn't in the running
world's list.

---

## Characters: `renut.actors`

Characters are Banjo, the NPCs, the Gruntbots and anything a mod spawned: objects whose class is the
game's actor class.

**Naming a character.** Every function below takes `actor` as either a character's address or a
**who-string**:

| Who | Means |
|---|---|
| `nil`, `""` or `"nearest"` | The character nearest Banjo, other than him. |
| `"banjo"` | Banjo (player 1). |
| `"last"` | The last character a mod spawned, while it's still in the world. |
| a name | The nearest character of that name: a game character (`"npc_klungo"`), a kind, or a mod's `"<world>/<id>"`. |
| digits | That address, as text. |

Errors: `not a character in this world`, `there's no character in this world`,
`there's no '<name>' in this world`, `Banjo isn't in this world`, `nothing a mod spawned is still in this
world`. Functions that use a character's strategy, mind or body raise `that character has no
strategy` / `mind` / `body` for one without it.

| Function | Returns | When | What |
|---|---|---|---|
| `renut.actors.list()` | list | any | The characters in the running world: `{object, kind, position}` each, `kind` its name. |
| `renut.actors.find(who)` | address or `nil` | any | The character a who-string names. |
| `renut.actors.spawn(character, at, yaw)` | address or `nil` | event | Spawns a character at a place (default `"front"`), turned `yaw` degrees. `character` is a game character's name (the node editor lists them; `"npc_klungo"`, `"gruntbot_blocker"`, ...) or a mod character `"<world>/<id>"` ([renut.npcs](#mod-characters-renutnpcs)). Its bundles load into this world first (see [Spawning other worlds' things](#objects-renutobjects)). Raises `no game character '<name>'`. |
| `renut.actors.place(actor, at)` | | event | Moves it to a place (default `"banjo"`). |
| `renut.actors.turn(actor, yaw)` | | event | Turns it to `yaw` degrees. |
| `renut.actors.yaw(actor)` | number | any | Its heading in degrees. |
| `renut.actors.play(actor, animation)` | | event | Plays one of its animations by the game's name. |
| `renut.actors.state(actor)` | string | any | Its strategy's state (what it's doing), by the game's name without `actorStrategyId_`; `""` when unknown. |
| `renut.actors.set_state(actor, state)` | | event | Puts its strategy in that state. Raises `no state '<name>'`. |
| `renut.actors.body_state(actor, state)` | boolean | event | Sets its body's animation state (the game's name without `actorBodyStateId_`); `false` when its body has no such state. |
| `renut.actors.current_body_state(actor)` | string | any | Its body state now (`"Idle"`, `"Jump"`, ...); `""` for none. |
| `renut.actors.action(actor, action)` | boolean | event | Plays one of its mind's actions by name (`"npc_idle"`); `false` when it has none of that name. |
| `renut.actors.walk_to(actor, at)` | | event | Clears its goals and walks it to a place (default `"banjo"`). |
| `renut.actors.stop(actor)` | | event | Clears its goals. |
| `renut.actors.follow(actor, target)` | | event | Makes it follow a character or vehicle: `target` is an address, a who-string (default `"banjo"`) or a vehicle object. Raises `that can't be followed` and `a character can't follow itself`. |
| `renut.actors.unfollow(actor)` | | event | Stops following, and clears its goals. |
| `renut.actors.target(actor)` | address or `nil` | any | What it's targeting or following. |
| `renut.actors.talk(actor, dialog)` | boolean | event | Starts one of the game's dialogs with this character as the speaker: `dialog` is a dialog asset (any [asset form](#asset-ids-and-names)). `false` (nothing happens) during a cutscene or another dialog. |
| `renut.actors.health(actor)` | `health, max` | any | |
| `renut.actors.set_health(actor, health)` | | event | Sets its health (0 or more): `0` kills it; more brings it back to life, raising its most to `health` if needed. |
| `renut.actors.damage(actor, amount)` | number | event | Damages it the game's way; its health after. |
| `renut.actors.defeat(actor)` | | event | Defeats it, the game's way. |
| `renut.actors.remove(actor)` | | event | Removes it from the world (not Banjo: `Banjo can't be removed`). |
| `renut.actors.vehicle(actor)` | vehicle or `nil` | any | The vehicle it rides. |
| `renut.actors.exit_vehicle(actor)` | boolean | event | Knocks it out of its vehicle; whether it was in one. |
| `renut.actors.velocity(actor)` | vector | event | Its velocity, as the game reports it. |
| `renut.actors.push(actor, v)` | | event | Adds velocity `v` (a vector or `{1, 2, 3}`), as the game's knockbacks do. |
| `renut.actors.on_ground(actor)` | boolean | event | Whether it stands on the floor. |
| `renut.actors.scale(actor)` | number | event | Its scale. |
| `renut.actors.set_model(actor, model)` | | event | Swaps its model for a model asset that's loaded in this world (`that model isn't loaded in this world` otherwise). |
| `renut.actors.on(event, fn)` | handle | any | `"spawn"`, `"hit"`, `"defeated"`. See [Event reference](#events-by-on). |

The node editor lists the state, body state and character names ([node-editor.md](node-editor.md)).

```lua
renut.game.on("world_enter", function()
  local guard = renut.actors.spawn("gruntbot_blocker", "front", 0)
  if guard then renut.actors.follow(guard, "banjo") end
end)
```

---

## Trigger zones: `renut.zones`

A zone is a sphere or a box that watches Banjo, his vehicle or the characters, and fires `enter` and
`exit` as they come and go. Zones are checked once per game tick. Zone names are shared by every mod.

| Function | Returns | When | What |
|---|---|---|---|
| `renut.zones.add(def)` | the name | any | Adds a zone, or replaces the one of that name. |
| `renut.zones.move(name, at)` | | any | Moves its centre to a place (default `"banjo"`). Raises `no zone '<name>'`. |
| `renut.zones.remove(name)` | | any | Removes it (no `exit` events). |
| `renut.zones.list()` | list of strings | any | Every zone's name. |
| `renut.zones.count(name)` | integer | any | How many watched things are inside it now. |
| `renut.zones.inside(name, object)` | boolean | any | Whether `object` (an address) is inside; without `object`, whether Banjo is. |
| `renut.zones.on(event, fn)` | handle | any | `"enter"`, `"exit"`: `fn(zone, object, what)`. See [Event reference](#events-by-on). |

`def`:

| Key | Default | What |
|---|---|---|
| `name` | required | Its name. |
| `at` | `"banjo"` | Its centre: a vector or a [place](#positions-places-angles-and-colours), fixed when added. |
| `radius` | 5 | A sphere's radius. |
| `size` | none | A vector of the box's full width, height and depth (axis-aligned); giving it makes a box. A size of 0 is 5. |
| `watch` | `"banjo"` | `"banjo"`, `"vehicle"` (his vehicle), `"characters"` (every other character), `"all"`, or a list of these. |
| `world` | any world | Only in that world: a game world's name (as `renut.game.world()` gives it) or a mod world's id. |

A zone stays across worlds; when a world goes, what was inside is forgotten without `exit` events.

```lua
renut.zones.add{name = "mymod_pit", at = renut.vec(10, 0, 40), size = renut.vec(8, 4, 8), watch = {"banjo", "vehicle"}}
renut.zones.on("enter", function(zone, object, what)
  if zone == "mymod_pit" then renut.hud.message("Mind the pit!", 2) end
end)
```

---

## The HUD: `renut.hud`

Two kinds of HUD: the **game's own** (its message box and a challenge's timer/score panel), and
**drawn** text, bars, markers and menus that reNut puts on screen through RexFabric's overlay
([`rex.draw`](rexfabric-api.md)).

### The game's message box and panel

| Function | Returns | When | What |
|---|---|---|---|
| `renut.hud.message(text, seconds)` | | event | Shows the game's message box with `text` (no buttons) for `seconds` (default 3; `0` or less keeps it up). |
| `renut.hud.close()` | | event | Closes the message box. |
| `renut.hud.choice(text, fn)` | id or `nil` | event | Shows the message box with two buttons, confirm (A) and cancel (B). When one is pressed, `fn(confirmed)` is called (`true` for A). Returns the choice's id (`nil` when the call was queued, see [When you can call what](#when-you-can-call-what)). |
| `renut.hud.timer(seconds, run)` | | event | Sets the panel's timer and shows the panel with it. `run`: `"down"` (default), `"up"` or `"hold"`. |
| `renut.hud.counter(value, target)` | | event | Sets the panel's counter (an integer) and shows the panel with it. With a `target` above 0 it reads `value/target`. |
| `renut.hud.panel{timer = bool, counter = bool}` | | event | Shows the panel with the parts asked for. |
| `renut.hud.set_timer(seconds, run)` | | any | Sets the timer without showing the panel. |
| `renut.hud.set_counter(value, target)` | | any | Sets the counter without showing the panel. |
| `renut.hud.time()` | number | any | The timer's seconds now. |
| `renut.hud.hide()` | | event | Hides the panel (and resets it). |
| `renut.hud.on(event, fn)` | handle | any | `"timer_done"`: `fn()` when a countdown reaches 0. `"choice"`: `fn(id, confirmed)` for every choice answered. |

- The panel's parts are chosen as it's shown. `hud.timer` or `hud.counter` while the panel is up without
  that part raises `the panel is up without that part: hide it, then show it with both
  (renut.hud.panel)`. To show both, set them with `set_timer` / `set_counter`, then call
  `renut.hud.panel{timer = true, counter = true}`.
- The timer counts in real time between game ticks (at most 0.1 s a tick), and is put on screen each tick.
- The message box and panel go with the world: they're gone after a world change.
- Errors: `there's no HUD up` / `there's no HUD up for the player` (in menus or loads),
  `a timer's seconds are 0 or more`, `a timer counts down, up, or holds`.

```lua
renut.hud.choice("Race the clock?", function(yes)
  if yes then renut.hud.timer(60, "down") end
end)
renut.hud.on("timer_done", function() renut.hud.message("Time's up!") end)
```

### Drawn text, bars and markers

These draw through RexFabric's overlay and work any time. `key` names the item within your mod (two mods'
keys never clash): drawing with the same key again replaces it. Positions and sizes are fractions of the
screen (`0`-`1`, `x` across, `y` down); what `size`, `align` and `seconds` mean exactly is RexFabric's
[`rex.draw`](rexfabric-api.md).

| Function | What |
|---|---|
| `renut.hud.text(key, text, opts)` | Text on screen. |
| `renut.hud.bar(key, fraction, opts)` | A bar filled to `fraction` (`0`-`1`), on a background, with an optional label. |
| `renut.hud.marker(key, at, text, opts)` | A dot over a place in the world, redrawn each frame where the camera sees it: `at` is a position or an object's address (it follows the object, and goes when the object does). `text` (optional) is written above it. Markers go when the world goes. |
| `renut.hud.remove(key)` | Removes one of your mod's texts, bars or markers. |
| `renut.hud.clear()` | Removes all of your mod's. |

| `text` option | Default | | `bar` option | Default |
|---|---|---|---|---|
| `x`, `y` | `0.5`, `0.15` | | `x`, `y` | `0.35`, `0.9` (top left) |
| `size` | `0.035` | | `width`, `height` | `0.3`, `0.025` |
| `colour` | white | | `colour` | `0x3CC864` |
| `alpha` | | | `alpha` | |
| `align` | | | `back_colour`, `back_alpha` | `0x000000`, `0.6` |
| `background` | | | `label` | none |
| `seconds` | `rex.draw`'s | | `seconds` | `rex.draw`'s |

`marker` options: `colour` (default `0xFFD040`), `alpha`, `size` (the dot, default `0.012`), `text_size`
(default `0.025`).

### Menus

```lua
renut.hud.menu{
  title = "Teleport",
  options = {"Start", "Top of the hill", "Cancel"},
  fn = function(index, label)
    if index == 2 then renut.player.teleport(renut.vec(0, 120, 0)) end
  end,
}
```

| Function | What |
|---|---|
| `renut.hud.menu(def)` | Opens a menu of `def.options` (a list of strings, at least one) with an optional `def.title`. Up and down move (d-pad, left stick, arrow keys, mouse), A, Return or a click picks, B or Escape closes. The game doesn't see the pad while it's open. `def.fn(index, label)` is called with the pick, or `fn(nil, nil)` when closed. Opening a menu closes one already open (its `fn` gets `nil`). |
| `renut.hud.close_menu()` | Closes it (its `fn` gets `nil`). |
| `renut.hud.menu_open()` | Whether one is open. |

`fn` runs from a frame callback, not a game event: event functions called in it
[wait for the next game tick](#when-you-can-call-what).

---

## Debug drawing: `renut.debug`

Lines, spheres, boxes and text at world positions, redrawn each frame from where the camera is (shown
only while a world runs). Any time.

| Function | What |
|---|---|
| `renut.debug.line(a, b, opts)` | A line between two positions. |
| `renut.debug.sphere(at, radius, opts)` | A circle the size of a sphere of `radius` (default 1) there. |
| `renut.debug.box(at, size, opts)` | The edges of an axis-aligned box: `at` its centre, `size` its full size (a vector). |
| `renut.debug.text(at, text, opts)` | Text at a position. |
| `renut.debug.clear()` | Removes every debug shape. |

Positions are vectors or `{1, 2, 3}`. `opts`:

| Option | Default | What |
|---|---|---|
| `seconds` | about 0.1 | How long it stays. Without it, it's gone unless drawn again (call it each frame or tick). |
| `key` | none | Drawing with the same key replaces the shape. Debug keys are shared by every mod. |
| `colour` | `0x00FF80` (text: white) | |
| `alpha`, `thickness` | | Lines, spheres and boxes. |
| `filled` | `false` | Spheres. |
| `size` | `0.025` | Text. |
| `background` | none | Text. |

Debug shapes are cleared when the world goes.

---

## Sound: `renut.sound`

### Sound files

| Function | Returns | When | What |
|---|---|---|---|
| `renut.sound.play(file, opts)` | sound or `nil` | any | Plays a sound file. `nil` when it couldn't play. |
| `renut.sound.music(file, volume)` | | any | Plays `file` as the music of every world from now on (the world's own is paused), at `volume` (default 1). `renut.sound.music()` gives the worlds their own music back. |

`file` is a path relative to your **mod's folder** (`"sounds/boing.ogg"`), or a full path. A relative
path works only when the call comes from a script directly in your mod's `scripts` folder (not a
subfolder); elsewhere it raises `... needs a full path here`. A missing file raises
`no sound file <path>`. For music in a mod world of your own, see `music` in
[`renut.worlds.add`](#mod-worlds-renutworlds).

`opts` for `play`:

| Option | Default | What |
|---|---|---|
| `volume` | 1 | |
| `pitch` | 1 | 2 is an octave up. |
| `loop` | `false` | Loops until stopped. |
| `at` | none | A position: the sound is placed in the world (fading with distance). Without it, it plays as is. |
| `range` | 70 | How far a placed sound carries. |

A sound object:

| Method | What |
|---|---|
| `s:stop()` | Stops it. |
| `s:playing()` | Whether it's still playing. |
| `s:set_volume(volume)` | |
| `s:move(at)` | Moves a placed sound (`nil`: no longer placed). |
| `s:set_pitch(pitch)` | |

### The game's sound cues

| Function | Returns | When | What |
|---|---|---|---|
| `renut.sound.cue(cue, opts)` | cue or `nil` | event | Plays one of the game's sound cues by its 32-bit cue id (an integer, or text such as `"0x1BFBF80D"`), not placed in the world. `opts.volume` (default 1, at most 4). `nil` when the game didn't play it. |

A cue object has `c.cue` (its id) and:

| Method | When | What |
|---|---|---|
| `c:stop()` | event | Stops it. |
| `c:playing()` | event | Whether it's still playing. |

At most 64 cues play at once (`64 cues are playing already`).

---

## Menu rows: `renut.menu`

Adds rows of your own to the game's list menus (the main menu, the pause menu, ...), at the end of the
game's rows.

| Function | Returns | When | What |
|---|---|---|---|
| `renut.menu.add(def)` | row | any | Adds a row, or updates the row with the same key. |
| `renut.menu.lists()` | list of strings | any | The list menus seen so far this session, by their XUI id path (`"StartMenu/singlePlayerList"`). Open a menu once to see it here. |

`def`:

| Key | Default | What |
|---|---|---|
| `list` | required | The list it goes in: the end of a path from `lists()`, such as `"singlePlayerList"` (it matches every list whose path ends with it). |
| `text` | required | The row's text (up to 127 characters; what the game's font can't show becomes `?`). |
| `on_select` | none | `function(row, user)`, called when the row is picked with A; `user` is the controller's user index. The game never sees that press. |
| `key` | `list .. "\n" .. text` | Identifies the row: adding again with the same key updates that row. |

A row object has `row.id`, `row.key`, `row.list`, `row.text`, `row.on_select` and:

| Method | What |
|---|---|
| `row:set_text(text)` | Changes its text (shown at once if the list is open). |
| `row:remove()` | Removes it; an open list keeps showing it until the menu fills the list again. |

A row shows the next time its menu fills the list (open the menu again). When the scripts reload, every
mod's rows are cleared and added again by the scripts.

`on_select` runs inside the game's menu code, which isn't a game event: event functions in it
[wait for the next game tick](#when-you-can-call-what), which doesn't run in the front end.
[`renut.net.open_multiplayer`](#online-renutnet) works only here.

```lua
renut.menu.add{list = "singlePlayerList", text = "My Mod Options", on_select = function(row)
  row:set_text("My Mod Options (opened)")
end}
```

---

## Online: `renut.net`

Messages between reNut players in the same online lobby or match, over reNut's own UDP socket next to
the game's connections. [tutorials/online-multiplayer.md](tutorials/online-multiplayer.md) shows a whole
mod.

| Function | Returns | When | What |
|---|---|---|---|
| `renut.net.send(name, value, peer)` | | any | Sends `value` as message `name` (1-64 characters) to `peer`, or to every peer when `peer` is `nil`. |
| `renut.net.on(name, fn)` | handle | any | `fn(value, peer)` for each message of that name received. |
| `renut.net.on_tick(fn)` | handle | any | `fn()` every frame, after the messages of that frame. |
| `renut.net.peers()` | list of integers | any | The other machines in the session, by address (as of the last frame). |
| `renut.net.state()` | string | any | `"off"`, `"lobby"` (a party session is up) or `"match"` (a game session is up). |
| `renut.net.is_host()` | boolean | any | Whether this machine hosts the session. |
| `renut.net.multiplayer()` | boolean | any | Whether the game's multiplayer front end or a match is up. |
| `renut.net.open_multiplayer(opts)` | | menu row | Opens the game's multiplayer, as the main menu's Multiplayer page does. `opts.live` (default `true`) is passed on as the game's Live choice. Works only inside a main menu row's `on_select` ([`renut.menu`](#menu-rows-renutmenu)); elsewhere it raises `renut.net.open_multiplayer works from a main menu row's on_select (renut.menu.add)`. |

- **Values** can be `nil`, booleans, numbers, strings and tables of them (nested up to 32 deep). Functions
  and other values raise `a <type> can't be sent`.
- **Size.** A message (its name plus its encoded value) is at most 1018 bytes; more raises
  `a message is at most <n> bytes with that name`.
- **Delivery.** Messages go out at the end of the frame, are sent only while there are peers, and can be
  lost or arrive out of order (UDP). A message that doesn't decode is dropped. You don't receive your
  own.
- `on` and `on_tick` listeners aren't game events: event functions in them
  [wait for the next game tick](#when-you-can-call-what).

```lua
renut.net.on("mymod_ping", function(value, peer)
  renut.net.send("mymod_pong", {time = value.time}, peer)
end)
renut.net.send("mymod_ping", {time = renut.time()})
```

---

## Saved data: `renut.data`

Values a mod keeps between sessions, in `mod_data/<mod>.json` in reNut's writable folder, written at
each change. Each mod sees only its own keys.

| Function | Returns | What |
|---|---|---|
| `renut.data.get(key, default)` | value | The value kept under `key`, or `default` when there's none. |
| `renut.data.set(key, value)` | | Keeps `value` under `key`; `nil` deletes it. |
| `renut.data.keys()` | list of strings | Every key (not the save keys below). |
| `renut.data.save_get(key, default)` | value | The same, kept per player profile. |
| `renut.data.save_set(key, value)` | | |
| `renut.data.save_keys()` | list of strings | |

- **Values** can be numbers (integers stay integers), strings, booleans and tables of them (nested up to
  32 deep). A key is 1-256 characters; a value at most 1 MB encoded.
- **Save keys** are kept per player 1's profile (their online id, else their gamertag); they raise
  `no player profile is signed in to keep a save's data for` with none signed in.
- Any time, any thread. The calls must come from a script directly in a mod's `scripts` folder (not a
  subfolder); elsewhere they raise `renut.data works from a mod's scripts`.

```lua
local best = renut.data.get("best_time", math.huge)
if time < best then renut.data.set("best_time", time) end
```

---

## The pad: `renut.pad`

A player's controller as the game read it this frame. Any time. `n` is the player, 1-4 (default 1); a
player with no pad reads as nothing pressed. While a mod holds the game's input (a `renut.hud.menu`,
`rex.input.hold_guest`), it reads what the game was kept from.

| Function | Returns | What |
|---|---|---|
| `renut.pad.down(button, n)` | boolean | Whether the button is held. |
| `renut.pad.pressed(button, n)` | boolean | Whether it went down this frame. |
| `renut.pad.released(button, n)` | boolean | Whether it went up this frame. |
| `renut.pad.stick(side, n)` | `x, y` | The `"left"` (default) or `"right"` stick, each `-1` to `1`. |
| `renut.pad.trigger(side, n)` | number | The `"left"` (default) or `"right"` trigger, `0` to `1`. |

Buttons (any case): `a`, `b`, `x`, `y`, `lb`, `rb`, `start`, `back`, `left_stick`, `right_stick`,
`dpad_up`, `dpad_down`, `dpad_left`, `dpad_right`. Others raise `not a pad button: ...`; a player number
outside 1-4 raises `a player is 1 to 4`.

For the keyboard and mouse, see `rex.input` in [rexfabric-api.md](rexfabric-api.md).

```lua
renut.game.on("tick", function()
  if renut.pad.down("lb") and renut.pad.pressed("y") then renut.camera.shake(1, 0.3) end
end)
```

---

## Lua challenges: `renut.challenges`

Challenges made entirely in Lua (not the game's challenge assets; for those see
[`renut.game.challenge_start`](#game-state-renutgame) and [`renut.npcs`](#mod-characters-renutnpcs)):
a timer, a counter, a goal, and rewards. One runs at a time.

| Function | Returns | What |
|---|---|---|
| `renut.challenges.add(def)` | the id | Adds (or replaces) a challenge. |
| `renut.challenges.start(id)` | challenge | Starts it. Raises `no challenge '<id>'` and `a challenge is running already: <id>`. |
| `renut.challenges.stop(won)` | | Ends the running one, won if `won` is `true` (default lost). |
| `renut.challenges.running()` | challenge or `nil` | The one running. |
| `renut.challenges.list()` | list | `{id, name, mod}` each, sorted by id. |
| `renut.challenges.on("finish", fn)` | handle | `fn(id, won, time, score)` when any ends. The handle has `:off()` only. |

`def`:

| Key | What |
|---|---|
| `id` | Required. |
| `name` | Shown in `list()` (default the id). |
| `time_limit` | Seconds: a countdown on the panel; reaching 0 loses. |
| `show_time` | `true`: a timer counting up on the panel (without a limit). |
| `goal` | The score that wins; shows the counter as `score/goal`. |
| `counter` | `true`: shows the counter without a goal. |
| `intro` | A message shown when it starts. |
| `win_text`, `lose_text` | Messages at the end (default `"Challenge complete!"`, `"Challenge failed"`). |
| `rewards` | Given when won: `{counters = {[name] = n}, flags = {...}, crates = {...}}` (by `renut.game.award`, `renut.game.set_flag`, `renut.crates.give`). |
| `on_start(c)`, `on_tick(c, dt)`, `on_end(c, won)` | Your functions. `on_tick` runs each game tick while it runs. |

A challenge object `c` has `c.id`, `c.def`, `c.score`, `c.time` (seconds of game time since it started)
and:

| Method | What |
|---|---|
| `c:set(n)` | Sets the score (and the counter); reaching `goal` wins. |
| `c:add(n)` | Adds `n` (default 1). |
| `c:win()`, `c:lose()` | Ends it. |
| `c:running()` | Whether it's the one running. |

A challenge is lost when the world changes. Starting, ending and the messages use the game's HUD, which
needs a game event: start one from a game event (a zone, a part, a timer) for it to show at once.

```lua
renut.challenges.add{id = "mymod_rings", name = "Ring Run", time_limit = 45, goal = 10,
  rewards = {counters = {JiggiesBanked = 1}}}
renut.zones.on("enter", function(zone)
  local c = renut.challenges.running()
  if zone == "mymod_start" and not c then renut.challenges.start("mymod_rings")
  elseif zone:match("^mymod_ring") and c then c:add() end
end)
```

---

## Other mods: `renut.mods`

| Function | Returns | What |
|---|---|---|
| `renut.mods.list()` | list of strings | The enabled mods' folder names. |
| `renut.mods.enabled(name)` | boolean | Whether that mod is enabled. |
| `renut.mods.this()` | string | Your mod's folder name (`"lua"` when not called from a mod's scripts). |
| `renut.mods.export(value)` | | Shares `value` (often a table of functions) as your mod's export. |
| `renut.mods.import(name)` | value or `nil` | What mod `name` exported, if it has yet. |
| `renut.mods.when(name, fn)` | | `fn(value)` once mod `name` exports; at once if it has. |

Mods load in order (see [mods-and-scripts.md](mods-and-scripts.md)), so a mod that loads later can't be
imported at the top level of an earlier one: use `when`.

```lua
renut.mods.when("coolhud", function(api) api.show_speed(true) end)
```

---

## Settings: `renut.settings`

A mod's own settings, shown in reNut's settings window (F4) under "Mod: <mod>". They're RexFabric cvars
named `<mod>_<key>` (lower case, other characters `_`), so they're saved like reNut's.

| Function | Returns | What |
|---|---|---|
| `renut.settings.add(def)` | what `rex.cvar` returns | Adds one: `{key, default, description}`. The default's type makes its kind: `true`/`false` a checkbox, an integer, a number, or text. Raises `a default is true/false, a number or text`. |
| `renut.settings.get(key)` | value | Its value now. |
| `renut.settings.set(key, value)` | | Sets it. |
| `renut.settings.on_change(key, fn)` | | `fn(value)` whenever it changes. Returns nothing. |

```lua
renut.settings.add{key = "boost", default = 8.0, description = "Booster strength"}
local boost = renut.settings.get("boost")
renut.settings.on_change("boost", function(v) boost = v end)
```

---

## The camera: `renut.camera`

| Function | Returns | When | What |
|---|---|---|---|
| `renut.camera.position()` | vector | any | Where the camera is this frame. |
| `renut.camera.forward()` | vector | any | The way it looks (see `facing_known`). |
| `renut.camera.up()`, `renut.camera.right()` | vector | any | Its other axes. |
| `renut.camera.facing_known()` | boolean | any | Whether reNut has seen a gameplay frame yet, so `forward()` surely points the way the camera looks. Before that, `forward()` is its matrix's third row, which may point backwards. |
| `renut.camera.fov()` | number | any | Its vertical field of view, in degrees. |
| `renut.camera.to_screen(p)` | `x, y, depth` or `nil` | any | Where a world position is on screen (`0`-`1` across and down) and how far in front of the camera; `nil` when it's behind the camera. |
| `renut.camera.set(at, look_at)` | boolean | any | Takes over the camera: it stands at `at` and looks at `look_at` until released. Returns whether it shows now: reNut learns how to point the camera from a gameplay frame, and until then (`false`) the setting waits. Raises `the camera can't look at where it is`. |
| `renut.camera.release()` | | any | Gives the camera back to the game (and stops `follow`). |
| `renut.camera.follow(target, offset, look_offset)` | | any | Keeps the camera on `target` each game tick (see below). Returns nothing. |
| `renut.camera.unfollow()` | | any | The same as `release`. |
| `renut.camera.set_fov(degrees)` | | any | Sets the vertical field of view from the next frame; `0` or `nil` gives the game's back. |
| `renut.camera.shake(strength, seconds)` | | any | Shakes the camera by up to `strength` units (default 1, at most 50), easing out over `seconds` (default 0.5). |

- A camera set with `set` or `follow` applies to the player's view, and ends when the world goes.
- `follow` targets: a vehicle object (`offset` is then along its own axes), an object's address, a
  position, or a function returning a position (or `nil` to skip that tick). `offset` defaults to
  `vec(0, 4, -10)`, `look_offset` (added to the target for where it looks) to `vec(0, 1, 0)`.

```lua
local v = renut.player.vehicle()
if v then renut.camera.follow(v, renut.vec(0, 2, 8)) end  -- in front, looking back
```

---

## The mouse cursor: `renut.mouse`

Any time.

| Function | Returns | What |
|---|---|---|
| `renut.mouse.position()` | `x, y` | Where the cursor is, `0`-`1` across and down the window (`0.5, 0.5` until it has been over the window). |
| `renut.mouse.inside()` | boolean | Whether the cursor has been over the window. |
| `renut.mouse.down(button)` | boolean | Whether a mouse button is held: `0` left (default), `1` right, `2` middle, `3` and `4` the side buttons (as `rex.input.mouse_button`). |
| `renut.mouse.ray(toward)` | `from, direction` | The line from the camera through the cursor into the world. Give `toward` (a position in front of the camera, such as your vehicle's) to make sure the camera's forward is the right way round. |

```lua
local from, dir = renut.mouse.ray(v:position())
local hit = renut.world.ray_cast(from, from + dir * 300, v, true)
```

---

## The physics world and the look: `renut.world`

| Function | Returns | When | What |
|---|---|---|---|
| `renut.world.ray_cast(from, to, vehicle, past_vehicle)` | `hit, normal, fraction` or `nil` | event | The first thing the line from `from` to `to` hits: where (a vector), its surface normal and how far along the line (`0`-`1`). `nil` when nothing. With a `vehicle`, it looks in that vehicle's physics world and sees the vehicle too, unless `past_vehicle` is `true`; without one, in the running world's. |
| `renut.world.ground(at, reach)` | `hit, normal, fraction` or `nil` | event | The ground under a position: the first hit going down from 2 units above it, within `reach` (default 50). |
| `renut.world.fog(def)` | table or nothing | any | Without `def`: the running world's fog `{near, far, most, colour}`. With `def`: sets those given (the rest stay). |
| `renut.world.env(offset)` | number | any | Reads the float at byte `offset` (a multiple of 4 under 448) of the world's active environment settings, of which fog is a part. |
| `renut.world.set_env(offset, value)` | | any | Writes it. |
| `renut.world.time_of_day()` | string | any | reNut's time-of-day setting: `"Game"` (the world's own), `"Morning"`, `"Afternoon"`, `"Evening"` or `"Night"`. |
| `renut.world.set_time_of_day(time)` | | any | Sets that setting. |

- Ray casts raise `this world has no physics world up` outside a level, `the vehicle is in no physics
  world`, and the vehicle's `has no body` error.
- `fog`, `env` and `set_env` raise `this world has no environment` outside a level. `near` and `far`
  are distances, `most` how thick the fog gets; `colour` is the game's own 32-bit colour word (read it
  first to see how a world sets it). Changes are to the running world only: a world loads its own again.

```lua
local fog = renut.world.fog()
renut.world.fog{near = fog.near * 0.3, far = fog.far * 0.3}
```

---

## Mod worlds: `renut.worlds`

A world of a mod's own, entered through a [door](#doors-renutdoors) in Showdown Town.
[NutTool](nuttool/README.md) builds a world's files and writes these calls;
[tutorials/worlds-and-doors.md](tutorials/worlds-and-doors.md) walks through it.

| Function | Returns | When | What |
|---|---|---|---|
| `renut.worlds.add(def)` | | any | Adds a world, or replaces the one with that id. |
| `renut.worlds.stats(id, stats)` | | any | Sets what the world door's stats panel shows. |
| `renut.worlds.list()` | list | any | Every world added: `{id, name}` each. |
| `renut.worlds.go(id, opts)` | | event | Goes to a mod world now, as through one of its doors (the running world unloads). Raises `no world '<id>' (renut.worlds.add it first)`. |
| `renut.worlds.set_shape(id, shape, weight)` | | any | Sets a shape key of the world's model ([`renut.shapes`](#shape-keys-renutshapes) on `renutworld_<id>`). |
| `renut.worlds.shape(id, shape)` | number or `nil` | any | Its weight. |
| `renut.worlds.set_material(id, name, values)` | | any | Changes one of the world model's materials ([`renut.materials`](#materials-renutmaterials)). |
| `renut.worlds.reset_material(id, name)` | | any | Puts it back. |
| `renut.worlds.materials(id)` | list of strings | any | The world model's material names. |

`add`'s `def`:

| Key | Default | What |
|---|---|---|
| `id` | required | Letters, digits and `_`. |
| `name` | the id | Its name, shown on its door's stats panel. |
| `scene` | `"renutworld_<id>"` | Its scene script asset (the `aid_script_` prefix is optional). |
| `music` | none | A sound file it loops while played in: relative to your mod's folder (from a script directly in `scripts`), or a full path. Raises `no music file <path>`. |
| `music_volume` | 0.8 | `0`-`1`. |
| `stats` | counted | As `renut.worlds.stats`. |
| `player_model` | Banjo's | The player model Banjo is made with in this world: a model made with NutTool's player-model tool, by its id (the asset `renut_player_<id>`). |
| `water` | none | Up to 8 water bodies: `{min = {x, z}, max = {x, z}, height = y}` each (or `{x = ..., z = ...}`); the n-th uses the world's n-th water setup. |

`stats` is a table with any of `jiggies`, `trophies`, `jinjos` and `notes`: each a total (a whole
number) or `{got = n, total = n}`. What's left out is counted from the world's characters' challenges
([`renut.npcs`](#mod-characters-renutnpcs)).

`go`'s `opts` (optional): `at` (a position) and `yaw` (degrees). Banjo (or his vehicle) is put there
during the new world's first 30 ticks, while the game settles him at its own start.

---

## Doors: `renut.doors`

| Function | When | What |
|---|---|---|
| `renut.doors.add(def)` | any | Adds a door into a mod world in Showdown Town, or moves the door with the same id. |

| Key | Default | What |
|---|---|---|
| `world` | required | The id of the world it leads to. A door to a world nobody added is left out. |
| `position` | required | Where it stands in Showdown Town: a vector, `{x = ..., y = ..., z = ...}` or `{x, y, z}`. |
| `yaw` | 0 | Which way it faces, in degrees. |
| `style` | `"wall"` | `"wall"` (the game's world door), `"palace"`, `"hidden"`, or `"arch"` (needs `look`). |
| `look` | the style's | A portal objparams asset (the `aid_objparams_` prefix is optional): its model and hologram. NutTool's world maker puts one in the mod. |
| `id` | the world's id | The door's own id, so a world can have several doors. |

Doors join Showdown Town as its map loads: one added or moved while the player is there shows up the
next time Showdown Town loads. A mod door is always open (no Jiggy cost, no flag). There is no list or
remove function.

```lua
renut.worlds.add{id = "mymod_isle", name = "Mystery Isle", music = "music/isle.ogg"}
renut.doors.add{world = "mymod_isle", position = {120, 4, -60}, yaw = 90}
```

---

## Mod characters: `renut.npcs`

Characters of a mod world that the player talks to, and the challenges they give. NutTool builds their
assets and writes the `add` call; [tutorials/characters-and-challenges.md](tutorials/characters-and-challenges.md)
explains it.

| Function | Returns | When | What |
|---|---|---|---|
| `renut.npcs.add(def)` | | any | Adds a character, or replaces the one with the same world and id (keeping its challenge and enemy settings when the new one has none). |
| `renut.npcs.list()` | list | any | Every character added: `{world, id, name, tag, speaker, challenge, voice}` each (`challenge` the challenge's id or `""`; `voice` a game voice's name, `"clips"` or `""`). |
| `renut.npcs.spawn(world, id, at, yaw)` | address or `nil` | event | Spawns one of them in the running world, whichever world that is, at a place (default `"front"`). Raises `no character <world>/<id> (renut.npcs.add)`. |
| `renut.npcs.on(event, fn)` | handle | any | `"spawn"`, `"tick"`, `"hit"`, `"defeated"`: see below. |

`def`:

| Key | Default | What |
|---|---|---|
| `world` | required | The id of the world it stands in. |
| `id` | required | Its id in that world. |
| `name` | the id | Its name. |
| `tag` | `"renut_<world>_<id>"` | Its objTag. |
| `chardata` | `"renutnpc_<world>_<id>"` | Its chardata asset (the `aid_chardata_` prefix is optional). |
| `scene` | `"renutworld_<world>"` | Its world's scene script (the `aid_script_` prefix is optional). |
| `speaker` | 32 | The `dbCharData_Id` slot its dialog lines name as speaker, `0`-`32`. Two characters in one world need different slots. |
| `text` | none | Texts by key: `{[key] = "text", ...}`, keys of letters, digits and `_`. |
| `challenge` | none | The challenge it gives: `{id, asset, flags, jiggies, notes}` (below). |
| `enemy` | none | `{health = n}` (default 3): hits wear it down, one hit at most every 0.75 s; at 0 it's defeated. |
| `voice` | none | A game character's voice by name (`"banjo"`, `"kazooie"`, `"mumbo"`, `"humba"`, `"grunty"`, `"klungo"`, `"log"`, `"bottles"`, `"jinjo"`, `"pikelet"`, `"pigpig"`, `"jolly"`, `"rhino"`, `"piddles"`, `"blubber"`, `"ghostpig"`, `"penguin"`, `"boggy"`, `"mrfit"`, `"kingjingaling"`, `"thomas"`), or a list of sound files, one played at random as each page of its dialog starts. |
| `voice_volume` | 0.9 | `0`-`1`. |
| `sounds` | none | Sound files by line: `{greeting = "...", win = "...", ...}`; keys are lines (`greeting`, `objectives`, `win`, `lose`, `completed`) or dialog keys. |

Sound files are `.ogg`, `.mp3`, `.flac` or `.wav`, relative to your mod's folder (from a script directly
in `scripts`) or full paths.

`challenge`:

| Key | What |
|---|---|
| `id` | The challenge's id. |
| `asset` | Its challenge asset (the `aid_challenge_` prefix is optional). It must be in a bundle loaded with the world. |
| `flags` | The game flags its asset names, in order: seen, notes, beaten, beaten with the T.T. trophy (flags 2-4 feed the door's stats). |
| `jiggies` | The counter its Jiggy goes into. |
| `notes` | The notes it pays, for the door's stats (a whole number, default 0). |

Names (`world`, `id`, `tag`, flags, `jiggies`) are letters, digits and `_`, under 64 characters. reNut
maps a mod's tags, flags and counters onto the game's spare ones: when those run out it raises
`no objTag is left`, `no game flag is left` or `no game counter is left`.

**NPC objects.** The `on` listeners get an NPC object with `npc.world`, `npc.id`, `npc.name` and:

| Method | What |
|---|---|
| `npc:in_play()` | Whether it's in the running world (and not defeated). |
| `npc:position()` / `npc:set_position(p)` | Where it is (`p` a vector or `{x, y, z}`). |
| `npc:yaw()` / `npc:set_yaw(degrees)` | Its heading. |
| `npc:play(anim)` | Plays an animation: `"stand"`, `"walk"`, `"run"`, `"idle1"`, `"idle2"`, `"hit"`, ... |
| `npc:health()` / `npc:set_health(n)` | Its enemy health (a whole number); `0` or less defeats it. |
| `npc:defeat()` | Defeats it (it explodes and goes). |
| `npc:shape(name)` / `npc:set_shape(name, weight)` | Its model's shape keys. |

The setters take effect on the NPC's next tick. All but `in_play` raise
`renut.npcs: <world>/<id> isn't in play` when it isn't.

**Events** (`renut.npcs.on`): `"spawn"` `fn(npc)` when it appears in a world; `"tick"` `fn(npc, dt)`
every frame it's in play; `"hit"` `fn(npc, hits)` when an enemy takes a hit; `"defeated"` `fn(npc)`.
They aren't game events: event functions in them [wait for the next game tick](#when-you-can-call-what).
Characters spawned with `renut.npcs.spawn` or `renut.actors.spawn` don't fire them.

---

## The player: `renut.player`

`n` is the player number, 1-4 (default 1: Banjo on this machine). Split screen and online matches have
more than one.

| Function | Returns | When | What |
|---|---|---|---|
| `renut.players()` | list | any | The players in the running world: `{number, avatar}` each. |
| `renut.player.avatar(n)` | address or `nil` | any | The player's character. |
| `renut.player.position(n)` | vector or `nil` | any | Where they are (`nil` outside a level). |
| `renut.player.yaw(n)` | number or `nil` | any | Their heading in degrees. |
| `renut.player.vehicle(n)` | vehicle or `nil` | any | The vehicle they're in. |
| `renut.player.driving()` | boolean | any | Whether player 1 is in a vehicle. |
| `renut.player.state(n)` | string | any | Their body state now (`"Idle"`, `"Jump"`, `"Swim..."`). |
| `renut.player.swimming(n)` | boolean | any | Whether that state is a swimming one. |
| `renut.player.velocity(n)` | vector | event | Their velocity. |
| `renut.player.push(v, n)` | | event | Adds velocity `v`. |
| `renut.player.on_ground(n)` | boolean | event | Whether they stand on the floor. |
| `renut.player.exit_vehicle(n)` | boolean | event | Knocks them out of their vehicle; whether they were in one. |
| `renut.player.teleport(at)` | | event | Moves player 1 to a position. Raises `the player is in a vehicle: move the vehicle instead (v:set_position)`. |
| `renut.player.play(animation)` | | event only | Plays one of Banjo's animations by name. Unlike other event functions it doesn't wait outside game events: it raises `this works inside the renut events`. |
| `renut.player.set_model(model)` | | any | The player model Banjo is made with from now on, in every world (over a world's `player_model`): a NutTool player model's id; `nil` or `""` for his own. It applies when Banjo is next made (a world load); a model the world hasn't loaded is skipped. |
| `renut.player.on(event, fn)` | handle | any | `"state"` `fn(new, old)`, `"jump"` `fn(state)`, `"land"` `fn(state)` for player 1. See [Event reference](#events-by-on). |

Functions that need a player raise `there's no player <n> in this world` when there isn't one.

```lua
renut.player.on("jump", function()
  renut.player.push(renut.vec(0, 6, 0))  -- a higher jump
end)
```

---

## Shape keys: `renut.shapes`

A model's shape keys (blend shapes), as NutTool exports them, set by weight for every instance of the
model. `model` is the model's asset name (the `aid_model_` prefix is optional). Any time.

| Function | Returns | What |
|---|---|---|
| `renut.shapes.define{model, shapes, weights}` | | Declares a model's shape keys: `shapes` a list of names, `weights` their starting weights (default 0). NutTool writes this call. |
| `renut.shapes.set(model, shape, weight)` | | Sets a shape's weight (`0` none, `1` all of it). Raises `<model> has no shape '<shape>'`. |
| `renut.shapes.get(model, shape)` | number or `nil` | Its weight; `nil` when the model has no such shape. |
| `renut.shapes.list(model)` | list of strings | Its shape names. |

For one part's own weights, see [`part:set_shape`](#turrets-axes-bones-shape-keys-and-paint); for a
world's model, `renut.worlds.set_shape`; for an NPC's, `npc:set_shape`.

---

## Materials: `renut.materials`

Changes to a model's materials as reNut draws them. `model` is the model's asset name (the `aid_model_`
prefix is optional), `name` a material's name. Any time.

| Function | Returns | What |
|---|---|---|
| `renut.materials.define{model, names}` | | Declares a model's material names in order. NutTool writes this call. |
| `renut.materials.set(model, name, values)` | | Changes a material: see `values` below. Raises `<model> has no material '<name>'`. |
| `renut.materials.reset(model, name)` | | Puts it back as NutTool made it. |
| `renut.materials.list(model)` | list of strings | Its material names. |

`values` (each optional):

| Key | What |
|---|---|
| `emissive` | `{r, g, b}`: the colour it glows with. |
| `emissive_strength` | How strongly. |
| `glow` | Its glow amount. |
| `opacity` | `0`-`1`. |

```lua
renut.worlds.set_material("mymod_isle", "lava", {emissive = {1, 0.4, 0.1}, emissive_strength = 3})
```

---

## Other values

| Value | What |
|---|---|
| `renut.projectiles` | The game's projectiles by name, for `part:fire`: `egg`, `eggbig`, `grenade`, `mumbobombo`, `laser`, `citrusslick`, `emp`, `clockworkkazooie`, `weldarsbreath`, `torpedile`, `freezeezy`, `rustbin` (each an objparams asset id). |
| `renut.fx.Effect` | Every known effect's asset id by name: `renut.fx.Effect.actor_banjo_jumpland`. The node editor offers the same list. |
| `renut.Part`, `renut.Vehicle`, `renut.Character` | Types the [node editor](node-editor.md) uses for its pins. `renut.Part(address)` makes a RexFabric object (with `.address`); the Lua API above uses its own part and vehicle objects instead. |

---

## Event reference

### Events by `.on`

Every `.on` returns a [handle](#handles-and-off). "Game event" means [event](#when-you-can-call-what)
functions work in it at once; the others make them wait for the next game tick.

**`renut.parts.on(event, fn)`**: every part in the game. Game events.

| Event | `fn` gets | When |
|---|---|---|
| `"damage"` | `part, amount` | A part is about to lose health. `renut.set_amount` changes it. |
| `"break"` | `part` | A part is breaking off its vehicle. |
| `"use"` | `part, control` | A part's button went down (the driver's). |
| `"release"` | `part, control` | A part's button went up. |

**`renut.vehicle.on(event, fn)`**: every vehicle in the game. Game events.

| Event | `fn` gets | When |
|---|---|---|
| `"spawn"` | `v` | A vehicle was made. Its parts are added after: use `part_added`. |
| `"removed"` | `v` | It's leaving its level. |
| `"tick"` | `v, dt` | Every frame, after its parts' physics. |
| `"damage"` | `v, amount, part, attacker` | It's about to take damage. `part` a part object or `nil`, `attacker` an address (`0` for none). `renut.set_amount` changes it. |
| `"health"` | `v, health` | Its health was set by the game. |
| `"part_added"` | `v, part` | A part went onto it. |
| `"part_removed"` | `v, part` | A part is coming off it. |
| `"collide"` | `v, strength, part, impact, other` | The frame's strongest hit: `part` the part hit (or `nil`), `impact` a vector, `other` the address of what it hit or `nil`. |
| `"enter"` | `v, driver, player` | A character got in: `driver` its address, `player` whether it's player 1. |
| `"exit"` | `v, driver, player` | A character got out. |

**`renut.game.on(event, fn)`**: game events.

| Event | `fn` gets | When |
|---|---|---|
| `"world_enter"` | `number, name, scene` | A world became the running one (including Showdown Town and mod worlds; `name` is `""` for those without one). |
| `"world_leave"` | `number, name, scene` | The world is being torn down. |
| `"challenge_start"` | `challenge` | One of the game's challenges began (its asset id). |
| `"challenge_finish"` | `challenge, won, outcome` | It ended: `won` a boolean, `outcome` the game's value for player 1 (1 won, 2 lost). |
| `"paused"`, `"resumed"` | | The pause menu opened or closed. |
| `"tick"` | | Every game tick (once a frame while a level runs). |
| `"save"` | | A save of the game began. |
| `"load"` | | A save was loaded (player 1's counters appeared or changed). |

**`renut.objects.on(event, fn)`**: game events.

| Event | `fn` gets | When |
|---|---|---|
| `"spawn"` | `object, kind, is_character` | The game made an object (it may join its world's list only on the world's next update). `kind` is `""` when unknown. |
| `"removed"` | `object, kind` | An object is about to be removed. |
| `"collected"` | `object, kind` | A collectable was collected. |

**`renut.actors.on(event, fn)`**: game events.

| Event | `fn` gets | When |
|---|---|---|
| `"spawn"` | `object, kind` | The game made a character. |
| `"hit"` | `character, attacker, damage` | A character took a hit; `attacker` an address or `0`. |
| `"defeated"` | `character, killer` | A character died, whichever way; `killer` an address or `0`. |

**`renut.player.on(event, fn)`**: player 1. Game events.

| Event | `fn` gets | When |
|---|---|---|
| `"state"` | `new, old` | Their body state changed (names as `renut.player.state`). |
| `"jump"` | `state` | They jumped (`Jump`, `JumpFromWater`, `JumpFromClimbPole`, `JumpAtVehicle`, `JumpAtVehicleAir`, `JumpAtVehicleWater`). |
| `"land"` | `state` | They landed (`Land`, `JumpLand`, `JumpLandSoft`, `FallCrashLand`). |

**Others.**

| Call | Event | `fn` gets | Game event? |
|---|---|---|---|
| `renut.zones.on` | `"enter"`, `"exit"` | `zone, object, what` (`what`: `"banjo"`, `"vehicle"` or `"character"`) | yes |
| `renut.hud.on` | `"timer_done"` | | yes |
| `renut.hud.on` | `"choice"` | `id, confirmed` | yes |
| `renut.crates.on` | `"award"` | `flag, contents` | yes |
| `renut.npcs.on` | `"spawn"`, `"tick"`, `"hit"`, `"defeated"` | `npc` (`tick`: `npc, dt`; `hit`: `npc, hits`) | no |
| `renut.net.on` | a message name | `value, peer` | no |
| `renut.net.on_tick` | | | no |
| `renut.challenges.on` | `"finish"` | `id, won, time, score` | (runs where the challenge ends) |
| `renut.settings.on_change` | a key | `value` | no |

An unknown event name raises an error naming the function and its events.

### Raw events for `rex.on`

Underneath, the API is built on RexFabric events that `rex.on` (and native mods, and the node editor)
can listen to directly. They pass addresses, not objects: `renut.parts.wrap`, `renut.parts.get` and
`renut.vehicle.at` make objects of them. All of them fire on the game thread.

Game events (`renut.<name>`, in which event functions work at once):

| Event | Arguments | Fires for |
|---|---|---|
| `renut.part_spawn` | `id, instance` | Custom parts with behaviour (callbacks). |
| `renut.part_destroy` | `id, instance` | 〃 |
| `renut.part_attach` | `id, instance, vehicle` | 〃 |
| `renut.part_detach` | `id, instance, vehicle` | 〃 |
| `renut.part_tick` | `id, instance, vehicle, dt` | 〃 |
| `renut.part_physics` | `id, instance, vehicle, dt` | 〃 |
| `renut.part_power` | `id, instance, vehicle, on` | 〃 (`on`: a driver got in) |
| `renut.part_button` | `id, instance, vehicle, down` | 〃 |
| `renut.part_damage` | `instance, amount` | Every part. `renut.set_amount` works. |
| `renut.part_break` | `instance` | Every part. |
| `renut.part_use` | `instance, control, down` | Every part's button, down and up. |
| `renut.vehicle_spawn` | `vehicle` | Every vehicle. |
| `renut.vehicle_removed` | `vehicle` | 〃 |
| `renut.vehicle_tick` | `vehicle, dt` | 〃 |
| `renut.vehicle_damage` | `vehicle, part, attacker, amount` | 〃 (note the order). `renut.set_amount` works. |
| `renut.vehicle_health` | `vehicle, health` | 〃 |
| `renut.vehicle_part_added` | `vehicle, part` | 〃 |
| `renut.vehicle_part_removed` | `vehicle, part` | 〃 |
| `renut.vehicle_collide` | `vehicle, part, strength, x, y, z, other` | 〃 (`part`, `other`: `0` for none) |
| `renut.vehicle_enter` | `vehicle, driver, player` | 〃 |
| `renut.vehicle_exit` | `vehicle, driver, player` | 〃 |
| `renut.world_enter` | `number, scene, name` | (note the order) |
| `renut.world_leave` | `number, scene, name` | |
| `renut.challenge_start` | `challenge` | |
| `renut.challenge_finish` | `challenge, outcome` | `outcome` 1 won, 2 lost. |
| `renut.game_paused` | | |
| `renut.game_resumed` | | |
| `renut.game_tick` | | Once a frame while a level runs. |
| `renut.game_save` | | |
| `renut.game_load` | | |
| `renut.crate_award` | `flag` | |
| `renut.zone_enter` | `zone, object, what` | |
| `renut.zone_exit` | `zone, object, what` | |
| `renut.hud_choice` | `id, confirmed` | |
| `renut.hud_timer_done` | | |
| `renut.actor_hit` | `character, attacker, damage` | |
| `renut.actor_defeated` | `character, killer` | |
| `renut.collected` | `object, kind` | |
| `renut.player_state` | `new, old` | |
| `renut.object_spawn` | `object, kind, is_character` | |
| `renut.object_removed` | `object, kind` | |

Other events (not game events: event functions in them wait for the next game tick):

| Event | Arguments | When |
|---|---|---|
| `renut.npc_spawn` | `world, id` | A mod character appears. |
| `renut.npc_tick` | `world, id, dt` | Every frame it's in play. |
| `renut.npc_hit` | `world, id, hits` | An enemy character takes a hit. |
| `renut.npc_defeated` | `world, id` | It's defeated. |
| `renut.menu_select` | `row, user` | A mod's menu row is picked (`row` is the row's id). |
| `renut.net_message` | `peer, name, data` | A `renut.net` message arrived (`data` still encoded). |
| `renut.net_tick` | | Every frame, after the messages. |

```lua
rex.on("renut.vehicle_collide", function(vehicle, part, strength)
  if strength > 20 then rex.log("big hit on " .. tostring(renut.vehicle.at(vehicle))) end
end)
```

