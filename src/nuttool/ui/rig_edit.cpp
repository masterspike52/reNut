#include "rig_edit.hpp"

#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <unordered_map>

namespace nut::gui {
namespace {

using V3 = std::array<float, 3>;
using Q4 = std::array<float, 4>;
using ViewCamera = ModelView::ViewCamera;
constexpr float kPi = 3.14159265358979f;

V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 scale(const V3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float length(const V3& a) { return std::sqrt(dot(a, a)); }
V3 normalized(const V3& a) {
    const float l = length(a);
    return l > 1e-12f ? scale(a, 1 / l) : V3{0, 0, 0};
}
float dist(const ImVec2& a, const ImVec2& b) { return std::hypot(a.x - b.x, a.y - b.y); }

Q4 qmul(const Q4& a, const Q4& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
Q4 axisAngle(const V3& axis, float angle) {
    const V3 a = normalized(axis);
    const float s = std::sin(angle / 2);
    return {a[0] * s, a[1] * s, a[2] * s, std::cos(angle / 2)};
}
V3 rotate(const V3& axis, float angle, const V3& v) {
    const V3 k = normalized(axis);
    const float c = std::cos(angle), s = std::sin(angle);
    return add(add(scale(v, c), scale(cross(k, v), s)), scale(k, dot(k, v) * (1 - c)));
}

// A world matrix's rotation, its columns normalized (column-major 3x3 as three axes).
std::array<V3, 3> rotationOf(const rigedit::Mat4& m) {
    return {normalized({m[0], m[1], m[2]}), normalized({m[4], m[5], m[6]}), normalized({m[8], m[9], m[10]})};
}
// v in the frame whose axes are `r` (r^T v).
V3 intoFrame(const std::array<V3, 3>& r, const V3& v) { return {dot(r[0], v), dot(r[1], v), dot(r[2], v)}; }

// Blender's XYZ Euler angles (degrees) of a quaternion, and back.
V3 eulerOf(const Q4& q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float r00 = 1 - 2 * (y * y + z * z), r10 = 2 * (x * y + z * w), r20 = 2 * (x * z - y * w);
    const float r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y), r12 = 2 * (y * z - x * w), r11 = 1 - 2 * (x * x + z * z);
    const float ry = std::asin(std::clamp(-r20, -1.0f, 1.0f));
    float rx, rz;
    if (std::fabs(std::cos(ry)) > 1e-5f) {
        rx = std::atan2(r21, r22);
        rz = std::atan2(r10, r00);
    } else {
        rx = std::atan2(-r12, r11);
        rz = 0;
    }
    return {rx * 180 / kPi, ry * 180 / kPi, rz * 180 / kPi};
}
Q4 quatOf(const V3& degrees) {
    const Q4 qx = axisAngle({1, 0, 0}, degrees[0] * kPi / 180), qy = axisAngle({0, 1, 0}, degrees[1] * kPi / 180),
             qz = axisAngle({0, 0, 1}, degrees[2] * kPi / 180);
    return qmul(qz, qmul(qy, qx));
}

// Blender's weight colours: blue at 0, through cyan, green and yellow, to red at 1.
ImU32 heat(float w, float shade) {
    w = std::clamp(w, 0.0f, 1.0f);
    float r, g, b;
    if (w < 0.25f) r = 0, g = w / 0.25f, b = 1;
    else if (w < 0.5f) r = 0, g = 1, b = 1 - (w - 0.25f) / 0.25f;
    else if (w < 0.75f) r = (w - 0.5f) / 0.25f, g = 1, b = 0;
    else r = 1, g = 1 - (w - 0.75f) / 0.25f, b = 0;
    if (w <= 0) r = 0.05f, g = 0.05f, b = 0.45f;
    return IM_COL32(int(r * shade * 255), int(g * shade * 255), int(b * shade * 255), 255);
}

constexpr rigedit::Mat4 kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
rigedit::Mat4 mul(const rigedit::Mat4& a, const rigedit::Mat4& b) {
    rigedit::Mat4 m{};
    for (size_t c = 0; c < 4; ++c)
        for (size_t r = 0; r < 4; ++r)
            for (size_t k = 0; k < 4; ++k) m[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
    return m;
}

const char* kAxisNames[] = {"X", "Y", "Z"};
const ImU32 kAxisColours[] = {IM_COL32(240, 80, 80, 255), IM_COL32(120, 220, 90, 255), IM_COL32(80, 140, 255, 255)};

} // namespace

// ---- state ----------------------------------------------------------------------------------------

void RigEditor::load(const caff::Caff& c, uint32_t symbol, const model::Model& m) {
    model_ = m;
    error_.clear();
    try {
        rig_ = rigedit::read(c, symbol, m);
    } catch (const std::exception& e) {
        rig_ = {};
        error_ = e.what();
    }
    loadedBones_ = rig_.bones;
    const size_t n = rig_.bones.size();
    pose_.assign(n, {});
    accum_.assign(n, kIdentity);
    origin_.resize(n);
    for (size_t i = 0; i < n; ++i) origin_[i] = int(i);
    boneSel_.assign(n, 0);
    if (active_ >= int(n)) active_ = -1;
    vertSel_.clear();
    for (const auto& me : model_.meshes) vertSel_.emplace_back(me.vertices.size(), 0);
    lastMesh_ = lastVertex_ = -1;
    shown_.resize(model_.meshes.size(), true);
    undo_.clear();
    redo_.clear();
    x_ = {};
    dirty_ = false;
    reshaped_ = false;
    savedPose_.clear();
    loadedShape_ = shape();
    adjReady_ = mirrorReady_ = false;
    posedDirty_ = true;
    stroking_ = boxing_ = false;
    notes_.clear();
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const auto& b : rig_.bones)
        for (int k = 0; k < 3; ++k) lo[size_t(k)] = std::min(lo[size_t(k)], b.pos[size_t(k)]), hi[size_t(k)] = std::max(hi[size_t(k)], b.pos[size_t(k)]);
    for (int k = 0; k < 3; ++k) lo[size_t(k)] = std::min(lo[size_t(k)], m.boundsMin[k]), hi[size_t(k)] = std::max(hi[size_t(k)], m.boundsMax[k]);
    size_ = std::max(length(sub(hi, lo)), 1e-3f);
    if (!n) mode_ = Mode::Object;
    if (mode_ == Mode::Weight) {
        adj_ = rigedit::adjacency(model_);
        adjReady_ = true;
    }
}

void RigEditor::setMeshesShown(const std::vector<bool>& shown) {
    if (shown == shown_) return;
    shown_ = shown;
    posedDirty_ = true;
}

void RigEditor::setMode(Mode m) {
    if (m == mode_) return;
    if (x_.kind) endTransform(false);
    // Edit mode bends the rest pose: Pose mode's pose waits until it's back.
    if (mode_ == Mode::Edit) {
        pose_ = savedPose_;
        pose_.resize(rig_.bones.size());
    } else if (m == Mode::Edit) {
        savedPose_ = pose_;
        std::fill(pose_.begin(), pose_.end(), rigedit::PoseKey{});
    }
    mode_ = m;
    stroking_ = boxing_ = false;
    posedDirty_ = true;
    if (m == Mode::Weight && !adjReady_) {
        adj_ = rigedit::adjacency(model_);
        adjReady_ = true;
        mirrorReady_ = false;
    }
    if (m == Mode::Object) commit();
}

void RigEditor::commit() {
    if (!dirty_ || !apply) return;
    dirty_ = false;
    auto r = std::make_shared<rigedit::Rig>(rig_);
    if (reshaped_) {
        const Shape s = shape();
        r->shape.resize(s.size());
        for (size_t mi = 0; mi < s.size(); ++mi)
            if (mi < rig_.skins.size() && !rig_.skins[mi].empty()) r->shape[mi] = s[mi];
    }
    apply(r);
}

RigEditor::Shape RigEditor::shape() const {
    Shape s(model_.meshes.size());
    for (size_t mi = 0; mi < model_.meshes.size(); ++mi)
        for (const auto& v : model_.meshes[mi].vertices)
            s[mi].push_back({v.pos[0], v.pos[1], v.pos[2], v.normal[0], v.normal[1], v.normal[2]});
    return s;
}

void RigEditor::setShape(const Shape& s) {
    for (size_t mi = 0; mi < model_.meshes.size() && mi < s.size(); ++mi) {
        auto& verts = model_.meshes[mi].vertices;
        for (size_t v = 0; v < verts.size() && v < s[mi].size(); ++v)
            for (int k = 0; k < 3; ++k) {
                verts[v].pos[k] = s[mi][v][size_t(k)];
                verts[v].normal[k] = s[mi][v][size_t(3 + k)];
            }
    }
}

void RigEditor::bake() {
    posedDirty_ = true;
    updatePosed();
    for (size_t mi = 0; mi < model_.meshes.size(); ++mi) {
        if (mi >= rig_.skins.size() || rig_.skins[mi].empty()) continue;
        model::Mesh& me = model_.meshes[mi];
        const std::vector<float> p = rigedit::skinMesh(me, rig_.skins[mi], skin_);
        for (size_t v = 0; v < me.vertices.size(); ++v) {
            V3 n{};
            float total = 0;
            for (const auto& in : rig_.skins[mi][v]) {
                if (in.weight <= 0 || in.bone >= skin_.size()) continue;
                const rigedit::Mat4& M = skin_[in.bone];
                const float* n0 = me.vertices[v].normal;
                for (int k = 0; k < 3; ++k) n[size_t(k)] += in.weight * (M[size_t(k)] * n0[0] + M[size_t(4 + k)] * n0[1] + M[size_t(8 + k)] * n0[2]);
                total += in.weight;
            }
            for (int k = 0; k < 3; ++k) me.vertices[v].pos[k] = p[v * 3 + size_t(k)];
            if (total > 0) {
                n = normalized(n);
                for (int k = 0; k < 3; ++k) me.vertices[v].normal[k] = n[size_t(k)];
            }
        }
    }
    for (size_t i = 0; i < rig_.bones.size() && i < world_.size(); ++i) rig_.bones[i].pos = {world_[i][12], world_[i][13], world_[i][14]};
    for (size_t i = 0; i < accum_.size() && i < skin_.size(); ++i) accum_[i] = mul(skin_[i], accum_[i]);
    std::fill(pose_.begin(), pose_.end(), rigedit::PoseKey{});
    reshaped_ = true;
    mirrorReady_ = false;
    changed(true);
}

RigEditor::Snapshot RigEditor::snapshot() const {
    return {rig_.bones, rig_.skins, pose_, reshaped_ ? shape() : Shape{}, reshaped_, accum_, origin_};
}

void RigEditor::restore(const Snapshot& s) {
    const bool bonesChanged = s.bones.size() != rig_.bones.size();
    rig_.bones = s.bones;
    rig_.skins = s.skins;
    pose_ = s.pose;
    accum_ = s.accum;
    origin_ = s.origin;
    if (s.reshaped) setShape(s.shape);
    else if (reshaped_) setShape(loadedShape_);
    if (s.reshaped != reshaped_) mirrorReady_ = false;
    reshaped_ = s.reshaped;
    if (bonesChanged) {
        boneSel_.assign(rig_.bones.size(), 0);
        if (active_ >= int(rig_.bones.size())) active_ = -1;
    }
    pose_.resize(rig_.bones.size());
    posedDirty_ = true;
}

void RigEditor::pushUndo() {
    undo_.push_back(snapshot());
    if (undo_.size() > 64) undo_.erase(undo_.begin());
    redo_.clear();
}

void RigEditor::undo() {
    if (undo_.empty()) return;
    redo_.push_back(snapshot());
    restore(undo_.back());
    undo_.pop_back();
    dirty_ = true;
}

void RigEditor::redo() {
    if (redo_.empty()) return;
    undo_.push_back(snapshot());
    restore(redo_.back());
    redo_.pop_back();
    dirty_ = true;
}

void RigEditor::changed(bool rig) {
    if (rig) dirty_ = true;
    posedDirty_ = true;
}

// ---- preview ----------------------------------------------------------------------------------------

void RigEditor::updatePosed() {
    if (!posedDirty_) return;
    posedDirty_ = false;
    rigedit::evaluate(rig_.bones, pose_, world_, skin_);
    posed_.resize(model_.meshes.size());
    normals_.resize(model_.meshes.size());
    for (size_t mi = 0; mi < model_.meshes.size(); ++mi) {
        const model::Mesh& me = model_.meshes[mi];
        if (mi < shown_.size() && !shown_[mi]) {
            posed_[mi].clear();
            normals_[mi].clear();
            continue;
        }
        if (mi < rig_.skins.size() && !rig_.skins[mi].empty()) {
            posed_[mi] = rigedit::skinMesh(me, rig_.skins[mi], skin_);
        } else {
            posed_[mi].resize(me.vertices.size() * 3);
            for (size_t v = 0; v < me.vertices.size(); ++v)
                for (int k = 0; k < 3; ++k) posed_[mi][v * 3 + size_t(k)] = me.vertices[v].pos[k];
        }
        auto& n = normals_[mi];
        n.assign(posed_[mi].size(), 0);
        const auto& p = posed_[mi];
        for (size_t i = 0; i + 2 < me.indices.size(); i += 3) {
            const size_t a = me.indices[i], b = me.indices[i + 1], c = me.indices[i + 2];
            if (std::max({a, b, c}) * 3 + 2 >= p.size()) continue;
            const V3 pa{p[a * 3], p[a * 3 + 1], p[a * 3 + 2]}, pb{p[b * 3], p[b * 3 + 1], p[b * 3 + 2]}, pc{p[c * 3], p[c * 3 + 1], p[c * 3 + 2]};
            const V3 f = cross(sub(pb, pa), sub(pc, pa));
            for (size_t v : {a, b, c})
                for (int k = 0; k < 3; ++k) n[v * 3 + size_t(k)] += f[size_t(k)];
        }
    }
}

std::vector<float> RigEditor::gameRows() {
    updatePosed();
    const size_t n = loadedBones_.size();
    std::vector<int> now(n, -1);  // the game's bone -> the bone it is now
    for (size_t i = 0; i < origin_.size(); ++i)
        if (origin_[i] >= 0 && size_t(origin_[i]) < n) now[size_t(origin_[i])] = int(i);
    std::vector<float> rows(n * 12);
    for (size_t g = 0; g < n; ++g) {
        // A deleted bone's vertices go with its nearest kept ancestor, as deleting gave them its weights.
        int i = now[g];
        for (int p = loadedBones_[g].parent; i < 0 && p >= 0 && size_t(p) < n; p = loadedBones_[size_t(p)].parent) i = now[size_t(p)];
        const rigedit::Mat4 m = i >= 0 && size_t(i) < skin_.size() && size_t(i) < accum_.size() ? mul(skin_[size_t(i)], accum_[size_t(i)]) : kIdentity;
        for (size_t r = 0; r < 3; ++r)
            for (size_t c = 0; c < 4; ++c) rows[g * 12 + r * 4 + c] = m[c * 4 + r];
    }
    return rows;
}

std::array<float, 3> RigEditor::jointAt(size_t bone) const {
    if (posing() && bone < world_.size()) return {world_[bone][12], world_[bone][13], world_[bone][14]};
    return bone < rig_.bones.size() ? rig_.bones[bone].pos : V3{};
}

void RigEditor::project(const ViewCamera& cam) {
    proj_.resize(model_.meshes.size());
    static const std::vector<float> none;
    for (size_t mi = 0; mi < model_.meshes.size(); ++mi) {
        Projected& pr = proj_[mi];
        const std::vector<float>& p = mi < posed_.size() ? posed_[mi] : none;
        const size_t n = p.size() / 3;
        pr.screen.resize(n);
        pr.depth.resize(n);
        pr.valid.assign(n, 0);
        for (size_t v = 0; v < n; ++v) pr.valid[v] = cam.toScreen(&p[v * 3], &pr.screen[v], &pr.depth[v]);
    }
}

// The nearest surface's distance in cells of a few pixels, for hiding what's behind it.
void RigEditor::buildDepth(const ViewCamera& cam) {
    depthCell_ = 4;
    depthAt_ = cam.vpPos;
    depthW_ = std::max(1, int(cam.vpSize.x / depthCell_) + 1);
    depthH_ = std::max(1, int(cam.vpSize.y / depthCell_) + 1);
    depthBuf_.assign(size_t(depthW_) * size_t(depthH_), 1e30f);
    for (size_t mi = 0; mi < model_.meshes.size() && mi < proj_.size(); ++mi) {
        const Projected& pr = proj_[mi];
        if (pr.screen.empty()) continue;
        const auto& idx = model_.meshes[mi].indices;
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            const size_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
            if (std::max({a, b, c}) >= pr.screen.size() || !pr.valid[a] || !pr.valid[b] || !pr.valid[c]) continue;
            const ImVec2 p[3] = {pr.screen[a], pr.screen[b], pr.screen[c]};
            const float z[3] = {pr.depth[a], pr.depth[b], pr.depth[c]};
            const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
            if (std::fabs(area) < 1e-6f) continue;
            const int x0 = std::max(0, int((std::min({p[0].x, p[1].x, p[2].x}) - depthAt_.x) / depthCell_));
            const int x1 = std::min(depthW_ - 1, int((std::max({p[0].x, p[1].x, p[2].x}) - depthAt_.x) / depthCell_));
            const int y0 = std::max(0, int((std::min({p[0].y, p[1].y, p[2].y}) - depthAt_.y) / depthCell_));
            const int y1 = std::min(depthH_ - 1, int((std::max({p[0].y, p[1].y, p[2].y}) - depthAt_.y) / depthCell_));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float px = depthAt_.x + (float(x) + 0.5f) * depthCell_, py = depthAt_.y + (float(y) + 0.5f) * depthCell_;
                    const float w0 = ((p[1].x - px) * (p[2].y - py) - (p[2].x - px) * (p[1].y - py)) / area;
                    const float w1 = ((p[2].x - px) * (p[0].y - py) - (p[0].x - px) * (p[2].y - py)) / area;
                    const float w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    float& d = depthBuf_[size_t(y) * size_t(depthW_) + size_t(x)];
                    d = std::min(d, w0 * z[0] + w1 * z[1] + w2 * z[2]);
                }
        }
    }
}

