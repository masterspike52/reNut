// A character of our own for reNut's worlds: a skinned model drawn by reNut's skinned shaders, an
// idle animation for its skeleton and the animation table that names it.
#pragma once

#include "blockmodel.hpp"
#include "common.hpp"
#include "model.hpp"

#include <array>

namespace nut::npcmodel {

struct Bone {
    std::string name;
    int parent = -1;
    std::array<float, 3> world{};  // bind position in model space (bind rotations are identity)
};

// A skeleton: parents before children, each depth's bones after the previous depth's, at most 255.
struct Rig {
    std::vector<Bone> bones;
};

// A skinned vertex: up to four bones (indices into the rig) and their weights, summing to 255.
struct SkinVertex {
    blockmodel::WorldVertex v;
    std::array<uint8_t, 4> bones{};
    std::array<uint8_t, 4> weights{255, 0, 0, 0};
    std::vector<blockmodel::ShapeDelta> shapes;  // its shape keys' deltas (in the bind pose)
};

// One draw: its own vertex and index buffers and the texture it binds (an index into `textures`).
struct SkinDraw {
    std::vector<SkinVertex> vertices;  // at most 65535
    std::vector<uint32_t> indices;     // a triangle list
    uint32_t texture = 0;
};

// A humanoid skeleton for a mesh standing on y = 0 and facing +z: BASE, BACKA (hips), BACKB (chest), NECK1,
// HEAD, and per side (LF_ on +x, RT_) _H _K _F (hip, knee, ankle) and _S _E (shoulder, elbow).
Rig autoRig(const std::vector<std::array<float, 3>>& positions);

// A vertex's bones and weights (sum 255) in that rig: blended by height between neighbouring spine
// joints, and towards a shoulder for what reaches out sideways from the chest (the arms).
void skinWeights(const Rig& rig, const std::array<float, 3>& pos, std::array<uint8_t, 4>& bones, std::array<uint8_t, 4>& weights);

// The model asset's CAFF, drawn by reNut's skinned vertex shader (physical part kind 7) and world pixel
// shader (kind 4): writeWorld's layout with 40-byte vertices, the rig in its pose and op 0x12.
Bytes writeSkinnedModel(const std::string& assetName, const Rig& rig, const std::vector<SkinDraw>& draws, const std::vector<std::string>& textures,
                        const blockmodel::ShapeTable* shapes = nullptr);

// A looping idle anim's CAFF for the rig, `seconds` long (2.4 when not above 0) at 30 frames a second, its
// last frame its first. `idle` is "breathe" or "walk" (built in) or a JSON timeline file (anim-import's format).
// `armsDown` lowers both shoulders by that many degrees in every frame (a T-posed model's arms to its sides).
Bytes writeIdleAnim(const std::string& assetName, const Rig& rig, const std::string& idle, float seconds, float armsDown = 0);

// An anim's CAFF for the rig from its keys, `frames` (30 a second) of a key per bone: local rotations (the bind
// rotations are identity), offsets from the bind positions, scales.
Bytes writeAnim(const std::string& assetName, const Rig& rig, const std::vector<model::Key>& keys, uint32_t frames);

// An animation table entry: its name (as state tables play it) and the anim asset's name (aid_anim_...).
struct AnimEntry {
    std::string name;
    std::string anim;
    bool loop = true;
    // The animation layer it plays on (the flags' low byte); an actor gets one more layer than its table's highest.
    uint8_t layer = 0;
};

// The animtable asset's CAFF: the entries sorted by name, looping ones flagged as the game's "stand" is.
Bytes writeAnimtable(const std::string& assetName, const std::vector<AnimEntry>& entries);

} // namespace nut::npcmodel
