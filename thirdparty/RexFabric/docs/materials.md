# Graph materials

A graph material is a material (content_and_classes.md, Materials) made of a node graph, as Unreal's materials are:
nodes worked out from textures, coordinates, numbers and the game's own scene values, ending in one Material node whose
inputs say what the surface is (its colour, its normal, how it shines). How that surface is lit is the material's
shading model, one of those the game offers; the game compiles the code the graph makes into its own shader and draws
whatever wears the material with it.

RexFabric keeps the graph, offers the nodes, compiles the graph to HLSL and gives the editor. The game declares what a
graph material of its can be (the Material node's inputs, its shading models, its own nodes) and draws it.

| Part | Where |
|---|---|
| The graph, its nodes and the compiler | `src/content/material_graph.h`, `material_graph.cpp`, `material_nodes.cpp` |
| What the game declares | `MaterialTypeDecl` in `include/rexfabric/content.h`: `graph_inputs`, `shading_models`, `graph_nodes`, `game_textures`, `check_program` |
| What the game reads | `FindMaterial`'s `MaterialView::program` (a `MaterialProgram`) |
| The editor | `src/ui/material_graph_editor.h`, `.cpp` |

## A graph

A material of a graph material type keeps its graph in its file beside its values:

```
mods/ModelProbe/content/GraphBarrel.json
{"asset": "material", "type": "TiP/Material", "help": "The barrel in gold",
 "values": {"blend": "Opaque"},
 "graph": {"shading_model": "Metallic",
           "nodes": [{"id": 1, "type": "material.result", "x": 600, "y": 0},
                     {"id": 3, "type": "parameter.vector", "x": 0, "y": 200, "settings": {"Name": "Tint", "Value": [1.0, 0.78, 0.3, 1.0]}}],
           "links": [{"from": [3, "RGB"], "to": [1, "Base Colour"]}]}}
```

| Part | What it is |
|---|---|
| `shading_model` | One of the type's shading models, by name |
| A node | Its id, its type (a built-in node, the game's, or `material.result`), its place on the canvas, its settings (a texture, a constant's value) and the values typed into its unlinked inputs, four floats each |
| A link | From an output, by node and pin name, into an input; an input takes one |

Every value is one to four floats. A pin of any width takes whatever comes in; a node's inputs of any width all become
as wide as the widest of them, a single float spreading over all four (Unreal's rule: a float3 and a float2 can't be
added, a float3 and a float can). A pin of a fixed width cuts what's wider and pads what's narrower with zeros.

Pins are named, so links survive a node type gaining pins, as in the node editor.

## Shading models

The game's to declare (`ShadingModelDecl`): a name, help, and which of the Material node's inputs it reads. The editor
greys out the inputs the material's shading model doesn't read, and the compiler leaves them at their initial values.
Trouble in Paradise's are in DeTiP's docs/modding.md, Graph materials: Unlit, Basic Lit, Metallic, Pinata and Fur.

The type's properties are the Material node's details: its blend mode, two sided, what a shading model needs besides
the graph. They're values as any material's are.

## Nodes

The built-in nodes (`material_nodes.cpp`), by the add menu's categories:

| Category | Nodes |
|---|---|
| Constants | Constant, Constant 2, 3 and 4 Vector, Colour, Pi, Time (seconds times a speed, and its sine, cosine and fraction) |
| Parameters | Scalar Parameter, Vector Parameter: a named value what's drawn with the material may change without the material changing (`MaterialProgram::parameters`) |
| Coordinates | Texture Coordinate (a UV set, tiled), Panner, Rotator, World Position, Vertex Normal, Camera Vector, Reflection Vector |
| Texture | Texture Sample (a mod's Texture asset or one of the game's textures at a UV; as a normal map its two channels unpacked), Cube Sample |
| Math | Add, Subtract, Multiply, Divide, Min, Max, Power, Lerp, Clamp, SmoothStep, Step, Remap Value Range, One Minus, Negate, Reciprocal, Square, Fmod, If, and the functions of one value: Saturate, Abs, Frac, Floor, Ceil, Round, Truncate, Sign, Square Root, Sine, Cosine, Tangent and their arcs, Arctangent2, Exp, Exp2, Log, Log2, Log10 |
| Vector | Normalize, Dot, Cross, Length, Distance, Reflect, Component Mask, Append, Break Out Components, Make Float2, 3 and 4 |
| Colour | Desaturation, Hue Shift, Fresnel, Noise |

The game adds its own (`MaterialNodeDecl`): nodes with outputs only, each an expression of the context its shader gives
the code (`c.vertex_colour`). Trouble in Paradise's are under Scene.

A node type is a `NodeType`: its pins, its settings, how its outputs' widths follow its inputs', and `emit`, which writes
each output's code from its inputs' code. A node reading a value more than once puts it in a local first
(`Compiler::Local`); one needing a function adds it once (`Compiler::Helper`). To add a node, add a `NodeType` to
`material_nodes.cpp`.

## The code

`Compile` works the graph out backwards from the Material node, writing each node it reaches once, its outputs in
locals, and gives a `MaterialProgram`:

```
// ModelProbe/GraphBarrel, Metallic
struct MaterialResult {
  float3 base_colour;
  float3 normal;
  ...
};

MaterialResult EvaluateMaterial(MaterialContext c) {
  float4 l0 = MaterialParameter(c, 0);
  float3 l1 = l0.rgb;
  MaterialResult r;
  r.base_colour = l1;
  r.normal = float3(0.0, 0.0, 1.0);
  ...
  return r;
}
```

`MaterialResult` has a field for each of the type's graph inputs, named after it in lower case with underscores
(`Base Colour` is `base_colour`). What the code reads, every game's shader gives it:

| What | Is |
|---|---|
| `MaterialContext c` | The game's struct, with at least `time` (seconds), `world_position`, `vertex_normal` (world space, one unit long) and `view` (from the point toward the camera, one unit long), and whatever its own nodes read |
| `MaterialUv(c, set)` | UV set 0 to 3 |
| `MaterialSample2D(c, n, uv)`, `MaterialSampleCube(c, n, direction)` | Texture n of `MaterialProgram::textures` |
| `MaterialParameter(c, n)` | Parameter n of `MaterialProgram::parameters`, four floats |

A graph with problems (a texture node without a texture, inputs of widths that don't mix, a loop, a parameter name
given to two widths) has its problems in the program, by node too, and no code. `MaterialProgram::hash` is the code's,
for a game keeping what it compiled. `FindMaterial` compiles a material's graph once for each change to it.

`check_program`, when the game gives one, says what the game's own compiler makes of the code: the editor shows it with
the material's problems. Trouble in Paradise compiles the shader in the background and gives its errors.

## The editor

A material of a graph material type opens in the material graph editor:

* Left: the game's preview of the material (the Avatar editor's viewport, on the type's preview), and below it the
  details: of the material (its type, help, shading model, what the preview shows it on, the type's properties, each
  unlinked input of the Material node, the parameters it has), or of the node selected (its help, its problems, its
  settings, its unlinked inputs).
* Middle: the canvas, in the node editor's look, links colored by their width; Show Code shows the HLSL the graph makes.

On the canvas a right click adds a node, a link dragged into empty space adds one connected to it, unlinked single
floats are typed into on the node, and colours show as swatches. Delete removes the selection (the Material node
stays); Ctrl+C, Ctrl+X, Ctrl+V and Ctrl+D copy, cut, paste and duplicate. Ctrl+S saves, Ctrl+Z and Ctrl+Y undo and
redo; a node dragged is one step. Whatever wears the material shows every change at once, saved or not.

A material of a type without a graph opens in the material editor as before (content_and_classes.md, Materials); each
editor offers only types of its own kind.
