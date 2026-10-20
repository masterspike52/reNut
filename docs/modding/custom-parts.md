# Making custom vehicle parts

reNut loads new vehicle parts from `.nbpatch` mods. A part mod is just data: part files, optional Lua scripts and optional
models. Nobody has to change reNut to use one. Players drop the `.nbpatch` into reNut's `mods` folder, and the parts are in
their garage, already owned.

You can make three kinds of part:

| Kind | What it is | Needs |
|---|---|---|
| **Variant** | A shipped part with your name and changed values (a stronger engine, a bigger fuel tank). | A part file. |
| **New kind of part** | A part with its own behaviour, written in Lua (the Teleporter example below). | A part file and a script. |
| **Custom model** | Any of the above with its own model, collision and attach points (wheels too). | An OBJ and a `models.json`. |

Contents:

- [What you need](#what-you-need)
- [How a part mod is put together](#how-a-part-mod-is-put-together)
- [Your first part: a variant](#your-first-part-a-variant)
- [Part file reference](#part-file-reference)
- [New kinds of parts: scripts](#new-kinds-of-parts-scripts)
- [Script reference](#script-reference)
- [Custom models](#custom-models)
- [Custom wheels](#custom-wheels)
- [Packaging and installing](#packaging-and-installing)
- [Testing and troubleshooting](#testing-and-troubleshooting)
- [Players, saves and removed mods](#players-saves-and-removed-mods)
- [Limits](#limits)

---

## What you need

- **reNut**, and your game.
- **NB Studio**, with a workspace made from your game (**File > New Workspace from Game Directory…**). Its
  **Build > reNut Parts** menu checks your part files, builds your models and lists materials, and
  **Build > Create Distributable Patch** packs everything into the `.nbpatch`.
- A text editor for the JSON and Lua files.
- For custom models: a 3D program that exports **OBJ** (Blender works).

Prefer the command line, or on Linux? [renut-modtool](#without-nb-studio-renut-modtool) builds the same files into the same
`.nbpatch`, without NB Studio.

[nb-studio-parts.md](nb-studio-parts.md) walks through a whole part in NB Studio step by step (a Hamster Ball wheel).

## How a part mod is put together

Everything for reNut parts lives in your workspace's `renut` folder. **Build > reNut Parts > Open reNut Parts Folder**
creates and opens it:

```
<your workspace>\renut\
  parts\
    turbo_engine.json        <- one part per .json file
    teleporter.json
    teleporter.lua           <- scripts, next to the parts that use them
  models.json                <- optional: your models
  teleporter.obj             <- the files models.json names
```

Part files go in `parts\`. A part file left in `renut\` itself isn't packed, and Check Part Files points it out.

**Create Distributable Patch** packs everything in `parts\` into the `.nbpatch` as `parts/<file>`, together with the
models built from `models.json`. reNut reads them when the game starts.

File paths inside part files (`"script"`, `"objparams"`) start with `parts/`.

## Your first part: a variant

1. **Build > reNut Parts > Open reNut Parts Folder**.
2. In `parts\`, make `turbo_engine.json`:

   ```json
   {
     "id": "mymod_turbo_engine",
     "base": "propulsion_engines_largepower",
     "name": "Turbo Engine",
     "count": 4,
     "fields": {
       "0x3FC": { "f32": 140 },
       "0x3D0": { "f32": 600 }
     }
   }
   ```

3. **Build > reNut Parts > Check Part Files**. It should report one part file and no problems.
4. **Build > Create Distributable Patch (.nbpatch)…**, then copy the patch to reNut's `mods` folder and start the game.

In the garage, Turbo Engine sits next to the large engine sizes, and the player has 4 of them.

The part starts as a copy of `base` (the shipped large power engine). It takes your name, and the values in `fields` are
written over the copy. It uses the base's model and icon unless you give it your own model.

## Part file reference

| Key | Required | Meaning |
|---|---|---|
| `id` | yes | Your part's id: 1–40 characters of `a-z`, `0-9`, `_`. It must be unique across **all** mods; prefix it with your mod's name (`mymod_turbo`). Never change it after release: players' vehicles store parts by id. |
| `base` | yes* | The shipped part to start from, e.g. `gadgets_horn`, `propulsion_engines_largepower`, `wheels_standard`. The `aid_objparams_banjox_vehicleblock_` prefix is optional. |
| `name` | no | The name shown in the garage. |
| `count` | no | How many the player owns, 1–99 (default 1). There is no store: the parts are simply owned. |
| `fields` | no | Values written over the record (see below). |
| `script` | no | A Lua script (`"parts/x.lua"`). This makes the part a new kind of part. |
| `params` | no | Settings for the script (any JSON object). The script reads them as `params`. |
| `model` | no | The part's model, see [Custom models](#custom-models). This can also be a shipped part's model, e.g. `"gadgets_spring"`. |
| `attach` | no | The part's footprint and attach points (a shipped part's name, or one from your `models.json`). |
| `objparams` | no* | A whole record file (`"parts/x.bin"`) to use instead of `base`. Advanced: a part made this way has no base to fall back to if the mod is removed. |

\* A part needs `base` or `objparams`.

**Finding base names.** Every part record in the game is called `aid_objparams_banjox_vehicleblock_<name>`, and
`<name>` is what `base` takes. NB Studio's asset list shows them all. If a name is wrong, Check Part Files tells you.

**The garage.** A variant gets an entry of its own next to its base: a size of its own in its base's type, or, for
parts the garage lists without sizes (gadgets, wheels), a type of its own in the same category.

**Fields.** Each entry is `"<offset>": { "<type>": <value> }`. The offset is a byte offset into the part's record, and
values are written big-endian, as the game stores them. The types are:

- `f32`: a number with decimals
- `u32`, `i32`, `u16`, `u8`: whole numbers
- `str`: text, written with its terminating zero

What an offset means depends on the part's class. To find the value you want, compare the records of shipped parts,
for example in NB Studio. Offsets that are the same for every part:

| Offset | What | Notes |
|---|---|---|
| `0x130` | Paint colour key (text, up to 31 characters), e.g. `colour_orange` | `str` |
| `0x3B8` | Garage icon: a texture's asset id | `u32`. The id of another part's icon gives your part that icon. |

Wheels (`base` is a `wheels_` part):

| Offset | What | Notes |
|---|---|---|
| `0x3C8` | The wheel's radius | `f32`. 0.5 on the standard wheel, 1.5 on the monster wheel. On a vehicle with two or more wheels it's the radius the wheel rolls on, and it sets how fast the wheel model spins. Match it to your model. |
| `0x3CC` | Probably the wheel's width | `f32`. 0.5 on most wheels, 1.0 on the monster wheel; not confirmed. |

The name, model, footprint and garage grouping offsets are managed by reNut through `name`, `model`, `attach` and
`script`. Don't set them with `fields`.

## New kinds of parts: scripts

A part with a `script` becomes a part type of its own in the garage, listed next to its base's type. It keeps
everything its base does (physics, weight, attaching, being hit), and your script adds the behaviour. The base's own
button action is left out: a scripted horn doesn't honk.

Pick as `base` a shipped part whose shape and garage category fit. `gadgets_horn` makes a good "button gadget".

`renut\parts\teleporter.json`:

```json
{
  "id": "mymod_teleporter",
  "base": "gadgets_horn",
  "name": "Teleporter",
  "count": 2,
  "script": "parts/teleporter.lua",
  "params": { "distance": 15, "lift": 1.5, "hop": 2, "duration": 0.25, "cooldown": 3, "effect": "0x4582A557" }
}
```

`renut\parts\teleporter.lua`:

```lua
-- Teleporter: the button dashes the vehicle forward in a short glide, then it recharges.

local function ease(t)            -- 0..1 -> 0..1, slow at both ends
  return t * t * (3 - 2 * t)
end

function on_spawn(part)
  part.wait = 0                   -- your own fields live on the part table
end

function on_button_down(part)
  local v = part:vehicle()
  if part.wait > 0 or part.dash or not v then
    return
  end
  if params.effect then
    fx.play(params.effect, v:position())
  end
  local f = v:forward()
  local flat = vec(f.x, 0, f.z):normalized()          -- dash level, whatever the tilt
  part.dash = {
    from = v:position(),
    offset = flat * params.distance + vec(0, params.lift, 0),
    velocity = v:velocity(),
    time = 0,
  }
  part.wait = params.cooldown
end

function on_tick(part, dt)
  part.wait = math.max(0, part.wait - dt)
  local v, dash = part:vehicle(), part.dash
  if not v or not dash then
    part.dash = nil
    return
  end
  dash.time = math.min(params.duration, dash.time + dt)
  local t = dash.time / params.duration
  v:set_position(dash.from + dash.offset * ease(t) + vec(0, math.sin(math.pi * t) * params.hop, 0))
  v:set_velocity(dash.velocity)                       -- no fall builds up during the glide
  if t >= 1 then
    part.dash = nil
    if params.effect then
      fx.play(params.effect, v:position())
    end
  end
end
```

## Script reference

### Events

Define any of these as global functions. Each gets the part table first.

| Event | When |
|---|---|
| `on_spawn(part)` | The part was made (in the garage or with a vehicle). |
| `on_attach(part)` | It was built onto a vehicle. |
| `on_tick(part, dt)` | Every frame while it is on a vehicle. `dt` is the frame time in seconds. |
| `on_button_down(part)` | Its action button was pressed. |
| `on_button_up(part)` | ...and let go. |
| `on_destroy(part)` | The part is gone. |

### The part

- Each part on each vehicle has its own table. Store your state in it (`part.wait = 3`) and it stays until the part is
  destroyed.
- `part.params` and the global `params` are the part file's `params`.
- `part_id` is the part's `id`.
- `part:vehicle()` returns the vehicle the part is on, or `nil`.

### The vehicle

| Method | Does |
|---|---|
| `v:position()` | The vehicle's position (a `vec`). |
| `v:set_position(p)` | Moves the vehicle to `p` (a teleport). |
| `v:move(d)` | Moves the vehicle by `d`. |
| `v:velocity()` / `v:set_velocity(vel)` | Linear velocity, in units per second. |
| `v:angular_velocity()` / `v:set_angular_velocity(w)` | Spin. |
| `v:forward()`, `v:up()`, `v:right()` | The vehicle's axes (unit `vec`s). |

These raise an error when the vehicle has no body for a moment, while it rebuilds after a part breaks off. The event
simply ends then. Wrap the call in `pcall` if you need to know.

### Vectors

`vec(x, y, z)` makes a vector.

- Its parts are `v.x`, `v.y`, `v.z`.
- Operators: `a + b`, `a - b`, `-a`, `a * 2`, `2 * a`, `a / 2`.
- Methods: `v:length()`, `v:normalized()`, `a:dot(b)`, `a:cross(b)`.
- `tostring(v)` prints it.

### Effects

`fx.play(effect, position)` plays a game particle effect, by ID as text (`"0x4582A557"`). It returns `true` if it played.
An effect the current level hasn't loaded is skipped, with a warning in the log. The list of every effect, and which ones
work everywhere, is in [effects.md](effects.md).

### Log

`log(...)` and `print(...)` write to reNut's log, which is handy while testing.

### Sandbox

Scripts get Lua 5.4's base functions without file or code loading, plus `math`, `string`, `table` and `utf8`. There is
no `io`, `os`, `require` or `load`.

- **Run time:** one event may run up to 5 million Lua instructions; a longer one is stopped with an error.
- **Errors:** they go to the log with your part's name and line number. After 20 errors the script is switched off for
  the session, and the part stays as a plain copy of its base.

## Custom models

A custom model starts as a copy of a shipped part model (the **template**). You give it your own geometry from an OBJ,
and it uses the template's materials: metal, rivets, paint and so on. NB Studio builds the models into bundle files that
belong to your mod. Your mod never changes the game's own bundles, so it never clashes with other mods.

### 1. Model it

- Export an OBJ into `renut\`, with **Y up** and the part's **front along +Z**. If the model shows up turned in the
  garage, fix it with `preRotate` (step 3).
- One grid cell of the garage is **1 unit**, and the part's cell is centred on the origin. A 1×1×1 part fits in
  -0.5..0.5 on every axis.
- Give each surface an OBJ material (`usemtl body`, `usemtl trim`...). You map these to the template's materials in
  step 3.

### 2. Pick the template's materials

**Build > reNut Parts > List a Template Model's Materials…** and type the template, e.g. `gadgets_horn`. The list (also
copied to the clipboard) holds the colour textures your OBJ materials can use. A wrong name in `materials` also prints
the list when you build.

Template names are part model names. Most match the part (`gadgets_horn`), but the wheels' are singular: `wheel_standard`,
`wheel_monster`, `wheel_highgrip`, `wheel_super`.

### 3. Write the models file

Make `renut\models.json`:

```json
{
  "bundle": "mymod_parts",
  "models": [
    {
      "id": "mymod_teleporter",
      "template": "gadgets_horn",
      "obj": "teleporter.obj",
      "materials": { "body": "aid_texture_banjox_shared_materials_metal_components_plain_colour_0x0e98eea5" },
      "tint": { "shared_materials_metal_components_plain_colour_0x0e98eea5": "0.6,0.2,1.0" },
      "collisionFit": "model"
    }
  ],
  "attach": [
    { "template": "gadgets_horn", "id": "mymod_teleporter", "attachable": "bottom" }
  ]
}
```

| Key | Meaning |
|---|---|
| `bundle` | The name of your mod's bundle files (`a-z`, `0-9`, `_`), unique to your mod. If you leave it out, NB Studio uses `renut_` and your workspace folder's name. |
| `models[].id` | The model's name; part files use it as `"model"`. |
| `models[].template` | The shipped part model to start from, e.g. `gadgets_horn`. |
| `models[].obj` | Your OBJ (path relative to `renut\`; `.fbx` works too). Leave it out to keep the template's shape and only recolour it. |
| `models[].materials` | Each OBJ material mapped to one of the template's colour textures. |
| `models[].tint` | Recolours the surfaces using a texture: `"r,g,b"` (0–1 each). |
| `models[].preRotate` | Turns the OBJ before import: `"x:90"`, or several such as `"x:90;y:180"`. |
| `models[].collisionFit` | Collision: `"model"` fits it to your OBJ's size, `"footprint"` to the footprint with the same id. Left out, it keeps the template's. |
| `models[].collisionBox` | The collision's exact box, overriding `collisionFit`: `{ "min": [x, y, z], "max": [x, y, z] }`. |
| `models[].spins` | `true` puts your model on the template's turning part, so it turns with it, and draws nothing else of the template. For wheels: see [Custom wheels](#custom-wheels). |
| `models[].keepAnimated` | `true` keeps the template's moving pieces (spinning parts and the like); the default hides them. |
| `models[].retarget`, `models[].textures` | New textures, see below. |
| `attach[].template` | The shipped part whose footprint (cells) and attach points to copy. |
| `attach[].id` | Its name; part files use it as `"attach"`. |
| `attach[].attachable` | Which faces other parts attach to: `all`, `none`, `bottom`, `top`, `sides`, `bottom+sides`, or face centres `"x,y,z;x,y,z"`. Left out, it keeps the template's. |

**New textures.** Add the PNG under `textures` and point the template's texture at it with `retarget`:

```json
"materials": { "body": "aid_texture_banjox_shared_materials_metal_components_plain_colour_0x0e98eea5" },
"retarget":  { "shared_materials_metal_components_plain_colour_0x0e98eea5": "aid_texture_banjox_mymod_stripes" },
"textures":  { "aid_texture_banjox_mymod_stripes": "stripes.png" }
```

Images can be PNG or TGA, with sides that are powers of two from 4 to 4096 (64×64, 256×128...). New textures go into a
second bundle of your mod. They are built by the tools but haven't been tested in game yet; plain template materials
with `tint` are tested.

### 4. Build it

**Build > reNut Parts > Build Models**. The log lists each asset it made. They go into your workspace as two new game
files, which Create Distributable Patch packs:

- `Bundle/50/<id>` holds the models, collision and footprints.
- `Bundle/4f/<id>` holds new textures. It's only made if you have any.

Build again whenever you change the OBJ or `models.json`. Create Distributable Patch offers to build them first if
they're out of date.

### 5. Use it

In the part file:

```json
"model": "mymod_teleporter",
"attach": "mymod_teleporter"
```

The collision always comes with the model. If a model isn't found when the game runs (for example, its bundle is missing
from the patch), the part keeps its base's model, and the log says why.

## Custom wheels

A wheel of your own shape is a variant of a shipped wheel with its own model. It drives like any wheel, so it needs no
script. What the game does with wheels sets a few rules:

1. **Base and template.** Use a wheel as `base` (e.g. `wheels_standard`) and its model as `template` (`wheel_standard`).
   Copy its footprint with an `attach` entry (`"template": "wheel_standard"`).
2. **`"spins": true`.** The game turns only a wheel's tyre and rim, so your geometry has to go there to roll with the
   wheel.
3. **Materials on the turning part.** With `spins`, only the template's turning-part textures can hold your geometry;
   the tools refuse the others and list the right ones. The materials menu marks them
   ("turning part: usable with spins"). Pick the **tyre** (`shared_materials_object_tyrestandard_colour_0x040d84b5` on
   the standard wheel): it's drawn once per level of detail. The rim metals are also drawn on the wheel nuts and spokes,
   which draws extra copies of your model.
4. **Size.** The wheel model's axle runs along **Y**, centred on the origin (the hub). Set the size twice:
   - `"collisionFit": "model"` fits the collision to your model. A vehicle with a single wheel rests on that.
   - the part's `fields` set `0x3C8` (the radius) to your model's radius. With two or more wheels, that's what the wheel
     rolls on, and how fast it spins.
5. **Mounting.** Wheels can't attach to seats. Put a block between the seat and the wheel.

```json
{
  "bundle": "mymod_wheels",
  "models": [ {
    "id": "mymod_bigwheel", "template": "wheel_standard", "obj": "bigwheel.obj", "spins": true,
    "materials": { "tyre": "aid_texture_banjox_shared_materials_object_tyrestandard_colour_0x040d84b5" },
    "collisionFit": "model"
  } ],
  "attach": [ { "template": "wheel_standard", "id": "mymod_bigwheel" } ]
}
```

```json
{
  "id": "mymod_bigwheel", "base": "wheels_standard", "name": "Big Wheel", "count": 4,
  "model": "mymod_bigwheel", "attach": "mymod_bigwheel",
  "fields": { "0x3C8": { "f32": 2.5 } }
}
```

## Packaging and installing

1. **Build > reNut Parts > Check Part Files** (optional: the next step checks too).
2. **Build > Create Distributable Patch (.nbpatch)…**: enter the mod's name, author and version, and save. NB Studio
   offers to build models that are out of date, checks the part files (and stops if one has a problem), then packs the
   parts and models.
3. Copy the `.nbpatch` into reNut's `mods` folder, next to `renut.exe`. It can be changed with the `renut_mods_folder`
   setting. Mods load in file-name order when the game starts.
4. Share the `.nbpatch`. Players only need reNut. The patch works with any copy of the game, since it adds files and
   changes none.

The workspace's own changes go into the same patch. Executable mods ("After-Party" mods) and world edits don't apply in
reNut (the log says so), so for a parts-only mod, use a workspace without them.

To check a patch, open it as a zip. It should hold `patch.json`, your `parts/...` files and, with models, `d/0000.bin`
entries listed in `patch.json` as `Bundle/50/...` (and `Bundle/4f/...`) with kind `new`.

### Command line (NB.Cli)

| Step | Command |
|---|---|
| Check the part files | `NB.Cli renut-check <workspace>` |
| List a template's materials | `NB.Cli renut-materials <workspace> <template> [--turning]` |
| Build the models | `NB.Cli renut-models <workspace>` |
| Make the patch | `NB.Cli patch-build <workspace> mymod.nbpatch --name "My Mod" --author "You"` |

### Without NB Studio: renut-modtool

renut-modtool, in reNut's source at `tools/renut_modtool`, builds the same files into the same `.nbpatch` without a
workspace. It runs on Windows and Linux and needs the [.NET 9 SDK](https://dotnet.microsoft.com/download):

```
dotnet publish tools/renut_modtool -c Release -o renut-modtool
```

Its mod folder is laid out like the workspace's `renut` folder, plus a `mod.json` (the patch's name and id):

```
mymod/
  mod.json         { "id": "mymod", "name": "My Mod", "version": "1", "author": "You", "description": "..." }
  parts/
  models.json
```

| Step | Command |
|---|---|
| Check, build the models and make the patch | `renut-modtool build mymod <game folder> [-o mymod.nbpatch]` |
| List a template's materials | `renut-modtool materials <game folder> <template> [--turning]` |

Without `bundle` in `models.json`, renut-modtool uses the mod's id. Keep the `id` the same between versions, so a new
version replaces the old one.

## Testing and troubleshooting

reNut's log (`logs/renut_<n>.log` next to `renut.exe`) reports each step. A working part mod logs:

```
mods: loaded 'My Mod' 1 (My Mod.nbpatch) by You: 1 file(s), 2 part(s)
mods: My Mod: part 'mymod_teleporter' (Teleporter), from banjox_vehicleblock_gadgets_horn, scripted by parts/teleporter.lua, model banjox_vehicleparts_mymod_teleporter
mods: My Mod: bundle 50/xxxxxx loaded
Custom part 'mymod_teleporter' (My Mod) is in the game: asset ..., from banjox_vehicleblock_gadgets_horn
Custom parts: 2 part(s) added to the garage inventory
```

| Problem | Look for |
|---|---|
| reNut stops after the Microsoft logo | The patch has models but no part files: they weren't in `parts\`. Check Part Files and Create Distributable Patch refuse this; move them and make the patch again. |
| The mod doesn't load | `mods: ... skipped: ...` says why: a bad part file (with the file and the problem), a duplicate mod id, or another mod changing the same game file. |
| A part is missing | `part '...' skipped: an earlier mod has a part with that id` (rename yours), or `its base part ... does not exist` (check `base`). |
| The part isn't in the garage | `the garage has no category ... / type ... for it`: the base's grouping wasn't found. Try another base. |
| Wrong or no model | `... is in no loaded bundle; it keeps its base part's`: the bundle isn't in the patch, or `model`/`attach` doesn't match the ids in `models.json`. |
| A wheel's model doesn't turn | `"spins": true` is missing. |
| Two of your wheels disappear (one alone shows) | Its geometry isn't on the wheel's turning part. Build with the current tools: they only put `spins` geometry there, and name the materials that work. |
| A wheel sinks into the ground | One wheel: `"collisionFit": "model"` is missing. Two or more: `0x3C8` in the part's `fields` doesn't match the model's radius. |
| A wheel doesn't stay on the vehicle | Wheels don't attach to seats; put a block in between. |
| The script does nothing | `Part script ...` errors with line numbers. Is the event name spelled right (`on_button_down`)? |
| An effect doesn't show | `effect ... isn't loaded here`: use one marked *anywhere (common)* in [effects.md](effects.md). |

To find an effect, turn on `renut_log_effects` in reNut's settings (F4, Debug), then do what shows the effect in the game.

## Players, saves and removed mods

- Vehicles store parts by id. If a player removes your mod, their vehicles still load: each of your parts becomes its
  base part. Parts made from a whole `objparams` record have no base, so they're left out.
- reNut remembers every custom part it has seen in `mods/.renut_custom_parts.json`. That file is what makes the fallback
  work, so players shouldn't delete it.
- Keep part ids the same between versions of your mod. A new id is a new part, and vehicles with the old one fall back to
  its base.

## Limits

- At most 64 scripted part kinds can be loaded at once, across all mods.
- Part ids must be unique across all loaded mods; the later mod's part is skipped.
- Two mods can't change the same game file; the later one is skipped. Part mods made as above add only their own files,
  so they don't clash with each other.
- reNut applies game files and parts. NB Studio's executable mods (default.xex patches) and world edits in a patch are
  not applied (the log says so).