bool RigEditor::visible(size_t mesh, size_t v, const ViewCamera& cam) const {
    if (mesh >= proj_.size() || v >= proj_[mesh].valid.size() || !proj_[mesh].valid[v]) return false;
    if (frontOnly_ && mesh < normals_.size() && v * 3 + 2 < normals_[mesh].size()) {
        const V3 n{normals_[mesh][v * 3], normals_[mesh][v * 3 + 1], normals_[mesh][v * 3 + 2]};
        const V3 p{posed_[mesh][v * 3], posed_[mesh][v * 3 + 1], posed_[mesh][v * 3 + 2]};
        const V3 eye{cam.eye[0], cam.eye[1] - cam.floor, cam.eye[2]};
        if (dot(n, sub(eye, p)) < 0) return false;
    }
    if (occlude_ && !depthBuf_.empty()) {
        const ImVec2 s = proj_[mesh].screen[v];
        const int x = int((s.x - depthAt_.x) / depthCell_), y = int((s.y - depthAt_.y) / depthCell_);
        if (x >= 0 && y >= 0 && x < depthW_ && y < depthH_) {
            float nearest = 1e30f;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (x + dx >= 0 && y + dy >= 0 && x + dx < depthW_ && y + dy < depthH_)
                        nearest = std::min(nearest, depthBuf_[size_t(y + dy) * size_t(depthW_) + size_t(x + dx)]);
            if (proj_[mesh].depth[v] > nearest + size_ * 0.02f) return false;
        }
    }
    return true;
}

