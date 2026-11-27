// The model tab's rig and weight editing over the studio's view: Blender's edit, pose and weight paint modes.
#pragma once

#include "rigedit.hpp"
#include "studio_view.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nut::gui {

class App;

class RigEditor {
public:
    enum class Mode { Object, Edit, Pose, Weight };
    // A model weights can be copied from.
    struct Source {
        std::string label;
        std::shared_ptr<const model::Model> model;
        std::shared_ptr<const rigedit::Rig> rig;
    };
    // Makes the edited rig a change of the model, which is then loaded again.
    std::function<void(std::shared_ptr<const rigedit::Rig>)> apply;
    // The models Transfer can copy weights from.
    std::function<std::vector<Source>()> sources;

    void load(const caff::Caff& c, uint32_t symbol, const model::Model& m);
    void setMeshesShown(const std::vector<bool>& shown);
    Mode mode() const { return mode_; }
    bool dirty() const { return dirty_; }
    // Hands unapplied edits to `apply`; markDirty takes them back when they couldn't be applied.
    void commit();
    void markDirty() { dirty_ = true; }
    void drawPanel(App& app);
    // Before the view draws: its input, its overlay and whether the game's model shows.
    void prepare(ModelView& view, App& app);

private:
    using Shape = std::vector<std::vector<std::array<float, 6>>>;  // per mesh, each vertex's position then normal
    struct Snapshot {
        std::vector<rigedit::Bone> bones;
        std::vector<std::vector<rigedit::VertexSkin>> skins;
        std::vector<rigedit::PoseKey> pose;
        Shape shape;  // when reshaped
        bool reshaped = false;
        std::vector<rigedit::Mat4> accum;
        std::vector<int> origin;
    };
    struct Transform {
        char kind = 0;  // 'G', 'R', 'S' or 'T' (the gizmo's trackball); 0 when none
        int axis = -1;  // a locked world axis
        bool drag = false;  // started on the rotate gizmo: applied when the button is let go
        std::string typed;
        ImVec2 start, pivotScreen;
        std::array<float, 3> pivot{};
        std::vector<size_t> bones;
        Snapshot before;
    };
    struct Projected {
        std::vector<ImVec2> screen;
        std::vector<float> depth;
        std::vector<char> valid;
    };

    void setMode(Mode m);
    Snapshot snapshot() const;
    void restore(const Snapshot& s);
    void pushUndo();
    void undo();
    void redo();
    void changed(bool rig);  // the rig or weights changed: dirty, the preview made again
    // The joints are where the pose (or a bend in Edit mode) puts them, the mesh bent with them.
    bool posing() const { return mode_ == Mode::Pose || mode_ == Mode::Weight || (mode_ == Mode::Edit && (meshFollows_ || reshaped_)); }
    // The game's model's skin matrices showing the pose and the bends on its shape as last saved (ShownPose's rows).
    std::vector<float> gameRows();
    Shape shape() const;
    void setShape(const Shape& s);
    // Edit mode, mesh following: the pose a transform made becomes the rest pose, the mesh bent with it.
    void bake();

    void updatePosed();
    std::array<float, 3> jointAt(size_t bone) const;  // where a joint shows in the current mode
    void project(const ModelView::ViewCamera& cam);
    void buildDepth(const ModelView::ViewCamera& cam);
    bool visible(size_t mesh, size_t v, const ModelView::ViewCamera& cam) const;

    void overlay(ImDrawList* dl, const ModelView::ViewCamera& cam, bool hovered, App& app);
    void drawMesh(ImDrawList* dl, const ModelView::ViewCamera& cam, bool heat);
    void drawSkeleton(ImDrawList* dl, const ModelView::ViewCamera& cam);
    void keys(const ModelView::ViewCamera& cam, App& app);
    int jointUnder(const ModelView::ViewCamera& cam, const ImVec2& p) const;
    void selectBox(const ModelView::ViewCamera& cam, const ImVec2& a, const ImVec2& b, int how);

