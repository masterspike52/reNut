// The model tab's view, drawn by the game: the model stands in reNut's studio and the camera orbits it. The tab
// leaves this view see-through; picking, the skeleton and the grid are worked out here with the game's camera.
#pragma once

#include "model.hpp"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nut::gui {

// A picture for the overlay (thumbnails, texture previews).
class GlTexture {
public:
    GlTexture() = default;
    GlTexture(const GlTexture&) = delete;
    GlTexture& operator=(const GlTexture&) = delete;
    ~GlTexture();
    void upload(const uint8_t* rgba, int w, int h, bool mipmaps, bool nearest);
    int width() const { return w_; }
    int height() const { return h_; }
    ImTextureID imgui() const { return id_; }

private:
    ImTextureID id_ = ImTextureID_Invalid;
    int w_ = 0, h_ = 0;
};

// A fur volume's slices: the game draws fur itself, so only their count is kept.
class GlTextureArray {
public:
    void upload(const uint8_t*, int, int, int layers) { layers_ = layers; }
    int layers() const { return layers_; }

private:
    int layers_ = 0;
};

class ModelView {
public:
    struct Options {
        bool textured = true;
        bool wireframe = false;
        bool bones = true;
        bool boneNames = false;
        bool grid = true;
        bool backfaces = true;
        bool tint = true;
        bool fur = true;
        float furLength = 0.015f;
        float furTiling = 6;
        int lighting = 0;
        bool lightFollowsCamera = true;
        float lightAzimuth = 40;
        float lightElevation = 35;
        float brightness = 1.0f;
        float ambient = 0.3f;
        float lightColour[3] = {1.0f, 0.97f, 0.92f};
    };

    struct Fur {
        const GlTextureArray* volume = nullptr;
        const GlTexture* root = nullptr;
        const GlTexture* tip = nullptr;
        const GlTexture* thinning = nullptr;
        bool noise = false;
    };

    // The view's camera for drawing over it: model-space points to the screen and back.
    struct ViewCamera {
        float eye[3] = {}, f[3] = {}, s[3] = {}, u[3] = {};
        float tanHalf = 1, aspect = 1, floor = 0;
        ImVec2 vpPos, vpSize;
        // A model-space point on the screen and its distance along the view; false behind the camera.
        bool toScreen(const float* p, ImVec2* out, float* depth = nullptr) const;
        // The ray through a screen point, in model space.
        void ray(const ImVec2& screen, float* origin, float* dir) const;
        // A screen pixel's model-space size at distance `depth`.
        float pixel(float depth) const { return 2 * depth * tanHalf / std::max(vpSize.y, 1.0f); }
    };
    // Draws over the view with its camera; `hovered` while the cursor is over it or a drag on it goes on.
    std::function<void(ImDrawList*, const ViewCamera&, bool hovered)> overlay;
    // Rig editing: the left and right buttons are the overlay's, the middle one orbits (Shift: pans), no fly keys.
    void setRigInput(bool on) { rigInput_ = on; }
    // The game's model leaves the studio while NutTool draws its own, and comes back when shown again.
    void setGameHidden(bool hidden);
    // The game's model drawn with these skin matrices (three rows of four floats a bone, as ShownPose gives them)
    // instead of its animation's; null gives it back.
    void holdGamePose(const std::vector<float>* rows);
    bool bonesHidden = false;  // the overlay draws the skeleton

    ModelView();
    ~ModelView();

    // The model the game shows: its asset id and the data bundle it's in (its stream bundle loads too).
    void setGameModel(uint32_t model, uint32_t bundle);
    // Why the game couldn't show it, or empty.
    std::string gameError() const;
    // The model's switches as the game shows them: switch id to state (-1: none of its parts).
    void setSwitches(const std::map<int, int>& switches);
    // An animation asset the game plays on the model (0: none, the model made again in its bind pose).
    void playGameAnimation(uint32_t anim);

    void setModel(const model::Model& m);
    void setMeshTexture(size_t mesh, const GlTexture* tex) { (void)mesh, (void)tex; }
    void setMeshVisible(size_t mesh, bool v);
    void setMeshColour(size_t mesh, const float* rgb) { (void)mesh, (void)rgb; }
    void setMeshTint(size_t mesh, const float* gain, const float* offset) { (void)mesh, (void)gain, (void)offset; }
    void setMeshFur(size_t mesh, const Fur& fur) { (void)mesh, (void)fur; }
    void setPose(const std::vector<float>* skinMatrices, const std::vector<float>* boneWorld);
    void frame();
    void draw(const ImVec2& size, const Options& opt);
    void setHighlight(const std::vector<bool>& meshes) { highlight_ = meshes; }
    std::optional<int> takeClick() { return std::exchange(click_, std::nullopt); }

private:
    struct Mesh {
        std::vector<float> bind;      // bind-pose positions of skinned meshes, 3 per vertex
        std::vector<uint8_t> boneIds; // 4 per vertex
        std::vector<float> weights;   // 4 per vertex
        std::vector<float> pos;       // current positions, 3 per vertex, for picking
        std::vector<uint32_t> indices;
        bool visible = true;
    };
    struct Camera {
        float eye[3], f[3], s[3], u[3];  // where it is and its axes (forward, right, up)
        float tanHalf, aspect;           // the game's field of view and the screen's shape
    };
    // The game's camera for the view's rectangle: aimed so the target sits in the rectangle's middle.
    Camera camera(const ImVec2& origin, const ImVec2& size) const;
    bool project(const Camera& c, const float* p, ImVec2* out) const;
    void skinMeshes();
    const float* boneAt(size_t i) const {
        return boneWorld_.size() >= (i + 1) * 3 ? &boneWorld_[i * 3] : bones_[i].world;
    }
    int pick(const Camera& c, float ndcX, float ndcY) const;

    // Shared with the game's thread, which spawns the model and keeps what it made.
    struct Game {
        std::atomic<uint32_t> object{0};
        std::atomic<uint64_t> visit{0};  // the studio visit `object` was made in
        std::atomic<bool> gone{false};
        std::string error;  // under the session's queue: written there, read here after `failed`
        std::atomic<bool> failed{false};
        std::mutex mutex;
        std::vector<float> rows;  // its bones' skin matrices as the game posed them (part_api::ShownPose)
        std::atomic<bool> asked{false};
        std::vector<float> held;  // the matrices it's held in (part_api::HoldShownPose)
        int hold = 0;             // 1: held each frame, 2: `held` sent once more as it's let go
    };
    // The game's pose of its model, when there is one, for the bones drawn and picking.
    void takeGamePose();
    std::shared_ptr<Game> game_ = std::make_shared<Game>();
    uint32_t model_ = 0, bundle_ = 0;
    uint64_t shownVisit_ = 0;  // the studio visit setGameModel last showed the model in
    std::map<int, int> switches_;
    uint32_t anim_ = 0;  // the animation the game plays on it, or 0
    bool rigInput_ = false, hidden_ = false, holding_ = false;

    std::vector<Mesh> meshes_;
    std::vector<bool> highlight_;
    std::optional<int> click_;
    std::vector<model::Bone> bones_;
    std::vector<float> skin_, boneWorld_;
    bool skinDirty_ = false;
    float center_[3] = {}, radius_ = 1, floor_ = 0;
    float yaw_ = 0.6f, pitch_ = 0.3f, distance_ = 3;
    float target_[3] = {};
};

} // namespace nut::gui
