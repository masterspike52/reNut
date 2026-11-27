// A model's rig and skin, edited and written back: joints, bones added after the game's own, vertex weights.
// Bind poses are world-aligned (Pose_ApplyInverseBindOffsets 0x82223EB0), so a joint is only a position.
#pragma once

#include "caff.hpp"
#include "model.hpp"

#include <array>
#include <map>
#include <unordered_map>

namespace nut::rigedit {

struct Bone {
    std::string name;
    int parent = -1;             // always a lower index
    std::array<float, 3> pos{};  // bind position in model space
    bool added = false;          // made here, after the model's own bones
};

struct Influence {
    uint16_t bone = 0;  // index into Rig::bones
    float weight = 0;
};
using VertexSkin = std::array<Influence, 4>;

struct Rig {
    std::vector<Bone> bones;
    size_t gameBones = 0;                        // the model's own bones, the first ones
    std::vector<std::vector<VertexSkin>> skins;  // per model mesh; empty for unskinned meshes
    std::vector<std::vector<uint64_t>> keys;     // per mesh vertex: its buffer vertex, shared by alternative parts
    std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> shared;  // keys more than one mesh vertex has
    // A new rest shape bent with the bones: per mesh, each vertex's position then normal; empty leaves the mesh as it is.
    std::vector<std::vector<std::array<float, 6>>> shape;
};

// The model's rig and skin (a vertex's indices are bone numbers: the shaders fetch the instance's matrices with them).
Rig read(const caff::Caff& c, size_t symbol, const model::Model& m);
// Writes `rig` (read from this model, its meshes unchanged) into the pose, the draws' bone lists (op 0x12) and the vertices.
std::vector<std::string> write(caff::Caff& c, size_t symbol, const Rig& rig);
// Gives the vertices sharing mesh `mesh` vertex `v`'s buffer vertex its weights.
void syncAliases(Rig& rig, size_t mesh, uint32_t v);

using Mat4 = std::array<float, 16>;  // column-major
struct PoseKey {
    std::array<float, 4> rotation{0, 0, 0, 1};  // local, x y z w
    std::array<float, 3> offset{};              // added to the bind offset from the parent
    std::array<float, 3> scale{1, 1, 1};
};
// Each bone's world and skin matrix (world * bind^-1); bones past the pose's end stay at rest.
void evaluate(const std::vector<Bone>& bones, const std::vector<PoseKey>& pose, std::vector<Mat4>& world, std::vector<Mat4>& skin);
// The mesh's vertices skinned by `skin`, three floats a vertex.
std::vector<float> skinMesh(const model::Mesh& mesh, const std::vector<VertexSkin>& weights, const std::vector<Mat4>& skin);

float weightOf(const VertexSkin& s, uint16_t bone);
// Sets `bone`'s weight, taking the lightest influence's place when it has none; `normalize` scales the others to fill the rest.
void setWeight(VertexSkin& s, uint16_t bone, float w, bool normalize);
void normalize(VertexSkin& s);
// The heaviest `maxInfluences` of a set of weights, normalized.
VertexSkin fromWeights(const std::map<uint16_t, float>& weights, int maxInfluences = 4);

// The vertices an operation changes: meshes not in `meshes` stay, and an empty vertex list is the whole mesh.
struct Mask {
    std::vector<char> meshes;
    std::vector<std::vector<char>> vertices;
    bool has(size_t mesh, size_t v) const {
        if (mesh >= meshes.size() || !meshes[mesh]) return false;
        return mesh >= vertices.size() || vertices[mesh].empty() || (v < vertices[mesh].size() && vertices[mesh][v]);
    }
};

enum class AutoMethod { Nearest, Envelope, Blend };
struct AutoOptions {
    AutoMethod method = AutoMethod::Blend;
    int maxInfluences = 4;
    float power = 4;           // Blend: weight falls off as 1 / distance^power
    float envelope = 0.15f;    // Envelope: the radius, a fraction of the skeleton's size
    std::vector<char> bones;   // the bones that may take weight (empty: all)
};
// Weights from the bones' segments (a joint to its main child): the nearest, those in an envelope, or blended by distance.
void autoWeights(const model::Model& m, Rig& rig, const Mask& mask, const AutoOptions& opt);

struct TransferOptions {
    int maxInfluences = 4;
    bool nearestSurface = true;  // else the nearest vertex
};
// Weights from another model's skin where its surface is nearest, bones matched by name (a bone without a match
// gives its weight to its nearest matched ancestor). Returns how many of its bones matched.
size_t transfer(const model::Model& src, const Rig& srcRig, const model::Model& dst, Rig& rig, const Mask& mask, const TransferOptions& opt);

// The skinned surface's places (vertices at one position joined: seams, alternative parts) and their neighbours.
struct Adjacency {
    std::vector<std::vector<uint32_t>> place;                         // [mesh][vertex] -> its place
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> members;  // place -> its (mesh, vertex)
    std::vector<std::vector<uint32_t>> neighbours;                    // place -> neighbouring places
};
Adjacency adjacency(const model::Model& m);

void smooth(const model::Model& m, const Adjacency& adj, Rig& rig, const Mask& mask, float factor, int iterations, int maxInfluences = 4);
void normalizeAll(Rig& rig, const Mask& mask);
// Drops influences under `threshold`, then normalizes.
void clean(Rig& rig, const Mask& mask, float threshold);
void limit(Rig& rig, const Mask& mask, int maxInfluences);

// The bone on the other side (LF_/RT_, L_/R_, _L/_R, .L/.R, Left/Right), or -1.
int mirrorBone(const Rig& rig, size_t bone);
// Copies one side's weights (positiveToNegative: +x's) to their mirror vertices with mirrored bones; returns how many matched.
size_t mirrorWeights(const model::Model& m, Rig& rig, const Mask& mask, bool positiveToNegative);
// Puts one side's joints at the mirror image of their twins; returns how many moved.
size_t mirrorJoints(Rig& rig, bool positiveToNegative);

// `base`, else base.001, base.002 and on: a name no bone has.
std::string uniqueName(const Rig& rig, const std::string& base);

} // namespace nut::rigedit
