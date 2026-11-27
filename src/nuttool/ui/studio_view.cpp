#include "studio_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "nuttool/session.h"
#include "renut_engine/game/field_of_view.h"
#include "renut_engine/game/part_api.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

namespace nut::gui {
namespace {

// The studio camera's vertical field of view (radians), as NutTool's own viewer had it.
constexpr float kFov = 0.8f;
constexpr float kPi = 3.14159265358979f;

void normalize(float* v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-9f)
        for (int k = 0; k < 3; ++k) v[k] /= l;
}

void cross(const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

float dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

} // namespace

// NutTool draws in reNut's overlay, which only shows its own pictures (overlay::AddImage).
GlTexture::~GlTexture() {
    if (id_ != ImTextureID_Invalid) renut::gpu::overlay::RemoveImage(uint64_t(id_));
}

void GlTexture::upload(const uint8_t* rgba, int w, int h, bool, bool) {
    if (id_ != ImTextureID_Invalid) renut::gpu::overlay::RemoveImage(uint64_t(id_));
    id_ = ImTextureID_Invalid;
    if (rgba && w > 0 && h > 0)
        id_ = ImTextureID(renut::gpu::overlay::AddImage(uint32_t(w), uint32_t(h), std::vector<uint8_t>(rgba, rgba + size_t(w) * size_t(h) * 4)));
    w_ = w;
    h_ = h;
}

ModelView::ModelView() = default;

// The model goes from the studio, and the game has its camera and field of view back.
ModelView::~ModelView() {
    game_->gone = true;
    renut::nuttool::OnGameThread([game = game_](PPCContext& ctx, uint8_t* base) {
        const uint32_t object = game->object.exchange(0);
        if (object && game->visit == renut::nuttool::StudioVisit()) renut::game::part_api::RemoveShown(ctx, base, object);
    });
    renut::game::part_api::ReleaseCamera();
    renut::game::SetFieldOfView(0);
}

bool ModelView::ViewCamera::toScreen(const float* p, ImVec2* out, float* depth) const {
    const float v[3] = {p[0] - eye[0], p[1] + floor - eye[1], p[2] - eye[2]};
    const float z = dot(v, f);
    if (z <= 1e-4f) return false;
    const float x = dot(v, s) / (z * tanHalf * aspect), y = dot(v, u) / (z * tanHalf);
    *out = ImVec2(vpPos.x + (x + 1) / 2 * vpSize.x, vpPos.y + (1 - y) / 2 * vpSize.y);
    if (depth) *depth = z;
    return true;
}

void ModelView::ViewCamera::ray(const ImVec2& screen, float* origin, float* dir) const {
    const float nx = (screen.x - vpPos.x) / std::max(vpSize.x, 1.0f) * 2 - 1, ny = 1 - (screen.y - vpPos.y) / std::max(vpSize.y, 1.0f) * 2;
    for (int k = 0; k < 3; ++k) dir[k] = f[k] + s[k] * nx * aspect * tanHalf + u[k] * ny * tanHalf;
    normalize(dir);
    origin[0] = eye[0], origin[1] = eye[1] - floor, origin[2] = eye[2];
}

void ModelView::setGameHidden(bool hidden) {
    if (hidden == hidden_) return;
    hidden_ = hidden;
    shownVisit_ = 0;  // shown again by the next draw
    if (hidden)
        renut::nuttool::OnGameThread([game = game_](PPCContext& ctx, uint8_t* base) {
            const uint32_t object = game->object.exchange(0);
            if (object && game->visit == renut::nuttool::StudioVisit()) renut::game::part_api::RemoveShown(ctx, base, object);
        });
}

void ModelView::setGameModel(uint32_t model, uint32_t bundle) {
    if (hidden_) {
        model_ = model;
        bundle_ = bundle;
        return;
    }
    if (model == model_ && bundle == bundle_ && shownVisit_ == renut::nuttool::StudioVisit()) return;
    model_ = model;
    bundle_ = bundle;
    shownVisit_ = renut::nuttool::StudioVisit();
    game_->failed = false;
    const float at[3] = {0, floor_, 0};
    renut::nuttool::OnGameThread([game = game_, model, bundle, at0 = at[0], at1 = at[1], at2 = at[2], switches = switches_,
                                  anim = anim_](PPCContext& ctx, uint8_t* base) {
        const uint32_t old = game->object.exchange(0);
        if (old && game->visit == renut::nuttool::StudioVisit()) renut::game::part_api::RemoveShown(ctx, base, old);
        if (game->gone || !model) return;
        try {
            const float where[3] = {at0, at1, at2};
            const uint32_t object = renut::game::part_api::ShowModel(ctx, base, model, bundle, where);
            for (const auto& [id, state] : switches) renut::game::part_api::SetShownSwitch(ctx, base, object, id, state);
            if (anim) renut::game::part_api::PlayShownAnimation(ctx, base, object, anim);
            game->visit = renut::nuttool::StudioVisit();
            game->object = object;
        } catch (const std::exception& e) {
            game->error = e.what();
            game->failed = true;
        }
    });
}

void ModelView::setSwitches(const std::map<int, int>& switches) {
    switches_ = switches;
    renut::nuttool::OnGameThread([game = game_, switches](PPCContext& ctx, uint8_t* base) {
        const uint32_t object = game->object.load();
        if (object && game->visit == renut::nuttool::StudioVisit())
            for (const auto& [id, state] : switches) renut::game::part_api::SetShownSwitch(ctx, base, object, id, state);
    });
}

void ModelView::playGameAnimation(uint32_t anim) {
    if (anim == anim_) return;
    const bool stopping = !anim;
    anim_ = anim;
    if (stopping) {
        // The game has no "stop": the model is made again, in its bind pose.
        const uint32_t model = model_, bundle = bundle_;
        model_ = 0;
        setGameModel(model, bundle);
        return;
    }
    renut::nuttool::OnGameThread([game = game_, anim](PPCContext& ctx, uint8_t* base) {
        const uint32_t object = game->object.load();
        if (object && game->visit == renut::nuttool::StudioVisit())
            renut::game::part_api::PlayShownAnimation(ctx, base, object, anim);
    });
}

std::string ModelView::gameError() const { return game_->failed ? game_->error : std::string(); }

void ModelView::setModel(const model::Model& m) {
    meshes_.clear();
    bones_ = m.bones;
    boneWorld_.clear();
    skin_.clear();
    for (auto& mesh : m.meshes) {
        Mesh g;
        for (auto& x : mesh.vertices) g.pos.insert(g.pos.end(), {x.pos[0], x.pos[1], x.pos[2]});
        if (mesh.layout.skinned) {
            g.bind = g.pos;
            for (auto& x : mesh.vertices)
                for (int k = 0; k < 4; ++k) {
                    g.boneIds.push_back(x.bones[k]);
                    g.weights.push_back(x.weights[k]);
                }
        }
        g.indices = mesh.indices;
        meshes_.push_back(std::move(g));
    }
    for (int k = 0; k < 3; ++k) center_[k] = (m.boundsMin[k] + m.boundsMax[k]) / 2;
    float dx = m.boundsMax[0] - m.boundsMin[0], dy = m.boundsMax[1] - m.boundsMin[1], dz = m.boundsMax[2] - m.boundsMin[2];
    radius_ = std::max(0.01f, std::sqrt(dx * dx + dy * dy + dz * dz) / 2);
    // It stands on the stage: its lowest point on the floor.
    floor_ = -m.boundsMin[1];
    frame();
}

void ModelView::setMeshVisible(size_t i, bool v) {
    if (i >= meshes_.size()) return;
    if (v && !meshes_[i].visible) skinDirty_ = true;
    meshes_[i].visible = v;
}

void ModelView::setPose(const std::vector<float>* skinMatrices, const std::vector<float>* boneWorld) {
    if (skinMatrices) skin_ = *skinMatrices;
    else skin_.clear();
    if (boneWorld) boneWorld_ = *boneWorld;
    else boneWorld_.clear();
    skinDirty_ = true;
}

void ModelView::holdGamePose(const std::vector<float>* rows) {
    if (!rows && !holding_) return;
    std::lock_guard lock(game_->mutex);
    if (rows) {
        game_->held = *rows;
        game_->hold = 1;
    } else {
        // Let go: an animation playing poses it again, else it goes back to its bind pose (each skin matrix the identity).
        game_->held.clear();
        if (!anim_)
            for (size_t b = 0; b < bones_.size(); ++b) game_->held.insert(game_->held.end(), {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
        game_->hold = 2;
    }
    holding_ = rows != nullptr;
}

// The bones as the game poses its model (it plays animations itself, root motion and all): NutTool's own sampling
// drifts from it, so the game's matrices win when it has some. The next frame's are asked for each frame.
void ModelView::takeGamePose() {
    if (hidden_ || !model_ || shownVisit_ != renut::nuttool::StudioVisit()) return;
    if (!game_->asked.exchange(true)) {
        const float at[3] = {0, floor_, 0};
        renut::nuttool::OnGameThread([game = game_, x = at[0], y = at[1], z = at[2]](PPCContext& ctx, uint8_t* base) {
            game->asked = false;
            const uint32_t object = game->object.load();
            std::vector<float> rows, held;
            int hold;
            {
                std::lock_guard lock(game->mutex);
                hold = game->hold;
                if (hold) held = game->held;
                if (hold == 2) game->hold = 0;
            }
            if (object && game->visit == renut::nuttool::StudioVisit()) {
                const float where[3] = {x, y, z};
                try {
                    if (hold) renut::game::part_api::HoldShownPose(ctx, base, object, held);
                    renut::game::part_api::ShownPose(ctx, base, object, where, rows);
                } catch (const std::exception&) {
                    rows.clear();
                }
            }
            std::lock_guard lock(game->mutex);
            game->rows = std::move(rows);
        });
    }
    std::vector<float> rows;
    {
        std::lock_guard lock(game_->mutex);
        rows = game_->rows;
    }
    const size_t n = bones_.size();
    if (!n || rows.size() != n * 12) return;
    boneWorld_.resize(n * 3);
    skin_.assign(n * 16, 0);
    for (size_t b = 0; b < n; ++b) {
        const float* r = &rows[b * 12];
        const float* bind = bones_[b].world;
        for (int k = 0; k < 3; ++k) {
            const float* row = r + 4 * k;
            boneWorld_[b * 3 + size_t(k)] = row[0] * bind[0] + row[1] * bind[1] + row[2] * bind[2] + row[3];
            for (int c = 0; c < 4; ++c) skin_[b * 16 + size_t(c) * 4 + size_t(k)] = row[c];
        }
        skin_[b * 16 + 15] = 1;
    }
    skinDirty_ = true;
}

// The pose's positions for picking (linear blend skinning of the visible skinned meshes).
void ModelView::skinMeshes() {
    if (!skinDirty_) return;
    skinDirty_ = false;
    for (auto& g : meshes_) {
        if (!g.visible || g.bind.empty()) continue;
        if (skin_.empty()) {
            g.pos = g.bind;
            continue;
        }
        const size_t nBones = skin_.size() / 16, nv = g.bind.size() / 3;
        for (size_t i = 0; i < nv; ++i) {
            const float* src = &g.bind[i * 3];
            float p[3] = {}, total = 0;
            for (int k = 0; k < 4; ++k) {
                const float w = g.weights[i * 4 + size_t(k)];
                const size_t b = g.boneIds[i * 4 + size_t(k)];
                if (w <= 0 || b >= nBones) continue;
                const float* M = &skin_[b * 16];
                total += w;
                for (int r = 0; r < 3; ++r) p[r] += w * (M[r] * src[0] + M[4 + r] * src[1] + M[8 + r] * src[2] + M[12 + r]);
            }
            if (total > 0)
                for (int r = 0; r < 3; ++r) g.pos[i * 3 + size_t(r)] = p[r] / total;
        }
    }
}

void ModelView::frame() {
    std::copy(center_, center_ + 3, target_);
    distance_ = radius_ * 2.6f;
    yaw_ = 0.6f;
    pitch_ = 0.3f;
}

ModelView::Camera ModelView::camera(const ImVec2& origin, const ImVec2& size) const {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    Camera c{};
    c.aspect = vp->Size.x / std::max(vp->Size.y, 1.0f);
    c.tanHalf = std::tan(kFov / 2);
    const float d[3] = {std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_), std::cos(pitch_) * std::cos(yaw_)};
    const float target[3] = {target_[0], target_[1] + floor_, target_[2]};
    for (int k = 0; k < 3; ++k) c.eye[k] = target[k] + distance_ * d[k];
    float f[3] = {target[0] - c.eye[0], target[1] - c.eye[1], target[2] - c.eye[2]};
    normalize(f);
    const float up[3] = {0, 1, 0};
    float s[3], u[3];
    cross(f, up, s);
    normalize(s);
    cross(s, f, u);
    // Turned so the target shows in the middle of the view's rectangle rather than the screen's.
    const float cx = (origin.x + size.x / 2 - vp->Pos.x) / vp->Size.x * 2 - 1;
    const float cy = 1 - (origin.y + size.y / 2 - vp->Pos.y) / vp->Size.y * 2;
    for (int k = 0; k < 3; ++k) c.f[k] = f[k] - s[k] * cx * c.aspect * c.tanHalf - u[k] * cy * c.tanHalf;
    normalize(c.f);
    cross(c.f, up, c.s);
    normalize(c.s);
    cross(c.s, c.f, c.u);
    return c;
}

bool ModelView::project(const Camera& c, const float* p, ImVec2* out) const {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float v[3] = {p[0] - c.eye[0], p[1] - c.eye[1], p[2] - c.eye[2]};
    const float z = dot(v, c.f);
    if (z <= 1e-4f) return false;
    const float x = dot(v, c.s) / (z * c.tanHalf * c.aspect), y = dot(v, c.u) / (z * c.tanHalf);
    *out = ImVec2(vp->Pos.x + (x + 1) / 2 * vp->Size.x, vp->Pos.y + (1 - y) / 2 * vp->Size.y);
    return true;
}

// The nearest visible mesh hit by the ray through a screen point (the screen's normalised device coordinates).
int ModelView::pick(const Camera& c, float ndcX, float ndcY) const {
    float d[3];
    for (int k = 0; k < 3; ++k) d[k] = c.f[k] + c.s[k] * ndcX * c.aspect * c.tanHalf + c.u[k] * ndcY * c.tanHalf;
    const float eye[3] = {c.eye[0], c.eye[1] - floor_, c.eye[2]};  // in the model's own space
    int best = -1;
    float bestT = 1e30f;
    for (size_t mi = 0; mi < meshes_.size(); ++mi) {
        const Mesh& m = meshes_[mi];
        if (!m.visible) continue;
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            const float* p[3] = {};
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                const size_t v = m.indices[i + size_t(k)];
                ok &= v * 3 + 2 < m.pos.size();
                if (ok) p[k] = &m.pos[v * 3];
            }
            if (!ok) continue;
            // Moller-Trumbore, both faces.
            float e1[3], e2[3], o[3];
            for (int k = 0; k < 3; ++k) e1[k] = p[1][k] - p[0][k], e2[k] = p[2][k] - p[0][k], o[k] = eye[k] - p[0][k];
            float q[3];
            cross(d, e2, q);
            const float det = dot(e1, q);
            if (std::fabs(det) < 1e-12f) continue;
            const float bu = dot(o, q) / det;
            if (bu < 0 || bu > 1) continue;
            float r[3];
            cross(o, e1, r);
            const float bv = dot(d, r) / det;
            if (bv < 0 || bu + bv > 1) continue;
            const float hit = dot(e2, r) / det;
            if (hit > 0 && hit < bestT) {
                bestT = hit;
                best = int(mi);
            }
        }
    }
    return best;
}