    std::vector<size_t> selectedBones() const;
    std::array<float, 3> transformPivot(const std::vector<size_t>& sel, char kind) const;
    // The rotate gizmo around the selection (Blender's): its part under `p`, 0-2 an axis ring, 3 the view ring,
    // 4 the trackball inside, -1 none.
    bool gizmoShown() const;
    int gizmoUnder(const ModelView::ViewCamera& cam, const ImVec2& p) const;
    void drawGizmo(ImDrawList* dl, const ModelView::ViewCamera& cam, int hot) const;
    void beginTransform(char kind, const ModelView::ViewCamera& cam, App& app);
    void updateTransform(const ModelView::ViewCamera& cam);
    void endTransform(bool confirm);

    void dab(const ModelView::ViewCamera& cam, const ImVec2& at);
    void buildMirror();
    void clickVertex(const ModelView::ViewCamera& cam, const ImVec2& p, bool add);

    void addBone(bool extrude, App& app, const ModelView::ViewCamera* cam);
    void deleteBones(App& app);
    void parentTo(App& app, bool clear);

    void drawEditPanel(App& app);
    void drawPosePanel(App& app);
    void drawWeightPanel(App& app);
    void drawBoneList();
    rigedit::Mask mask(bool selectedOnly) const;
    int selectedVertices() const;

    model::Model model_;
    rigedit::Rig rig_;
    std::vector<rigedit::Bone> loadedBones_;
    rigedit::Adjacency adj_;
    bool adjReady_ = false;
    std::vector<int> mirrorPlace_;  // place -> its mirror place, -1 for none
    bool mirrorReady_ = false;
    std::vector<bool> shown_;
    float size_ = 1;  // the skeleton's size

    Mode mode_ = Mode::Object;
    bool dirty_ = false;
    std::vector<rigedit::PoseKey> pose_;
    std::vector<rigedit::Mat4> world_, skin_;
    std::vector<std::vector<float>> posed_, normals_;
    bool posedDirty_ = true;
    std::vector<Projected> proj_;
    std::vector<float> depthBuf_;
    int depthW_ = 0, depthH_ = 0;
    ImVec2 depthAt_;
    float depthCell_ = 4;

    std::vector<char> boneSel_;
    int active_ = -1;
    std::vector<std::vector<char>> vertSel_;
    int lastMesh_ = -1, lastVertex_ = -1;
    Transform x_;
    std::vector<Snapshot> undo_, redo_;
    std::string error_;

    bool childrenFollow_ = true, names_ = false, showGame_ = false, gizmo_ = true;
    bool meshFollows_ = true, reshaped_ = false;
    std::vector<rigedit::PoseKey> savedPose_;  // Pose mode's pose, kept while Edit mode bends from the rest
    Shape loadedShape_;                        // the meshes as loaded, for undoing back past the first bend
    std::vector<rigedit::Mat4> accum_;         // per bone: the bends made since loading, from the game's shape to the rest now
    std::vector<int> origin_;                  // per bone: its number in the game's model, -1 for one added since
    int pivot_ = 0;  // bone head, median, active joint
    int tool_ = 0;   // weight paint: brush, select
    int brush_ = 0;  // add, subtract, draw, smooth
    float radius_ = 40, strength_ = 0.25f, value_ = 1;
    int falloff_ = 0;  // smooth, linear, constant
    bool autoNormalize_ = true, frontOnly_ = true, occlude_ = true, mirrorPaint_ = false;
    bool stroking_ = false, boxing_ = false;
    ImVec2 lastDab_, boxStart_;

    rigedit::AutoOptions auto_;
    int autoBones_ = 0;  // all, selected, added
    bool opsSelected_ = false;
    rigedit::TransferOptions transfer_;
    std::vector<Source> sourceList_;
    int source_ = 0;
    float smoothFactor_ = 0.5f, cleanThreshold_ = 0.01f, assign_ = 1, selectAbove_ = 0.1f;
    int smoothIterations_ = 3, limit_ = 4;
    bool mirrorPositive_ = true;
    std::string rename_;
    std::vector<std::string> notes_;
};

} // namespace nut::gui
