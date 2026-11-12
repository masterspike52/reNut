---@meta

-- LuaLS definitions for reNut's `renut` table, from lua-reference-renut.md.
-- reNut never runs this file: editors read it for autocomplete.

---------------------------------------------------------------------------------------------------
-- Vectors
---------------------------------------------------------------------------------------------------

---A vector: a plain table `{x, y, z}` with a metatable. Distances are game units; `y` is up.
---`s * a` (a number first) works too, and `a == b` compares the components.
---@class renut.Vec
---@field x number
---@field y number
---@field z number
---@operator add(renut.Vec): renut.Vec
---@operator sub(renut.Vec): renut.Vec
---@operator unm: renut.Vec
---@operator mul(number): renut.Vec
---@operator div(number): renut.Vec
local Vec = {}

---Its length.
---@nodiscard
---@return number
function Vec:length() end

---Unit vector the same way (the zero vector for a zero vector).
---@nodiscard
---@return renut.Vec
function Vec:normalized() end

---Dot product.
---@nodiscard
---@param b renut.VecLike
---@return number
function Vec:dot(b) end

---Cross product.
---@nodiscard
---@param b renut.VecLike
---@return renut.Vec
function Vec:cross(b) end

---Functions that take a vector read only `.x`, `.y` and `.z`, so a plain table works as an argument.
---@alias renut.VecLike renut.Vec|{x: number, y: number, z: number}

---A quaternion.
---@alias renut.Quat {x: number, y: number, z: number, w: number}

---------------------------------------------------------------------------------------------------
-- The renut table
---------------------------------------------------------------------------------------------------

---reNut's own Lua API for Banjo-Kazooie: Nuts & Bolts: parts, vehicles, weapons, effects, the
---camera and mouse, the physics world, and a mod's own worlds, doors and characters.
renut = {}

---Makes a vector. Missing components are 0.
---@nodiscard
---@param x? number
---@param y? number
---@param z? number
---@return renut.Vec
function renut.vec(x, y, z) end

---Inside a `damage` event, changes the damage about to be dealt. Negative values become `0`.
---@param x number
function renut.set_amount(x) end

---------------------------------------------------------------------------------------------------
-- Listeners and timers
---------------------------------------------------------------------------------------------------

---What every `.on`, `rex.on`, `renut.after` and `renut.every` returns.
---@class renut.Handle
local Handle = {}

---Stops it: its function isn't called again.
function Handle:off() end

---Whether it's still on.
---@return boolean
function Handle:active() end

---Stops a listener or timer (`nil` is fine).
---@param handle renut.Handle?
function renut.off(handle) end

---Listens once: `renut.once(renut.game, "world_enter", fn)`.
---@param space table A renut table with an `on`: `renut.game`, `renut.vehicle`, `renut.zones`...
---@param event string
---@param fn function
---@return renut.Handle
function renut.once(space, event, fn) end

---Calls `fn` once after `seconds` of game time (game ticks, not while paused), inside the game's tick.
---@param seconds number
---@param fn fun()
---@return renut.Handle
function renut.after(seconds, fn) end

---Calls `fn` every `seconds` of game time, until stopped.
---@param seconds number
---@param fn fun()
---@return renut.Handle
function renut.every(seconds, fn) end

---Pauses the callback it's called from (an event's, a timer's) for `seconds` of game time, then carries on.
---@param seconds number
function renut.wait(seconds) end

---Game time in seconds since the scripts loaded: it counts game ticks, and stops while paused.
---@nodiscard
---@return number
function renut.time() end

---One row of `renut.players()`.
---@class renut.PlayerInfo
---@field number integer 1-4.
---@field avatar integer Their object's address.

---The players in the running world (split screen has two).
---@return renut.PlayerInfo[]
function renut.players() end

---------------------------------------------------------------------------------------------------
-- Parts
---------------------------------------------------------------------------------------------------

renut.parts = {}

---Adds a vehicle part. Call it from a script's top level; returns the id (lower-cased).
---@param def renut.PartDef
---@return string id
function renut.parts.add(def) end

---Every custom part loaded, from `.nbpatch` files and Lua mods.
---@return renut.PartInfo[]
function renut.parts.list() end

---Your own part's object (the table with your fields) by its address, if the API has seen it.
---@param instance integer
---@return renut.Part?
function renut.parts.get(instance) end

---A part object for any part address: yours (its own table) or a shipped part's (a new table each time).
---@param instance integer?
---@return renut.Part?
function renut.parts.wrap(instance) end

---A shipped part's asset id from its name (`"gadgets_horn"`, the prefix optional), as `part:aid()` and crate contents give it.
---@param name string
---@return integer aid
function renut.parts.aid(name) end

---How many of a part the player has in the garage, and how many aren't on a vehicle.
---The game counts them again from the crates collected at each level load.
---@param part string|integer Its name or asset id.
---@return integer total
---@return integer available
function renut.parts.owned(part) end

---@alias renut.PartEvent
---| "damage" # A part is about to lose health. `renut.set_amount(x)` changes it (`0` blocks it).
---| "break"  # A part is breaking off.
---| "use"     # A part's button went down, the driver's or a script's (`control`: which of its buttons).
---| "release" # A part's button went up.

---Listens to every part in the game. It returns a handle: `h:off()` stops it.
---@param event renut.PartEvent
---@param fn function
---@return renut.Handle
---@overload fun(event: "damage", fn: fun(part: renut.Part, amount: number)): renut.Handle
---@overload fun(event: "break", fn: fun(part: renut.Part)): renut.Handle
---@overload fun(event: "use", fn: fun(part: renut.Part, control: integer)): renut.Handle
---@overload fun(event: "release", fn: fun(part: renut.Part, control: integer)): renut.Handle
function renut.parts.on(event, fn) end

---One row of `renut.parts.list()`.
---@class renut.PartInfo
---@field id string
---@field mod string
---@field name string
---@field base string
---@field lua boolean Whether a Lua mod added it.

---A value written over the copied record. One type per offset.
---@class renut.PartField
---@field f32? number
---@field u32? integer
---@field i32? integer
---@field u16? integer
---@field u8? integer
---@field str? string Text with its terminating zero.

---The table `renut.parts.add` takes. A part with at least one `on_...` callback is a new kind of part;
---one without is a variant.
---@class renut.PartDef
---@field id string 1-40 characters of `a-z`, `0-9`, `_`. Unique across all mods; prefix it with your mod's name.
---@field base? string The shipped part it copies, e.g. `gadgets_horn`. Without a base the part is a block of its own.
---@field name? string Its name in the garage. Default: the base's.
---@field count? integer How many the player owns, clamped to 1-99. Default 1.
---@field fields? table<integer, renut.PartField> Values written over the copied record: `{ [0x3FC] = { f32 = 140 } }`.
---@field model? string A model's name. Required for a block of its own (in your mod's `bundles`).
---@field attach? string Its footprint and attach points. Required for a block of its own.
---@field type? string A garage type the part is a size of (1-40 characters).
---@field params? any Given to every instance as `part.params`. Shared by all of them: don't write to it.
---@field button? "A"|"B"|"X"|false Gives the part an action button; `false` takes a base's button away.
---@field base_action? boolean `true` keeps the base part's own button action as well as your `on_use`. Default `false`.
---@field category? string Blocks of their own: the garage category it's listed in (`body`, `gadget`, `seat`, ...). Required for one.
---@field mass? number Blocks of their own. Default 10.
---@field health? number Blocks of their own. Default 5.
---@field paint? string Blocks of their own: its colour before the player paints it. Default `colour_orange`.
---@field on_spawn? fun(part: renut.Part) The part was made (in the garage or with a vehicle).
---@field on_attach? fun(part: renut.Part) It was built onto a vehicle.
---@field on_detach? fun(part: renut.Part) It came off its vehicle. `part:vehicle()` is still the old vehicle here.
---@field on_tick? fun(part: renut.Part, dt: number) Every frame while it's on a vehicle.
---@field on_physics? fun(part: renut.Part, dt: number) Every physics step while it's on a vehicle. The place for forces.
---@field on_throttle? fun(part: renut.Part, throttle: number, dt: number) Every frame, after `on_tick`, with the driver's throttle (`-1` to `1`).
---@field on_use? fun(part: renut.Part) Its action button was pressed.
---@field on_release? fun(part: renut.Part) Its action button was let go.
---@field on_button_down? fun(part: renut.Part) The same as `on_use`, under the `.nbpatch` part scripts' name.
---@field on_button_up? fun(part: renut.Part) The same as `on_release`, under the `.nbpatch` part scripts' name.
---@field on_driver_in? fun(part: renut.Part) A driver got into the vehicle.
---@field on_driver_out? fun(part: renut.Part) A driver got out of the vehicle.
---@field on_damage? fun(part: renut.Part, amount: number) It's about to lose `amount` health. `renut.set_amount(x)` changes it.
---@field on_break? fun(part: renut.Part) It's breaking off its vehicle.
---@field on_destroy? fun(part: renut.Part) The part is gone. Drop anything you keep about it.
---@field on_collide? fun(part: renut.Part, strength: number, impact: renut.Vec, other: integer?) Its vehicle hit something hard and this part took it.

---A part on any vehicle, yours or a shipped one. Your own parts each keep one table until
---`on_destroy`, so you can store state in it. Compares equal (`==`) to another for the same part.
---@class renut.Part
---@field instance integer The part's address in game memory (an integer), for `rex.mem`.
---@field id string? Your part's id; `nil` for shipped parts and `.nbpatch` parts.
---@field params any The `params` from `renut.parts.add` (your parts only).
---@field [string] any Your own state (`part.fuel = 3`).
local Part = {}

---The table you gave `renut.parts.add` (your parts only).
---@return renut.PartDef?
function Part:def() end

---The vehicle it's on.
---@return renut.Vehicle?
function Part:vehicle() end

---Its game class: `vehicleBlock`, `vehicleBlockEngine`, `vehicleBlockWheel`, `vehicleBlockWeapon`, ...
---@return string
function Part:class() end

---Whether it's that class or derives from it.
---@param class string
---@return boolean
function Part:is_a(class) end

---Its record's asset id.
---@return integer
function Part:aid() end

---Where it is in the world.
---@return renut.Vec
function Part:position() end

---Its grid cell on the vehicle (integers).
---@return integer x
---@return integer y
---@return integer z
function Part:cell() end

