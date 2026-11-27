// Binary glTF (.glb) meshes as NutTool's OBJ, so a node of a .glb can become a model's meshes.
#pragma once

#include "blockmodel.hpp"
#include "common.hpp"
#include "image.hpp"

#include <array>
#include <optional>
#include <utility>

namespace nut::gltfobj {

// True for a binary glTF file (starts with "glTF").
bool isGlb(const Bytes& file);

// The names of the file's nodes that have a mesh.
std::vector<std::string> meshNodes(const Bytes& glb);

// A node's mesh (with its and its parents' transforms): a vertex per place and normal, normals
// smoothed where faces meet at less than `smoothDegrees`, and a triangle list.
struct MeshData {
    std::vector<std::array<float, 3>> positions, normals;
    std::vector<uint32_t> indices;
    // Shape keys (the mesh's morph targets): names, default weights, per vertex deltas (empty: none).
    std::vector<std::string> shapeNames;
    std::vector<float> shapeWeights;
    std::vector<std::vector<blockmodel::ShapeDelta>> shapes;
};
MeshData mesh(const Bytes& glb, const std::string& node, float smoothDegrees = 40);

// Every mesh node of the file in world space, as one mesh: a vertex per place, normal, colour (sRGB),
// texture coordinate and image, a triangle list, and the image each triangle shows.
struct SceneMesh {
    std::vector<std::array<float, 3>> positions, normals;
    std::vector<std::array<float, 4>> colours;
    std::vector<std::array<float, 2>> uvs;     // u, v with v from the image's top (glTF's way)
    std::vector<uint32_t> indices;
    std::vector<int> triangleImage;            // per triangle: an index into `images`, or -1 for none
    std::vector<image::Rgba> images;
    std::vector<std::string> imageNames;       // where each image came from
    std::vector<std::string> warnings;         // textures that could not be read (their material is untextured)
    // Water: triangles whose object, material or texture isWaterName (a node, mesh, OBJ object or group,
    // or .dae node or geometry), by that name. They stay in the mesh too, marked by triangleWater.
    struct Water {
        std::string name;
        std::vector<std::array<std::array<float, 3>, 3>> triangles;
    };
    std::vector<Water> water;
    std::vector<int> triangleWater;  // per triangle: its entry in `water`, or -1
    // Shape keys (glTF morph targets): their names across the file, default weights, and per vertex the
    // deltas of those that move it (empty when the file has none).
    std::vector<std::string> shapeNames;
    std::vector<float> shapeWeights;
    std::vector<std::vector<blockmodel::ShapeDelta>> vertexShapes;
    // Materials by name, as the file gives them (glTF's alpha mode, cutoff, emissive and double-sided;
    // a .dae's emission and transparency; an .mtl's Ke and d), and each triangle's (-1: none).
    struct Material {
        std::string name;
        blockmodel::WorldMaterial look;  // its emissiveTexture an index into `images`
        std::optional<bool> solid;       // in the collision; unset: unless it adds (light, glow)
    };
    std::vector<Material> materials;
    std::vector<int> triangleMaterial;
};

// The material `name` in `materials`, added the first time; its index.
int addMaterial(std::vector<SceneMesh::Material>& materials, const std::string& name, const blockmodel::WorldMaterial& look);

// Whether an object's name makes it water: it holds "_water", or one of `extra` (any case).
bool isWaterName(const std::string& name, const std::vector<std::string>& extra = {});
// A triangle of the water object `name`, which gets an entry in `water` the first time; its index.
int addWater(std::vector<SceneMesh::Water>& water, const std::string& name, const std::array<std::array<float, 3>, 3>& tri);
// The colour is the material's base colour times the vertex's COLOR_0, and the image its base
// colour texture (in the file's buffer, or a file `dir` holds), at its TEXCOORD set.
SceneMesh scene(const Bytes& glb, float smoothDegrees = 40, const fs::path& dir = {}, const std::vector<std::string>& water = {});

// A triangle corner's colour (sRGB) and texture coordinate.
struct Corner {
    std::array<float, 4> colour{1, 1, 1, 1};
    std::array<float, 2> uv{0, 0};
    std::vector<blockmodel::ShapeDelta> shapes;  // its shape keys' deltas (corners with some aren't merged)
};

// The same from triangles over welded places, each with its corners and image: a vertex per place,
// normal, colour, texture coordinate and image, normals smoothed where faces meet at less than `smoothDegrees`.
SceneMesh sceneFromTriangles(const std::vector<std::array<float, 3>>& places, const std::vector<std::array<int, 3>>& tris,
                             const std::vector<std::array<Corner, 3>>& corners, const std::vector<int>& triangleImage, float smoothDegrees);

// A Wavefront OBJ (and the MTL it names) as a SceneMesh: a material with a texture (map_Kd, read
// relative to the OBJ) shows it in white, one without is its Kd; `obj` is the file's path.
SceneMesh objScene(const fs::path& obj, float smoothDegrees = 40, const std::vector<std::string>& water = {});

// A COLLADA file (.dae) as a SceneMesh: each geometry its visual scene places (with the nodes'
// transforms; Z-up turned to Y-up), its material's diffuse texture or colour, times its vertex colours.
SceneMesh daeScene(const fs::path& dae, float smoothDegrees = 40, const std::vector<std::string>& water = {});

struct Options {
    std::string node;               // the node whose mesh is taken (with its and its parents' transforms)
    std::vector<int> into;          // the model's meshes that get it
    std::vector<int> hide;          // the model's meshes hidden
    std::string skinBone;           // the bone it is bound to (empty: weights from the nearest vertex)
    float smoothDegrees = 40;       // corners whose faces meet at less than this share a normal
    // One flat colour: every vertex samples one spot of the textures (u, v from the image's top
    // left), and with `flatUv2` one spot of the second texture coordinate set's (ambient occlusion).
    bool flat = false, flatUv2 = false;
    float flatUv[2] = {}, flatUv2At[2] = {};
};

// A skinned model's meshes for model import (a NutTool export brought back from Blender as .glb): each mesh node by
// name, its vertices at rest in world space, normals, first texture coordinates (v up, as FBX stores them), triangles,
// joint names and weights per vertex, and its first material's base colour (linear) when that has no texture, else
// that texture's image as the file holds it (PNG...).
struct SkinnedMesh {
    std::string name;
    std::vector<std::array<float, 3>> points, normals;
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::array<uint32_t, 3>> triangles;
    std::vector<std::vector<std::pair<std::string, float>>> skin;
    std::optional<std::array<float, 3>> plainColour;
    Bytes colourImage;
    std::string colourImageName;
};
std::vector<SkinnedMesh> skinnedMeshes(const Bytes& glb);

// A .glb's skeleton: its first skin's joints, parents (among them) before children, each depth's after the previous
// depth's, with their rest transforms in world space (column-major), the pose skinnedMeshes' vertices are in.
struct Joint {
    std::string name;
    int parent = -1;
    std::array<float, 16> bind{};
};
std::vector<Joint> skeleton(const Bytes& glb);

// The file's animations sampled `fps` times a second: each frame's world transform of every joint of skeleton().
struct JointAnimation {
    std::string name;
    float seconds = 0;
    std::vector<std::vector<std::array<float, 16>>> frames;  // [frame][joint]
};
std::vector<JointAnimation> jointAnimations(const Bytes& glb, float fps);

// OBJ text for model import: the node's triangles (normals smoothed by angle, texture coordinates
// projected along each face's main axis, or `flat`) in an object per `into` mesh, and `hide`'s
// meshes removed.
std::string toObj(const Bytes& glb, const Options& o);

} // namespace nut::gltfobj