void RigEditor::drawMesh(ImDrawList* dl, const ViewCamera& cam, bool heatMap) {
    project(cam);
    struct Tri {
        float depth;
        ImVec2 p[3];
        ImU32 c[3];
    };
    std::vector<Tri> tris;
    const V3 eye{cam.eye[0], cam.eye[1] - cam.floor, cam.eye[2]};
    for (size_t mi = 0; mi < model_.meshes.size(); ++mi) {
        const Projected& pr = proj_[mi];
        if (pr.screen.empty()) continue;
        const auto& p = posed_[mi];
        const model::Mesh& me = model_.meshes[mi];
        const bool skinned = mi < rig_.skins.size() && !rig_.skins[mi].empty();
        const auto& idx = me.indices;
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            const size_t v[3] = {idx[i], idx[i + 1], idx[i + 2]};
            if (std::max({v[0], v[1], v[2]}) >= pr.screen.size() || !pr.valid[v[0]] || !pr.valid[v[1]] || !pr.valid[v[2]]) continue;
            const V3 a{p[v[0] * 3], p[v[0] * 3 + 1], p[v[0] * 3 + 2]}, b{p[v[1] * 3], p[v[1] * 3 + 1], p[v[1] * 3 + 2]},
                c{p[v[2] * 3], p[v[2] * 3 + 1], p[v[2] * 3 + 2]};
            const V3 n = normalized(cross(sub(b, a), sub(c, a)));
            const float shade = 0.35f + 0.65f * std::fabs(dot(n, normalized(sub(eye, a))));
            Tri t;
            t.depth = (pr.depth[v[0]] + pr.depth[v[1]] + pr.depth[v[2]]) / 3;
            for (int k = 0; k < 3; ++k) {
                t.p[k] = pr.screen[v[k]];
                if (heatMap && skinned && active_ >= 0) t.c[k] = heat(rigedit::weightOf(rig_.skins[mi][v[k]], uint16_t(active_)), shade);
                else if (heatMap) t.c[k] = heat(0, shade);
                else {
                    const int g = int(shade * 200);
                    t.c[k] = IM_COL32(g, g, std::min(255, g + 12), 255);
                }
            }
            tris.push_back(t);
        }
    }
    std::sort(tris.begin(), tris.end(), [](const Tri& a, const Tri& b) { return a.depth > b.depth; });
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    for (size_t i = 0; i < tris.size();) {
        const size_t n = std::min<size_t>(tris.size() - i, 4000);
        dl->PrimReserve(int(n * 3), int(n * 3));
        for (size_t k = i; k < i + n; ++k)
            for (int j = 0; j < 3; ++j) dl->PrimVtx(tris[k].p[j], uv, tris[k].c[j]);
        i += n;
    }
}

void RigEditor::drawSkeleton(ImDrawList* dl, const ViewCamera& cam) {
    const size_t n = rig_.bones.size();
    for (size_t i = 0; i < n; ++i) {
        const V3 p = jointAt(i);
        ImVec2 s;
        if (!cam.toScreen(p.data(), &s)) continue;
        const bool sel = i < boneSel_.size() && boneSel_[i];
        const int parent = rig_.bones[i].parent;
        if (parent >= 0 && size_t(parent) < n) {
            const V3 q = jointAt(size_t(parent));
            ImVec2 t;
            if (cam.toScreen(q.data(), &t)) {
                const ImU32 col = sel ? IM_COL32(255, 160, 40, 255) : rig_.bones[i].added ? IM_COL32(120, 220, 255, 220) : IM_COL32(200, 200, 200, 200);
                dl->AddLine(t, s, col, sel ? 3.0f : 2.0f);
            }
        }
        const bool act = int(i) == active_;
        dl->AddCircleFilled(s, act ? 5.0f : 4.0f, sel ? IM_COL32(255, 180, 60, 255) : IM_COL32(230, 230, 230, 255));
        if (act) dl->AddCircle(s, 7.5f, IM_COL32(255, 255, 255, 255), 0, 1.5f);
        if (names_ || (act && mode_ == Mode::Weight))
            dl->AddText(ImVec2(s.x + 7, s.y - 8), IM_COL32(255, 230, 160, 255), rig_.bones[i].name.c_str());
    }
}

int RigEditor::jointUnder(const ViewCamera& cam, const ImVec2& p) const {
    int best = -1;
    float bestD = 10;
    for (size_t i = 0; i < rig_.bones.size(); ++i) {
        const V3 j = jointAt(i);
        ImVec2 s;
        if (!cam.toScreen(j.data(), &s)) continue;
        const float d = dist(s, p);
        if (d < bestD) bestD = d, best = int(i);
    }
    return best;
}

int RigEditor::selectedVertices() const {
    int n = 0;
    for (const auto& s : vertSel_)
        for (char c : s) n += c != 0;
    return n;
}

rigedit::Mask RigEditor::mask(bool selectedOnly) const {
    rigedit::Mask m;
    m.meshes.resize(model_.meshes.size());
    for (size_t mi = 0; mi < model_.meshes.size(); ++mi)
        m.meshes[mi] = (mi >= shown_.size() || shown_[mi]) && mi < rig_.skins.size() && !rig_.skins[mi].empty();
    if (selectedOnly) m.vertices = vertSel_;
    return m;
}

// ---- input ------------------------------------------------------------------------------------------

void RigEditor::selectBox(const ViewCamera& cam, const ImVec2& a, const ImVec2& b, int how) {
    const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
    auto inside = [&](const ImVec2& s) { return s.x >= lo.x && s.x <= hi.x && s.y >= lo.y && s.y <= hi.y; };
    auto set = [how](char& c) { c = how == 2 ? 0 : 1; };
    if (mode_ == Mode::Weight) {
        if (how == 0)
            for (auto& s : vertSel_) std::fill(s.begin(), s.end(), 0);
        if (occlude_) buildDepth(cam);
        for (size_t mi = 0; mi < proj_.size(); ++mi)
            for (size_t v = 0; v < proj_[mi].screen.size() && mi < vertSel_.size() && v < vertSel_[mi].size(); ++v)
                if (inside(proj_[mi].screen[v]) && visible(mi, v, cam)) set(vertSel_[mi][v]);
        return;
    }
    if (how == 0) std::fill(boneSel_.begin(), boneSel_.end(), 0);
    for (size_t i = 0; i < rig_.bones.size(); ++i) {
        const V3 j = jointAt(i);
        ImVec2 s;
        if (cam.toScreen(j.data(), &s) && inside(s)) {
            set(boneSel_[i]);
            if (how != 2) active_ = int(i);
        }
    }
}

void RigEditor::clickVertex(const ViewCamera& cam, const ImVec2& p, bool addTo) {
    if (occlude_) buildDepth(cam);
    int bm = -1, bv = -1;
    float best = 12;
    for (size_t mi = 0; mi < proj_.size(); ++mi)
        for (size_t v = 0; v < proj_[mi].screen.size(); ++v) {
            if (!proj_[mi].valid[v]) continue;
            const float d = dist(proj_[mi].screen[v], p);
            if (d < best && visible(mi, v, cam)) best = d, bm = int(mi), bv = int(v);
        }
    if (!addTo)
        for (auto& s : vertSel_) std::fill(s.begin(), s.end(), 0);
    if (bm < 0) return;
    char& c = vertSel_[size_t(bm)][size_t(bv)];
    c = addTo ? !c : 1;
    lastMesh_ = bm;
    lastVertex_ = bv;
}

void RigEditor::keys(const ViewCamera& cam, App& app) {
    const ImGuiIO& io = ImGui::GetIO();
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    if (pressed(ImGuiKey_Tab)) {
        if (io.KeyCtrl) setMode(mode_ == Mode::Pose ? Mode::Object : Mode::Pose);
        else setMode(mode_ == Mode::Edit ? Mode::Object : Mode::Edit);
        return;
    }
    if (mode_ == Mode::Object) return;
    if (io.KeyCtrl && pressed(ImGuiKey_Z)) {
        if (io.KeyShift) redo();
        else undo();
        return;
    }
    if (io.KeyCtrl && pressed(ImGuiKey_Y)) {
        redo();
        return;
    }
    const bool plain = !io.KeyCtrl && !io.KeyAlt;
    if (mode_ == Mode::Edit || mode_ == Mode::Pose) {
        if (plain && !io.KeyShift && pressed(ImGuiKey_G)) beginTransform('G', cam, app);
        else if (plain && !io.KeyShift && pressed(ImGuiKey_R)) beginTransform('R', cam, app);
        else if (plain && !io.KeyShift && pressed(ImGuiKey_S)) beginTransform('S', cam, app);
        else if (plain && !io.KeyShift && pressed(ImGuiKey_A)) std::fill(boneSel_.begin(), boneSel_.end(), 1);
        else if (io.KeyAlt && pressed(ImGuiKey_A)) std::fill(boneSel_.begin(), boneSel_.end(), 0);
    }
    if (mode_ == Mode::Edit) {
        if (plain && io.KeyShift && pressed(ImGuiKey_A)) addBone(false, app, &cam);
        else if (plain && !io.KeyShift && pressed(ImGuiKey_E)) addBone(true, app, &cam);
        else if (plain && (pressed(ImGuiKey_X) || pressed(ImGuiKey_Delete))) deleteBones(app);
        else if (io.KeyCtrl && pressed(ImGuiKey_P)) parentTo(app, false);
        else if (io.KeyAlt && pressed(ImGuiKey_P)) parentTo(app, true);
        else if (pressed(ImGuiKey_F2) && active_ >= 0) {
            rename_ = rig_.bones[size_t(active_)].name;
            ImGui::OpenPopup("Rename bone");
        }
    }
    if (mode_ == Mode::Pose && io.KeyAlt) {
        const bool g = pressed(ImGuiKey_G), r = pressed(ImGuiKey_R), s = pressed(ImGuiKey_S);
        if (g || r || s) {
            pushUndo();
            for (size_t i = 0; i < pose_.size(); ++i) {
                if (!boneSel_[i]) continue;
                if (g) pose_[i].offset = {0, 0, 0};
                if (r) pose_[i].rotation = {0, 0, 0, 1};
                if (s) pose_[i].scale = {1, 1, 1};
            }
            changed(false);
        }
    }
    if (mode_ == Mode::Weight) {
        if (pressed(ImGuiKey_LeftBracket)) {
            if (io.KeyShift) strength_ = std::max(0.01f, strength_ - 0.05f);
            else radius_ = std::max(4.0f, radius_ / 1.15f);
        }
        if (pressed(ImGuiKey_RightBracket)) {
            if (io.KeyShift) strength_ = std::min(1.0f, strength_ + 0.05f);
            else radius_ = std::min(500.0f, radius_ * 1.15f);
        }
        if (tool_ == 1) {
            if (plain && pressed(ImGuiKey_A))
                for (auto& s : vertSel_) std::fill(s.begin(), s.end(), 1);
            if (io.KeyAlt && pressed(ImGuiKey_A))
                for (auto& s : vertSel_) std::fill(s.begin(), s.end(), 0);
            if (io.KeyCtrl && pressed(ImGuiKey_I))
                for (auto& s : vertSel_)
                    for (char& c : s) c = !c;
        }
    }
}

// ---- transforms ---------------------------------------------------------------------------------