---Which way it's turned on the grid, `0`-`23`.
---@return integer
function Part:orientation() end

---Its health.
---@return number
function Part:health() end

---Its most health.
---@return number
function Part:max_health() end

---Its health, `0`-`1`.
---@return number
function Part:health_fraction() end

---`0` (fine) to `3` (about to break).
---@return integer
function Part:damage_level() end

---Sets its health, clamped to `0`-max; the game works out its damage level again.
---@param h number
function Part:set_health(h) end

---Its paint, each `0`-`1`.
---@return number r
---@return number g
---@return number b
function Part:paint() end

---*Event.* Repaints it, drawn so from the next frame.
---@param colour integer|{r: number, g: number, b: number} `0xRRGGBB`, or `{r, g, b}` from `0` to `1`.
---@param effect? boolean `true` plays the garage's paint effect on it.
function Part:set_paint(colour, effect) end

---Full health.
function Part:repair() end

---*Event.* Damages it the game's way; returns `true` if it broke off (and breaks it off).
---@param amount number
---@return boolean
function Part:damage(amount) end

---*Event.* Breaks it off its vehicle; the vehicle splits on its next update.
function Part:break_off() end

---*Event.* Presses the part's button as a driver would.
---@param control? integer `0` (the default) for parts with one button.
function Part:press(control) end

---*Event.* Lets go of the part's button as a driver would.
---@param control? integer `0` (the default) for parts with one button.
function Part:release(control) end

---*Event.* Launches one projectile from `position` at `velocity` (units per second). Uses no ammo.
---Returns `nil` when none is ready.
---@param position renut.VecLike
---@param velocity renut.VecLike
---@param options? renut.FireOptions|renut.Quat A quaternion alone is taken as `rotation`.
---@return integer? projectile The projectile's address.
function Part:fire(position, velocity, options) end

---*Event.* Whether `fire` with these options would launch one now. `options.projectile` is required.
---@param options renut.FireOptions
---@return boolean
function Part:can_fire(options) end

---Holds (`true`, the default) or lets go of a weapon's trigger. Weapons only.
---@param held? boolean
function Part:hold(held) end

---The ammo each shot of the base weapon takes. Weapons only.
---@return number
function Part:ammo_per_shot() end

---Where the barrel ends and which way it points, as of the weapon's last frame. Weapons only.
---@return renut.Vec? position `nil` before its first frame on a vehicle.
---@return renut.Vec? direction
function Part:muzzle() end

---Turns a weapon's barrel to point along `direction` (a world vector), within its limits. Call it every frame.
---@param direction renut.Vec
---@param signs? renut.AimSigns
function Part:aim_at(direction, signs) end