void ModelView::draw(const ImVec2& size, const Options& opt) {
    // Back in the studio after leaving it: the model is made again (the last visit's went with that world).
    if (model_ && !hidden_ && shownVisit_ != renut::nuttool::StudioVisit()) setGameModel(model_, bundle_);
    takeGamePose();
    const float w = std::max(16.0f, size.x), h = std::max(16.0f, size.y);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    // An invisible button over the see-through view keeps drags going when the cursor leaves it.
    ImGui::InvisibleButton("##viewport", ImVec2(w, h),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    ImGuiIO& io = ImGui::GetIO();
    const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    auto dir = [this](float* d) {
        d[0] = std::cos(pitch_) * std::sin(yaw_);
        d[1] = std::sin(pitch_);
        d[2] = std::cos(pitch_) * std::cos(yaw_);
    };
    if (hovered && rigInput_) {
        if (io.MouseWheel != 0) distance_ *= std::pow(0.88f, io.MouseWheel);
    } else if (hovered) {
        if (io.MouseWheel != 0) distance_ *= std::pow(0.88f, io.MouseWheel);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            frame();
        } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 16 && !io.KeyShift) {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const float nx = (io.MousePos.x - vp->Pos.x) / vp->Size.x * 2 - 1, ny = 1 - (io.MousePos.y - vp->Pos.y) / vp->Size.y * 2;
            skinMeshes();
            click_ = pick(camera(origin, ImVec2(w, h)), nx, ny);
        }
    }
    if (active && rigInput_) {
        const ImVec2 dm = io.MouseDelta;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) && io.KeyShift) {
            const float s = distance_ * 0.0015f;
            const float rx[3] = {std::cos(yaw_), 0, -std::sin(yaw_)};
            const float up[3] = {-std::sin(pitch_) * std::sin(yaw_), std::cos(pitch_), -std::sin(pitch_) * std::cos(yaw_)};
            for (int k = 0; k < 3; ++k) target_[k] += (-dm.x * rx[k] + dm.y * up[k]) * s;
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            yaw_ -= dm.x * 0.01f;
            pitch_ = std::clamp(pitch_ + dm.y * 0.01f, -1.55f, 1.55f);
        }
    } else if (active) {
        const ImVec2 dm = io.MouseDelta;
        const bool pan = ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (ImGui::IsMouseDown(ImGuiMouseButton_Left) && io.KeyShift);
        if (pan) {
            const float s = distance_ * 0.0015f;
            const float rx[3] = {std::cos(yaw_), 0, -std::sin(yaw_)};
            const float up[3] = {-std::sin(pitch_) * std::sin(yaw_), std::cos(pitch_), -std::sin(pitch_) * std::cos(yaw_)};
            for (int k = 0; k < 3; ++k) target_[k] += (-dm.x * rx[k] + dm.y * up[k]) * s;
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            yaw_ -= dm.x * 0.01f;
            pitch_ = std::clamp(pitch_ + dm.y * 0.01f, -1.55f, 1.55f);
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            float d[3], eye[3];
            dir(d);
            for (int k = 0; k < 3; ++k) eye[k] = target_[k] + distance_ * d[k];
            yaw_ -= dm.x * 0.005f;
            pitch_ = std::clamp(pitch_ + dm.y * 0.005f, -1.55f, 1.55f);
            dir(d);
            for (int k = 0; k < 3; ++k) target_[k] = eye[k] - distance_ * d[k];
        }
    }
    if ((hovered || active) && !io.WantTextInput && !rigInput_) {
        float d[3];
        dir(d);
        const float fwd[3] = {-d[0], -d[1], -d[2]}, right[3] = {std::cos(yaw_), 0, -std::sin(yaw_)}, up[3] = {0, 1, 0};
        float move[3] = {};
        auto add = [&](const float* v, float s) { for (int k = 0; k < 3; ++k) move[k] += v[k] * s; };
        if (ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_UpArrow)) add(fwd, 1);
        if (ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_DownArrow)) add(fwd, -1);
        if (ImGui::IsKeyDown(ImGuiKey_D) || ImGui::IsKeyDown(ImGuiKey_RightArrow)) add(right, 1);
        if (ImGui::IsKeyDown(ImGuiKey_A) || ImGui::IsKeyDown(ImGuiKey_LeftArrow)) add(right, -1);
        if (ImGui::IsKeyDown(ImGuiKey_E)) add(up, 1);
        if (ImGui::IsKeyDown(ImGuiKey_Q)) add(up, -1);
        const float speed = radius_ * 0.8f * io.DeltaTime * (io.KeyShift ? 4.0f : 1.0f);
        for (int k = 0; k < 3; ++k) target_[k] += move[k] * speed;
    }
    // The game draws to 5000 at most: a big model's (a world's) far side is kept within that.
    constexpr float kGameFar = 5000;
    distance_ = std::clamp(distance_, radius_ * 0.02f, std::max(radius_ * 0.02f, std::min(radius_ * 100, kGameFar - radius_ * 1.05f)));

    // The game's camera, then what's drawn over its picture with the same camera.
    const Camera c = camera(origin, ImVec2(w, h));
    const float look[3] = {c.eye[0] + c.f[0], c.eye[1] + c.f[1], c.eye[2] + c.f[2]};
    if (renut::nuttool::InStudio()) {  // never once NutTool is being left
        // The view reaches the model's far side (the game keeps its own when that's further).
        const float toCentre[3] = {center_[0] - c.eye[0], center_[1] + floor_ - c.eye[1], center_[2] - c.eye[2]};
        const float reach = std::sqrt(dot(toCentre, toCentre)) + radius_ * 1.05f;
        renut::game::part_api::SetCamera(c.eye, look, reach);
        renut::game::SetFieldOfView(kFov * 180 / kPi);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + w, origin.y + h), true);
    auto line = [&](const float* a, const float* b, ImU32 colour, float thick) {
        ImVec2 sa, sb;
        if (project(c, a, &sa) && project(c, b, &sb)) dl->AddLine(sa, sb, colour, thick);
    };
    if (opt.grid) {
        const float step = std::pow(10.0f, std::floor(std::log10(std::max(radius_, 0.01f)))), half = step * 10;
        for (int i = -10; i <= 10; ++i) {
            const float t = float(i) * step;
            const float a[3] = {t, 0, -half}, b[3] = {t, 0, half}, e[3] = {-half, 0, t}, g[3] = {half, 0, t};
            const ImU32 colour = i == 0 ? IM_COL32(255, 255, 255, 110) : IM_COL32(255, 255, 255, 45);
            line(a, b, colour, 1);
            line(e, g, colour, 1);
        }
    }
    if (overlay) {
        ViewCamera vc;
        std::copy(c.eye, c.eye + 3, vc.eye);
        std::copy(c.f, c.f + 3, vc.f);
        std::copy(c.s, c.s + 3, vc.s);
        std::copy(c.u, c.u + 3, vc.u);
        vc.tanHalf = c.tanHalf;
        vc.aspect = c.aspect;
        vc.floor = floor_;
        vc.vpPos = ImGui::GetMainViewport()->Pos;
        vc.vpSize = ImGui::GetMainViewport()->Size;
        overlay(dl, vc, hovered || active);
    }
    if (opt.bones && !bonesHidden && !bones_.empty()) {
        for (size_t bi = 0; bi < bones_.size(); ++bi) {
            const float* bp = boneAt(bi);
            const float p[3] = {bp[0], bp[1] + floor_, bp[2]};
            if (bones_[bi].parent >= 0 && size_t(bones_[bi].parent) < bones_.size()) {
                const float* pp = boneAt(size_t(bones_[bi].parent));
                const float q[3] = {pp[0], pp[1] + floor_, pp[2]};
                line(p, q, IM_COL32(255, 200, 80, 220), 2);
            }
            ImVec2 s;
            if (project(c, p, &s)) {
                dl->AddCircleFilled(s, 3, IM_COL32(255, 230, 140, 255));
                if (opt.boneNames) dl->AddText(ImVec2(s.x + 5, s.y - 7), IM_COL32(255, 220, 140, 255), bones_[bi].name.c_str());
            }
        }
    }
    if (const std::string error = gameError(); !error.empty())
        dl->AddText(ImVec2(origin.x + 8, origin.y + 8), IM_COL32(255, 120, 100, 255), ("The game can't show it: " + error).c_str());
    dl->PopClipRect();
}

} // namespace nut::gui
