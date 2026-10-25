# Making a Hamster Ball in NB Studio

This guide builds a complete custom part for reNut in NB Studio: a **Hamster Ball**, a giant see-through cage ball that
rolls. Put one on each side of a seat and Banjo sits in the middle while the ball rolls around him.

It uses NB Studio's **Build > reNut Parts** menu. If your NB Studio doesn't have that menu, use renut-modtool; it takes
the same files, and [custom-parts.md](custom-parts.md) explains it. That page is also the full reference for every key
used here.

> **Status:** a single Hamster Ball sits on the ground and stands upright in game. Two of them side by side still
> disappear, and the ball's model is sometimes cut off at some camera angles; both are being worked on, so step 9's
> two-ball build doesn't work yet.

## How it works

The Hamster Ball is a **wheel**, a variant of the game's standard wheel:

- The game spins a wheel's rim and tyre as it drives. We swap them for a cage ball, so the ball turns with the wheel.
- A wheel touches the ground at the edge of its collision shape. We fit that shape to the ball, so the wheel touches
  the ground where the ball does.
- Because it's a wheel, the vehicle drives it with its engine, like any other wheel. It needs no script.

You'll make four files, all in your workspace's `renut` folder:

```
renut\
  hamster_ball.obj           <- the ball's model
  models.json                <- how to build it
  parts\
    hamster_ball.json        <- the part itself
```

Ready-made copies of all three are in [examples/hamster_ball](examples/hamster_ball). You can copy them in and skip to
step 5, but making them yourself shows how each piece works.

## 1. Open a workspace

Start NB Studio and open your workspace (**File > Open Workspace…**). If you don't have one, make one with
**File > New Workspace from Game Directory…** and pick your game folder (the one with `default.xex`).

## 2. Open the reNut folder

Choose **Build > reNut Parts > Open reNut Parts Folder**. NB Studio makes `renut\parts` in your workspace and opens
`renut` in Explorer. Keep it open; every file below goes in there.

## 3. Make the ball

In Blender:

1. Delete the default cube, then **Add > Mesh > UV Sphere**. In its options (bottom left), set **Segments 12**,
   **Ring Count 6**, **Radius 2.5** and **Location 0, 0, 0**. The ball's centre must be at the origin: that's where the
   wheel's hub is.
2. **Add Modifier > Generate > Wireframe**, set **Thickness** to **0.09**, then apply it (Ctrl+A over the modifier).
   The sphere becomes a cage of thin bars, so Banjo stays visible inside.
3. In the Material tab, add a material and name it **cage**.
4. **File > Export > Wavefront (.obj)**. Keep the default axes (Forward -Z, Up Y), tick **Normals**, **UV
   Coordinates** and **Materials**, and save it as `renut\hamster_ball.obj`.

The ball needs no textures of its own: it uses the wheel's metal, recoloured in the next step.

## 4. Write the models file

The ball is built on the standard wheel's model (the **template**). To see which materials that model has, choose
**Build > reNut Parts > List a Template Model's Materials…** and type `wheel_standard`. The ball uses the patterned
metal.

Make `renut\models.json`:

```json
{
  "bundle": "hamster_ball",
  "models": [
    {
      "id": "hamster_ball",
      "template": "wheel_standard",
      "obj": "hamster_ball.obj",
      "spins": true,
      "collisionFit": "model",
      "materials": { "cage": "aid_texture_banjox_shared_materials_metal_components_patterned_colour_0x0d4d03d5" },
      "tint": { "shared_materials_metal_components_patterned_colour_0x0d4d03d5": "0.45,0.75,1.0" }
    }
  ],
  "attach": [ { "template": "wheel_standard", "id": "hamster_ball" } ]
}
```