---Sets the barrel's angles directly, in radians from the part's own forward. Weapons only.
---@param yaw number Positive to the left (wrapped).
---@param pitch number Positive down (clamped to the weapon's limits).
---@param seat_yaw_sign? number `-1` if the seat's animation turns the wrong way.
function Part:aim(yaw, pitch, seat_yaw_sign) end

---The barrel's angles now, in radians. Weapons only.
---@return number yaw
---@return number pitch
function Part:aim_angles() end

---The part's own axes in the world.
---@return renut.Vec right
---@return renut.Vec up
---@return renut.Vec forward
function Part:axes() end

---*Event.* Turns bone `index` of the part's model by `angle` radians about `axis`, in place of its pose.
---Call it every frame (`on_tick`) to hold it.
---@param index integer
---@param angle number Radians.
---@param axis? renut.VecLike In the bone's parent's space; default x.
function Part:bone(index, angle, axis) end

---How many bones the part's model has (`0` without a model).
---@return integer
function Part:bone_count() end

---A shape key's weight on this part's model, this part's own; `nil` when it has no such shape.
---@param name string
---@return number?
function Part:shape(name) end

---Sets a shape key's weight on this part only. Raises `its model has no shape '...'`.
---@param name string
---@param weight number
function Part:set_shape(name, weight) end

---`part:aim_at`'s signs: `-1` for any that turns the wrong way.
---@class renut.AimSigns
---@field yaw_sign? number
---@field pitch_sign? number
---@field seat_yaw_sign? number

---`part:fire` and `part:can_fire` options.
---@class renut.FireOptions
---@field projectile? renut.ProjectileName|string|integer A `renut.projectiles` name, another name, a full `"aid_objparams_..."` name, or an asset id.
---@field rotation? renut.Quat Default: none (`{0, 0, 0, 1}`).
---@field max_flying? integer How many of that projectile the part can have in the air at once, 1-30. Default 10.

---@alias renut.ProjectileName
---| "egg"
---| "eggbig"
---| "grenade"
---| "mumbobombo"
---| "laser"
---| "citrusslick"
---| "emp"
---| "clockworkkazooie"
---| "weldarsbreath"
---| "torpedile"
---| "freezeezy"
---| "rustbin"

---A table of projectile name to asset id.
---@class renut.Projectiles
---@field egg integer
---@field eggbig integer The fixed egg turret's.
---@field grenade integer
---@field mumbobombo integer
---@field laser integer
---@field citrusslick integer
---@field emp integer
---@field clockworkkazooie integer
---@field weldarsbreath integer
---@field torpedile integer
---@field freezeezy integer
---@field rustbin integer

---@type renut.Projectiles
renut.projectiles = {}

---------------------------------------------------------------------------------------------------
-- Vehicles
---------------------------------------------------------------------------------------------------

renut.vehicle = {}

---A vehicle object for a vehicle's address (`nil` for `0`/`nil`).
---@param address integer?
---@return renut.Vehicle?
function renut.vehicle.at(address) end

---Every blueprint `renut.vehicle.spawn` takes by name: the enabled mods' (`"mod: name"`) and the save's own vehicles
---(`"save: name"`).
---@return {name: string, file: string}[]
function renut.vehicle.blueprints() end

---*Event.* Builds a whole vehicle from a blueprint, as the garage does, and returns it. Every part is built, owned or not.
---@param blueprint string An enabled mod's `vehicles/*.blueprint` (`"mod: name"` or `"name"`), a vehicle saved in the
---garage (`"save: name"`), or a `.blueprint` file's full path.
---@param at? renut.Place Default: `"front"`. `"front"` and `"banjo"` are put on the ground there; a position is where its
---lowest part goes, but never below the ground there.
---@param yaw? number Degrees.
---@return renut.Vehicle?
function renut.vehicle.spawn(blueprint, at, yaw) end

---Where a character sits in a vehicle.
---@alias renut.Seat
---| "driver"
---| "passenger"
---| "other"

---*Event.* A character gets into the vehicle, on its next update, as the game seats a challenge's driver.
---@param v renut.Vehicle|integer
---@param actor? renut.Who Default: Banjo.
---@param seat? renut.Seat|integer Default: `"driver"`. The game's seat numbers (`2`, `4`, `8`) work too.
function renut.vehicle.seat(v, actor, seat) end

---@alias renut.VehicleEvent
---| "spawn"        # A vehicle was made. Its parts are added after: use `part_added`.
---| "removed"      # It's leaving the level. Forget anything you keep about it.
---| "tick"         # Every frame, after its update and its parts' physics.
---| "damage"       # It's about to take vehicle damage. `renut.set_amount(x)` changes the amount.
---| "health"       # Its health was set.
---| "part_added"   # A part went onto it.
---| "part_removed" # A part is coming off it.
---| "collide"      # It hit something hard: its strongest hit of the frame, at most one a frame.
---| "enter"        # Someone got in: `driver` is their object's address, `player` whether it's Banjo.
---| "exit"         # Someone got out.

---Listens to every vehicle in the game. It returns a handle: `h:off()` stops it.
---@param event renut.VehicleEvent
---@param fn function
---@overload fun(event: "spawn", fn: fun(v: renut.Vehicle))
---@overload fun(event: "removed", fn: fun(v: renut.Vehicle))
---@overload fun(event: "tick", fn: fun(v: renut.Vehicle, dt: number))
---@overload fun(event: "damage", fn: fun(v: renut.Vehicle, amount: number, part: renut.Part?, attacker: integer))
---@overload fun(event: "health", fn: fun(v: renut.Vehicle, health: number))
---@overload fun(event: "part_added", fn: fun(v: renut.Vehicle, part: renut.Part))
---@overload fun(event: "part_removed", fn: fun(v: renut.Vehicle, part: renut.Part))
---@overload fun(event: "collide", fn: fun(v: renut.Vehicle, strength: number, part: renut.Part?, impact: renut.Vec, other: integer?))
---@overload fun(event: "enter", fn: fun(v: renut.Vehicle, driver: integer, player: boolean))
---@overload fun(event: "exit", fn: fun(v: renut.Vehicle, driver: integer, player: boolean))
---@return renut.Handle
function renut.vehicle.on(event, fn) end

---Every vehicle in the game now.
---@return renut.Vehicle[]
function renut.vehicle.list() end

---A vehicle's resources: `"fuel"`, `"ammo"` and `"power"`, or `0`, `1` and `2`.
---@alias renut.ResourceKind "fuel"|"ammo"|"power"|0|1|2

---What `v:input()` returns.
---@class renut.VehicleInput
---@field steer number `-1`-`1`.
---@field stick_y number `-1`-`1`.
---@field brake number `0`-`1`.
---@field accelerate number `0`-`1`.
---@field braking boolean

---A vehicle. Compares equal (`==`) to another for the same vehicle.
---@class renut.Vehicle
---@field address integer The vehicle's address in game memory.
local Vehicle = {}

---Whether it has a physics body right now. Never raises.
---@return boolean
function Vehicle:has_body() end

---Where it is.
---@return renut.Vec
function Vehicle:position() end

---Teleports the vehicle (its centre of mass moves with it).
---@param p renut.VecLike
function Vehicle:set_position(p) end

---Moves it by `d`.
---@param d renut.VecLike
function Vehicle:move(d) end

---Its velocity, units per second.
---@return renut.Vec
function Vehicle:velocity() end

---Sets its velocity, units per second. Doesn't wake a resting vehicle.
---@param vel renut.VecLike
function Vehicle:set_velocity(vel) end

---Its spin, radians per second about each axis.
---@return renut.Vec
function Vehicle:angular_velocity() end

---Sets its spin, radians per second about each axis.
---@param w renut.VecLike
function Vehicle:set_angular_velocity(w) end

---Its forward axis (a unit vector).
---@return renut.Vec
function Vehicle:forward() end

---Its up axis (a unit vector).
---@return renut.Vec
function Vehicle:up() end

---Its right axis (a unit vector).
---@return renut.Vec
function Vehicle:right() end

---Speed along its forward axis (negative backwards).
---@return number
function Vehicle:speed() end

---Its mass (`math.huge` for an immovable body).
---@return number
function Vehicle:mass() end

---*Event.* Applies `force` for `dt` seconds: a thrust. Call it each step from `on_physics`.
---@param force renut.VecLike
---@param dt number
---@param point? renut.VecLike A world position: the force acts there and can spin the vehicle.
function Vehicle:push(force, dt, point) end

---*Event.* A kick: changes its velocity at once by `j / mass`; with `point`, it spins it too.
---@param j renut.VecLike
---@param point? renut.VecLike
function Vehicle:impulse(j, point) end

---*Event.* A spin kick.
---@param j renut.VecLike
function Vehicle:angular_impulse(j) end

---Makes it handle `scale` times as heavy, until the game rebuilds its body (when isn't known yet, so check it held).
---@param scale number Above 0: `2` is twice the mass.
function Vehicle:scale_mass(scale) end

---*Event.* True when there's nothing within `reach` units straight below its lowest parts.
---@param reach? number Default `1.5`.
---@return boolean
function Vehicle:in_air(reach) end

---`-1` (full reverse or brake) to `1` (full throttle): `accelerate - brake`.
---@return number
function Vehicle:throttle() end

---`-1` (left) to `1` (right).
---@return number
function Vehicle:steering() end

---The driver's input. For a vehicle without wheels it only works inside a `renut` event.
---@return renut.VehicleInput
function Vehicle:input() end

---*Event.* The driver (an actor's address), or `nil` when nobody drives it.
---@return integer?
function Vehicle:driver() end

---Its health.
---@return number
function Vehicle:health() end

---Its most health.
---@return number
function Vehicle:max_health() end

---Its health, `0`-`1`.
---@return number
function Vehicle:health_fraction() end

---`0`-`3` (how much it smokes).
---@return integer
function Vehicle:damage_level() end

---*Event.* Sets its health, clamped to `0`-max; `0` disables the vehicle. Doesn't fire the `health` event.
---@param h number
function Vehicle:set_health(h) end

---*Event.* Full health, no longer disabled.
function Vehicle:repair() end

---*Event.* Damages the vehicle as a whole, the game's way. Doesn't fire the `damage` event.
---@param amount number
---@param part? renut.Part
---@param attacker? integer An address.
function Vehicle:damage(amount, part, attacker) end

---Whether it's disabled (no health, no drive).
---@return boolean
function Vehicle:disabled() end

---Whether it ignores damage.
---@return boolean
function Vehicle:invulnerable() end

---Sets whether it ignores damage. With no argument it turns it on.
---@param on? boolean
function Vehicle:set_invulnerable(on) end

---How much of a resource it has.
---@param kind renut.ResourceKind
---@return number
function Vehicle:resource(kind) end

---The most of a resource it can have.
---@param kind renut.ResourceKind
---@return number
function Vehicle:max_resource(kind) end

---Sets a resource, clamped to `0`-most (a vehicle with no tanks has a most of 0).
---@param kind renut.ResourceKind
---@param value number
function Vehicle:set_resource(kind, value) end

---*Event.* Uses some up, the game's way; returns whether there was enough.
---@param kind renut.ResourceKind
---@param amount number
---@return boolean
function Vehicle:use(kind, amount) end

---Whether it never runs out of a resource.
---@param kind renut.ResourceKind
---@return boolean
function Vehicle:infinite(kind) end

---Sets whether it never runs out of a resource. `on` defaults to `true`.
---@param kind renut.ResourceKind
---@param on? boolean
function Vehicle:set_infinite(kind, on) end

---Shorthand for `v:resource("fuel")`.
---@return number
function Vehicle:fuel() end

---Shorthand for `v:max_resource("fuel")`.
---@return number
function Vehicle:max_fuel() end

---Shorthand for `v:set_resource("fuel", x)`.
---@param x number
function Vehicle:set_fuel(x) end

---Shorthand for `v:resource("ammo")`.
---@return number
function Vehicle:ammo() end

---Shorthand for `v:max_resource("ammo")`.
---@return number
function Vehicle:max_ammo() end

---Shorthand for `v:set_resource("ammo", x)`.
---@param x number
function Vehicle:set_ammo(x) end

---Every part on it.
---@return renut.Part[]
function Vehicle:parts() end

---The parts that are (or derive from) `class`: `v:find_parts("vehicleBlockWheel")`.
---@param class string
---@return renut.Part[]
function Vehicle:find_parts(class) end

---Its name, as the player saved it.
---@return string
function Vehicle:name() end

---The level it's in (an address).
---@return integer
function Vehicle:level() end

---*Event.* It goes from the level, with its parts. Raises an error while the player is in it.
function Vehicle:remove() end

---Its rotation.
---@return renut.Quat
function Vehicle:rotation() end

---*Event.* Turns it (its centre of mass with it) and wakes it.
---@param q renut.Quat
function Vehicle:set_rotation(q) end

---Which way it faces, in degrees (0 faces +z, 90 faces +x).
---@return number
function Vehicle:yaw() end

---*Event.* Stands it upright facing `degrees`.
---@param degrees number
function Vehicle:set_yaw(degrees) end

---Renames it (up to 63 characters).
---@param name string
function Vehicle:set_name(name) end

---------------------------------------------------------------------------------------------------
-- Effects
---------------------------------------------------------------------------------------------------

renut.fx = {}

---A place: a vector, `"front"` (in front of Banjo, facing him) or `"banjo"` (where he is).
---@alias renut.Place renut.VecLike|"front"|"banjo"

---*Event.* Plays one of the game's particle effects at `position`, in `vehicle`'s level.
---Returns `false` if this level hasn't loaded that effect.
---@param effect integer|string Its ID (`0x4582A557` or `"0x4582A557"`), or its asset reference name.
---@param position? renut.Place Default: `"banjo"`.
---@param vehicle? renut.Vehicle The level it plays in. Default: Banjo's.
---@param seconds? number How long it plays. Default: its own length.
---@return boolean played
function renut.fx.play(effect, position, vehicle, seconds) end

---------------------------------------------------------------------------------------------------
-- The game's state
---------------------------------------------------------------------------------------------------

renut.game = {}

---The world being played: its number (`1` Showdown Town ... `8` the car park), game name and scene's asset id.
---All `nil` when none is.
---@return integer? number
---@return string? name `"nuttyacres"`.
---@return integer? scene Its scene's asset id, which tells acts and mod worlds apart.
function renut.game.world() end

---The running challenge's asset id, or `nil`.
---@return integer?
function renut.game.challenge() end

---A challenge's asset id from its asset name, to compare with `renut.game.challenge()`.
---@param name string
---@return integer
function renut.game.challenge_id(name) end

---Whether the pause menu is open.
---@return boolean
function renut.game.paused() end

---A game flag, player 1's in the save loaded. Raises an error without a save.
---@param name string Its full name (`"gameFlag_Normal_..."`, `_Global_` or `_Volatile_`).
---@return boolean
function renut.game.flag(name) end

---Sets a game flag on the game's next frame, saved with the game. Raises an error in an online session that doesn't allow cheats.
---@param name string Its full name (`"gameFlag_Normal_..."`, `_Global_` or `_Volatile_`).
---@param on? boolean Default `true`.
function renut.game.set_flag(name, on) end

---A game counter, player 1's in the save loaded. Raises an error without a save.
---@param name string Its name without `"gameCounter_BanjoX_"` (`"JiggiesBanked"`, `"Notes"`), or its full name.
---@return integer
function renut.game.counter(name) end

---Sets a game counter on the game's next frame, saved with the game. Raises an error in an online session that doesn't allow cheats.
---@param name string Its name without `"gameCounter_BanjoX_"` (`"JiggiesBanked"`, `"Notes"`), or its full name.
---@param value integer
function renut.game.set_counter(name, value) end

---Adds `n` to a game counter (as `set_counter` does, without the game's pop-up).
---@param name string Its name without `"gameCounter_BanjoX_"` (`"JiggiesBanked"`, `"Notes"`), or its full name.
---@param n integer
function renut.game.add_counter(name, n) end

---*Event.* Adds to a counter as the game awards it (`"Notes"`, `"JiggiesWon_NuttyAcres"`), with its pop-up.
---@param counter string
---@param amount? integer Default 1.
---@param show? boolean `false` for no pop-up. Default `true`.
function renut.game.award(counter, amount, show) end

---*Event.* Starts a challenge, as an NPC's does, with the player put at its start. It must be loaded in this world, with none running.
---@param challenge string Its asset: `"aid_challenge_..."`, its name or id.
function renut.game.challenge_start(challenge) end

---*Event.* While one is being played, ends it as the game's own ending does. Its rewards follow as they would.
---@param won? boolean Won (`true`, the default) or lost.
function renut.game.challenge_end(won) end

---*Event.* The challenge stage's time limit and how much of it has gone, in seconds.
---@param limit number `0` takes the limit away.
---@param elapsed? number Default 0.
function renut.game.challenge_time(limit, elapsed) end

---Holds the challenge's timer (`false` lets it go).
---@param held? boolean Default `true`.
function renut.game.challenge_hold(held) end

---@alias renut.GameEvent
---| "world_enter"      # A world (or act) has loaded and is being played.
---| "world_leave"      # It's going (to another, or to the menus).
---| "challenge_start"  # A challenge begins (its asset id).
---| "challenge_finish" # Its outcome is settled: `won` for player 1, `outcome` `1` won, `2` lost, `0` neither.
---| "paused"           # The pause menu opens.
---| "resumed"          # The pause menu closes.
---| "tick"             # Once a frame while a level runs, on the game's thread, on foot or driving.
---| "save"             # The game began saving.
---| "load"             # A save's data is up (a save was loaded or started).

---Listens to the game's state. It returns a handle: `h:off()` stops it.
---@param event renut.GameEvent
---@param fn function
---@overload fun(event: "world_enter", fn: fun(number: integer, name: string, scene: integer))
---@overload fun(event: "world_leave", fn: fun(number: integer, name: string, scene: integer))
---@overload fun(event: "challenge_start", fn: fun(challenge: integer))
---@overload fun(event: "challenge_finish", fn: fun(challenge: integer, won: boolean, outcome: integer))
---@overload fun(event: "paused", fn: fun())
---@overload fun(event: "resumed", fn: fun())
---@overload fun(event: "tick", fn: fun())
---@overload fun(event: "save", fn: fun())
---@overload fun(event: "load", fn: fun())
---@return renut.Handle
function renut.game.on(event, fn) end

---*Event.* Saves the game as its own autosave does; `false` when it can't now (no storage, a save going on).
---@return boolean
function renut.game.save() end

---*Event.* Opens (`true`, the default) or closes the pause menu, as Start and Resume do.
---@param paused? boolean
function renut.game.set_paused(paused) end

---How fast the game runs: `1` normal, `0.5` slow motion. Everything the game times by its frame takes it.
---@return number
function renut.game.time_scale() end

---Sets how fast the game runs, `0.05` to `4` (`1`: normal).
---@param scale? number Default `1`.
function renut.game.set_time_scale(scale) end

---------------------------------------------------------------------------------------------------
-- Crates
---------------------------------------------------------------------------------------------------

renut.crates = {}

---One part a crate gives.
---@class renut.CrateItem
---@field aid integer The part's asset id (`renut.parts.aid` names the parts).
---@field count integer

---One crate still to collect in this world.
---@class renut.CrateInWorld
---@field flag string
---@field position renut.Vec
---@field object integer

---Every crate's flag (`"gameFlag_Normal_ShowdownTown_CollectedCrate_LOG_05"`): the worlds' crates, LOG's shop, prizes...
---@return string[]
function renut.crates.list() end

---What a crate gives.
---@param flag string
---@return renut.CrateItem[]
function renut.crates.contents(flag) end

---This crate gives what `other` gives, from now on and every time the game counts the garage again.
---@param flag string
---@param other? string `nil` (or `""`) undoes it.
function renut.crates.swap(flag, other) end

---*Event.* Gives the crate as if collected: sets its flag and adds its parts.
---@param flag string
---@param show? boolean `false` doesn't show what was in it. Default `true`.
function renut.crates.give(flag, show) end

---The crates still to collect in this world.
---@return renut.CrateInWorld[]
function renut.crates.in_world() end

---*Event.* Places a crate for `flag` in this world and returns its object. Visit Showdown Town once each session first.
---@param flag string A crate's flag (`renut.crates.list`); one already collected works too.
---@param at? renut.Place Default: `"front"`.
---@return integer? object
function renut.crates.spawn(flag, at) end

---@alias renut.CrateEvent
---| "award" # A crate is given, before its parts are added, so a `swap` in it still changes this one.

---Listens to every crate given. It returns a handle: `h:off()` stops it.
---@param event renut.CrateEvent
---@param fn function
---@overload fun(event: "award", fn: fun(flag: string, contents: renut.CrateItem[]))
---@return renut.Handle
function renut.crates.on(event, fn) end

---------------------------------------------------------------------------------------------------
-- Objects and characters
---------------------------------------------------------------------------------------------------

renut.objects = {}

---*Event.* Makes an object of a kind the game has made this session, or from an objparams asset this world has
---loaded (`"aid_objparams_..."`, its name or `"0x1F..."`).
---@param kind string
---@param at? renut.Place Default: `"front"`.
---@param yaw? number Degrees.
---@return integer? object
function renut.objects.spawn(kind, at, yaw) end

---Every kind learned so far (the objTag without `objTag_BanjoX_`, and its class).
---@return { name: string, class: string }[]
function renut.objects.kinds() end

---The objects in this world; `kind` is `""` for one made before reNut was watching.
---@return { object: integer, kind: string, position: renut.Vec }[]
function renut.objects.list() end

---*Event.* It goes, with what it holds.
---@param object integer
function renut.objects.remove(object) end

---Its kind, or `""`.
---@param object integer
---@return string
function renut.objects.kind(object) end

---Where it is.
---@param object integer
---@return renut.Vec
function renut.objects.position(object) end

---Whether it's a character (an actor).
---@param object integer
---@return boolean
function renut.objects.is_character(object) end

---@alias renut.ObjectEvent
---| "collected" # A collectable (the game's `entityPropsCollectable` objects: a challenge's pickups) is collected.
---| "spawn"     # The game made an object (it may join its world's list only on the world's next update).
---| "removed"   # The game is removing an object (not `renut.objects.remove`'s).

---Listens to every object in the game. It returns a handle: `h:off()` stops it.
---@param event renut.ObjectEvent
---@param fn function
---@return renut.Handle
---@overload fun(event: "collected", fn: fun(object: integer, kind: string)): renut.Handle
---@overload fun(event: "spawn", fn: fun(object: integer, kind: string, character: boolean)): renut.Handle
---@overload fun(event: "removed", fn: fun(object: integer, kind: string)): renut.Handle
function renut.objects.on(event, fn) end

---*Event.* Moves it there.
---@param object integer
---@param at renut.VecLike
function renut.objects.move(object, at) end

---*Event.* Turns it, in degrees.
---@param object integer
---@param yaw number
---@param pitch? number
---@param roll? number
function renut.objects.turn(object, yaw, pitch, roll) end

---Which way it faces, in degrees (0 faces +z, 90 faces +x).
---@param object integer
---@return number
function renut.objects.yaw(object) end

renut.actors = {}

---A character: its object, or who as text (`"nearest"`, `"last"`, `"banjo"`, or a character's name).
---@alias renut.Who integer|string

---*Event.* Makes one of the game's characters (`"npc_mumbo"`, `"jinjo"`, `"gruntbot_shooter"`) in any world.
---A mod's character is `"<world>/<id>"`, as `renut.npcs.spawn` makes it.
---@param character string
---@param at? renut.Place Default: `"front"`.
---@param yaw? number Degrees.
---@return integer? character
function renut.actors.spawn(character, at, yaw) end

---The character `who` names, or `nil`.
---@param who? renut.Who Default: the nearest.
---@return integer?
function renut.actors.find(who) end

---The characters in this world.
---@return { object: integer, kind: string, position: renut.Vec }[]
function renut.actors.list() end

---*Event.* Moves it there.
---@param actor renut.Who
---@param at? renut.Place Default: `"banjo"`.
function renut.actors.place(actor, at) end

---*Event.* Turns it to face `yaw` degrees.
---@param actor renut.Who
---@param yaw number
function renut.actors.turn(actor, yaw) end

---*Event.* Plays its animation of that name.
---@param actor renut.Who
---@param animation string
function renut.actors.play(actor, animation) end

---Its behaviour's state, or `""`.
---@param actor renut.Who
---@return string
function renut.actors.state(actor) end

---*Event.* Puts it in a state: `"Idle"`, `"Follow"`, `"Celebrate"` and the rest of the game's list.
---@param actor renut.Who
---@param state string
function renut.actors.set_state(actor, state) end

---*Event.* Puts its body in an animation state (`"Walk"`, `"Talk"`, `"Hit"`).
---@param actor renut.Who
---@param state string
---@return boolean
function renut.actors.body_state(actor, state) end

---*Event.* Plays one of its mind's actions (`"npc_idle"`, `"celebrate"`).
---@param actor renut.Who
---@param action string
---@return boolean
function renut.actors.action(actor, action) end

---*Event.* It walks there.
---@param actor renut.Who
---@param at? renut.Place Default: `"banjo"`.
function renut.actors.walk_to(actor, at) end

---*Event.* It drops what it was going to do.
---@param actor renut.Who
function renut.actors.stop(actor) end

---*Event.* It goes (not Banjo).
---@param actor renut.Who
function renut.actors.remove(actor) end

---Its health and most health (its body's).
---@param actor renut.Who
---@return number health
---@return number max
function renut.actors.health(actor) end

---*Event.* Sets its health, bringing a dead one back (its most raised to fit); `0` defeats it as damage would.
---@param actor renut.Who
---@param health number
function renut.actors.set_health(actor, health) end

---*Event.* Takes `amount` off, past what makes Banjo immune on foot; at `0` it's dead. Returns what's left.
---@param actor renut.Who
---@param amount number
---@return number left
function renut.actors.damage(actor, amount) end

---*Event.* It dies as the game kills a character, and goes (Banjo stays).
---@param actor renut.Who
function renut.actors.defeat(actor) end

---*Event.* It targets `target` and goes into its Follow state. A Gruntbot's target is what it attacks.
---@param actor renut.Who
---@param target? renut.Who|renut.Vehicle A character, or a vehicle object. Default: Banjo.
function renut.actors.follow(actor, target) end

---*Event.* It drops its target and what it was doing.
---@param actor renut.Who
function renut.actors.unfollow(actor) end

---What it targets (an address), or `nil`.
---@param actor renut.Who
---@return integer?
function renut.actors.target(actor) end

---*Event.* Gives it another model loaded in this world. Its animations must fit the new model's skeleton.
---@param actor renut.Who
---@param model string `"aid_model_..."`, its name or id.
function renut.actors.set_model(actor, model) end

---@alias renut.ActorEvent
---| "hit"      # A character takes a hit (its body's, Banjo's too).
---| "defeated" # A character dies, however.
---| "spawn"    # The game made a character.

---Listens to every character in the game. It returns a handle: `h:off()` stops it.
---@param event renut.ActorEvent
---@param fn function
---@return renut.Handle
---@overload fun(event: "hit", fn: fun(actor: integer, attacker: integer, damage: number)): renut.Handle
---@overload fun(event: "defeated", fn: fun(actor: integer, killer: integer)): renut.Handle
---@overload fun(event: "spawn", fn: fun(actor: integer, kind: string)): renut.Handle
function renut.actors.on(event, fn) end

---*Event.* Its velocity, units per second (the game's GetVelocity message).
---@param actor renut.Who
---@return renut.Vec
function renut.actors.velocity(actor) end

---*Event.* Adds `v` to its velocity, as a knock-back does (the game's ApplyVelocityAccumulator).
---@param actor renut.Who
---@param v renut.VecLike
function renut.actors.push(actor, v) end

---*Event.* Whether it stands on the floor.
---@param actor renut.Who
---@return boolean
function renut.actors.on_ground(actor) end

---*Event.* Its scale.
---@param actor renut.Who
---@return number
function renut.actors.scale(actor) end

---*Event.* It gets out of the vehicle it drives or rides, as when knocked out; `false` when it's in none.
---@param actor renut.Who
---@return boolean
function renut.actors.exit_vehicle(actor) end

---Which way it faces, in degrees (0 faces +z, 90 faces +x).
---@param actor renut.Who
---@return number
function renut.actors.yaw(actor) end

---Its body's state now: `"Stand"`, `"Walk"`, `"Jump"`, `"Swim"`... (`actors.body_state` sets one); `""` for none.
---@param actor renut.Who
---@return string
function renut.actors.current_body_state(actor) end

---The vehicle it drives or rides, or `nil`.
---@param actor renut.Who
---@return renut.Vehicle?
function renut.actors.vehicle(actor) end

---*Event.* It speaks a dialog: `true` when it starts.
---@param actor renut.Who
---@param dialog string
---@return boolean started
function renut.actors.talk(actor, dialog) end

---------------------------------------------------------------------------------------------------
-- Trigger zones
---------------------------------------------------------------------------------------------------

renut.zones = {}

---What a zone watches, or a list of them.
---@alias renut.ZoneWatch
---| "banjo"      # Banjo (the default).
---| "vehicle"    # The vehicle he drives.
---| "characters" # Every character.
---| "all"        # All of them.
---| string[]     # A list of them.

---The table `renut.zones.add` takes: a sphere with `radius`, or a box with `size`.
---@class renut.ZoneDef
---@field name string Adding a zone with the same name again replaces it.
---@field at? renut.Place Its centre. Default: `"banjo"`.
---@field radius? number A sphere's radius. 0 is 5 units.
---@field size? renut.VecLike A box's width, height and depth, in place of `radius`. A size of 0 is 5 units.
---@field watch? renut.ZoneWatch Default `"banjo"`.
---@field world? string A world's name (`"nuttyacres"`) or a mod world's id to only work there. Default: any.

---Adds (or replaces) a zone, checked every game tick. Zones stay until removed, across worlds.
---@param def renut.ZoneDef
---@return string name
function renut.zones.add(def) end

---Moves a zone. Raises `no zone '...'` for a name nobody added.
---@param name string
---@param at? renut.Place Default: `"banjo"`.
function renut.zones.move(name, at) end

---Removes a zone.
---@param name string
function renut.zones.remove(name) end

---The zones' names.
---@nodiscard
---@return string[]
function renut.zones.list() end

---How many watched things are in it.
---@nodiscard
---@param name string
---@return integer
function renut.zones.count(name) end

---Whether `object` is in it; without one, whether Banjo is.
---@param name string
---@param object? integer
---@return boolean
function renut.zones.inside(name, object) end

---@alias renut.ZoneEvent
---| "enter" # Something it watches came in. `what` is `"banjo"`, `"vehicle"` or `"character"`.
---| "exit"  # Something it watches went out (not when a world goes).

---Listens to every zone, inside the game's tick, so anything can be called from it.
---@param event renut.ZoneEvent
---@param fn function
---@overload fun(event: "enter", fn: fun(zone: string, object: integer, what: string))
---@overload fun(event: "exit", fn: fun(zone: string, object: integer, what: string))
---@return renut.Handle
function renut.zones.on(event, fn) end

---------------------------------------------------------------------------------------------------
-- The HUD
---------------------------------------------------------------------------------------------------

renut.hud = {}

---How the panel's timer runs.
---@alias renut.TimerRun
---| "down" # Counts down; `timer_done` fires at 0 (the default).
---| "up"   # Counts up.
---| "hold" # Stays where it is.

---The table `renut.hud.panel` takes: the parts it shows.
---@class renut.PanelDef
---@field timer? boolean
---@field counter? boolean

---*Event.* The game's message box with `text` and no buttons.
---@param text string
---@param seconds? number Default 3; `0` stays until `renut.hud.close`.
function renut.hud.message(text, seconds) end

---*Event.* Closes the message box.
function renut.hud.close() end

---*Event.* The message box with Confirm and Cancel buttons. Returns the choice's number (`renut.hud.on("choice")` gets it too).
---@param text string
---@param fn? fun(confirmed: boolean) `true` for Confirm, `false` for Cancel.
---@return integer? choice
function renut.hud.choice(text, fn) end

---*Event.* Shows the panel's timer at `seconds`; called again, it sets it.
---Raises an error when the panel is up without a timer: `hide` it first.
---@param seconds? number Default 0.
---@param run? renut.TimerRun Default `"down"`.
function renut.hud.timer(seconds, run) end

---Sets the panel's timer without showing the panel.
---@param seconds number
---@param run? renut.TimerRun Default `"down"`.
function renut.hud.set_timer(seconds, run) end

---*Event.* Shows the panel's counter: `value`, or `"value/target"` with a target. Called again, it sets it.
---Raises an error when the panel is up without a counter: `hide` it first.
---@param value? integer Default 0.
---@param target? integer
function renut.hud.counter(value, target) end

---Sets the panel's counter without showing the panel.
---@param value integer
---@param target? integer
function renut.hud.set_counter(value, target) end

---*Event.* Shows the panel with the parts `def` asks for. Its parts are chosen as it shows: to add one, `hide` it first.
---@param def renut.PanelDef
function renut.hud.panel(def) end

---The timer's seconds.
---@return number
function renut.hud.time() end

---*Event.* The panel goes.
function renut.hud.hide() end

---@alias renut.HudEvent
---| "timer_done" # A countdown reached 0.
---| "choice"     # A choice was answered.

---Listens to the HUD. It returns a handle: `h:off()` stops it.
---@param event renut.HudEvent
---@param fn function
---@overload fun(event: "timer_done", fn: fun())
---@overload fun(event: "choice", fn: fun(choice: integer, confirmed: boolean))
---@return renut.Handle
function renut.hud.on(event, fn) end

---`renut.hud.text`'s options.
---@class renut.HudTextOptions
---@field x? number `0`-`1` across. Default `0.5`.
---@field y? number `0`-`1` down. Default `0.15`.
---@field size? number Its height, a fraction of the screen's. Default `0.035`.
---@field colour? integer|{r: number, g: number, b: number}
---@field alpha? number
---@field align? "left"|"center"|"right"
---@field background? boolean
---@field seconds? number `0` (the default): until removed.

---Text on the screen (RexFabric's overlay), kept by `key`: called again, it changes.
---@param key string|integer
---@param text any
---@param opts? renut.HudTextOptions
function renut.hud.text(key, text, opts) end

---`renut.hud.bar`'s options.
---@class renut.HudBarOptions
---@field x? number Its left. Default `0.35`.
---@field y? number Its top. Default `0.9`.
---@field width? number Default `0.3`.
---@field height? number Default `0.025`.
---@field colour? integer|{r: number, g: number, b: number} Default green.
---@field back_colour? integer|{r: number, g: number, b: number} Default black.
---@field alpha? number
---@field back_alpha? number Default `0.6`.
---@field label? string Text over it.
---@field seconds? number `0` (the default): until removed.

---A bar filled `fraction` (`0`-`1`) of the way, kept by `key`.
---@param key string|integer
---@param fraction number
---@param opts? renut.HudBarOptions
function renut.hud.bar(key, fraction, opts) end

---`renut.hud.marker`'s options.
---@class renut.MarkerOptions
---@field colour? integer|{r: number, g: number, b: number} Default gold.
---@field alpha? number
---@field size? number The dot's radius, a fraction of the screen's height. Default `0.012`.
---@field text_size? number Default `0.025`.

---A dot (and `text` over it) on a world position or an object, followed every frame until removed.
---@param key string|integer
---@param at integer|renut.VecLike An object, or a position.
---@param text? string
---@param opts? renut.MarkerOptions
function renut.hud.marker(key, at, text, opts) end

---Removes your text, bar or marker with that key.
---@param key string|integer
function renut.hud.remove(key) end

---Removes all your text, bars and markers.
function renut.hud.clear() end

---`renut.hud.menu`'s table.
---@class renut.MenuDef
---@field title? string
---@field options string[]
---@field fn? fun(index: integer?, label: string?) Picked: its number and text; closed: `nil`.

---A menu picked from with the d-pad, the left stick, the arrow keys or the mouse; A, Return or a click picks, B or
---Escape closes it. The game doesn't see the pad while it's open. One at a time: a new one closes the last.
---@param def renut.MenuDef
function renut.hud.menu(def) end

---Closes the menu (its `fn` gets `nil`).
function renut.hud.close_menu() end

---Whether a menu is open.
---@return boolean
function renut.hud.menu_open() end

---------------------------------------------------------------------------------------------------
-- Debug shapes
---------------------------------------------------------------------------------------------------

renut.debug = {}

---Debug shapes' options.
---@class renut.DebugOptions
---@field colour? integer|{r: number, g: number, b: number} Default green (text: white).
---@field alpha? number
---@field thickness? number Pixels.
---@field filled? boolean Spheres. Default `false`.
---@field size? number Text: its height, a fraction of the screen's.
---@field background? boolean Text.
---@field seconds? number How long it stays. Default a tenth of a second: call it every tick to keep it.
---@field key? string The shape with the same key is replaced.

---A line in the world.
---@param a renut.VecLike
---@param b renut.VecLike
---@param opts? renut.DebugOptions
function renut.debug.line(a, b, opts) end

---A sphere in the world (drawn as its outline on screen).
---@param at renut.VecLike
---@param radius? number Default `1`.
---@param opts? renut.DebugOptions
function renut.debug.sphere(at, radius, opts) end

---A box in the world, along the world's axes.
---@param at renut.VecLike Its centre.
---@param size renut.VecLike Its width, height and depth.
---@param opts? renut.DebugOptions
function renut.debug.box(at, size, opts) end

---Text at a world position.
---@param at renut.VecLike
---@param text any
---@param opts? renut.DebugOptions
function renut.debug.text(at, text, opts) end

---Removes every debug shape.
function renut.debug.clear() end

---------------------------------------------------------------------------------------------------
-- Sound
---------------------------------------------------------------------------------------------------

renut.sound = {}

---`renut.sound.play` options.
---@class renut.SoundOptions
---@field volume? number `0` to `4`. Default `1`.
---@field pitch? number Speed and pitch together: `2` is an octave up and twice as fast. Default `1`.
---@field loop? boolean Plays until stopped. Default `false`.
---@field at? renut.VecLike A position: the sound gets quieter away from the camera and comes from its side.
---@field range? number With `at`, how far from the camera it fades out to nothing, in units. Default `70`.

---Plays a sound file of your mod's (`.wav`, `.mp3`, `.flac` or `.ogg`). Returns `nil` when the file won't play.
---@param file string A path in your mod's folder, or a full one.
---@param options? renut.SoundOptions
---@return renut.Sound?
function renut.sound.play(file, options) end

---Makes `file` the music of every world from now on: it loops, the level's own music paused, as a mod world's music does.
---With no file, the worlds get their own back.
---@param file? string A path in your mod's folder, or a full one.
---@param volume? number Default `1`.
function renut.sound.music(file, volume) end

---A sound `renut.sound.play` started.
---@class renut.Sound
local Sound = {}

---Stops it.
function Sound:stop() end

---Whether it's still playing (a sound paused with the game counts).
---@return boolean
function Sound:playing() end

---Changes its volume.
---@param v number
function Sound:set_volume(v) end

---Moves it (`nil`: everywhere); to follow something, move it from a tick event.
---@param at? renut.VecLike
function Sound:move(at) end

---Changes its speed and pitch together (`2`: an octave up), `0.05` to `8`.
---@param pitch number
function Sound:set_pitch(pitch) end

---`renut.sound.cue`'s options.
---@class renut.CueOptions
---@field volume? number Default `1`.

---*Event.* Plays one of the game's own sound cues by its id (as the Stereo plays the game's songs), without a
---position. Its wavebank must be loaded; `nil` when it doesn't play.
---@param cue integer|string Its 32-bit id: `0x1BFBF80D` or `"0x1BFBF80D"`.
---@param opts? renut.CueOptions
---@return renut.Cue?
function renut.sound.cue(cue, opts) end

---A cue `renut.sound.cue` started.
---@class renut.Cue
---@field cue integer
local Cue = {}

---*Event.* Stops it.
function Cue:stop() end

---*Event.* Whether it's still playing.
---@return boolean
function Cue:playing() end

---------------------------------------------------------------------------------------------------
-- Menu rows
---------------------------------------------------------------------------------------------------

renut.menu = {}

---`renut.menu.add`'s row.
---@class renut.MenuRowDef
---@field list string The list's XUI Id (`"singlePlayerList"`), or the end of its Id path (`"StartMenu/singlePlayerList"`).
---@field text string Up to 127 characters.
---@field on_select? fun(row: renut.MenuRow, user: integer) A on the row (`user`: the controller, `0`-`3`); the menu never sees that press.
---@field key? string Adding a row with the same key again changes that row. Default: `list` and `text`.

---Adds a row of your mod's to the end of one of the game's list menus; it shows the next time the menu fills its list.
---@param def renut.MenuRowDef
---@return renut.MenuRow
function renut.menu.add(def) end

---The Id path of every list the game has filled since it started ("StartMenu/singlePlayerList").
---@return string[]
function renut.menu.lists() end

---A row `renut.menu.add` added.
---@class renut.MenuRow
---@field id integer
---@field key string
---@field list string
---@field text string
---@field on_select? fun(row: renut.MenuRow, user: integer)
local MenuRow = {}

---Changes its text; an open menu shows it at once.
---@param text string
function MenuRow:set_text(text) end

---Takes it out; an open menu drops it the next time it fills its list.
function MenuRow:remove() end

---------------------------------------------------------------------------------------------------
-- Online
---------------------------------------------------------------------------------------------------

renut.net = {}

---Sends `value` (nil, boolean, number, string, or tables of them) as message `name`, to `peer` or everyone.
---@param name string 1-64 characters.
---@param value any About 1000 bytes at most.
---@param peer? integer One of `renut.net.peers()`.
function renut.net.send(name, value, peer) end

---`fn(value, peer)` for each message `name` that comes in. It returns a handle: `h:off()` stops it.
---@param name string
---@param fn fun(value: any, peer: integer)
---@return renut.Handle
function renut.net.on(name, fn) end

---`fn()` once a frame, menus included. It returns a handle: `h:off()` stops it.
---@param fn fun()
---@return renut.Handle
function renut.net.on_tick(fn) end

---The machines the game is connected to.
---@return integer[]
function renut.net.peers() end

---`"off"`, `"lobby"` (a party session is up) or `"match"` (a match session is up).
---@return "off"|"lobby"|"match"
function renut.net.state() end

---This machine hosts the match, or the lobby when there's no match.
---@return boolean
function renut.net.is_host() end

---The game is in multiplayer (its lobby or a match).
---@return boolean
function renut.net.multiplayer() end

---`renut.net.open_multiplayer`'s options.
---@class renut.OpenMultiplayerOptions
---@field live? boolean Live (`true`, default) or System Link (`false`).

---From a main menu row's `on_select`: goes to multiplayer as the main menu's Multiplayer does (its save data loaded first).
---@param opts? renut.OpenMultiplayerOptions
function renut.net.open_multiplayer(opts) end

---------------------------------------------------------------------------------------------------
-- Keeping data
---------------------------------------------------------------------------------------------------

renut.data = {}

---Numbers, text, `true`/`false` and tables of them (nested too).
---@alias renut.DataValue number|string|boolean|table

---The value kept under `key`, or `default` (or `nil`) when there's none.
---@param key string 1-256 characters.
---@param default? any
---@return any
function renut.data.get(key, default) end

---Keeps `value` under `key`, written at once; `nil` removes the key.
---@param key string 1-256 characters.
---@param value renut.DataValue?
function renut.data.set(key, value) end

---A list of the keys kept (not the save's own).
---@return string[]
function renut.data.keys() end

---As `renut.data.get`, but kept for the save being played (player 1's profile) rather than every one.
---@param key string
---@param default? any
---@return any
function renut.data.save_get(key, default) end

---As `renut.data.set`, for the save being played.
---@param key string
---@param value renut.DataValue?
function renut.data.save_set(key, value) end

---The keys kept for the save being played.
---@return string[]
function renut.data.save_keys() end

---------------------------------------------------------------------------------------------------
-- The pad
---------------------------------------------------------------------------------------------------

renut.pad = {}

---@alias renut.PadButton
---| "a" | "b" | "x" | "y" | "lb" | "rb" | "start" | "back" | "left_stick" | "right_stick"
---| "dpad_up" | "dpad_down" | "dpad_left" | "dpad_right"

---Whether a player's button is held, as the game read it this frame (keys bound to the pad count).
---@param button renut.PadButton
---@param n? integer The player, 1-4. Default 1.
---@return boolean
function renut.pad.down(button, n) end

---Whether it went down this frame.
---@param button renut.PadButton
---@param n? integer
---@return boolean
function renut.pad.pressed(button, n) end

---Whether it came up this frame.
---@param button renut.PadButton
---@param n? integer
---@return boolean
function renut.pad.released(button, n) end

---A stick, `-1` to `1` each way (`y` up positive).
---@param side? "left"|"right" Default left.
---@param n? integer
---@return number x
---@return number y
function renut.pad.stick(side, n) end

---A trigger, `0` to `1`.
---@param side? "left"|"right" Default left.
---@param n? integer
---@return number
function renut.pad.trigger(side, n) end

---------------------------------------------------------------------------------------------------
-- Challenges made in Lua
---------------------------------------------------------------------------------------------------

renut.challenges = {}

---What a challenge's winner gets.
---@class renut.ChallengeRewards
---@field counters? table<string, integer> Counters awarded as the game awards them: `{Notes = 50}`.
---@field flags? string[] Game flags set.
---@field crates? string[] Crates given, by flag.

---The table `renut.challenges.add` takes.
---@class renut.LuaChallengeDef
---@field id string Prefix it with your mod's name.
---@field name? string
---@field intro? string A message as it starts.
---@field time_limit? number Seconds on a countdown; at 0 it's lost.
---@field show_time? boolean A timer counting up, without a limit.
---@field goal? integer A counter's target: reaching it wins.
---@field counter? boolean A counter without a target.
---@field rewards? renut.ChallengeRewards
---@field win_text? string Default "Challenge complete!".
---@field lose_text? string Default "Challenge failed".
---@field on_start? fun(c: renut.LuaChallenge)
---@field on_tick? fun(c: renut.LuaChallenge, dt: number) Every game tick while it runs.
---@field on_end? fun(c: renut.LuaChallenge, won: boolean) Won, lost, or the world left (lost).

---A running challenge.
---@class renut.LuaChallenge
---@field id string
---@field score integer
---@field time number Seconds since it started (game time).
---@field def renut.LuaChallengeDef The table `renut.challenges.add` took.
local LuaChallenge = {}

---Sets the counter (winning at `goal`).
---@param n integer
function LuaChallenge:set(n) end

---Adds to the counter. Default 1.
---@param n? integer
function LuaChallenge:add(n) end

---*Event.* Wins it: its rewards are given.
function LuaChallenge:win() end

---*Event.* Loses it.
function LuaChallenge:lose() end

---Whether it's still the one running.
---@return boolean
function LuaChallenge:running() end

---Adds a challenge of the mod's own, run by Lua: the HUD's timer and counter, its rewards. No challenge asset needed.
---@param def renut.LuaChallengeDef
---@return string id
function renut.challenges.add(def) end

---*Event.* Starts one; raises when one's running already.
---@param id string
---@return renut.LuaChallenge
function renut.challenges.start(id) end

---*Event.* Ends the running one, won (`true`) or lost (the default).
---@param won? boolean
function renut.challenges.stop(won) end

---The running one, or `nil`.
---@return renut.LuaChallenge?
function renut.challenges.running() end

---Every challenge added.
---@return {id: string, name: string, mod: string}[]
function renut.challenges.list() end

---Listens for a challenge ending: `fn(id, won, time, score)`. It returns a handle: `h:off()` stops it.
---@param event "finish"
---@param fn fun(id: string, won: boolean, time: number, score: integer)
---@return {off: fun(self)}
function renut.challenges.on(event, fn) end

---------------------------------------------------------------------------------------------------
-- Mods
---------------------------------------------------------------------------------------------------

renut.mods = {}

---The enabled mods' folder names.
---@return string[]
function renut.mods.list() end

---Whether a mod (by folder name) is enabled.
---@param name string
---@return boolean
function renut.mods.enabled(name) end

---The calling mod's folder name.
---@return string
function renut.mods.this() end

---Shares a value (a table of functions, say) with other mods, under the calling mod's name.
---@param value any
function renut.mods.export(value) end

---What a mod exported, or `nil` (it may not have loaded yet: `renut.mods.when`).
---@param name string
---@return any
function renut.mods.import(name) end

---Calls `fn(value)` once `name` exports, at once if it has.
---@param name string
---@param fn fun(value: any)
function renut.mods.when(name, fn) end

---------------------------------------------------------------------------------------------------
-- Settings
---------------------------------------------------------------------------------------------------

renut.settings = {}

---The table `renut.settings.add` takes.
---@class renut.SettingDef
---@field key string The setting's name within the mod.
---@field default boolean|integer|number|string Its type is the default's.
---@field description? string

---Adds a setting of the mod's own (a cvar `<mod>_<key>`), shown in reNut's settings (F4) under "Mod: <mod>" and
---saved with them. Returns its value now.
---@param def renut.SettingDef
---@return boolean|integer|number|string
function renut.settings.add(def) end

---A setting's value.
---@param key string
---@return boolean|integer|number|string
function renut.settings.get(key) end

---Sets a setting of the calling mod's, saved with reNut's settings.
---@param key string
---@param value boolean|integer|number|string
function renut.settings.set(key, value) end

---Calls `fn(value)` after it changes.
---@param key string
---@param fn fun(value: any)
function renut.settings.on_change(key, fn) end

---------------------------------------------------------------------------------------------------
-- The camera and the mouse cursor
---------------------------------------------------------------------------------------------------

renut.camera = {}

---The game camera's position this frame.
---@return renut.Vec
function renut.camera.position() end

---The way the camera looks, once reNut has seen a gameplay frame (`facing_known`); before that its matrix's third
---row, which may point the other way.
---@return renut.Vec
function renut.camera.forward() end

---Whether `forward` is known to be the way the camera looks.
---@return boolean
function renut.camera.facing_known() end

---Where a world position is on the screen: `0`-`1` across and down, and how far in front it is. `nil` behind the camera.
---@param p renut.VecLike
---@return number? x
---@return number? y
---@return number? depth
function renut.camera.to_screen(p) end

---Shakes the camera, easing out.
---@param strength? number Units it's thrown about. Default `1`.
---@param seconds? number Default `0.5`.
function renut.camera.shake(strength, seconds) end

---Keeps the camera on `target` from each game tick until released: `offset` from it (along a vehicle's own axes),
---looking at it plus `look_offset`.
---@param target renut.Vehicle|integer|renut.VecLike|fun(): renut.VecLike? A vehicle, an object, a position, or a function's.
---@param offset? renut.VecLike Default `{0, 4, -10}`.
---@param look_offset? renut.VecLike Default `{0, 1, 0}`.
function renut.camera.follow(target, offset, look_offset) end

---The same as `renut.camera.release`.
function renut.camera.unfollow() end

---The camera's up axis.
---@return renut.Vec
function renut.camera.up() end

---The camera's right axis.
---@return renut.Vec
function renut.camera.right() end

---Its vertical field of view, in degrees.
---@return number
function renut.camera.fov() end

---Puts the player's camera at `at`, looking at `look_at`, from the next frame until released; call it again to move it.
---Returns `false` when reNut hasn't seen the game's camera yet (a level's first frames): it takes over once it has.
---@param at renut.VecLike
---@param look_at renut.VecLike
---@return boolean
function renut.camera.set(at, look_at) end

---Gives the camera back to the game, which carries on from where it was. Leaving a world releases it too.
function renut.camera.release() end

---The vertical field of view, in place of the game's.
---@param degrees? number 5 to 150; `nil` or `0` gives the game's back.
function renut.camera.set_fov(degrees) end

renut.mouse = {}

---Where the cursor is, `0`-`1` across and down the window (`0.5, 0.5` until it has been over the window).
---@return number x
---@return number y
function renut.mouse.position() end

---Whether the cursor has been over the window.
---@return boolean
function renut.mouse.inside() end

---Whether a mouse button is held. Same as `rex.input.mouse_button`.
---@param button? integer `0` left (default), `1` right, `2` middle, `3`/`4` side.
---@return boolean
function renut.mouse.down(button) end

---The line from the camera through the cursor into the world.
---@param toward? renut.Vec Pass the vehicle's position so the camera's forward is turned the right way.
---@return renut.Vec from
---@return renut.Vec direction
function renut.mouse.ray(toward) end

---------------------------------------------------------------------------------------------------
-- The physics world
---------------------------------------------------------------------------------------------------

renut.world = {}

---*Event.* Finds the first thing the line from `from` to `to` hits in the running world's physics (`vehicle`'s when
---given). Returns `nil` when nothing is hit.
---@param from renut.VecLike
---@param to renut.VecLike
---@param vehicle? renut.Vehicle
---@param past_vehicle? boolean With a vehicle, `true` to see through it to what's beyond.
---@return renut.Vec? hit
---@return renut.Vec? normal
---@return number? fraction How far along the line it is (`0`-`1`).
function renut.world.ray_cast(from, to, vehicle, past_vehicle) end

---*Event.* The ground under a place: what a line down from just above it hits within `reach`.
---@param at renut.VecLike
---@param reach? number Default `50`.
---@return renut.Vec? hit
---@return renut.Vec? normal
function renut.world.ground(at, reach) end

---Showdown Town's time of day setting (`renut_time_of_day`): `"Game"`, `"Morning"`, `"Afternoon"`, `"Evening"` or `"Night"`.
---@return string
function renut.world.time_of_day() end

---Sets it; it takes effect the next time Showdown Town loads.
---@param time "Game"|"Morning"|"Afternoon"|"Evening"|"Night"
function renut.world.set_time_of_day(time) end

---The world's fog, as `renut.world.fog` takes and returns it. Which value does what isn't confirmed on screen.
---@class renut.Fog
---@field near? number
---@field far? number
---@field most? number
---@field colour? integer The game's packed colour: try values and read them back.

---Sets the world's fog from the next frame until the world goes; values not given stay as they are.
---With no table, returns the fog now.
---@param def? renut.Fog
---@return renut.Fog?
function renut.world.fog(def) end

---A float of the world's active environment set (448 bytes: the sun, the ambient light, the fog).
---@param offset integer A multiple of 4 under 448.
---@return number
function renut.world.env(offset) end

---Writes a float of the world's active environment set (448 bytes: the sun, the ambient light, the fog).
---@param offset integer A multiple of 4 under 448.
---@param value number
function renut.world.set_env(offset, value) end

---------------------------------------------------------------------------------------------------
-- Worlds and doors
---------------------------------------------------------------------------------------------------

renut.worlds = {}

---`got/total` for one row of a world door's stats panel.
---@class renut.StatRow
---@field got? integer
---@field total? integer

---A world door's stats panel. Each row is a total or `{got = n, total = n}`; a row left out is counted.
---@class renut.WorldStats
---@field jiggies? integer|renut.StatRow
---@field trophies? integer|renut.StatRow T.T. trophies.
---@field jinjos? integer|renut.StatRow Jinjo tokens.
---@field notes? integer|renut.StatRow

---A body of water: the *n*-th takes the scene's *n*-th water setup.
---@class renut.Water
---@field min {x: number, z: number}|number[]
---@field max {x: number, z: number}|number[]
---@field height number

---The table `renut.worlds.add` takes.
---@class renut.WorldDef
---@field id string Letters, digits and `_`. Adding a world with the same id again replaces it.
---@field name? string Its name, as shown to players. Default: the id.
---@field scene? string The asset name of its scene script. Default `renutworld_<id>`.
---@field music? string A sound file that loops while the player is in the world: a path from the mod's folder or a full path.
---@field music_volume? number How loud the music plays, `0`-`1`. Default 0.8.
---@field player_model? string A player model's id (NutTool's `renut-player-model`): Banjo is made with it in this world.
---@field stats? renut.WorldStats Its door's stats panel.
---@field water? renut.Water[] Up to 8 bodies of water.

---One row of `renut.worlds.list()`.
---@class renut.WorldInfo
---@field id string
---@field name string

---Adds a world of a mod's own, entered through a door in Showdown Town.
---@param def renut.WorldDef
function renut.worlds.add(def) end

---Every world added.
---@return renut.WorldInfo[]
function renut.worlds.list() end

---*Event.* Takes the player to the mod world `id`, as its door does. Raises `no world '...'` for an id nobody added.
---With `opts.at`, he's put there (facing `opts.yaw` degrees) over the new world's first ticks, on foot or driving.
---@param id string
---@param opts? {at: renut.VecLike, yaw?: number}
function renut.worlds.go(id, opts) end

---Sets what the stats panel of the world's doors shows. Raises `no world '...'` for an id nobody added.
---@param id string
---@param stats renut.WorldStats
function renut.worlds.stats(id, stats) end

---Sets a shape key of the world's model (every chunk of it).
---@param id string
---@param name string
---@param weight number
function renut.worlds.set_shape(id, name, weight) end

---A shape key's weight on the world's model; `nil` when it has no such shape.
---@param id string
---@param name string
---@return number?
function renut.worlds.shape(id, name) end

---Changes a material of the world while it's played.
---@param id string
---@param name string
---@param values renut.MaterialValues
function renut.worlds.set_material(id, name, values) end

---Puts a material of the world back to how NutTool made it.
---@param id string
---@param name string
function renut.worlds.reset_material(id, name) end

---The world's material names.
---@param id string
---@return string[]
function renut.worlds.materials(id) end

renut.doors = {}

---@alias renut.DoorStyle
---| "wall"   # The world door, one-sided: against a wall.
---| "arch"   # The stone arch home, two-sided: anywhere; needs `look`.
---| "palace" # L.O.G.'s palace door.
---| "hidden" # No model: a trigger in a doorway already there.

---The table `renut.doors.add` takes.
---@class renut.DoorDef
---@field world string The id of the world it leads to.
---@field position renut.VecLike|number[] Where it stands in Showdown Town.
---@field yaw? number Which way it faces, in degrees about the vertical axis. Default 0.
---@field id? string The door's own id, so a world can have several doors. Default: the world's id.
---@field style? renut.DoorStyle The kind of door. Default `"wall"`.
---@field look? string Its portal objparams asset, which gives its model and hologram, over the style's.

---Adds a door in Showdown Town into a world. It shows up the next time Showdown Town loads.
---@param def renut.DoorDef
function renut.doors.add(def) end

---------------------------------------------------------------------------------------------------
-- Characters
---------------------------------------------------------------------------------------------------

renut.npcs = {}

---A game character whose voice a mod's character can have.
---@alias renut.GameVoice
---| "banjo"
---| "kazooie"
---| "mumbo"
---| "humba"
---| "grunty"
---| "klungo"
---| "log"
---| "bottles"
---| "jinjo"
---| "pikelet"
---| "pigpig"
---| "jolly"
---| "rhino"
---| "piddles"
---| "blubber"
---| "ghostpig"
---| "penguin"
---| "boggy"
---| "mrfit"
---| "kingjingaling"
---| "thomas"

---The challenge a character gives.
---@class renut.ChallengeDef
---@field id? string The challenge's id.
---@field asset? string Its challenge asset. It must be in a bundle that's loaded while the world is.
---@field flags? string[] The game flags its asset names: seen, notes only, beaten, beaten with the T.T. trophy.
---@field jiggies? string The counter its jiggy goes into: the world's own jiggies.
---@field notes? integer The notes it pays (0 if left out), for its world door's stats panel.

---Makes a character an enemy.
---@class renut.EnemyDef
---@field health? integer The hits it takes before it's beaten. Default 3.

---The table `renut.npcs.add` takes.
---@class renut.NpcDef
---@field world string The id of the world (`renut.worlds.add`) the character is in.
---@field id string Its id in that world.
---@field name? string Its name. Default: the id.
---@field tag? string Its objTag. Default `renut_<world>_<id>`.
---@field chardata? string Its chardata asset. Default `renutnpc_<world>_<id>`.
---@field speaker? integer The `dbCharData_Id` slot its dialog lines name as their speaker, `0`-`32`. Default 32.
---@field scene? string Its world's scene script. Default `renutworld_<world>`.
---@field text? table<string, string> Texts by their full key: `["dialog__<key>"] = "..."`.
---@field challenge? renut.ChallengeDef The challenge it gives.
---@field voice? renut.GameVoice|string|string[] A game character's name, or a list of sound files played at random.
---@field voice_volume? number How loud its sound files play, `0`-`1`. Default 0.9.
---@field sounds? table<string, string> A sound file per line: `greeting`, `objectives`, `win`, `lose`, `completed` or a dialog's full key.
---@field enemy? renut.EnemyDef Makes it an enemy.

---One row of `renut.npcs.list()`.
---@class renut.NpcInfo
---@field world string
---@field id string
---@field name string
---@field tag string
---@field speaker integer
---@field challenge string The challenge's id, `""` for none.
---@field voice string The game character's name, `"clips"` for its own files, `""` for none.

---Adds a character of a mod's world that the player talks to.
---@param def renut.NpcDef
function renut.npcs.add(def) end

---Every character added.
---@return renut.NpcInfo[]
function renut.npcs.list() end

---*Event.* Puts a mod's character into the world being played, which needn't be its own, and returns it.
---@param world string The id of its world (`renut.worlds.add`).
---@param id string Its id in that world.
---@param at? renut.Place Default: `"front"`.
---@param yaw? number Degrees.
---@return integer? character
function renut.npcs.spawn(world, id, at, yaw) end

---@alias renut.NpcEvent
---| "spawn"    # Its actor appears (as the world loads).
---| "tick"     # Every frame; `dt` is seconds since the last.
---| "hit"      # An enemy was hit; its health is already lower.
---| "defeated" # An enemy was beaten; it explodes and is removed.

---Calls `fn` for every character of a mod's world while it's in play, on the game thread.
---@param event renut.NpcEvent
---@param fn function
---@overload fun(event: "spawn", fn: fun(npc: renut.Npc))
---@overload fun(event: "tick", fn: fun(npc: renut.Npc, dt: number))
---@overload fun(event: "hit", fn: fun(npc: renut.Npc, hits: integer))
---@overload fun(event: "defeated", fn: fun(npc: renut.Npc))
---@return renut.Handle
function renut.npcs.on(event, fn) end

---A character of a mod's world. The methods other than `in_play`, `shape` and `set_shape` raise
---`... isn't in play` when it isn't.
---@class renut.Npc
---@field world string
---@field id string
---@field name string
local Npc = {}

---Whether its actor is up (and not beaten).
---@return boolean
function Npc:in_play() end

---Its feet's position.
---@return renut.Vec
function Npc:position() end

---Puts it there (done after this frame's update; the game's gravity still pulls it down).
---@param p renut.VecLike
function Npc:set_position(p) end

---Which way it faces, in degrees (0 faces +z, 90 faces +x).
---@return number
function Npc:yaw() end

---Turns it to face that way.
---@param degrees number
function Npc:set_yaw(degrees) end

---Plays an animation of its table by name: `"stand"`, `"walk"`, `"run"`, `"idle1"`, `"hit"`...
---@param anim string
function Npc:play(anim) end

---An enemy's health (hits left).
---@return integer
function Npc:health() end

---Sets an enemy's health (hits left); 0 or less beats it.
---@param n integer
function Npc:set_health(n) end

---Beats it now.
function Npc:defeat() end

---A shape key's weight on its model; `nil` when it has no such shape.
---@param name string
---@return number?
function Npc:shape(name) end

---Sets a shape key of its model (a blink, a smile...).
---@param name string
---@param weight number
function Npc:set_shape(name, weight) end

renut.player = {}

---Banjo's position, driving or not; `nil` when not in a level. `n`: another player (1-4), as with the calls below.
---@param n? integer
---@return renut.Vec?
function renut.player.position(n) end

---*Event.* His velocity, units per second.
---@param n? integer
---@return renut.Vec
function renut.player.velocity(n) end

---*Event.* Adds `v` to his velocity on foot, as a knock-back does.
---@param v renut.VecLike
---@param n? integer
function renut.player.push(v, n) end

---*Event.* Whether he stands on the floor.
---@param n? integer
---@return boolean
function renut.player.on_ground(n) end

---His body's state: `"Stand"`, `"Walk"`, `"Jump"`, `"Swim"`, `"DriveVehicle"`... (the game's actorBodyStateId names).
---@param n? integer
---@return string
function renut.player.state(n) end

---Whether he's swimming or treading water.
---@param n? integer
---@return boolean
function renut.player.swimming(n) end

---*Event.* He gets out of his vehicle, as when knocked out; `false` when he's in none.
---@param n? integer
---@return boolean
function renut.player.exit_vehicle(n) end

---@alias renut.PlayerEvent
---| "state" # Player 1's body state changed: `fn(new, old)`.
---| "jump"  # He jumped (`fn(state)`).
---| "land"  # He landed (`fn(state)`).

---Listens to player 1. It returns a handle: `h:off()` stops it.
---@param event renut.PlayerEvent
---@param fn function
---@return renut.Handle
---@overload fun(event: "state", fn: fun(new: string, old: string)): renut.Handle
---@overload fun(event: "jump", fn: fun(state: string)): renut.Handle
---@overload fun(event: "land", fn: fun(state: string)): renut.Handle
function renut.player.on(event, fn) end

---Makes Banjo with a player model (NutTool's `renut-player-model` id) in every world, from the next time he's made.
---No model (or `""`) gives him back his own, or the world's `player_model`.
---@param model? string
function renut.player.set_model(model) end

---Which way Banjo faces, in degrees; `nil` when not in a level.
---@param n? integer
---@return number?
function renut.player.yaw(n) end

---Whether he's in a vehicle.
---@return boolean
function renut.player.driving() end

---The vehicle he's in, or `nil`.
---@param n? integer
---@return renut.Vehicle?
function renut.player.vehicle(n) end

---His object's address (for `rex.mem` and the events' `driver`), or `nil` when not in a level.
---@param n? integer
---@return integer?
function renut.player.avatar(n) end

---*Event.* Moves him there on foot, as the game puts him at a challenge's start. Raises an error while he's driving.
---@param at renut.VecLike
function renut.player.teleport(at) end

---*Event.* Plays one of his animations by the game's name for it.
---@param animation string
function renut.player.play(animation) end

---------------------------------------------------------------------------------------------------
-- Shape keys
---------------------------------------------------------------------------------------------------

renut.shapes = {}

---The table `renut.shapes.define` takes.
---@class renut.ShapesDef
---@field model string The model's asset name without `aid_model_`.
---@field shapes string[] Its shape keys, in its table's order.
---@field weights? number[] Their starting weights.

---The model's shape keys and their starting weights. NutTool writes this call.
---@param def renut.ShapesDef
function renut.shapes.define(def) end

---Sets one shape key's weight. Raises `... has no shape '...'` when the model doesn't have it.
---@param model string
---@param name string
---@param weight number 0 not at all, 1 all the way; any number works.
function renut.shapes.set(model, name, weight) end

---Its weight now; `nil` when the model has no such shape.
---@param model string
---@param name string
---@return number?
function renut.shapes.get(model, name) end

---The model's shape names.
---@param model string
---@return string[]
function renut.shapes.list(model) end

---------------------------------------------------------------------------------------------------
-- World materials
---------------------------------------------------------------------------------------------------

renut.materials = {}

---The table `renut.materials.define` takes.
---@class renut.MaterialsDef
---@field model string The model's asset name without `aid_model_`.
---@field names string[] Its materials by name, in its table's order.

---The values `renut.materials.set` changes; the ones left out keep theirs.
---@class renut.MaterialValues
---@field emissive? number[]|{r: number, g: number, b: number}|renut.VecLike The colour it gives off, unlit (0 to 1 each, sRGB).
---@field emissive_strength? number The emissive times this; above 1 is brighter than white.
---@field glow? number How much it feeds the game's bloom, 0 to 1; below 0: full where it gives off light.
---@field opacity? number Its alpha times this, 0 to 1.

---The model's materials by name. NutTool writes this call.
---@param def renut.MaterialsDef
function renut.materials.define(def) end

---Sets some of the material's values over NutTool's. Raises `... has no material '...'`.
---@param model string
---@param name string
---@param values renut.MaterialValues
function renut.materials.set(model, name, values) end

---Back to how NutTool made it.
---@param model string
---@param name string
function renut.materials.reset(model, name) end

---Its material names.
---@param model string
---@return string[]
function renut.materials.list(model) end
