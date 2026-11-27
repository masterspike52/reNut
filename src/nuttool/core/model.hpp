// Rare model assets (asset type 0x04): a "rendergraph" (version 26.09.07.0039) in .data,
// vertex/index buffers in .gpu and compiled material programs in .stream.
//
// What is decoded (from default.xex and the shipped data):
//   .data +0           -> chunk directory of {u32 type, u32 ptr} pairs; +4 u32 pair count
//   chunk 0x1E         -> pointer to the rendergraph header ("rendergraph\0" + version)
//   chunk 0x0A         -> render list
//   rendergraph +0x20  property dictionary (bone name -> index, texturefileN -> texture asset name,
//                      shader_* -> material); entries {hash, ?, ->{name, name, value}}
//   rendergraph +0x104 pose ("pose", version 19.12.06.0036): u32 boneCount, records of 52 bytes
//                      {vec3 local, vec3 world, quat rotation, u16 parent, ...}, then a name table
//   rendergraph +0xFC  GPU resources: vertex buffer records {stride, object, .gpu offset, size} and
//                      index buffer records {object, .gpu offset, bytes, format}
//   render list +0x00  ->draw groups, +0x04 count; a group is {u32 passes, ->passList, u32 draws,
//                      ->draws}; a draw is eight
//                      pointers: stream binding {vb object, stride}, ..., sampler bindings, index pair
//                      {ib, count}; sampler bindings are {count, ->program, {slot, u16 N+1, u16 N, ..}}
//                      naming texturefileN (split textures use N for the mip half, N+1 for the top)
//   rendergraph +0x28  authoring nodes {count, ->0x50-byte records {->name, ...}}: Maya paths such
//                      as "LODBIT_|transform25|Banjo_0_1_" (drawn at detail levels 0 and 1),
//                      "|Kazooie_SWITCH0_1|Kazooie_LOD_0" or "|camera_SWITCH1_2" (drawn while
//                      switch 1 is in state 2)
//   draw group         {u32 n, ->node path[n], u32 draws, ->draws}: the group draws when every node
//                      on its path is active
//   draw field 5       material constants: register blocks {->.stream values, count, ?} at +0xd4;
//                      the material's parameters (c:basecolour, c:specularPower...) take them in order
// Index buffers are 16-bit big-endian triangle lists. Vertex layouts are inferred per stride.
#pragma once

#include "caff.hpp"
#include "common.hpp"

#include <array>
#include <map>