std::vector<size_t> RigEditor::selectedBones() const {
    std::vector<size_t> sel;
    for (size_t i = 0; i < boneSel_.size(); ++i)
        if (boneSel_[i]) sel.push_back(i);
    return sel;
}

// What R and S turn and scale about: the selection's median, or with joints moving alone the Pivot setting's.
std::array<float, 3> RigEditor::transformPivot(const std::vector<size_t>& sel, char kind) const {
    V3 pivot{};
    for (size_t i : sel) pivot = add(pivot, jointAt(i));
    if (!sel.empty()) pivot = scale(pivot, 1.0f / float(sel.size()));
    if (mode_ == Mode::Edit && !meshFollows_ && kind != 'G') {
        if (pivot_ == 0 && sel.size() == 1 && rig_.bones[sel[0]].parent >= 0) pivot = rig_.bones[size_t(rig_.bones[sel[0]].parent)].pos;
        else if (pivot_ == 2 && active_ >= 0) pivot = rig_.bones[size_t(active_)].pos;
    }
    return pivot;
}

// ---- the rotate gizmo -------------------------------------------------------------------------------

namespace {
constexpr float kGizmoRadius = 70, kViewRing = 84, kGizmoGrab = 7;  // pixels
constexpr float kTrackballSpeed = 0.01f;                            // radians a pixel
constexpr int kRingSegments = 64;

// An axis ring's points on the screen, each with whether it's on the camera's side of the sphere.
struct Ring {
    ImVec2 p[kRingSegments + 1];
    bool front[kRingSegments + 1];
    bool ok[kRingSegments + 1];
};
Ring ringOf(int axis, const V3& centre, float radius, const ViewCamera& cam) {
    Ring r{};
    const V3 e1 = axis == 0 ? V3{0, 1, 0} : axis == 1 ? V3{0, 0, 1} : V3{1, 0, 0};
    const V3 e2 = axis == 0 ? V3{0, 0, 1} : axis == 1 ? V3{1, 0, 0} : V3{0, 1, 0};
    const V3 eye{cam.eye[0], cam.eye[1] - cam.floor, cam.eye[2]};
    const V3 toEye = normalized(sub(eye, centre));
    for (int i = 0; i <= kRingSegments; ++i) {
        const float t = 2 * kPi * float(i) / float(kRingSegments);
        const V3 d = add(scale(e1, std::cos(t)), scale(e2, std::sin(t)));
        const V3 q = add(centre, scale(d, radius));
        r.ok[i] = cam.toScreen(q.data(), &r.p[i]);
        r.front[i] = dot(d, toEye) >= -0.02f;
    }
    return r;
}
float segmentDistance(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    const float t = l2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0f, 1.0f) : 0;
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}
} // namespace

bool RigEditor::gizmoShown() const {
    return gizmo_ && (mode_ == Mode::Edit || mode_ == Mode::Pose) && std::find(boneSel_.begin(), boneSel_.end(), 1) != boneSel_.end() &&
           (!x_.kind || x_.kind == 'R' || x_.kind == 'T');
}

int RigEditor::gizmoUnder(const ViewCamera& cam, const ImVec2& p) const {
    const V3 centre = transformPivot(selectedBones(), 'R');
    ImVec2 c;
    float depth;
    if (!cam.toScreen(centre.data(), &c, &depth)) return -1;
    const float radius = kGizmoRadius * cam.pixel(depth);
    int best = -1;
    float bestDist = kGizmoGrab;
    for (int a = 0; a < 3; ++a) {
        const Ring r = ringOf(a, centre, radius, cam);
        for (int i = 0; i < kRingSegments; ++i) {
            if (!r.ok[i] || !r.ok[i + 1] || !r.front[i] || !r.front[i + 1]) continue;
            const float d = segmentDistance(p, r.p[i], r.p[i + 1]);
            if (d < bestDist) bestDist = d, best = a;
        }
    }
    const float fromCentre = dist(p, c);
    if (std::fabs(fromCentre - kViewRing) < bestDist) best = 3;
    if (best < 0 && fromCentre < kGizmoRadius) best = 4;
    return best;
}

void RigEditor::drawGizmo(ImDrawList* dl, const ViewCamera& cam, int hot) const {
    const V3 centre = x_.kind ? x_.pivot : transformPivot(selectedBones(), 'R');
    ImVec2 c;
    float depth;
    if (!cam.toScreen(centre.data(), &c, &depth)) return;
    const float radius = kGizmoRadius * cam.pixel(depth);
    if (hot == 4) dl->AddCircleFilled(c, kGizmoRadius, IM_COL32(255, 255, 255, 28), 48);
    dl->AddCircle(c, kGizmoRadius, IM_COL32(255, 255, 255, 60), 48, 1.0f);
    dl->AddCircle(c, kViewRing, hot == 3 ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 150), 48, hot == 3 ? 3.0f : 2.0f);
    for (int a = 0; a < 3; ++a) {
        const Ring r = ringOf(a, centre, radius, cam);
        const ImU32 col = hot == a ? IM_COL32(255, 255, 255, 255) : kAxisColours[a];
        for (int i = 0; i < kRingSegments; ++i)
            if (r.ok[i] && r.ok[i + 1] && r.front[i] && r.front[i + 1]) dl->AddLine(r.p[i], r.p[i + 1], col, hot == a ? 3.5f : 2.5f);
    }
}

void RigEditor::beginTransform(char kind, const ViewCamera& cam, App& app) {
    const std::vector<size_t> sel = selectedBones();
    if (sel.empty()) {
        app.status("Select a joint first (click it, or A for all)");
        return;
    }
    if (posing()) updatePosed();
    // Joints alone (Edit mode without the mesh following), or each bone turning about its own joint, the mesh with it.
    const bool joints = mode_ == Mode::Edit && !meshFollows_;
    std::set<size_t> moved(sel.begin(), sel.end());
    if (joints && childrenFollow_)
        for (size_t i = 0; i < rig_.bones.size(); ++i)
            for (int p = rig_.bones[i].parent; p >= 0; p = rig_.bones[size_t(p)].parent)
                if (moved.count(size_t(p))) {
                    moved.insert(i);
                    break;
                }
    x_ = {};
    x_.pivot = transformPivot(sel, kind);
    pushUndo();
    x_.before = snapshot();
    x_.kind = kind;
    x_.bones.assign(moved.begin(), moved.end());
    x_.start = ImGui::GetIO().MousePos;
    if (!cam.toScreen(x_.pivot.data(), &x_.pivotScreen)) x_.pivotScreen = x_.start;
}

void RigEditor::updateTransform(const ViewCamera& cam) {
    const ImGuiIO& io = ImGui::GetIO();
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    for (int a = 0; a < 3; ++a)
        if (pressed(ImGuiKey(ImGuiKey_X + a))) x_.axis = x_.axis == a ? -1 : a;
    if (x_.kind == 'T' && x_.axis >= 0) x_.kind = 'R';  // an axis key turns the trackball into a turn about it
    for (int d = 0; d <= 9; ++d)
        if (pressed(ImGuiKey(ImGuiKey_0 + d)) || pressed(ImGuiKey(ImGuiKey_Keypad0 + d))) x_.typed.push_back(char('0' + d));
    if ((pressed(ImGuiKey_Period) || pressed(ImGuiKey_KeypadDecimal)) && x_.typed.find('.') == std::string::npos) x_.typed.push_back('.');
    if (pressed(ImGuiKey_Minus) || pressed(ImGuiKey_KeypadSubtract)) {
        if (!x_.typed.empty() && x_.typed[0] == '-') x_.typed.erase(0, 1);
        else x_.typed.insert(0, "-");
    }
    if (pressed(ImGuiKey_Backspace) && !x_.typed.empty()) x_.typed.pop_back();
    const bool typed = !x_.typed.empty() && x_.typed != "-" && x_.typed != ".";
    const float value = typed ? float(std::atof(x_.typed.c_str())) : 0;
    const ImVec2 m = io.MousePos;
    const V3 axisVec = x_.axis >= 0 ? V3{float(x_.axis == 0), float(x_.axis == 1), float(x_.axis == 2)} : V3{};
    const V3 camF{cam.f[0], cam.f[1], cam.f[2]}, camS{cam.s[0], cam.s[1], cam.s[2]}, camU{cam.u[0], cam.u[1], cam.u[2]};

    // What the mouse (or the typed value) asks for.
    V3 delta{};
    float angle = 0, factor = 1;
    V3 rotAxis = camF;
    if (x_.kind == 'G') {
        if (typed) delta = scale(x_.axis >= 0 ? axisVec : V3{1, 0, 0}, value);
        else {
            const V3 rel{x_.pivot[0] - cam.eye[0], x_.pivot[1] + cam.floor - cam.eye[1], x_.pivot[2] - cam.eye[2]};
            const float px = cam.pixel(std::max(dot(rel, camF), 1e-3f));
            delta = add(scale(camS, (m.x - x_.start.x) * px), scale(camU, -(m.y - x_.start.y) * px));
            if (x_.axis >= 0) delta = scale(axisVec, dot(delta, axisVec));
        }
    } else if (x_.kind == 'R') {
        const float a0 = std::atan2(x_.start.y - x_.pivotScreen.y, x_.start.x - x_.pivotScreen.x);
        const float a1 = std::atan2(m.y - x_.pivotScreen.y, m.x - x_.pivotScreen.x);
        angle = typed ? value * kPi / 180 : a1 - a0;
        if (x_.axis >= 0) {
            rotAxis = axisVec;
            if (!typed && dot(axisVec, camF) < 0) angle = -angle;
        }
    } else if (x_.kind == 'T') {
        // The sphere's front point follows the mouse: the turn is about the axis across the drag, in the view's plane.
        const V3 w = add(scale(camS, m.x - x_.start.x), scale(camU, -(m.y - x_.start.y)));
        if (length(w) > 1e-6f) {
            rotAxis = normalized(cross(scale(camF, -1), w));
            angle = dist(m, x_.start) * kTrackballSpeed;
        }
    } else {
        const float d0 = std::max(dist(x_.start, x_.pivotScreen), 1.0f);
        factor = typed ? value : dist(m, x_.pivotScreen) / d0;
    }

    if (mode_ == Mode::Edit && !meshFollows_) {
        for (size_t i : x_.bones) {
            const V3 before = x_.before.bones[i].pos;
            V3& p = rig_.bones[i].pos;
            if (x_.kind == 'G') p = add(before, delta);
            else if (x_.kind == 'R' || x_.kind == 'T') p = add(x_.pivot, rotate(rotAxis, angle, sub(before, x_.pivot)));
            else {
                V3 v = sub(before, x_.pivot);
                if (x_.axis >= 0) v[size_t(x_.axis)] *= factor;
                else v = scale(v, factor);
                p = add(x_.pivot, v);
            }
        }
    } else {
        std::vector<rigedit::Mat4> world, skin;
        rigedit::evaluate(x_.before.bones, x_.before.pose, world, skin);
        pose_ = x_.before.pose;
        for (size_t i : x_.bones) {
            const int parent = rig_.bones[i].parent;
            const std::array<V3, 3> rp = parent >= 0 && size_t(parent) < world.size() ? rotationOf(world[size_t(parent)])
                                                                                     : std::array<V3, 3>{V3{1, 0, 0}, V3{0, 1, 0}, V3{0, 0, 1}};
            rigedit::PoseKey& k = pose_[i];
            if (x_.kind == 'G') {
                const V3 local = intoFrame(rp, delta);
                for (int c = 0; c < 3; ++c) k.offset[size_t(c)] += local[size_t(c)];
            } else if (x_.kind == 'R' || x_.kind == 'T') {
                k.rotation = qmul(axisAngle(intoFrame(rp, rotAxis), angle), k.rotation);
            } else if (x_.axis >= 0) {
                const V3 own = intoFrame(rotationOf(world[i]), axisVec);
                int c = 0;
                for (int q = 1; q < 3; ++q)
                    if (std::fabs(own[size_t(q)]) > std::fabs(own[size_t(c)])) c = q;
                k.scale[size_t(c)] *= factor;
            } else {
                for (float& s : k.scale) s *= factor;
            }
        }
    }
    posedDirty_ = true;
}