| Key | What it does here |
|---|---|
| `bundle` | The name of the files your models are built into. Any name of your own (`a-z`, `0-9`, `_`). |
| `id` | The model's name. The part file uses it in step 6. |
| `template` | Starts from the standard wheel's model. |
| `obj` | Your ball. |
| `spins` | Puts the ball on the wheel's turning part. Without it the ball would sit still while the wheel drives. |
| `collisionFit` | `"model"` fits the wheel's collision to the ball. A wheel rolls on its collision, so without this the ball sinks into the ground to the standard wheel's size. |
| `materials` | The OBJ's `cage` material uses the wheel's patterned metal. |
| `tint` | Recolours that metal light blue (red, green, blue from 0 to 1). |
| `attach` | Gives the part the standard wheel's footprint and attach points, so it fits onto a block like any wheel. |

## 5. Build the model

Choose **Build > reNut Parts > Build Models**. The log shows what was made:

```
aid_model_banjox_vehicleparts_hamster_ball (...): ... vertices, ... triangles; on the moving pieces
```

"on the moving pieces" means the ball will turn with the wheel. Whenever you change the OBJ or `models.json`, build
again (Create Distributable Patch reminds you).

## 6. Write the part

Make `renut\parts\hamster_ball.json`:

```json
{
  "id": "hamster_ball",
  "base": "wheels_standard",
  "name": "Hamster Ball",
  "count": 2,
  "model": "hamster_ball",
  "attach": "hamster_ball"
}
```

| Key | What it does here |
|---|---|
| `id` | The part's own id. Never change it once people use your mod: their vehicles store parts by id. |
| `base` | Starts as a copy of the standard wheel: its grip, weight and wheel behaviour. |
| `name` | What the garage shows. |
| `count` | The player gets two, one for each side. |
| `model`, `attach` | The model and footprint built in step 5. |

To change the ball's size, change its radius in Blender and build the models again; `collisionFit` follows the model.

## 7. Check it

Choose **Build > reNut Parts > Check Part Files**. It should say "1 part file(s), no problems". If not, it names the
file and what's wrong, such as a typo, a missing comma, or a `model` that doesn't match the `id` in `models.json`.

## 8. Make the patch

Choose **Build > Create Distributable Patch (.nbpatch)…**, enter a name (for example "Hamster Ball"), your name and a
version, and save. NB Studio checks the part again and packs the part and its model into the patch.

## 9. Roll

1. Copy the `.nbpatch` into reNut's `mods` folder (next to `renut.exe`) and start the game.
2. In the garage, find the Hamster Ball with the wheels. You own two.
3. Build the vehicle:
   - a **seat**;
   - one **Hamster Ball** on its left side and one on its right;
   - an **engine** and **fuel** underneath the seat.

   The two balls overlap into one big ball around the seat. Keeping the engine and fuel low hangs the weight under the
   balls' axle, which should keep Banjo upright as it rolls.
4. Drive it.

## If something's wrong

| What you see | Why, and what to do |
|---|---|
| reNut stops after the Microsoft logo | The part file isn't in `renut\parts` (a patch with only the model does this). Move it there and make the patch again. Check Part Files points this out. |
| The garage shows the standard wheel's shape | The model wasn't found. reNut's log says `... is in no loaded bundle`. Build the models (step 5), then make the patch again. |
| The ball doesn't turn | `"spins": true` is missing from `models.json`. |
| The ball sinks into the ground | `"collisionFit": "model"` is missing from `models.json`, so the wheel still rolls on the standard wheel's small collision. |
| Banjo tips over or spins round with the ball | Move the heavy parts (engine, fuel) lower, under the seat. |
| The part isn't in the garage at all | Check the newest reNut log (`logs\renut_<number>.log`) for lines with `skipped` or `ERROR`; [custom-parts.md](custom-parts.md#testing-and-troubleshooting) explains them. |

## Command line

The same steps without the window (NB.Cli, next to NB Studio):

| Step | Command |
|---|---|
| List a template's materials | `NB.Cli renut-materials <workspace> wheel_standard` |
| Build the model | `NB.Cli renut-models <workspace>` |
| Check the part files | `NB.Cli renut-check <workspace>` |
| Make the patch | `NB.Cli patch-build <workspace> hamster-ball.nbpatch --name "Hamster Ball" --author "You"` |