namespace nut::model {

struct Bone {
    std::string name;
    int parent = -1;
    float local[3] = {};
    float world[3] = {};   // bind-pose position in model space
    float rotation[4] = {0, 0, 0, 1}; // local rotation (x, y, z, w)
};

struct Vertex {
    float pos[3] = {};
    float normal[3] = {0, 1, 0};
    float uv[2] = {};
    float uv2[2] = {}; // second texture coordinate set (fur thinning maps, ambient occlusion), if any
    float uv3[2] = {}; // third set (fur volumes), if any
    uint8_t bones[4] = {};
    float weights[4] = {1, 0, 0, 0};
};

// Vertex layout detected from the data. Every format is: half4 (or float3) position; for skinned buffers
// u16x4 bone indices and u8x4 weights (reversed byte order); a DEC3N normal; an optional DEC3N tangent;
// then zero or more half2 texture coordinate sets.
struct Layout {
    bool floatPosition = false; // float3 position instead of half4
    int attributes = 8;         // offset of the first field after the position
    bool skinned = false;
    int normal = -1;            // byte offsets, -1 when absent
    int tangent = -1;
    std::vector<int> uvSets;
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; // triangle list
    uint32_t stride = 0;
    int group = 0;       // render list group the draw belongs to
    uint32_t buffer = 0; // vertex buffer object (shared by alternatives of one part)
    Layout layout;
    std::vector<int> textures; // textures bound to the draw's samplers (indices into Model::textures)
    std::vector<int> textureSlots; // sampler slot of each entry in `textures`
    int texture = -1;          // the colour map among them, picked by name (-1 = none)
    std::string material;      // shader name, when identified
    struct Constant {
        std::string name;
        std::array<float, 4> value;
        int symbol = -1;     // where the value is stored: symbol (0-based) and byte offset of 4 big-endian floats
        uint32_t offset = 0;
    };
    std::vector<Constant> constants; // material parameters of this draw
    float colour[4] = {0.72f, 0.70f, 0.66f, 1};
    float specular[3] = {0, 0, 0}; // c:specularColour
    float specularPower = 16;      // c:specularPower
    bool hasColour = false;          // colour comes from c:basecolour
    // The colour map is drawn as texture * textureGain + textureOffset (c:textureGain<slot>...).
    float textureGain[4] = {1, 1, 1, 1};
    float textureOffset[4] = {0, 0, 0, 0};
    // Where the draw's data is stored, for writing it back (modelimport.hpp). Offsets without a
    // symbol are in the model's .data symbol.
    struct Source {
        int vbSymbol = -1;        // .gpu symbol (0-based) holding the vertex buffer
        uint32_t vbOffset = 0;    // buffer start in it
        uint32_t vbCount = 0;     // vertices in the whole buffer
        uint32_t vbRecord = 0;    // resource record {->header, ->.gpu offset, size}
        uint32_t vbHeader = 0;    // header {..., stride at -4, ->.gpu offset at +4, size at +8}
        int ibSymbol = -1;
        uint32_t ibOffset = 0;
        uint32_t ibRecord = 0;    // resource record {->header, ->.gpu offset, bytes, format}
        bool wideIndices = false; // 32-bit indices
        uint32_t indexCountAt = 0; // the draw's index count
        uint32_t groupRecord = 0;  // its group {u32 path count, ->path, u32 draws, ->draws}
        uint32_t drawRecord = 0;   // its 32-byte record in the group's draws
        std::vector<uint32_t> samplers; // each of `textures`' sampler record {u32 slot, u16 N+1, u16 N, ..}; draws may share one
        int programSymbol = -1;    // its texture binding (op 0x43) in the render program (.stream part): symbol and offset
        uint32_t programAt = 0;
        std::vector<uint32_t> vertex; // buffer index of each of `vertices`
    } source;
};

// A node of the authoring hierarchy.
struct Node {
    std::string name;
    uint32_t lods = 0; // bit n: drawn at detail level n (0 = not a detail-level node)
    int switchId = -1, switchState = -1;
};

struct Model {
    std::string name;
    std::vector<Bone> bones;
    std::vector<Mesh> meshes;
    std::vector<std::string> textures;  // texture asset names (split halves merged)
    std::vector<int> textureFiles;      // the texturefileN of each of `textures` (a split one's mip half)
    std::vector<std::string> materials; // shader_* names
    struct Group {
        std::vector<size_t> meshes;
        size_t triangles = 0;
        std::vector<int> path; // nodes that must be active
        bool shown = false;    // default visibility: detail level 0, every switch in its first state
    };
    std::vector<Group> groups;
    std::vector<Node> nodes;
    int lodCount = 1;
    int defaultLod = 0; // the level `shown` reflects (0 unless level 0 has no geometry)
    std::map<int, std::vector<int>> switches; // switch id -> its states
    float boundsMin[3] = {}, boundsMax[3] = {};
    std::vector<std::string> warnings;
};

// True when the symbol holds a model rendergraph.
bool isModelSymbol(const caff::Caff& c, size_t symbol);
// Parses the model whose .data part is `symbol`, with the draws of every group.
Model parse(const caff::Caff& c, size_t symbol);
// Which groups draw at detail level `lod` with the given switch states (missing switches: any).
std::vector<bool> visibleGroups(const Model& m, int lod, const std::map<int, int>& switchState);
// Wavefront OBJ (+MTL referencing `<texture>.png` files). `include` picks meshes (default: the
// meshes of shown groups); `meshTexture` optionally picks a texture index per mesh (-1 = none),
// by default each mesh uses its colour map.
std::string toObj(const Model& m, const std::string& mtlFile, const std::vector<int>& meshTexture = {}, const std::vector<bool>& include = {});
std::string toMtl(const Model& m);

// Skeletal animation (asset type "anim", header "animation" 19.12.06.0036): a QUAT_BITSTREAM32
// track of per-bone local rotation, translation and scale, sampled at evenly spaced frames, and
// the pose (bone names, hierarchy) it was made for. Bones are matched to a model by name.
struct Animation {
    struct Key {
        float rotation[4] = {0, 0, 0, 1}; // x, y, z, w
        float translation[3] = {};
        float scale[3] = {1, 1, 1};
    };
    std::string name;
    std::string codec;
    float duration = 0;  // seconds from the first to the last frame (30 authored frames a second)
    int frames = 0;            // stored frames (keys)
    int authoredFrames = 0;    // frames the animation was made with
    std::vector<float> frameTimes; // time of each stored frame, seconds
    uint32_t bones = 0;
    std::vector<std::string> boneNames;
    std::vector<int> parents;
    std::vector<Bone> pose; // the skeleton it was made for, in its bind pose
    std::vector<Key> keys; // frames * bones
    std::vector<std::string> warnings;
};
using Key = Animation::Key;
bool isAnimationSymbol(const caff::Caff& c, size_t symbol);
Animation parseAnimation(const caff::Caff& c, size_t symbol);

// Replaces the keys of the animation whose .data part is `symbol` (frames * bones, laid out as
// Animation::keys). Its length, frame count, pose and other tracks stay; the rotation track's
// tables are re-encoded and appended to the symbol, and its pointers moved to them.
void writeAnimationKeys(caff::Caff& c, size_t symbol, const std::vector<Key>& keys);

// A rotation track's tables in the narrow form (16-bit bases, 4-bit widths, each frame on whole words
// and 8 spare bytes after the last), as writeAnimationKeys and new animations write them.
struct EncodedTrack {
    Bytes masks, bases, widths, frames;
    uint32_t stride = 0;  // bytes per frame
};
// Encodes `keys` (frames * records) with the track's nine channel scales and defaults (rotation x y z,
// translation, scale). `name` is the animation's, for errors.
EncodedTrack encodeTrack(const std::string& name, const std::vector<Key>& keys, uint32_t frames, uint32_t records,
                         const std::vector<float>& scale, const std::vector<float>& def);

} // namespace nut::model