void RigEditor::endTransform(bool confirm) {
    if (!x_.kind) return;
    x_.kind = 0;
    if (!confirm) {
        restore(x_.before);
        if (!undo_.empty()) undo_.pop_back();
    } else if (mode_ == Mode::Edit && meshFollows_) {
        bake();
    } else {
        changed(mode_ == Mode::Edit);
    }
}

// ---- edit mode operations ---------------------------------------------------------------------------

void RigEditor::addBone(bool extrude, App& app, const ViewCamera* cam) {
    if (rig_.bones.size() >= 0xFFFE) return;
    std::vector<size_t> from;
    if (extrude) {
        for (size_t i = 0; i < boneSel_.size(); ++i)
            if (boneSel_[i]) from.push_back(i);
        if (from.empty()) {
            app.status("Select the joints to extrude from");
            return;
        }
    }
    pushUndo();
    std::fill(boneSel_.begin(), boneSel_.end(), 0);
    auto make = [&](int parent, const V3& pos) {
        rig_.bones.push_back({rigedit::uniqueName(rig_, parent >= 0 ? rig_.bones[size_t(parent)].name + "_child" : "Bone"), parent, pos, true});
        boneSel_.push_back(1);
        pose_.push_back({});
        accum_.push_back(kIdentity);
        origin_.push_back(-1);
        active_ = int(rig_.bones.size() - 1);
    };
    if (extrude) {
        for (size_t i : from) make(int(i), rig_.bones[i].pos);
    } else {
        const int parent = active_;
        const V3 base = parent >= 0 ? rig_.bones[size_t(parent)].pos : V3{0, 0, 0};
        make(parent, add(base, {0, size_ * 0.08f, 0}));
    }
    changed(true);
    // The new joints follow the mouse at once, as Blender's extrude does (Esc leaves them where they were made).
    if (cam) beginTransform('G', *cam, app);
}

void RigEditor::deleteBones(App& app) {
    const size_t n = rig_.bones.size();
    std::vector<char> gone(n, 0);
    bool game = false, any = false;
    for (size_t i = 0; i < n; ++i)
        if (boneSel_[i]) {
            if (rig_.bones[i].added) gone[i] = 1, any = true;
            else game = true;
        }
    if (game) app.status("The model's own bones stay (its animations drive them by number); only added bones can be deleted");
    if (!any) return;
    pushUndo();
    auto keptAncestor = [&](size_t i) {
        int p = int(i);
        while (p >= 0 && gone[size_t(p)]) p = rig_.bones[size_t(p)].parent;
        return p;
    };
    std::vector<int> target(n), index(n, -1);
    for (size_t i = 0; i < n; ++i) target[i] = keptAncestor(i);
    std::vector<rigedit::Bone> bones;
    std::vector<rigedit::PoseKey> pose;
    std::vector<rigedit::Mat4> accum;
    std::vector<int> origin;
    for (size_t i = 0; i < n; ++i)
        if (!gone[i]) {
            index[i] = int(bones.size());
            bones.push_back(rig_.bones[i]);
            pose.push_back(pose_[i]);
            accum.push_back(accum_[i]);
            origin.push_back(origin_[i]);
        }
    for (auto& b : bones) {
        const int p = b.parent >= 0 ? target[size_t(b.parent)] : -1;
        b.parent = p >= 0 ? index[size_t(p)] : -1;
    }
    for (auto& mesh : rig_.skins)
        for (auto& s : mesh) {
            std::map<uint16_t, float> w;
            for (const auto& in : s) {
                if (in.weight <= 0) continue;
                const int t = in.bone < n ? target[in.bone] : 0;
                w[uint16_t(t >= 0 ? index[size_t(t)] : 0)] += in.weight;
            }
            if (!w.empty()) s = rigedit::fromWeights(w);
        }
    rig_.bones = std::move(bones);
    pose_ = std::move(pose);
    accum_ = std::move(accum);
    origin_ = std::move(origin);
    boneSel_.assign(rig_.bones.size(), 0);
    active_ = -1;
    changed(true);
}

void RigEditor::parentTo(App& app, bool clear) {
    if (!clear && active_ < 0) {
        app.status("Pick the new parent last (it's the active joint)");
        return;
    }
    bool game = false, late = false, any = false;
    pushUndo();
    for (size_t i = 0; i < rig_.bones.size(); ++i) {
        if (!boneSel_[i] || (int(i) == active_ && !clear)) continue;
        if (!rig_.bones[i].added) {
            game = true;
            continue;
        }
        if (clear) {
            rig_.bones[i].parent = -1;
            any = true;
        } else if (active_ < int(i)) {
            rig_.bones[i].parent = active_;
            any = true;
        } else {
            late = true;
        }
    }
    if (game) app.status("The model's own bones keep their parents (the game's animations need its hierarchy)");
    if (late) app.status("A bone's parent has to come before it in the list: pick an earlier bone as the parent");
    if (any) changed(true);
    else if (!undo_.empty()) undo_.pop_back();
}

// ---- weight painting ----------------------------------------------------------------------------

void RigEditor::buildMirror() {
    mirrorPlace_.assign(adj_.members.size(), -1);
    const float eps = size_ * 1e-3f;
    auto posOf = [&](uint32_t place) {
        const auto [mi, v] = adj_.members[place].front();
        const float* p = model_.meshes[mi].vertices[v].pos;
        return V3{p[0], p[1], p[2]};
    };
    auto key = [](int64_t x, int64_t y, int64_t z) {
        return uint64_t(x) * 73856093ull ^ uint64_t(y) * 19349663ull ^ uint64_t(z) * 83492791ull;
    };
    std::unordered_multimap<uint64_t, uint32_t> grid;
    for (uint32_t p = 0; p < adj_.members.size(); ++p) {
        const V3 q = posOf(p);
        grid.emplace(key(std::llround(q[0] / eps), std::llround(q[1] / eps), std::llround(q[2] / eps)), p);
    }
    for (uint32_t p = 0; p < adj_.members.size(); ++p) {
        const V3 q = posOf(p), m{-q[0], q[1], q[2]};
        float best = eps * eps * 4;
        const int64_t cx = std::llround(m[0] / eps), cy = std::llround(m[1] / eps), cz = std::llround(m[2] / eps);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    auto range = grid.equal_range(key(cx + dx, cy + dy, cz + dz));
                    for (auto it = range.first; it != range.second; ++it) {
                        const V3 r = sub(posOf(it->second), m);
                        const float d = dot(r, r);
                        if (d <= best) best = d, mirrorPlace_[p] = int(it->second);
                    }
                }
    }
    mirrorReady_ = true;
}

void RigEditor::dab(const ViewCamera& cam, const ImVec2& at) {
    if (active_ < 0 || size_t(active_) >= rig_.bones.size()) return;
    if (mirrorPaint_ && !mirrorReady_) buildMirror();
    const uint16_t bone = uint16_t(active_);
    const int twin = rigedit::mirrorBone(rig_, size_t(active_));
    std::set<uint32_t> done;
    auto applyAt = [&](uint32_t place, uint16_t b, float f) {
        if (!done.insert(place * 2 + (b != bone)).second) return;
        const auto [mi, v] = adj_.members[place].front();
        if (mi >= rig_.skins.size() || rig_.skins[mi].empty()) return;
        float w = rigedit::weightOf(rig_.skins[mi][v], b);
        switch (brush_) {
        case 0: w += value_ * f; break;
        case 1: w -= value_ * f; break;
        case 2: w += (value_ - w) * f; break;
        default: {
            float avg = 0;
            int count = 0;
            for (uint32_t q : adj_.neighbours[place]) {
                const auto [qm, qv] = adj_.members[q].front();
                if (qm >= rig_.skins.size() || rig_.skins[qm].empty()) continue;
                avg += rigedit::weightOf(rig_.skins[qm][qv], b);
                ++count;
            }
            if (count) w += (avg / float(count) - w) * f;
        }
        }
        rigedit::VertexSkin s = rig_.skins[mi][v];
        rigedit::setWeight(s, b, w, autoNormalize_);
        for (auto [m2, v2] : adj_.members[place])
            if (m2 < rig_.skins.size() && !rig_.skins[m2].empty()) {
                rig_.skins[m2][v2] = s;
                rigedit::syncAliases(rig_, m2, v2);
            }
    };
    for (size_t mi = 0; mi < proj_.size() && mi < adj_.place.size(); ++mi) {
        if (mi >= rig_.skins.size() || rig_.skins[mi].empty()) continue;
        for (size_t v = 0; v < proj_[mi].screen.size(); ++v) {
            if (!proj_[mi].valid[v]) continue;
            const float d = dist(proj_[mi].screen[v], at);
            if (d > radius_ || !visible(mi, v, cam)) continue;
            const float t = d / radius_;
            const float fall = falloff_ == 0 ? (1 - t * t) * (1 - t * t) : falloff_ == 1 ? 1 - t : 1;
            const float f = std::clamp(strength_ * fall, 0.0f, 1.0f);
            const uint32_t place = adj_.place[mi][v];
            applyAt(place, bone, f);
            if (mirrorPaint_ && place < mirrorPlace_.size() && mirrorPlace_[place] >= 0)
                applyAt(uint32_t(mirrorPlace_[place]), twin >= 0 ? uint16_t(twin) : bone, f);
        }
    }
    dirty_ = true;
    bool posedRest = true;
    for (const auto& k : pose_) posedRest &= k.rotation == Q4{0, 0, 0, 1} && k.offset == V3{} && k.scale == V3{1, 1, 1};
    if (!posedRest) posedDirty_ = true;
}

