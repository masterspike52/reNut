// Autodesk FBX export (binary FBX 7.4) of a model: meshes skinned to its skeleton with a bind pose,
// materials with their colour maps (PNG, embedded), and animations as takes (animation stacks).
//
// Bones become LimbNode models under a Null named after the model, with their bind-pose local
// transforms (in the game's bone frames, or reoriented along the bones); each mesh is a Mesh model under the same Null with a Skin deformer whose clusters
// carry the bind matrices. Animations are sampled at `fps` the way the viewer plays them: a matched
// bone takes the animation's rotation and scale and adds its translation to the bind offset.
// Rotations are written as XYZ Euler angles in degrees, kept continuous from frame to frame.
#pragma once

#include "image.hpp"
#include "model.hpp"

#include <array>
#include <functional>
#include <optional>

namespace nut::fbx {

struct NamedAnimation {
    std::string name; // take name
    const model::Animation* anim = nullptr;
};

struct Options {
    std::vector<bool> include;               // meshes to write (default: the shown groups)
    std::vector<NamedAnimation> animations;
    // PNG data of a texture (by Model::textures name) to embed; without it the file only refers
    // to "<name>.png" next to it.
    std::function<std::optional<Bytes>(const std::string&)> texture;
    // Bake each material's colour gain and offset into its texture (texture * c:textureGain + c:textureOffset),
    // as the game's shaders draw it; one tinted copy per texture and tint.
    bool tint = true;
    // Turn each bone so `boneAxis` (0-2 +X/+Y/+Z, 3-5 -X/-Y/-Z) runs along it, towards its main child; the skin,
    // bind pose and animations are written to match, so the mesh and motion look the same. +Y is Blender's bone axis.
    bool reorient = false;
    int boneAxis = 1;
    // A "<bone>_end" child for each bone without children, boneEndSize (a fraction of the skeleton's size) down its
    // bone axis: Blender sizes bones by their children, so these draw as short stubs (Ignore Leaf Bones drops them).
    bool boneEnds = false;
    float boneEndSize = 0.015f;
    float fps = 30;
    float unitScale = 100; // centimetres per model unit (FBX UnitScaleFactor); the game works in metres
    // Fur shells, as the model viewer draws them: each written mesh that binds a fur volume gets one more
    // mesh per volume layer, pushed out along its normals and skinned like it. Each layer's texture is
    // baked from the root and tip colour maps, with alpha = the layer's coverage.
    bool fur = true;
    float furLength = 0.015f; // shell thickness at the tips, as a fraction of the model's radius
    float furTiling = 6;      // repeats of the fur volume over the surface
    uint32_t furSize = 512;   // baked texture size
    // Level 0 of a texture as RGBA, every slice of a volume; needed for fur.
    std::function<std::optional<std::vector<image::Rgba>>(const std::string&)> pixels;
};

struct Report {
    size_t meshes = 0, vertices = 0, triangles = 0, bones = 0, materials = 0, textures = 0, embedded = 0, animations = 0;
    size_t furShells = 0;
    std::vector<std::string> notes;
};

Bytes write(const model::Model& m, const Options& opt, Report* report = nullptr);

// The same export as Wavefront OBJ: meshes, materials (<base>.mtl) with their tinted textures and fur shells as in
// the FBX. `files` are the PNGs and side files to write next to <base>.obj, by file name.
struct ObjFiles {
    std::string obj, mtl;
    std::vector<std::pair<std::string, Bytes>> files;
};
ObjFiles writeObj(const model::Model& m, const Options& opt, const std::string& base, Report* report = nullptr);

// ---- reading ----------------------------------------------------------------------------------

bool isFbx(const Bytes& file);

// A mesh of a binary FBX file (7.x), in the game's space: metres, Y up, facing +Z. The file's node
// transforms, unit scale and axis settings are applied.
struct MeshData {
    std::string name;                         // the mesh object's name
    std::vector<std::array<float, 3>> points; // control points
    std::vector<std::vector<int>> polygons;   // each polygon's corners (indices into the corner arrays)
    std::vector<int> cornerPoint;             // control point of each corner
    std::vector<std::array<float, 3>> cornerNormal; // empty when the file has no normals
    std::vector<std::array<float, 2>> cornerUv;     // first UV set as stored (v up); empty when none
    // Per control point: bone (by name) and weight; empty when the mesh has no skin.
    std::vector<std::vector<std::pair<std::string, float>>> skin;
    // Its first material's colour (DiffuseColor, as stored: linear) when that material has no texture.
    std::optional<std::array<float, 3>> plainColour;
    // Its first material's colour texture, encoded (PNG...), when a .glb gives it one, and that image's name.
    Bytes colourImage;
    std::string colourImageName;
};
std::vector<MeshData> readMeshes(const Bytes& file);

// A skeleton bone (LimbNode) of a binary FBX file, its joint in the game's space as for MeshData. The position is
// its bind pose (skin cluster or BindPose) when the file has one, else where the node stands.
struct BoneData {
    std::string name, parent; // parent: empty for a top bone
    std::array<float, 3> position{};
    bool fromBind = false;
};
std::vector<BoneData> readBones(const Bytes& file);

} // namespace nut::fbx