// ---- the overlay --------------------------------------------------------------------------------

void RigEditor::overlay(ImDrawList* dl, const ViewCamera& cam, bool hovered, App& app) {
    ImGuiIO& io = ImGui::GetIO();
    if (rig_.bones.empty()) return;
    if (mode_ == Mode::Object) {
        if (hovered && !io.WantTextInput) keys(cam, app);
        return;
    }
    if (posing()) updatePosed();
    // Edit and Pose modes bend the game's own textured model (prepare); Weight Paint draws its heat map.
    if (mode_ == Mode::Weight) drawMesh(dl, cam, true);
    drawSkeleton(dl, cam);
    const ImVec2 mouse = io.MousePos;

    // The gizmo's part under the mouse; the trackball gives way to a joint there, so joints inside it can be picked.
    int hot = -1;
    if (x_.kind == 'T') hot = 4;
    else if (x_.kind == 'R') hot = x_.axis >= 0 ? x_.axis : x_.drag ? 3 : -1;
    else if (gizmoShown() && hovered && !boxing_) {
        hot = gizmoUnder(cam, mouse);
        if (hot == 4 && jointUnder(cam, mouse) >= 0) hot = -1;
    }
    if (gizmoShown()) drawGizmo(dl, cam, hot);

    if (x_.kind) {
        updateTransform(cam);
        if (x_.axis >= 0) {
            ImVec2 a, b;
            const float far = size_ * 4;
            V3 lo = x_.pivot, hi = x_.pivot;
            lo[size_t(x_.axis)] -= far, hi[size_t(x_.axis)] += far;
            if (cam.toScreen(lo.data(), &a) && cam.toScreen(hi.data(), &b)) dl->AddLine(a, b, kAxisColours[x_.axis], 1.5f);
        }
        char buf[160];
        const char* what = x_.kind == 'G' ? "Move" : x_.kind == 'R' ? "Rotate" : x_.kind == 'T' ? "Trackball" : "Scale";
        std::snprintf(buf, sizeof(buf), "%s%s%s  %s   X/Y/Z: axis, type a value, %s: apply, Esc or right-click: cancel", what,
                      x_.axis >= 0 ? " along " : "", x_.axis >= 0 ? kAxisNames[x_.axis] : "", x_.typed.c_str(),
                      x_.drag ? "let go" : "Enter or click");
        dl->AddText(ImVec2(cam.vpPos.x + 12, cam.vpPos.y + cam.vpSize.y - 64), IM_COL32(255, 255, 255, 255), buf);
        const bool apply = x_.drag ? ImGui::IsMouseReleased(ImGuiMouseButton_Left) : ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (apply || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
            endTransform(true);
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            endTransform(false);
        return;
    }
    if (hovered && !io.WantTextInput) keys(cam, app);

    // Dragging a ring turns about its axis (the outer one about the view), inside it the trackball turns freely.
    if (hot >= 0 && !x_.kind && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        beginTransform(hot == 4 ? 'T' : 'R', cam, app);
        if (x_.kind) {
            x_.axis = hot < 3 ? hot : -1;
            x_.drag = true;
        }
        return;
    }
    if (mode_ == Mode::Edit || mode_ == Mode::Pose || (mode_ == Mode::Weight && tool_ == 1)) {
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            boxing_ = true;
            boxStart_ = mouse;
        }
        if (boxing_) {
            const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
            if (dist(mouse, boxStart_) > 4) {
                dl->AddRect(boxStart_, mouse, IM_COL32(255, 255, 255, 200));
                dl->AddRectFilled(boxStart_, mouse, IM_COL32(255, 255, 255, 25));
            }
            if (released) {
                boxing_ = false;
                const int how = io.KeyCtrl ? 2 : io.KeyShift ? 1 : 0;
                if (dist(mouse, boxStart_) > 4) {
                    selectBox(cam, boxStart_, mouse, how);
                } else if (mode_ == Mode::Weight) {
                    clickVertex(cam, mouse, io.KeyShift);
                } else {
                    const int j = jointUnder(cam, mouse);
                    if (!io.KeyShift) std::fill(boneSel_.begin(), boneSel_.end(), 0);
                    if (j >= 0) {
                        boneSel_[size_t(j)] = io.KeyShift ? char(!(boneSel_[size_t(j)] && active_ == j)) : 1;
                        active_ = j;
                    }
                }
            } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                boxing_ = false;
            }
        }
    }
    if (mode_ == Mode::Weight && tool_ == 1) {
        for (size_t mi = 0; mi < proj_.size() && mi < vertSel_.size(); ++mi)
            for (size_t v = 0; v < proj_[mi].screen.size() && v < vertSel_[mi].size(); ++v)
                if (vertSel_[mi][v] && proj_[mi].valid[v]) dl->AddRectFilled(ImVec2(proj_[mi].screen[v].x - 1.5f, proj_[mi].screen[v].y - 1.5f),
                                                                           ImVec2(proj_[mi].screen[v].x + 1.5f, proj_[mi].screen[v].y + 1.5f), IM_COL32(255, 150, 30, 255));
    }
    if (mode_ == Mode::Weight && tool_ == 0) {
        if (hovered) dl->AddCircle(mouse, radius_, IM_COL32(255, 255, 255, 200), 0, 1.5f);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (io.KeyCtrl) {
                const int j = jointUnder(cam, mouse);
                if (j >= 0) active_ = j;
            } else if (active_ < 0) {
                app.status("Pick the bone to paint first (Ctrl+click its joint, or in the bone list)");
            } else {
                pushUndo();
                stroking_ = true;
                if (occlude_) buildDepth(cam);
                dab(cam, mouse);
                lastDab_ = mouse;
            }
        }
        if (stroking_) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) stroking_ = false;
            else if (dist(mouse, lastDab_) >= std::max(2.0f, radius_ * 0.2f)) {
                dab(cam, mouse);
                lastDab_ = mouse;
            }
        }
    }

    if (ImGui::BeginPopupModal("Rename bone", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = inputText("Name", rename_, ImGuiInputTextFlags_EnterReturnsTrue);
        if ((enter || ImGui::Button("Rename")) && active_ >= 0) {
            if (!rig_.bones[size_t(active_)].added) app.status("The model's own bones keep their names (the game finds some of them by name)");
            else if (rename_.empty() || rigedit::uniqueName(rig_, rename_) != rename_) app.status("That name is empty or taken");
            else {
                pushUndo();
                rig_.bones[size_t(active_)].name = rename_;
                changed(true);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void RigEditor::prepare(ModelView& view, App& app) {
    const bool editing = mode_ != Mode::Object && !rig_.bones.empty();
    view.setRigInput(editing);
    view.bonesHidden = editing;
    view.setGameHidden(editing && mode_ == Mode::Weight && !showGame_);
    if (editing) {
        const std::vector<float> rows = gameRows();
        view.holdGamePose(&rows);
    } else {
        view.holdGamePose(nullptr);
    }
    view.overlay = [this, &app](ImDrawList* dl, const ViewCamera& cam, bool hovered) { overlay(dl, cam, hovered, app); };
}

// ---- the panel ------------------------------------------------------------------------------------

void RigEditor::drawBoneList() {
    if (!ImGui::TreeNodeEx("Bones", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::BeginChild("##riglist", ImVec2(0, em(200)), ImGuiChildFlags_Borders);
    std::function<void(int, int)> rows = [&](int parent, int depth) {
        for (size_t i = 0; i < rig_.bones.size(); ++i) {
            if (rig_.bones[i].parent != parent) continue;
            ImGui::PushID(int(i));
            ImGui::Indent(float(depth) * em(10));
            const std::string label = rig_.bones[i].name + (rig_.bones[i].added ? "  (added)" : "");
            const bool sel = boneSel_[i] != 0;
            if (ImGui::Selectable(label.c_str(), sel || int(i) == active_)) {
                const ImGuiIO& io = ImGui::GetIO();
                if (!io.KeyCtrl && !io.KeyShift) std::fill(boneSel_.begin(), boneSel_.end(), 0);
                boneSel_[i] = io.KeyCtrl ? char(!sel) : 1;
                active_ = int(i);
            }
            ImGui::Unindent(float(depth) * em(10));
            ImGui::PopID();
            if (depth < 64) rows(int(i), depth + 1);
        }
    };
    rows(-1, 0);
    ImGui::EndChild();
    ImGui::TreePop();
}

void RigEditor::drawEditPanel(App& app) {
    if (ImGui::Checkbox("Mesh follows the bones", &meshFollows_)) posedDirty_ = true;
    helpMarker("On: G, R and S bend the model with the bones (each bone turning about its own joint, the bones under it along), "
               "and that becomes its new shape: raised arms stay raised, and saving writes the new shape into the model.\n"
               "Off: the joints move alone and the model keeps its shape, for fitting the rig to a new or replacement model.");
    ImGui::BeginDisabled(meshFollows_);
    ImGui::Checkbox("Children follow", &childrenFollow_);
    helpMarker("Moving, turning or scaling a joint takes the joints under it along, as moving a bone does in Blender.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em(150));
    const char* pivots[] = {"Bone's head", "Median point", "Active joint"};
    ImGui::Combo("Pivot", &pivot_, pivots, IM_ARRAYSIZE(pivots));
    helpMarker("What R and S turn and scale around. Bone's head: one joint turns around its parent's joint, so the bone between them swings.");
    ImGui::EndDisabled();
    if (reshaped_) ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "The model's shape was bent with the bones (saved with the rig)");
    ImGui::Checkbox("Bone names", &names_);
    ImGui::SameLine();
    ImGui::Checkbox("Rotate gizmo", &gizmo_);
    helpMarker("Blender's rotate sphere around the selected joints: drag a coloured ring to turn about X (red), Y (green) or Z (blue), "
               "the white outer ring to turn about the view, or inside the sphere to turn freely. Esc or right-click cancels.");
    if (active_ >= 0 && size_t(active_) < rig_.bones.size()) {
        rigedit::Bone& b = rig_.bones[size_t(active_)];
        ImGui::SeparatorText(b.added ? "Active bone (added)" : "Active bone");
        if (b.added) {
            std::string name = b.name;
            if (inputText("Name", name, ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (!name.empty() && rigedit::uniqueName(rig_, name) == name) {
                    pushUndo();
                    b.name = name;
                    changed(true);
                } else {
                    app.status("That name is empty or taken");
                }
            }
            const std::string cur = b.parent >= 0 ? rig_.bones[size_t(b.parent)].name : "(none)";
            if (ImGui::BeginCombo("Parent", cur.c_str())) {
                if (ImGui::Selectable("(none)", b.parent < 0)) {
                    pushUndo();
                    b.parent = -1;
                    changed(true);
                }
                for (int k = 0; k < active_; ++k)
                    if (ImGui::Selectable(rig_.bones[size_t(k)].name.c_str(), b.parent == k)) {
                        pushUndo();
                        b.parent = k;
                        changed(true);
                    }
                ImGui::EndCombo();
            }
        } else {
            ImGui::Text("%s", b.name.c_str());
            ImGui::TextDisabled("parent: %s", b.parent >= 0 ? rig_.bones[size_t(b.parent)].name.c_str() : "(none)");
        }
        V3 p = b.pos;
        if (ImGui::DragFloat3("Joint (alone)", p.data(), size_ * 0.001f, 0, 0, "%.4f")) {
            const V3 delta = sub(p, b.pos);
            for (size_t i = 0; i < rig_.bones.size(); ++i) {
                bool take = int(i) == active_;
                if (childrenFollow_)
                    for (int q = rig_.bones[i].parent; q >= 0 && !take; q = rig_.bones[size_t(q)].parent) take = q == active_;
                if (take) rig_.bones[i].pos = add(rig_.bones[i].pos, delta);
            }
            changed(true);
        }
        if (ImGui::IsItemActivated()) pushUndo();
    }
    ImGui::SeparatorText("Bones");
    if (ImGui::Button("Add bone")) addBone(false, app, nullptr);
    ImGui::SameLine();
    if (ImGui::Button("Extrude")) addBone(true, app, nullptr);
    ImGui::SameLine();
    if (ImGui::Button("Delete")) deleteBones(app);
    ImGui::SameLine();
    if (ImGui::Button("Parent to active")) parentTo(app, false);
    ImGui::SameLine();
    if (ImGui::Button("Clear parent")) parentTo(app, true);
    helpMarker("New bones come after the model's own. The game's animations don't drive them: they follow their parent, "
               "until an animation of your own moves them. Weight vertices to them in Weight Paint.");
    if (ImGui::Button("Snap to selected vertices")) {
        if (active_ < 0 || selectedVertices() == 0) {
            app.status("Select vertices (Weight Paint > Select) and a joint first");
        } else {
            V3 c{};
            int n = 0;
            for (size_t mi = 0; mi < vertSel_.size(); ++mi)
                for (size_t v = 0; v < vertSel_[mi].size(); ++v)
                    if (vertSel_[mi][v]) {
                        const float* q = model_.meshes[mi].vertices[v].pos;
                        c = add(c, {q[0], q[1], q[2]});
                        ++n;
                    }
            pushUndo();
            rig_.bones[size_t(active_)].pos = scale(c, 1.0f / float(n));
            changed(true);
        }
    }
    helpMarker("Puts the active joint in the middle of the vertices selected in Weight Paint: an elbow at the middle of a ring of the arm's vertices.");
    ImGui::SameLine();
    if (ImGui::Button(mirrorPositive_ ? "Mirror joints +X to -X" : "Mirror joints -X to +X")) {
        pushUndo();
        const size_t n = rigedit::mirrorJoints(rig_, mirrorPositive_);
        changed(n > 0);
        app.status(std::to_string(n) + " joint(s) mirrored");
    }
    ImGui::SameLine();
    ImGui::Checkbox("+X side", &mirrorPositive_);
    if (ImGui::Button("Reset joints to the model's")) {
        pushUndo();
        for (size_t i = 0; i < loadedBones_.size() && i < rig_.bones.size(); ++i) rig_.bones[i].pos = loadedBones_[i].pos;
        changed(true);
    }
}

void RigEditor::drawPosePanel(App& app) {
    (void)app;
    ImGui::Checkbox("Bone names", &names_);
    ImGui::SameLine();
    ImGui::Checkbox("Rotate gizmo", &gizmo_);
    helpMarker("Blender's rotate sphere around the selected joints: drag a coloured ring to turn about X (red), Y (green) or Z (blue), "
               "the white outer ring to turn about the view, or inside the sphere to turn freely. Esc or right-click cancels.");
    if (active_ >= 0 && size_t(active_) < pose_.size()) {
        rigedit::PoseKey& k = pose_[size_t(active_)];
        ImGui::SeparatorText(rig_.bones[size_t(active_)].name.c_str());
        V3 e = eulerOf(k.rotation);
        if (ImGui::DragFloat3("Rotation", e.data(), 0.5f, -360, 360, "%.1f deg")) {
            k.rotation = quatOf(e);
            changed(false);
        }
        if (ImGui::IsItemActivated()) pushUndo();
        if (ImGui::DragFloat3("Offset", k.offset.data(), size_ * 0.001f, 0, 0, "%.4f")) changed(false);
        if (ImGui::IsItemActivated()) pushUndo();
        if (ImGui::DragFloat3("Scale", k.scale.data(), 0.005f, 0.01f, 100, "%.3f")) changed(false);
        if (ImGui::IsItemActivated()) pushUndo();
    }
    if (ImGui::Button("Clear selected")) {
        pushUndo();
        for (size_t i = 0; i < pose_.size(); ++i)
            if (boneSel_[i]) pose_[i] = {};
        changed(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear all (rest pose)")) {
        pushUndo();
        std::fill(pose_.begin(), pose_.end(), rigedit::PoseKey{});
        changed(false);
    }
}

void RigEditor::drawWeightPanel(App& app) {
    ImGui::Checkbox("Show the game's model", &showGame_);
    ImGui::SameLine();
    ImGui::Checkbox("Bone names", &names_);
    const std::string cur = active_ >= 0 ? rig_.bones[size_t(active_)].name : "(pick a bone)";
    if (ImGui::BeginCombo("Bone", cur.c_str())) {
        for (size_t i = 0; i < rig_.bones.size(); ++i)
            if (ImGui::Selectable(rig_.bones[i].name.c_str(), int(i) == active_)) active_ = int(i);
        ImGui::EndCombo();
    }
    helpMarker("The bone being painted, shown as a heat map: blue has none of its weight, red all of it. Ctrl+click a joint to pick it.");
    ImGui::RadioButton("Brush", &tool_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Select", &tool_, 1);
    helpMarker("Select: click a vertex (Shift: add), drag a box (Shift: add, Ctrl: take away), A all, Alt+A none, Ctrl+I invert. "
               "The selection is what Assign and the Only selected options use.");

    if (tool_ == 0) {
        ImGui::SeparatorText("Brush");
        const char* brushes[] = {"Add", "Subtract", "Draw", "Smooth"};
        for (int b = 0; b < 4; ++b) {
            if (b) ImGui::SameLine();
            ImGui::RadioButton(brushes[b], &brush_, b);
        }
        helpMarker("Add and Subtract change the weight by Value x Strength; Draw moves it towards Value; Smooth towards its neighbours'.");
        ImGui::SetNextItemWidth(em(180));
        ImGui::SliderFloat("Value", &value_, 0, 1, "%.2f");
        ImGui::SetNextItemWidth(em(180));
        ImGui::SliderFloat("Strength", &strength_, 0.01f, 1, "%.2f");
        ImGui::SetNextItemWidth(em(180));
        ImGui::SliderFloat("Radius", &radius_, 4, 400, "%.0f px");
        helpMarker("[ and ]: radius; Shift+[ and Shift+]: strength.");
        ImGui::SetNextItemWidth(em(180));
        const char* falloffs[] = {"Smooth", "Linear", "Constant"};
        ImGui::Combo("Falloff", &falloff_, falloffs, IM_ARRAYSIZE(falloffs));
        ImGui::Checkbox("Auto normalize", &autoNormalize_);
        helpMarker("The vertex's other bones give up (or take) what this one gains (or loses), so its weights add up to 1 as the game uses them.");
        ImGui::SameLine();
        ImGui::Checkbox("X mirror", &mirrorPaint_);
        helpMarker("Paints the other side too, on the mirrored bone (LF_/RT_, L_/R_, _L/_R...).");
        ImGui::Checkbox("Front faces only", &frontOnly_);
        ImGui::SameLine();
        ImGui::Checkbox("Not through the model", &occlude_);
        helpMarker("Leaves vertices hidden behind the model's surface alone. Off: paints through it.");
    } else {
        ImGui::SeparatorText("Selection");
        ImGui::Text("%d vertices selected", selectedVertices());
        if (ImGui::Button("All")) for (auto& s : vertSel_) std::fill(s.begin(), s.end(), 1);
        ImGui::SameLine();
        if (ImGui::Button("None")) for (auto& s : vertSel_) std::fill(s.begin(), s.end(), 0);
        ImGui::SameLine();
        if (ImGui::Button("Invert"))
            for (auto& s : vertSel_)
                for (char& c : s) c = !c;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em(90));
        ImGui::SliderFloat("##above", &selectAbove_, 0, 1, "> %.2f");
        ImGui::SameLine();
        if (ImGui::Button("Select by bone") && active_ >= 0)
            for (size_t mi = 0; mi < vertSel_.size(); ++mi)
                for (size_t v = 0; v < vertSel_[mi].size(); ++v)
                    if (mi < rig_.skins.size() && !rig_.skins[mi].empty() && rigedit::weightOf(rig_.skins[mi][v], uint16_t(active_)) > selectAbove_)
                        vertSel_[mi][v] = 1;
        helpMarker("Adds the vertices the picked bone has more than that much of.");
        ImGui::SeparatorText("Assign to the selection");
        ImGui::SetNextItemWidth(em(180));
        ImGui::SliderFloat("Weight", &assign_, 0, 1, "%.2f");
        auto onSelection = [&](auto f) {
            if (active_ < 0) return app.status("Pick a bone first");
            pushUndo();
            for (size_t mi = 0; mi < vertSel_.size(); ++mi)
                for (size_t v = 0; v < vertSel_[mi].size(); ++v)
                    if (vertSel_[mi][v] && mi < rig_.skins.size() && !rig_.skins[mi].empty()) {
                        f(rig_.skins[mi][v]);
                        rigedit::syncAliases(rig_, mi, uint32_t(v));
                    }
            changed(true);
        };
        const uint16_t b = uint16_t(std::max(active_, 0));
        if (ImGui::Button("Replace")) onSelection([&](rigedit::VertexSkin& s) { rigedit::setWeight(s, b, assign_, autoNormalize_); });
        ImGui::SameLine();
        if (ImGui::Button("Add")) onSelection([&](rigedit::VertexSkin& s) { rigedit::setWeight(s, b, rigedit::weightOf(s, b) + assign_, autoNormalize_); });
        ImGui::SameLine();
        if (ImGui::Button("Subtract"))
            onSelection([&](rigedit::VertexSkin& s) { rigedit::setWeight(s, b, rigedit::weightOf(s, b) - assign_, autoNormalize_); });
        ImGui::SameLine();
        if (ImGui::Button("Remove bone")) onSelection([&](rigedit::VertexSkin& s) {
            rigedit::setWeight(s, b, 0, false);
            rigedit::normalize(s);
        });
        ImGui::SameLine();
        if (ImGui::Button("Copy from last clicked")) {
            if (lastMesh_ < 0 || size_t(lastMesh_) >= rig_.skins.size() || rig_.skins[size_t(lastMesh_)].empty()) app.status("Click a vertex first");
            else {
                const rigedit::VertexSkin from = rig_.skins[size_t(lastMesh_)][size_t(lastVertex_)];
                onSelection([&](rigedit::VertexSkin& s) { s = from; });
            }
        }
        ImGui::Checkbox("Auto normalize", &autoNormalize_);
    }

    ImGui::Checkbox("Only selected vertices", &opsSelected_);
    helpMarker("Automatic, Transfer and the clean-up tools change only the vertices selected (Select tool); off: every shown mesh.");
    auto run = [&](const char* what, auto f) {
        try {
            pushUndo();
            f();
            changed(true);
            app.status(what);
        } catch (const std::exception& e) {
            if (!undo_.empty()) undo_.pop_back();
            app.error(e.what());
        }
    };

    if (ImGui::CollapsingHeader("Automatic weights")) {
        int method = int(auto_.method);
        const char* methods[] = {"Nearest bone", "Envelope", "Blend by distance"};
        ImGui::SetNextItemWidth(em(180));
        if (ImGui::Combo("Method", &method, methods, IM_ARRAYSIZE(methods))) auto_.method = rigedit::AutoMethod(method);
        helpMarker("Each bone reaches from its joint to its main child's. Nearest bone: all of a vertex to the closest. Envelope: the "
                   "bones within a radius, more the closer. Blend: the closest bones, falling off with distance.");
        const char* which[] = {"All bones", "Selected bones", "Added bones"};
        ImGui::SetNextItemWidth(em(180));
        ImGui::Combo("Bones", &autoBones_, which, IM_ARRAYSIZE(which));
        ImGui::SetNextItemWidth(em(180));
        ImGui::SliderInt("Most bones a vertex", &auto_.maxInfluences, 1, 4);
        if (auto_.method == rigedit::AutoMethod::Blend) {
            ImGui::SetNextItemWidth(em(180));
            ImGui::SliderFloat("Falloff power", &auto_.power, 0.5f, 12, "%.1f");
        }
        if (auto_.method == rigedit::AutoMethod::Envelope) {
            ImGui::SetNextItemWidth(em(180));
            ImGui::SliderFloat("Envelope", &auto_.envelope, 0.01f, 1, "%.2f");
            helpMarker("The radius, a share of the skeleton's size.");
        }
        if (ImGui::Button("Make weights"))
            run("Weights made from the bones", [&] {
                auto_.bones.assign(rig_.bones.size(), autoBones_ == 0);
                for (size_t i = 0; i < rig_.bones.size(); ++i)
                    if ((autoBones_ == 1 && boneSel_[i]) || (autoBones_ == 2 && rig_.bones[i].added)) auto_.bones[i] = 1;
                rigedit::autoWeights(model_, rig_, mask(opsSelected_), auto_);
            });
    }

    if (ImGui::CollapsingHeader("Transfer from another model")) {
        if (sourceList_.empty() && sources) sourceList_ = sources();
        if (sourceList_.empty()) ImGui::TextDisabled("No other copy of this model to copy from.");
        else {
            source_ = std::clamp(source_, 0, int(sourceList_.size()) - 1);
            ImGui::SetNextItemWidth(em(260));
            if (ImGui::BeginCombo("From", sourceList_[size_t(source_)].label.c_str())) {
                for (size_t i = 0; i < sourceList_.size(); ++i)
                    if (ImGui::Selectable(sourceList_[i].label.c_str(), int(i) == source_)) source_ = int(i);
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Refresh") && sources) sourceList_ = sources();
            ImGui::Checkbox("Nearest surface", &transfer_.nearestSurface);
            helpMarker("On: weights blended across the nearest triangle. Off: the nearest vertex's, as they are.");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(em(120));
            ImGui::SliderInt("Most bones a vertex##t", &transfer_.maxInfluences, 1, 4);
            if (ImGui::Button("Transfer weights"))
                run("Weights copied", [&] {
                    const Source& s = sourceList_[size_t(source_)];
                    const size_t matched = rigedit::transfer(*s.model, *s.rig, model_, rig_, mask(opsSelected_), transfer_);
                    notes_ = {std::to_string(matched) + " of its " + std::to_string(s.rig->bones.size()) + " bones matched this model's by name"};
                });
            helpMarker("Copies each vertex's weights from the other model where its surface is nearest, its bones matched to this "
                       "model's by name. What replacing a model needs: the old model's weights on the new shape.");
        }
        for (auto& n : notes_) ImGui::TextDisabled("%s", n.c_str());
    }

    if (ImGui::CollapsingHeader("Clean up")) {
        ImGui::SetNextItemWidth(em(120));
        ImGui::SliderFloat("##factor", &smoothFactor_, 0.05f, 1, "factor %.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em(100));
        ImGui::SliderInt("##iter", &smoothIterations_, 1, 20, "%d times");
        ImGui::SameLine();
        if (ImGui::Button("Smooth"))
            run("Weights smoothed", [&] { rigedit::smooth(model_, adj_, rig_, mask(opsSelected_), smoothFactor_, smoothIterations_); });
        if (ImGui::Button("Normalize")) run("Weights normalized", [&] { rigedit::normalizeAll(rig_, mask(opsSelected_)); });
        helpMarker("Makes each vertex's weights add up to 1 (saving does it too).");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em(110));
        ImGui::SliderFloat("##clean", &cleanThreshold_, 0.001f, 0.5f, "under %.3f");
        ImGui::SameLine();
        if (ImGui::Button("Clean")) run("Small weights removed", [&] { rigedit::clean(rig_, mask(opsSelected_), cleanThreshold_); });
        ImGui::SetNextItemWidth(em(110));
        ImGui::SliderInt("##limit", &limit_, 1, 4, "%d bones");
        ImGui::SameLine();
        if (ImGui::Button("Limit")) run("Bones per vertex limited", [&] { rigedit::limit(rig_, mask(opsSelected_), limit_); });
        helpMarker("Keeps each vertex's heaviest bones only (the game takes four at most).");
        if (ImGui::Button(mirrorPositive_ ? "Mirror weights +X to -X" : "Mirror weights -X to +X"))
            run("Weights mirrored", [&] {
                const size_t n = rigedit::mirrorWeights(model_, rig_, mask(opsSelected_), mirrorPositive_);
                notes_ = {std::to_string(n) + " vertices took their mirror image's weights"};
            });
        ImGui::SameLine();
        ImGui::Checkbox("+X side##w", &mirrorPositive_);
    }
}

void RigEditor::drawPanel(App& app) {
    ImGui::SeparatorText("Rig & weights");
    if (!error_.empty()) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", error_.c_str());
        return;
    }
    if (rig_.bones.empty()) {
        ImGui::TextDisabled("This model has no skeleton.");
        return;
    }
    const struct {
        const char* label;
        Mode mode;
    } modes[] = {{"Object", Mode::Object}, {"Edit", Mode::Edit}, {"Pose", Mode::Pose}, {"Weight Paint", Mode::Weight}};
    for (size_t i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        const bool on = mode_ == modes[i].mode;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(modes[i].label)) setMode(modes[i].mode);
        if (on) ImGui::PopStyleColor();
    }
    helpMarker("Over the view: Tab toggles Edit, Ctrl+Tab Pose. Middle-drag orbits, Shift+middle-drag pans, the wheel zooms.\n"
               "Edit and Pose: click a joint (Shift: add), drag a box, A all, Alt+A none; G move, R rotate, S scale, then X/Y/Z for an "
               "axis and a typed value, Enter or click to apply, Esc or right-click to cancel.\n"
               "Edit: Shift+A add a bone, E extrude, X delete (added bones), Ctrl+P parent to the active joint, Alt+P clear, F2 rename.\n"
               "Pose: Alt+G/R/S clear. Weight Paint: paint with the left button, Ctrl+click a joint for its bone.\nCtrl+Z undo, Ctrl+Shift+Z redo.");
    if (dirty_) {
        ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "Rig and weight edits not applied yet");
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply to the model")) commit();
        helpMarker("Makes them a change of the model (Object mode does it too); Ctrl+S then saves them.");
    }
    ImGui::BeginDisabled(undo_.empty());
    if (ImGui::SmallButton("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(redo_.empty());
    if (ImGui::SmallButton("Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu bones (%zu added)", rig_.bones.size(), rig_.bones.size() - rig_.gameBones);

    if (mode_ == Mode::Edit) drawEditPanel(app);
    else if (mode_ == Mode::Pose) drawPosePanel(app);
    else if (mode_ == Mode::Weight) drawWeightPanel(app);
    if (mode_ != Mode::Object) drawBoneList();
}

} // namespace nut::gui
