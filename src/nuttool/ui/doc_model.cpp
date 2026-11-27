// 3D model viewer: geometry, textures and skeleton of a model from the catalog.
#include "docs.hpp"
#include "fbx.hpp"

#include "animkeys.hpp"
#include "assetdb.hpp"
#include "studio_view.hpp"
#include "model.hpp"
#include "modelimport.hpp"
#include "rig_edit.hpp"
#include "rigedit.hpp"
#include "streambundle.hpp"
#include "ops.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <map>
#include <optional>
#include <set>

namespace nut::gui {
namespace {

std::string shortModelName(const std::string& n) {
    for (const char* p : {"aid_model_banjox_", "aid_model_banjoxdlc_", "aid_model_"})
        if (startsWith(n, p)) return n.substr(std::string(p).size());
    return n;
}

// The animation `name` in a CAFF: symbol `hint` when it is that animation, else the first one named so.
std::optional<size_t> findAnimation(const caff::Caff& c, const std::string& name, size_t hint) {
    std::string want = lower(name);
    auto is = [&](size_t i) { return i < c.symbols.size() && model::isAnimationSymbol(c, i) && lower(assetdb::baseAssetName(c.symbolName(i))) == want; };
    if (is(hint)) return hint;
    for (size_t i = 0; i < c.symbols.size(); ++i)
        if (is(i)) return i;
    return std::nullopt;
}

// The animation a new motion builds on: the copy at `loc`, or with `fromOriginal` the same copy in the file's .bak when there is one.
model::Animation motionBase(const AssetLoc& loc, const std::string& name, bool fromOriginal, CaffCache& cache) {
    AssetLoc from = loc;
    fs::path bak = loc.file;
    bak += ".bak";
    std::error_code ec;
    if (fromOriginal && fs::exists(bak, ec)) from.file = bak;
    caff::Caff& c = cache.get(from);
    auto sym = findAnimation(c, name, loc.symbol);
    if (!sym) fail(name + " is not in " + toUtf8(from.file));
    return model::parseAnimation(c, *sym);
}

// Column-major 4x4 matrices for the skeleton.
using Mat4 = std::array<float, 16>;

Mat4 identity4() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat4 mul4(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[size_t(k * 4 + row)] * b[size_t(c * 4 + k)];
            r[size_t(c * 4 + row)] = s;
        }
    return r;
}

// Translation * rotation (quaternion x, y, z, w) * scale.
Mat4 trs(const float* t, const float* q, const float* s) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n > 0) { x /= n; y /= n; z /= n; w /= n; }
    Mat4 m = identity4();
    m[0] = (1 - 2 * (y * y + z * z)) * s[0];
    m[1] = (2 * (x * y + z * w)) * s[0];
    m[2] = (2 * (x * z - y * w)) * s[0];
    m[4] = (2 * (x * y - z * w)) * s[1];
    m[5] = (1 - 2 * (x * x + z * z)) * s[1];
    m[6] = (2 * (y * z + x * w)) * s[1];
    m[8] = (2 * (x * z + y * w)) * s[2];
    m[9] = (2 * (y * z - x * w)) * s[2];
    m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    return m;
}

// Inverse of a rotation + translation matrix (bind poses carry no scale).
Mat4 rigidInverse(const Mat4& m) {
    Mat4 r = identity4();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[size_t(j * 4 + i)] = m[size_t(i * 4 + j)];
    for (int i = 0; i < 3; ++i) r[size_t(12 + i)] = -(r[size_t(i)] * m[12] + r[size_t(4 + i)] * m[13] + r[size_t(8 + i)] * m[14]);
    return r;
}

std::string shortAnimName(const std::string& n, const std::string& prefix) {
    if (!prefix.empty() && startsWith(n, prefix)) return n.substr(prefix.size());
    for (const char* p : {"aid_anim_banjox_", "aid_anim_"})
        if (startsWith(n, p)) return n.substr(std::string(p).size());
    return n;
}

// The data bundle a game file is (Bundle/4f/<id>) or goes with (Bundle/50/<id>, its stream bundle); 0 for another file.
uint32_t bundleOf(const fs::path& file) {
    const std::string kind = lower(file.parent_path().filename().string()), id = file.filename().string();
    if ((kind != "4f" && kind != "50") || id.size() != 6 || id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        return 0;
    return 0x4F000000u | parseHex(id);
}

std::string layoutText(const model::Mesh& m) {
    std::string s = std::to_string(m.stride) + " B: pos";
    if (m.layout.skinned) s += ", skin";
    if (m.layout.normal >= 0) s += ", normal";
    if (m.layout.tangent >= 0) s += ", tangent";
    if (!m.layout.uvSets.empty()) s += ", " + std::to_string(m.layout.uvSets.size()) + " UV";
    return s;
}

class ModelDocument : public Document {
public:
    ModelDocument(const ModelInfo& info, std::shared_ptr<const Catalog> catalog, const fs::path& root)
        : info_(info), catalog_(std::move(catalog)), root_(root) {
        path = info.locs.front().file;
        rig_.apply = [this](std::shared_ptr<const rigedit::Rig> r) { pendingRig_ = std::move(r); };
        rig_.sources = [this] { return rigSources(); };
        loadModel(cache_.get(info.locs.front()), info.locs.front().symbol);
    }

    // (Re)builds everything shown from one copy of the model.
    void loadModel(const caff::Caff& c, uint32_t symbol) {
        stopAnimation();
        view_.reset(); // the next frame uploads the new meshes and textures
        textures_.clear();
        ownAnims_.clear();
        hasFur_ = false;
        colourGroups_.clear();
        colourEdited_.clear();
        pickedMaterial_.clear();
        hoveredMaterial_.clear();
        pickedMesh_ = -1;
        model_ = model::parse(c, symbol);
        try {
            furBlocks_ = model::furBlocks(c, symbol);
        } catch (const std::exception&) {
            furBlocks_.clear();
        }
        // Everything read from `c` comes before the textures: decoding them through cache_ may drop it.
        rig_.load(c, symbol, model_);
        // Decode every referenced texture (level 0, scaled down to at most 1024 for the viewport).
        for (auto& name : model_.textures) {
            LoadedTex lt;
            lt.name = name;
            const TextureInfo* ti = catalog_ ? catalog_->texture(name) : nullptr;
            if (!ti) {
                lt.error = "not found in the game files";
            } else {
                try {
                    ops::DecodedTexture d = decodeCatalogTexture(*ti, cache_);
                    if (d.header.kind == texture::Kind::Volume) {
                        // Fur volumes: every slice of level 0, as layers of one array texture.
                        lt.volumeLayers = int(d.levels[0].slices.size());
                        lt.noAlpha = true;
                        for (int s = 0; s < lt.volumeLayers; ++s) {
                            image::Rgba sl = texture::toRgba(d.header, d.levels[0], uint32_t(s));
                            for (size_t p = 3; p < sl.pixels.size(); p += 4) lt.noAlpha &= sl.pixels[p] == 255;
                            lt.volume.insert(lt.volume.end(), sl.pixels.begin(), sl.pixels.end());
                        }
                    }
                    image::Rgba img = texture::toRgba(d.header, d.levels[0], 0);
                    uint32_t w = img.width, h = img.height;
                    while (w > 1024 || h > 1024) { w = std::max(1u, w / 2); h = std::max(1u, h / 2); }
                    if (w != img.width || h != img.height) img = texture::downscale(img, w, h);
                    lt.img = std::move(img);
                } catch (std::exception& e) {
                    lt.error = e.what();
                }
            }
            hasFur_ |= lt.volumeLayers > 0;
            textures_.push_back(std::move(lt));
        }
        // Animations named after this model ("aid_model_x" -> "aid_anim_x_...").
        if (catalog_ && startsWith(model_.name, "aid_model_")) {
            animPrefix_ = "aid_anim_" + model_.name.substr(10) + "_";
            for (auto& a : catalog_->anims)
                if (startsWith(a.name, animPrefix_)) ownAnims_.push_back(a.name);
        }
        computeBindPose();
        initMeshState();
        collectColours();
    }

public:
    std::string title() const override { return "model " + shortModelName(model_.name); }
    bool showsGame() const override { return true; }

    void draw(App& app) override {
        // A reload asked for while the panel was drawn (it replaces the view the rest of that frame uses).
        if (deferred_) std::exchange(deferred_, nullptr)();
        applyPendingRig(app);
        if (!view_) {
            view_ = std::make_unique<ModelView>();
            for (auto& t : textures_) {
                if (t.img.pixels.empty()) continue;
                t.gl = std::make_unique<GlTexture>();
                t.gl->upload(t.img.pixels.data(), int(t.img.width), int(t.img.height), true, false);
                if (t.volumeLayers > 0) {
                    t.glArray = std::make_unique<GlTextureArray>();
                    t.glArray->upload(t.volume.data(), int(t.img.width), int(t.img.height), t.volumeLayers);
                }
            }
            pushModel();
            view_->setSwitches(switchState_);
            view_->setGameModel(assetdb::idFromName(model_.name).value_or(0), bundleOf(info_.locs.front().file));
        }
        float h = ImGui::GetContentRegionAvail().y, w = ImGui::GetContentRegionAvail().x;
        float panel = std::clamp(panelFrac_ * w, em(260), w - em(300));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetColorU32(ImGuiCol_WindowBg));
        ImGui::BeginChild("##props", ImVec2(panel, h), ImGuiChildFlags_Borders);
        ImGui::PopStyleColor();
        drawPanel(app);
        ImGui::EndChild();
        splitter("##modelsplit", panel, em(260), w - em(300), h);
        panelFrac_ = panel / w;
        ImGui::BeginChild("##viewport", ImVec2(0, h));
        if (!startupChecked_) {
            startupChecked_ = true;
            std::string a = app.takeStartupAnimation();
            // --replace-with: the same path as picking from "Replace with another model" (then --anim,
            // since reloading the model stops animations).
            std::string r = app.takeStartupReplacement();
            if (!r.empty())
                deferred_ = [this, &app, r, a] {
                    addChange(app, {Change::Kind::Swap, r, {}, false, 0});
                    if (!a.empty()) loadAnimation(a);
                };
            else if (!a.empty())
                loadAnimation(a);
            fs::path fbxOut = app.takeStartupFbx(); // --export-fbx: as "Export FBX..." with the default choices
            if (!fbxOut.empty()) exportFbxTo(app, fbxOut);
        }
        updateAnimation();
        // The material picked on the model, or the one hovered in the Materials list, stands out.
        const std::string& lit = hoveredMaterial_.empty() ? pickedMaterial_ : hoveredMaterial_;
        std::vector<bool> highlight(model_.meshes.size(), false);
        for (auto& e : materials_)
            if (!lit.empty() && e.label == lit)
                for (size_t m : e.meshes) highlight[m] = true;
        if (pickedMaterial_.empty() && hoveredMaterial_.empty() && pickedMesh_ >= 0 && size_t(pickedMesh_) < highlight.size()) highlight[size_t(pickedMesh_)] = true;
        view_->setHighlight(highlight);
        std::vector<bool> shown(model_.meshes.size());
        for (size_t i = 0; i < shown.size(); ++i) shown[i] = meshShown(i);
        rig_.setMeshesShown(shown);
        rig_.prepare(*view_, app);
        view_->draw(ImGui::GetContentRegionAvail(), opt_);
        if (auto c = view_->takeClick(); c && rig_.mode() == RigEditor::Mode::Object) pickMesh(app, *c);
        ImGui::EndChild();
    }

private:
    // A model change not saved yet. Bind's mesh numbers the meshes as the changes before it leave them.
    struct Change {
        enum class Kind { Swap, Import, Bind, Restore, Fur, Rig } kind;
        std::string name; // Swap: the model copied in; Import: the file's name; Bind: the texture
        Bytes file;       // Import: the OBJ, FBX or .glb
        bool rig = false; // Import: the rig override
        size_t mesh = 0;  // Bind
        uint32_t block = 0; // Fur: the render program block (model::FurBlock::at) and whether its fur is on
        bool on = true;
        std::shared_ptr<const rigedit::Rig> rigEdit;  // Rig: the edited joints, bones and weights
    };

    struct LoadedTex {
        std::string name;
        image::Rgba img;
        std::unique_ptr<GlTexture> gl;
        std::string error;
        // Volume textures (fur): all slices of level 0, uploaded as an array texture.
        std::vector<uint8_t> volume;
        int volumeLayers = 0;
        bool noAlpha = false; // the volume carries no coverage; shells use strand noise
        std::unique_ptr<GlTextureArray> glArray;
    };

    // Fur draws bind {0 root colour, 1 fur volume, 2 tip colour, 3 thinning, ...}.
    ModelView::Fur furOf(const model::Mesh& m) const {
        ModelView::Fur f;
        auto at = [&](int slot) -> const LoadedTex* {
            for (size_t k = 0; k < m.textures.size() && k < m.textureSlots.size(); ++k)
                if (m.textureSlots[k] == slot && size_t(m.textures[k]) < textures_.size()) return &textures_[size_t(m.textures[k])];
            return nullptr;
        };
        const LoadedTex* vol = at(1);
        if (!vol || !vol->glArray) return f;
        const LoadedTex *root = at(0), *tip = at(2), *thin = at(3);
        f.volume = vol->glArray.get();
        f.noise = vol->noAlpha;
        f.root = root && root->gl && !root->glArray ? root->gl.get() : nullptr;
        f.tip = tip && tip->gl && !tip->glArray ? tip->gl.get() : f.root;
        f.thinning = thin && thin->gl && !thin->glArray ? thin->gl.get() : nullptr;
        if (!f.root) f.root = f.tip;
        return f;
    }

    // The model texture a mesh binds to sampler `slot`, or -1.
    static int textureInSlot(const model::Mesh& m, int slot) {
        for (size_t k = 0; k < m.textures.size() && k < m.textureSlots.size(); ++k)
            if (m.textureSlots[k] == slot) return m.textures[k];
        return -1;
    }

    bool furred(const model::Mesh& m) const {
        int v = textureInSlot(m, 1);
        return v >= 0 && size_t(v) < textures_.size() && textures_[size_t(v)].volumeLayers > 0;
    }

    // Whether mesh `i` is in a render block whose fur is turned off.
    bool furOff(size_t i) const {
        for (const model::FurBlock& b : furBlocks_)
            if (!b.on && std::find(b.meshes.begin(), b.meshes.end(), i) != b.meshes.end()) return true;
        return false;
    }

    // A furred mesh's row: its material and its mask (the thinning map in sampler slot 3).
    void furMeshRow(App& app, size_t i) {
        const model::Mesh& m = model_.meshes[i];
        const int mask = textureInSlot(m, 3);
        ImGui::PushID(int(i) + 30000);
        ImGui::BulletText("mesh %zu%s%s", i, m.material.empty() ? "" : ": ", m.material.c_str());
        ImGui::SameLine();
        if (mask < 0) {
            ImGui::TextDisabled("no mask");
        } else {
            ImGui::TextDisabled("mask %s", shortTex(model_.textures[size_t(mask)]).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Open mask")) app.openTexture(model_.textures[size_t(mask)]);
        }
        ImGui::PopID();
    }

    // The fur: each render block of furred draws with its switch, and each furred mesh with its mask.
    void drawFur(App& app) {
        auto shown = [&](size_t i) { return groupShown_[size_t(model_.meshes[i].group)] && furred(model_.meshes[i]); };
        bool any = false;
        for (size_t i = 0; i < model_.meshes.size(); ++i) any |= shown(i);
        if (!any) return;
        ImGui::SeparatorText("Fur");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Fur on turns a group of furred draws' fur on or off (the game draws them in groups); off keeps their "
                           "surface. Ctrl+S writes it. A mesh's mask is its thinning map: fur grows where the mask is bright, as long "
                           "as it is bright, and not where it is black. Open a mask to import a black and white image over it; every "
                           "model using that mask changes with it. A mesh with no mask is furred all over.");
        ImGui::PopStyleColor();
        std::set<size_t> listed;
        for (size_t k = 0; k < furBlocks_.size(); ++k) {
            const model::FurBlock& b = furBlocks_[k];
            bool visible = false;
            for (size_t i : b.meshes) visible |= groupShown_[size_t(model_.meshes[i].group)];
            if (!visible) continue;
            ImGui::PushID(int(k) + 40000);
            bool on = b.on;
            char label[64];
            std::snprintf(label, sizeof label, "Fur on (group at 0x%x, %zu mesh%s)", unsigned(b.at), b.meshes.size(), b.meshes.size() == 1 ? "" : "es");
            if (ImGui::Checkbox(label, &on)) {
                Change ch{Change::Kind::Fur, "the group at 0x" + hex32(b.at), {}, false, 0};
                ch.block = b.at;
                ch.on = on;
                deferred_ = [this, &app, ch] { addChange(app, ch); };
            }
            ImGui::Indent();
            for (size_t i : b.meshes)
                if (shown(i) && listed.insert(i).second) furMeshRow(app, i);
            ImGui::Unindent();
            ImGui::PopID();
        }
        for (size_t i = 0; i < model_.meshes.size(); ++i)
            if (shown(i) && !listed.count(i)) furMeshRow(app, i);
    }

    void initMeshState() {
        meshTex_.clear();
        for (auto& m : model_.meshes) meshTex_.push_back(m.layout.uvSets.empty() ? -1 : m.texture);
        meshVisible_.assign(model_.meshes.size(), true);
        lod_ = model_.defaultLod;
        switchState_.clear();
        for (auto& [id, states] : model_.switches) switchState_[id] = states.front();
        resetGroups();
    }

    // Groups the game draws at the chosen detail level and switch states.
    void resetGroups() {
        if (model_.nodes.empty()) {
            groupShown_.clear();
            for (auto& g : model_.groups) groupShown_.push_back(g.shown);
        } else {
            groupShown_ = model::visibleGroups(model_, lod_, switchState_);
        }
    }

    // What a switch state shows: the names of its nodes ("wrench", "camera"...).
    static constexpr int kSwitchOff = -1;

    std::string switchLabel(int id, int state) const {
        if (state == kSwitchOff) return "off";
        std::string out;
        for (auto& n : model_.nodes) {
            if (n.switchId != id || n.switchState != state) continue;
            std::string seg = n.name.substr(n.name.rfind('|') == std::string::npos ? 0 : n.name.rfind('|') + 1);
            size_t p = lower(seg).rfind("_switch");
            if (p != std::string::npos) seg = seg.substr(0, p);
            if (seg.rfind(':') != std::string::npos) seg = seg.substr(seg.rfind(':') + 1);
            if (out.find(seg) == std::string::npos) out += (out.empty() ? "" : ", ") + seg;
        }
        return std::to_string(state) + (out.empty() ? "" : ": " + out);
    }

    // A readable name for a group: its deepest named node.
    std::string groupLabel(size_t g) const {
        const auto& path = model_.groups[g].path;
        for (size_t k = path.size(); k-- > 0;) {
            const std::string& n = model_.nodes[size_t(path[k])].name;
            if (n.empty()) continue;
            std::string seg = n.substr(n.rfind('|') == std::string::npos ? 0 : n.rfind('|') + 1);
            if (!seg.empty()) return seg;
        }
        return "group " + std::to_string(g);
    }

    bool meshShown(size_t i) const { return meshVisible_[i] && groupShown_[size_t(model_.meshes[i].group)]; }

    void pushModel() {
        view_->setModel(model_);
        applyMeshState();
    }

    void applyMeshState() {
        for (size_t i = 0; i < model_.meshes.size(); ++i) {
            int t = meshTex_[i];
            view_->setMeshTexture(i, t >= 0 && size_t(t) < textures_.size() && textures_[size_t(t)].gl ? textures_[size_t(t)].gl.get() : nullptr);
            view_->setMeshVisible(i, meshShown(i));
            view_->setMeshColour(i, model_.meshes[i].colour);
            view_->setMeshTint(i, model_.meshes[i].textureGain, model_.meshes[i].textureOffset);
            view_->setMeshFur(i, furOff(i) ? ModelView::Fur{} : furOf(model_.meshes[i]));
        }
    }

    void drawPanel(App& app) {
        ImGui::TextWrapped("%s", model_.name.c_str());
        size_t verts = 0, tris = 0, shownMeshes = 0;
        for (size_t i = 0; i < model_.meshes.size(); ++i) {
            if (!meshShown(i)) continue;
            ++shownMeshes;
            verts += model_.meshes[i].vertices.size();
            tris += model_.meshes[i].indices.size() / 3;
        }
        ImGui::TextDisabled("%zu meshes shown, %zu vertices, %zu triangles, %zu bones", shownMeshes, verts, tris, model_.bones.size());
        if (model_.meshes.empty() && !model_.warnings.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", model_.warnings.front().c_str());
        ImGui::SeparatorText("View");
        ImGui::Checkbox("Textures", &opt_.textured);
        ImGui::SameLine();
        ImGui::Checkbox("Wireframe", &opt_.wireframe);
        ImGui::SameLine();
        ImGui::Checkbox("Grid", &opt_.grid);
        ImGui::Checkbox("Bones", &opt_.bones);
        ImGui::SameLine();
        ImGui::BeginDisabled(!opt_.bones);
        ImGui::Checkbox("Bone names", &opt_.boneNames);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Back faces", &opt_.backfaces);
        ImGui::SameLine();
        ImGui::Checkbox("Material tint", &opt_.tint);
        helpMarker("Multiplies textures by their material's colour gain (c:textureGain), as the game's shaders do.");
        if (hasFur_) {
            ImGui::Checkbox("Fur", &opt_.fur);
            ImGui::SameLine();
            ImGui::BeginDisabled(!opt_.fur);
            float pct = opt_.furLength * 100;
            ImGui::SetNextItemWidth(em(120));
            if (ImGui::SliderFloat("Fur length", &pct, 0.2f, 6.0f, "%.1f%%")) opt_.furLength = pct / 100;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(em(100));
            ImGui::SliderFloat("Tiling", &opt_.furTiling, 1, 16, "%.0f");
            ImGui::EndDisabled();
            helpMarker("Fur is drawn as shells: one layer per slice of the fur volume texture, pushed out along the surface. "
                       "The length is a percentage of the model's size and the tiling is how often the volume repeats; "
                       "the game sets both in its fur shader, which is not decoded.");
        }
        drawLighting();
        if (ImGui::Button("Reset camera")) view_->frame();
        helpMarker("Left-click: pick the material under the cursor (Materials section).\nLeft-drag: orbit around the model.\nRight-drag: turn the camera (look around).\nMiddle-drag or Shift+left-drag: pan.\n"
                   "Wheel: zoom.\nW/A/S/D or arrow keys: fly, Q/E: down/up, hold Shift to go faster (while the mouse is over the view).\n"
                   "Double-click: reset.");
        ImGui::SameLine();
        if (ImGui::Button("Export OBJ...")) ImGui::OpenPopup("Export OBJ");
        helpMarker("Writes the shown meshes as .obj + .mtl with the same materials, tinted textures (PNG files next to it) and fur "
                   "as the FBX export. OBJ holds no skeleton, skin or animation: use Export FBX for those.");
        drawExport(app, true);
        ImGui::SameLine();
        if (ImGui::Button("Export FBX...")) ImGui::OpenPopup("Export FBX");
        helpMarker("Writes the shown meshes skinned to the skeleton, materials with their textures (embedded) and animations as one .fbx file.");
        drawExport(app, false);
        const App::ModelPanels& show = app.modelPanels;
        if (show.warnings && !model_.warnings.empty() && ImGui::CollapsingHeader("Warnings"))
            for (auto& w : model_.warnings) ImGui::BulletText("%s", w.c_str());

        drawReplace(app);
        rig_.drawPanel(app);
        drawAnimation(app);
        if (show.parts && model_.groups.size() > 1) drawParts();
        if (show.materials) drawColours();
        else hoveredMaterial_.clear();

        if (show.fur) drawFur(app);
        if (show.meshes) drawMeshes(app);
        if (show.textures) drawTextureList(app);

        if (show.skeleton) {
            ImGui::SeparatorText("Skeleton");
            if (model_.bones.empty()) ImGui::TextDisabled("No bones.");
            else drawBoneTree(-1, 0);
        }

        if (show.materialNames && !model_.materials.empty()) {
            ImGui::SeparatorText("Materials");
            for (auto& m : model_.materials) ImGui::BulletText("%s", m.c_str());
        }
    }

    void drawMeshes(App& app) {
        ImGui::SeparatorText("Meshes");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Textured meshes show the colour map among the textures their draw binds; the others use their material's "
                           "base colour. Picking another of the model's textures for a mesh that draws a colour map makes it draw that "
                           "one, saved with Ctrl+S (draws sharing its binding change with it). For other meshes, and (none), the "
                           "choice only changes the view.");
        ImGui::PopStyleColor();
        int bindMesh = -1, bindTo = -1;
        for (size_t i = 0; i < model_.meshes.size(); ++i) {
            auto& m = model_.meshes[i];
            if (!groupShown_[size_t(m.group)]) continue;
            ImGui::PushID(int(i));
            bool vis = meshVisible_[i];
            if (ImGui::Checkbox("##vis", &vis)) { meshVisible_[i] = vis; applyMeshState(); }
            ImGui::SameLine();
            if (meshTex_[i] < 0 && m.hasColour) {
                ImGui::ColorButton("##col", ImVec4(m.colour[0], m.colour[1], m.colour[2], 1), ImGuiColorEditFlags_NoTooltip,
                                   ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
                ImGui::SameLine();
            }
            ImGui::Text("%s  %zu tris%s", m.material.empty() ? ("mesh " + std::to_string(i)).c_str() : m.material.c_str(), m.indices.size() / 3,
                        furred(m) ? "  fur" : "");
            if (ImGui::BeginItemTooltip()) {
                ImGui::Text("mesh %zu in %s\n%zu vertices, layout %s", i, groupLabel(size_t(m.group)).c_str(), m.vertices.size(), layoutText(m).c_str());
                if (m.textures.empty()) ImGui::TextUnformatted("No textures bound.");
                for (int t : m.textures) ImGui::BulletText("%s", shortTex(model_.textures[size_t(t)]).c_str());
                for (auto& c : m.constants)
                    ImGui::Text("%s = %.3g %.3g %.3g %.3g", c.name.c_str(), c.value[0], c.value[1], c.value[2], c.value[3]);
                ImGui::EndTooltip();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            std::string cur = meshTex_[i] < 0 ? "(none)" : shortTex(model_.textures[size_t(meshTex_[i])]);
            const bool bindable = m.texture >= 0 && !m.source.samplers.empty();
            ImGui::BeginDisabled(m.layout.uvSets.empty());
            if (ImGui::BeginCombo("##tex", cur.c_str())) {
                if (ImGui::Selectable(bindable ? "(none, view only)" : "(none)", meshTex_[i] < 0)) { meshTex_[i] = -1; applyMeshState(); }
                // Textures bound to this draw first, then the rest of the model's. Fur volumes (3D) can't be a colour map.
                auto item = [&](size_t t) {
                    ImGui::PushID(int(t));
                    ImGui::BeginDisabled(bindable && textures_[t].volumeLayers > 0);
                    if (ImGui::Selectable(shortTex(model_.textures[t]).c_str(), meshTex_[i] == int(t))) {
                        if (bindable && int(t) != m.texture) bindMesh = int(i), bindTo = int(t);
                        else { meshTex_[i] = int(t); applyMeshState(); }
                    }
                    ImGui::EndDisabled();
                    ImGui::PopID();
                };
                for (int t : m.textures) item(size_t(t));
                if (!m.textures.empty()) ImGui::Separator();
                for (size_t t = 0; t < model_.textures.size(); ++t)
                    if (std::find(m.textures.begin(), m.textures.end(), int(t)) == m.textures.end()) item(t);
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (bindMesh >= 0) bindTexture(app, size_t(bindMesh), bindTo);
    }

    void drawTextureList(App& app) {
        ImGui::SeparatorText("Textures");
        for (size_t i = 0; i < textures_.size(); ++i) {
            auto& t = textures_[i];
            ImGui::PushID(int(i) + 10000);
            if (t.gl) {
                ImGui::Image(t.gl->imgui(), ImVec2(em(40), em(40)));
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            ImGui::TextUnformatted(shortTex(t.name).c_str());
            if (!t.error.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", t.error.c_str());
            else if (ImGui::SmallButton("Open")) app.openTexture(t.name);
            ImGui::EndGroup();
            ImGui::PopID();
        }
    }

    // ---- animation ----------------------------------------------------------------

    void computeBindPose() {
        size_t n = model_.bones.size();
        bindWorld_.assign(n, identity4());
        bindInverse_.assign(n, identity4());
        const float one[3] = {1, 1, 1};
        for (size_t i = 0; i < n; ++i) {
            const auto& b = model_.bones[i];
            Mat4 local = trs(b.local, b.rotation, one);
            bindWorld_[i] = b.parent >= 0 && size_t(b.parent) < i ? mul4(bindWorld_[size_t(b.parent)], local) : local;
            if (b.parent >= 0 && size_t(b.parent) >= i) bindWorld_[i] = trs(b.world, b.rotation, one); // parent listed later
            bindInverse_[i] = rigidInverse(bindWorld_[i]);
        }
    }

    // A new motion for an animation (a JSON timeline, animkeys.hpp): previewed here, written on save.
    struct MotionEdit {
        std::string json;
        fs::path source; // the JSON file, for Reload
        bool fromOriginal = true;
        std::vector<std::string> notes;
    };
    struct AnimLoad {
        std::shared_ptr<model::Animation> anim;
        std::vector<std::string> notes; // from applying a motion
    };

    // Loads an animation, with `motion` (or its unsaved one) applied to it.
    void loadAnimation(const std::string& name, std::optional<MotionEdit> motion = std::nullopt) {
        const ModelInfo* info = catalog_ ? catalog_->anim(name) : nullptr;
        if (!info || info->locs.empty()) return;
        if (!motion)
            if (auto it = motions_.find(name); it != motions_.end()) motion = it->second;
        AssetLoc loc = info->locs.front();
        pendingName_ = name;
        pendingMotion_ = motion;
        animError_.clear();
        pending_ = std::async(std::launch::async, [loc, name, motion] {
            CaffCache cache;
            if (!motion) {
                caff::Caff& c = cache.get(loc);
                return AnimLoad{std::make_shared<model::Animation>(model::parseAnimation(c, loc.symbol)), {}};
            }
            auto a = std::make_shared<model::Animation>(motionBase(loc, name, motion->fromOriginal, cache));
            model::KeysReport r;
            a->keys = model::keysFromJson(*a, motion->json, &r);
            std::vector<std::string> notes{std::to_string(r.bones) + " bone(s) keyed from " + std::to_string(r.entries) + " timeline entr" +
                                           (r.entries == 1 ? "y" : "ies")};
            notes.insert(notes.end(), r.notes.begin(), r.notes.end());
            return AnimLoad{a, notes};
        });
    }

    // Reads a JSON timeline and previews it on the animation `name`.
    void applyMotionFile(App& app, const std::string& name, const fs::path& p, bool fromOriginal) {
        MotionEdit e;
        try {
            e.json = readText(p);
        } catch (std::exception& ex) {
            app.error(ex.what());
            return;
        }
        e.source = p;
        e.fromOriginal = fromOriginal;
        loadAnimation(name, e);
    }

    // Track of each model bone, matched by name.
    void bindTracks() {
        trackOf_.assign(model_.bones.size(), -1);
        matched_ = 0;
        std::map<std::string, int> byName;
        for (size_t t = 0; t < anim_->boneNames.size(); ++t) byName.emplace(anim_->boneNames[t], int(t));
        for (size_t b = 0; b < model_.bones.size(); ++b) {
            auto it = byName.find(model_.bones[b].name);
            if (it != byName.end()) {
                trackOf_[b] = it->second;
                ++matched_;
            }
        }
    }

    void updateAnimation() {
        if (pending_.valid() && pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                AnimLoad r = pending_.get();
                anim_ = r.anim;
                animName_ = pendingName_;
                if (pendingMotion_) {
                    pendingMotion_->notes = r.notes;
                    motions_[animName_] = *pendingMotion_;
                }
                bindTracks();
                animTime_ = 0;
                playing_ = true;
                // The game plays it too (its switch tracks show parts such as Banjo's Kazooie).
                if (view_) view_->playGameAnimation(assetdb::idFromName(animName_).value_or(0));
            } catch (std::exception& e) {
                animError_ = (pendingMotion_ ? "new motion: " : "") + std::string(e.what());
                if (!pendingMotion_) anim_.reset(); // a failed motion leaves the animation shown as it was
            }
            pendingName_.clear();
            pendingMotion_.reset();
        }
        if (!anim_ || !view_) return;
        if (playing_) {
            animTime_ += ImGui::GetIO().DeltaTime * speed_;
            float len = std::max(anim_->duration, 1e-3f);
            if (animTime_ > len) {
                if (loop_) animTime_ = std::fmod(animTime_, len);
                else {
                    animTime_ = len;
                    playing_ = false;
                }
            }
        }
        applyPose(animTime_);
    }

    // Model bones posed at time t: matched bones take the animation's rotation and scale and add
    // its translation to their bind offset; the others keep the bind pose.
    void applyPose(float t) {
        const auto& a = *anim_;
        size_t n = model_.bones.size();
        if (a.frames <= 0 || a.keys.empty() || n == 0) return;
        // Stored frames are not always evenly spaced (the last one sits on the final authored frame).
        int f0 = 0, f1 = 0;
        float w = 0;
        if (a.frameTimes.size() == size_t(a.frames) && a.frames > 1) {
            auto it = std::upper_bound(a.frameTimes.begin(), a.frameTimes.end(), t);
            f1 = std::clamp(int(it - a.frameTimes.begin()), 1, a.frames - 1);
            f0 = f1 - 1;
            float span = a.frameTimes[size_t(f1)] - a.frameTimes[size_t(f0)];
            w = span > 0 ? std::clamp((t - a.frameTimes[size_t(f0)]) / span, 0.0f, 1.0f) : 0.0f;
        }
        std::vector<Mat4> world(n);
        std::vector<bool> done(n, false);
        std::function<void(size_t)> solve = [&](size_t i) {
            if (done[i]) return;
            const auto& b = model_.bones[i];
            float tr[3], q[4], s[3] = {1, 1, 1};
            std::copy(b.local, b.local + 3, tr);
            std::copy(b.rotation, b.rotation + 4, q);
            int k = trackOf_[i];
            if (k >= 0 && uint32_t(k) < a.bones) {
                const auto& k0 = a.keys[size_t(f0) * a.bones + size_t(k)];
                const auto& k1 = a.keys[size_t(f1) * a.bones + size_t(k)];
                float dot = 0;
                for (int c = 0; c < 4; ++c) dot += k0.rotation[c] * k1.rotation[c];
                float sign = dot < 0 ? -1.0f : 1.0f;
                for (int c = 0; c < 4; ++c) q[c] = k0.rotation[c] * (1 - w) + sign * k1.rotation[c] * w;
                for (int c = 0; c < 3; ++c) {
                    tr[c] += k0.translation[c] * (1 - w) + k1.translation[c] * w;
                    s[c] = k0.scale[c] * (1 - w) + k1.scale[c] * w;
                }
            }
            Mat4 local = trs(tr, q, s);
            done[i] = true;
            if (b.parent >= 0 && size_t(b.parent) < n && size_t(b.parent) != i) {
                solve(size_t(b.parent));
                world[i] = mul4(world[size_t(b.parent)], local);
            } else {
                world[i] = local;
            }
        };
        for (size_t i = 0; i < n; ++i) solve(i);
        skin_.resize(n * 16);
        boneWorld_.resize(n * 3);
        for (size_t i = 0; i < n; ++i) {
            Mat4 m = mul4(world[i], bindInverse_[i]);
            std::copy(m.begin(), m.end(), &skin_[i * 16]);
            boneWorld_[i * 3] = world[i][12];
            boneWorld_[i * 3 + 1] = world[i][13];
            boneWorld_[i * 3 + 2] = world[i][14];
        }
        view_->setPose(&skin_, &boneWorld_);
    }

    void stopAnimation() {
        anim_.reset();
        animName_.clear();
        playing_ = false;
        if (view_) {
            view_->setPose(nullptr, nullptr);
            view_->playGameAnimation(0);
        }
    }

    // Replacing the shown animation's motion with a JSON timeline.
    void drawMotion(App& app) {
        const std::string name = animName_;
        if (name.empty()) return;
        bool busy = pending_.valid();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Replace motion...")) {
            bool orig = motionFromOriginal_;
            app.openFileDialog("New motion for " + shortAnimName(name, animPrefix_), {{"Animation timeline", "json"}},
                               [this, &app, name, orig](const fs::path& p) { applyMotionFile(app, name, p, orig); });
        }
        ImGui::EndDisabled();
        helpMarker("Gives this animation new motion from a JSON timeline of poses (reNut's docs/nuttool/animations.md), played here straight away. "
                   "Ctrl+S writes it into every bundle holding the animation. It keeps its name and length, so the game plays your "
                   "motion wherever it played the original.");
        ImGui::SameLine();
        ImGui::Checkbox("Build on the original", &motionFromOriginal_);
        helpMarker("Keys the game's original animation, read from the bundle's .bak (made before NutTool first writes it), so a new try "
                   "doesn't build on an earlier one. Without a .bak, or with this off, it builds on the animation as the bundle has it now.");
        auto it = motions_.find(name);
        if (it == motions_.end()) return;
        ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "New motion from %s, not saved yet (Ctrl+S)", toUtf8(it->second.source.filename()).c_str());
        ImGui::BeginDisabled(busy);
        if (ImGui::SmallButton("Reload")) {
            fs::path src = it->second.source;
            applyMotionFile(app, name, src, motionFromOriginal_);
        }
        helpMarker("Reads the JSON file again, after you edit it.");
        ImGui::SameLine();
        bool discard = ImGui::SmallButton("Discard");
        ImGui::EndDisabled();
        if (discard) {
            motions_.erase(it);
            loadAnimation(name);
            return;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        for (auto& n : it->second.notes) ImGui::TextWrapped("%s", n.c_str());
        ImGui::PopStyleColor();
    }

    void drawAnimation(App& app) {
        ImGui::SeparatorText("Animation");
        if (model_.bones.empty()) {
            ImGui::TextDisabled("This model has no skeleton.");
            return;
        }
        if (!pendingName_.empty()) ImGui::TextDisabled("Loading %s...", shortAnimName(pendingName_, animPrefix_).c_str());
        if (!animError_.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", animError_.c_str());
        if (anim_) {
            ImGui::TextUnformatted(shortAnimName(animName_, animPrefix_).c_str());
            ImGui::TextDisabled("%d frames (%d stored), %.2f s, %u bones (%d of this model's %zu matched)", anim_->authoredFrames, anim_->frames, anim_->duration,
                                anim_->bones, matched_, model_.bones.size());
            if (ImGui::Button(playing_ ? "Pause" : "Play", ImVec2(em(60), 0))) {
                if (!playing_ && animTime_ >= anim_->duration && !loop_) animTime_ = 0;
                playing_ = !playing_;
            }
            ImGui::SameLine();
            if (ImGui::Button("Bind pose")) stopAnimation();
            if (anim_) {
                ImGui::SameLine();
                ImGui::Checkbox("Loop", &loop_);
                ImGui::SetNextItemWidth(-FLT_MIN);
                char label[48];
                std::snprintf(label, sizeof label, "%.2f / %.2f s", animTime_, anim_->duration);
                if (ImGui::SliderFloat("##time", &animTime_, 0, std::max(anim_->duration, 0.001f), label)) playing_ = false;
                ImGui::SetNextItemWidth(em(120));
                ImGui::SliderFloat("Speed", &speed_, 0.1f, 2.0f, "%.2fx");
                for (auto& w : anim_->warnings) ImGui::TextDisabled("note: %s", w.c_str());
                drawMotion(app);
            }
        }
        // Choice of animation: this model's own, or any by search.
        ImGui::SetNextItemWidth(-FLT_MIN);
        inputText("##animfilter", animFilter_);
        if (animFilter_.empty() && !ImGui::IsItemActive()) {
            ImVec2 pos = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + ImGui::GetStyle().FramePadding.x, pos.y + ImGui::GetStyle().FramePadding.y),
                                                ImGui::GetColorU32(ImGuiCol_TextDisabled), ownAnims_.empty() ? "Search all animations..." : "Filter...");
        }
        helpMarker("Lists the animations named after this model. Type to filter; with no match it searches every animation in the game "
                   "(bones are matched by name, so other characters' animations work only partly).");
        std::vector<const std::string*> shown;
        std::string needle = lower(animFilter_);
        for (auto& n : ownAnims_)
            if (needle.empty() || lower(n).find(needle) != std::string::npos) shown.push_back(&n);
        bool global = shown.empty() && !needle.empty() && catalog_;
        if (global)
            for (auto& a : catalog_->anims)
                if (lower(a.name).find(needle) != std::string::npos) shown.push_back(&a.name);
        if (shown.empty()) {
            ImGui::TextDisabled(ownAnims_.empty() ? "No animations are named after this model; search to pick any." : "No match.");
            return;
        }
        ImGui::TextDisabled("%zu animation(s)%s", shown.size(), global ? " in the whole game" : "");
        float rowH = ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("##anims", ImVec2(0, std::min(rowH * float(shown.size()) + em(8), em(260))), ImGuiChildFlags_Borders);
        ImGuiListClipper clip;
        clip.Begin(int(shown.size()), rowH);
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const std::string& n = *shown[size_t(i)];
                ImGui::PushID(i);
                if (ImGui::Selectable(shortAnimName(n, global ? "" : animPrefix_).c_str(), n == animName_)) loadAnimation(n);
                if (ImGui::BeginItemTooltip()) {
                    ImGui::TextUnformatted(n.c_str());
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
        ImGui::EndChild();
    }

    // Detail level and switches (the game's own visibility rules), plus per-group overrides.
    void drawParts() {
        ImGui::SeparatorText("Parts");
        bool changed = false;
        if (model_.lodCount > 1) {
            ImGui::SetNextItemWidth(em(120));
            changed |= ImGui::SliderInt("Detail level", &lod_, 0, model_.lodCount - 1);
            helpMarker("0 is the most detailed. The game switches to lower levels with distance.");
        }
        for (auto& [id, states] : model_.switches) {
            ImGui::PushID(id);
            ImGui::SetNextItemWidth(em(200));
            std::string cur = switchLabel(id, switchState_[id]);
            if (ImGui::BeginCombo(("Switch " + std::to_string(id)).c_str(), cur.c_str())) {
                for (int s : states)
                    if (ImGui::Selectable(switchLabel(id, s).c_str(), switchState_[id] == s)) {
                        switchState_[id] = s;
                        changed = true;
                    }
                // A state no node uses: hides every alternative (e.g. goggles taken off).
                if (ImGui::Selectable("off", switchState_[id] == kSwitchOff)) {
                    switchState_[id] = kSwitchOff;
                    changed = true;
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        if (!model_.switches.empty())
            helpMarker("Switches pick between alternative parts the game shows depending on the situation (a held tool, an open or closed lid...).");
        if (changed) {
            resetGroups();
            applyMeshState();
            if (view_) view_->setSwitches(switchState_);
        }
        if (ImGui::SmallButton("Show everything")) {
            groupShown_.assign(model_.groups.size(), true);
            applyMeshState();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Back to the game's choice")) {
            resetGroups();
            applyMeshState();
        }
        if (!ImGui::TreeNode("##groups", "Draw groups (%zu)", model_.groups.size())) return;
        float rowH = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("##parts", ImVec2(0, std::min(rowH * float(model_.groups.size()) + em(8), em(220))), ImGuiChildFlags_Borders);
        ImGuiListClipper clip;
        clip.Begin(int(model_.groups.size()), rowH);
        while (clip.Step()) {
            for (int g = clip.DisplayStart; g < clip.DisplayEnd; ++g) {
                const auto& grp = model_.groups[size_t(g)];
                bool on = groupShown_[size_t(g)];
                std::string label = groupLabel(size_t(g)) + "  " + std::to_string(grp.triangles) + " tris###grp" + std::to_string(g);
                ImGui::BeginDisabled(grp.meshes.empty());
                if (ImGui::Checkbox(label.c_str(), &on)) {
                    groupShown_[size_t(g)] = on;
                    applyMeshState();
                }
                ImGui::EndDisabled();
                if (ImGui::BeginItemTooltip()) {
                    ImGui::Text("group %d, %zu mesh(es)", g, grp.meshes.size());
                    for (int n : grp.path) ImGui::BulletText("%s", model_.nodes[size_t(n)].name.c_str());
                    ImGui::EndTooltip();
                }
            }
        }
        ImGui::EndChild();
        ImGui::TreePop();
    }

    void drawLighting() {
        if (!ImGui::TreeNode("Lighting")) return;
        ImGui::SetNextItemWidth(em(140));
        ImGui::Combo("Mode", &opt_.lighting, "Studio (key, fill, rim)\0Headlight\0Unlit\0");
        helpMarker("Studio: three lights around the model. Headlight: one light at the camera. Unlit: flat texture colours.");
        ImGui::BeginDisabled(opt_.lighting == 2);
        ImGui::SetNextItemWidth(em(140));
        ImGui::SliderFloat("Brightness", &opt_.brightness, 0, 2.5f, "%.2f");
        ImGui::SetNextItemWidth(em(140));
        ImGui::SliderFloat("Ambient", &opt_.ambient, 0, 1, "%.2f");
        ImGui::SetNextItemWidth(em(140));
        ImGui::ColorEdit3("Light colour", opt_.lightColour, ImGuiColorEditFlags_NoInputs);
        ImGui::EndDisabled();
        if (opt_.lighting == 0) {
            ImGui::Checkbox("Lights follow the camera", &opt_.lightFollowsCamera);
            ImGui::BeginDisabled(opt_.lightFollowsCamera);
            ImGui::SetNextItemWidth(em(140));
            ImGui::SliderFloat("Direction", &opt_.lightAzimuth, -180, 180, "%.0f deg");
            ImGui::SetNextItemWidth(em(140));
            ImGui::SliderFloat("Height", &opt_.lightElevation, -89, 89, "%.0f deg");
            ImGui::EndDisabled();
        }
        if (ImGui::SmallButton("Reset lighting")) {
            ModelView::Options def;
            opt_.lighting = def.lighting;
            opt_.lightFollowsCamera = def.lightFollowsCamera;
            opt_.lightAzimuth = def.lightAzimuth;
            opt_.lightElevation = def.lightElevation;
            opt_.brightness = def.brightness;
            opt_.ambient = def.ambient;
            std::copy(def.lightColour, def.lightColour + 3, opt_.lightColour);
        }
        ImGui::TreePop();
    }

    void drawBoneTree(int parent, int depth) {
        if (depth > 64) return;
        for (size_t i = 0; i < model_.bones.size(); ++i) {
            const auto& b = model_.bones[i];
            if (b.parent != parent) continue;
            bool hasChildren = false;
            for (auto& c : model_.bones) hasChildren |= c.parent == int(i);
            ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_DefaultOpen | (hasChildren ? 0 : ImGuiTreeNodeFlags_Leaf);
            if (collapsingAll() && hasChildren) ImGui::SetNextItemOpen(false);
            bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(intptr_t(i + 1)), f, "%s", b.name.c_str());
            if (ImGui::BeginItemTooltip()) {
                ImGui::Text("bone %zu\nposition %.3f %.3f %.3f", i, b.world[0], b.world[1], b.world[2]);
                ImGui::EndTooltip();
            }
            if (open) {
                drawBoneTree(int(i), depth + 1);
                ImGui::TreePop();
            }
        }
    }

    static std::string shortTex(const std::string& n) {
        for (const char* p : {"aid_texture_banjox_", "aid_texture_"})
            if (startsWith(n, p)) return n.substr(std::string(p).size());
        return n;
    }

    // ---- FBX / OBJ export ---------------------------------------------------------

    void drawExport(App& app, bool obj) {
        if (!ImGui::BeginPopup(obj ? "Export OBJ" : "Export FBX")) return;
        if (!obj) {
            ImGui::TextUnformatted("Animations");
            std::string own = "Named after this model (" + std::to_string(ownAnims_.size()) + ")";
            ImGui::BeginDisabled(ownAnims_.empty());
            ImGui::RadioButton(own.c_str(), &fbxAnims_, 1);
            ImGui::EndDisabled();
            std::string cur = anim_ ? "Only the one playing (" + shortAnimName(animName_, animPrefix_) + ")" : std::string("Only the one playing (none)");
            ImGui::BeginDisabled(!anim_);
            ImGui::RadioButton(cur.c_str(), &fbxAnims_, 2);
            ImGui::EndDisabled();
            ImGui::RadioButton("None", &fbxAnims_, 0);
            if (fbxAnims_ == 1 && ownAnims_.empty()) fbxAnims_ = 0;
            if (fbxAnims_ == 2 && !anim_) fbxAnims_ = ownAnims_.empty() ? 0 : 1;
        }
        if (hasFur_) {
            if (!obj) ImGui::Separator();
            ImGui::Checkbox("Fur shells", &fbxFur_);
            helpMarker("Adds the fur as the viewer draws it: one shell mesh per layer of the fur volume, pushed out along the "
                       "surface by the Fur length and tiled by the Tiling set in the viewer, skinned like its mesh. Each layer "
                       "gets a baked texture (the fur colour, with alpha = where the layer has fur), so any program shows it "
                       "with an ordinary alpha-tested material.");
        }
        if (!obj && !model_.bones.empty()) {
            ImGui::Separator();
            ImGui::Checkbox("Reorient bones", &fbxReorient_);
            helpMarker("Turns each bone so its Bone axis runs along it, towards its child (the one with the most bones under it), "
                       "so the bones point along the limbs in Blender and other programs. The skin, bind pose and animations are "
                       "written to match: the model and its motion look the same. Off keeps the game's bone frames.");
            ImGui::BeginDisabled(!fbxReorient_);
            static const char* axes[] = {"+X", "+Y (Blender)", "+Z", "-X", "-Y", "-Z"};
            ImGui::SetNextItemWidth(140);
            ImGui::Combo("Bone axis", &fbxAxis_, axes, IM_ARRAYSIZE(axes));
            helpMarker("The bone's local axis that points down the bone. Blender's bones run along +Y; for other programs pick the "
                       "axis their importer expects (often +X), and set the same axis in the importer's bone axis options.");
            ImGui::EndDisabled();
            ImGui::Checkbox("Short end bones", &fbxEnds_);
            helpMarker("Blender makes each bone as long as the average distance to its children, and a bone without children as "
                       "long as its parent, so wrench and face bones come out huge. This adds a short <bone>_end child to each "
                       "bone without children, so it draws as a small stub. Tick Ignore Leaf Bones in Blender's importer to hide "
                       "them; the stubs stay. They have no weights and NutTool's import ignores them.");
            ImGui::BeginDisabled(!fbxEnds_);
            ImGui::SetNextItemWidth(140);
            ImGui::SliderFloat("End length", &fbxEndPct_, 0.2f, 5.0f, "%.1f%%");
            helpMarker("The stubs' length, in percent of the skeleton's size.");
            ImGui::EndDisabled();
        }
        ImGui::Separator();
        if (ImGui::Button("Save as...")) {
            ImGui::CloseCurrentPopup();
            exportFile(app, obj);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        // Tools > Export a model has the command line's other options (every animation, another bundle's, fur texture size...).
        const AssetLoc* bundle = nullptr;
        for (auto& l : info_.locs)
            if (l.entry < 0 && !bundle) bundle = &l;
        ImGui::SameLine();
        ImGui::BeginDisabled(!bundle);
        if (ImGui::Button("More options...")) {
            ImGui::CloseCurrentPopup();
            app.openModelExport(bundle->file, info_.name);
        }
        ImGui::EndDisabled();
        helpMarker("Opens Tools > Export a model for this model's bundle, with the command line's other options: every animation, "
                   "animations from another bundle, no textures, the fur texture size. It exports the model as the bundle holds it, "
                   "not the meshes shown here.");
        ImGui::EndPopup();
    }

    void exportFile(App& app, bool obj) {
        std::string ext = obj ? "obj" : "fbx";
        app.saveFileDialog(obj ? "Export OBJ" : "Export FBX", {{obj ? "OBJ model" : "FBX model", ext}}, [this, &app, obj, ext](const fs::path& picked) {
            fs::path out = picked;
            if (lower(out.extension().string()) != "." + ext) out += "." + ext;
            exportTo(app, out, obj);
        }, exportName(model_.name, ext));
    }

    void exportFbxTo(App& app, const fs::path& out) { exportTo(app, out, false); }

    void exportTo(App& app, const fs::path& out, bool obj) {
        // Everything the worker needs, copied: the tab may change or close meanwhile.
        auto m = std::make_shared<model::Model>(model_);
        for (size_t i = 0; i < m->meshes.size() && i < meshTex_.size(); ++i) m->meshes[i].texture = meshTex_[i];
        std::vector<bool> include(m->meshes.size());
        for (size_t i = 0; i < include.size(); ++i) include[i] = meshShown(i);
        std::vector<std::string> names;
        std::shared_ptr<model::Animation> playing;
        if (!obj && fbxAnims_ == 1) names = ownAnims_;
        if (!obj && fbxAnims_ == 2) playing = anim_;
        std::string playingName = shortAnimName(animName_, animPrefix_), prefix = animPrefix_;
        auto catalog = catalog_;
        bool fur = hasFur_ && fbxFur_;
        float furLength = opt_.furLength, furTiling = opt_.furTiling;
        bool tint = opt_.tint, reorient = fbxReorient_, ends = fbxEnds_;
        int boneAxis = fbxAxis_;
        float endSize = fbxEndPct_ / 100;
        app.runTask("Exporting " + toUtf8(out.filename()), false,
                    [&app, m, include, names, playing, playingName, prefix, catalog, out, fur, furLength, furTiling, tint, reorient, boneAxis, ends,
                     endSize, obj](Task& t) -> std::function<void()> {
            CaffCache cache;
            std::vector<std::shared_ptr<model::Animation>> anims;
            fbx::Options opt;
            opt.include = include;
            opt.fur = fur;
            opt.tint = tint;
            opt.reorient = reorient;
            opt.boneAxis = boneAxis;
            opt.boneEnds = ends;
            opt.boneEndSize = endSize;
            opt.furLength = furLength;
            opt.furTiling = furTiling;
            std::vector<std::string> notes;
            for (size_t i = 0; i < names.size() && catalog; ++i) {
                if (t.cancel) return {};
                t.report(0.8 * double(i) / double(names.size()), "Reading " + names[i]);
                const ModelInfo* info = catalog->anim(names[i]);
                if (!info) continue;
                try {
                    const AssetLoc& loc = info->locs.front();
                    anims.push_back(std::make_shared<model::Animation>(model::parseAnimation(cache.get(loc), loc.symbol)));
                    opt.animations.push_back({shortAnimName(names[i], prefix), anims.back().get()});
                } catch (std::exception& e) {
                    notes.push_back(names[i] + ": " + e.what());
                }
            }
            if (playing) opt.animations.push_back({playingName, playing.get()});
            // Textures at full size, straight from the game files.
            opt.texture = [&](const std::string& name) -> std::optional<Bytes> {
                const TextureInfo* ti = catalog ? catalog->texture(name) : nullptr;
                if (!ti) return std::nullopt;
                try {
                    ops::DecodedTexture d = decodeCatalogTexture(*ti, cache);
                    return image::encodePng(texture::toRgba(d.header, d.levels[0], 0));
                } catch (std::exception& e) {
                    notes.push_back(name + ": " + e.what());
                    return std::nullopt;
                }
            };
            // Every slice of level 0, for baking fur.
            opt.pixels = [&](const std::string& name) -> std::optional<std::vector<image::Rgba>> {
                const TextureInfo* ti = catalog ? catalog->texture(name) : nullptr;
                if (!ti) return std::nullopt;
                try {
                    ops::DecodedTexture d = decodeCatalogTexture(*ti, cache);
                    std::vector<image::Rgba> slices;
                    for (uint32_t s = 0; s < d.levels[0].slices.size(); ++s) slices.push_back(texture::toRgba(d.header, d.levels[0], s));
                    return slices;
                } catch (std::exception& e) {
                    notes.push_back(name + ": " + e.what());
                    return std::nullopt;
                }
            };
            t.report(0.85, "Writing " + toUtf8(out.filename()));
            fbx::Report r;
            if (obj) {
                std::string base = toUtf8(out.stem());
                fbx::ObjFiles files = fbx::writeObj(*m, opt, base, &r);
                writeText(out, files.obj);
                writeText(out.parent_path() / fromUtf8(base + ".mtl"), files.mtl);
                for (auto& [name, data] : files.files) writeFile(out.parent_path() / fromUtf8(name), data);
            } else {
                writeFile(out, fbx::write(*m, opt, &r));
            }
            for (auto& n : r.notes) notes.push_back(n);
            return [&app, r, notes, out, obj] {
                for (auto& n : notes) app.log((obj ? "OBJ export: " : "FBX export: ") + n);
                app.status("Exported " + toUtf8(out.filename()) + ": " + std::to_string(r.meshes) + " meshes, " + std::to_string(r.textures) +
                           " textures" + (r.bones ? ", " + std::to_string(r.bones) + " bones" : std::string()) +
                           (r.animations ? ", " + std::to_string(r.animations) + " animations" : std::string()) +
                           (r.furShells ? ", " + std::to_string(r.furShells) + " fur shells" : std::string()));
            };
        });
    }

    // ---- materials ------------------------------------------------------------------
    // Material parameters (c:basecolour, c:specularPower...) that share a material, a name and a value are edited
    // together: every detail level repeats the material with its own copy of the value.
    struct ColourGroup {
        std::string material, name;
        std::vector<std::pair<size_t, size_t>> uses; // mesh, constant
        std::array<float, 4> original;
        bool colour = false; // edited with a colour picker
    };
    // The groups of one material (by name; unnamed materials are listed per mesh), for the editor.
    struct MaterialEntry {
        std::string label;
        std::vector<size_t> groups;
        std::set<size_t> meshes;
        std::string search; // lower-case: label, parameter and texture names
    };

    void collectColours() {
        std::map<std::string, size_t> byKey;
        for (size_t i = 0; i < model_.meshes.size(); ++i) {
            const auto& cs = model_.meshes[i].constants;
            for (size_t k = 0; k < cs.size(); ++k) {
                if (cs[k].symbol < 0) continue;
                std::string key = model_.meshes[i].material + '\x1f' + (model_.meshes[i].material.empty() ? std::to_string(i) : "") + '\x1f' +
                                  cs[k].name + '\x1f' + std::string(reinterpret_cast<const char*>(cs[k].value.data()), 16);
                auto [it, added] = byKey.emplace(key, colourGroups_.size());
                if (added) {
                    std::string ln = lower(cs[k].name);
                    bool colour = ln.find("colour") != std::string::npos || ln.find("color") != std::string::npos;
                    colourGroups_.push_back({model_.meshes[i].material, cs[k].name, {}, cs[k].value, colour});
                }
                colourGroups_[it->second].uses.push_back({i, k});
            }
        }
        materials_.clear();
        std::map<std::string, size_t> byLabel;
        for (size_t gi = 0; gi < colourGroups_.size(); ++gi) {
            const ColourGroup& g = colourGroups_[gi];
            std::string label = g.material.empty() ? "mesh " + std::to_string(g.uses[0].first) + " (unnamed material)" : g.material;
            auto [it, added] = byLabel.emplace(label, materials_.size());
            if (added) materials_.push_back({label, {}, {}, lower(label)});
            MaterialEntry& e = materials_[it->second];
            e.groups.push_back(gi);
            e.search += '\x1f' + lower(g.name);
            for (auto [mi, ki] : g.uses)
                if (e.meshes.insert(mi).second)
                    for (int t : model_.meshes[mi].textures) e.search += '\x1f' + lower(model_.textures[size_t(t)]);
        }
        std::sort(materials_.begin(), materials_.end(), [](const MaterialEntry& a, const MaterialEntry& b) { return a.label < b.label; });
    }

    const std::array<float, 4>& colourOf(const ColourGroup& g) const {
        return model_.meshes[g.uses[0].first].constants[g.uses[0].second].value;
    }

    void setColour(size_t gi, const std::array<float, 4>& v) {
        ColourGroup& g = colourGroups_[gi];
        for (auto [mi, ki] : g.uses) {
            auto& mesh = model_.meshes[mi];
            mesh.constants[ki].value = v;
            if (g.name == "c:basecolour") std::copy(v.begin(), v.end(), mesh.colour);
            if (g.name == "c:specularColour") std::copy(v.begin(), v.begin() + 3, mesh.specular);
            if (g.name == "c:specularPower") mesh.specularPower = v[0];
            // The colour map's tint (model::Mesh::textureGain), as the parser picks it.
            for (size_t t = 0; t < mesh.textures.size() && t < mesh.textureSlots.size(); ++t) {
                if (mesh.textures[t] != mesh.texture) continue;
                std::string slot = std::to_string(mesh.textureSlots[t]);
                if (g.name == "c:textureGain" + slot) std::copy(v.begin(), v.end(), mesh.textureGain);
                if (g.name == "c:textureOffset" + slot) std::copy(v.begin(), v.end(), mesh.textureOffset);
            }
        }
        if (v == g.original) colourEdited_.erase(gi);
        else colourEdited_.insert(gi);
        if (view_) applyMeshState();
    }

    // A click on the model: the material of the mesh under the cursor becomes the one listed (-1: clears it).
    void pickMesh(App& app, int mesh) {
        pickedMesh_ = mesh;
        pickedMaterial_.clear();
        if (mesh < 0) return;
        for (auto& e : materials_)
            if (e.meshes.count(size_t(mesh))) pickedMaterial_ = e.label;
        scrollToPicked_ = true;
        const auto& m = model_.meshes[size_t(mesh)];
        app.status("Picked mesh " + std::to_string(mesh) + (m.material.empty() ? "" : " (" + m.material + ")") +
                   (pickedMaterial_.empty() ? ": no editable material values" : ""));
    }

    void drawColours() {
        hoveredMaterial_.clear();
        if (colourGroups_.empty()) return;
        ImGui::SeparatorText("Materials");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("A change applies to every mesh and detail level using that material value. Save (Ctrl+S) "
                           "writes it into the bundle; the viewport shows base colour, texture tint and specular changes.");
        ImGui::PopStyleColor();
        // Picking on the model narrows the list to that mesh's material.
        if (pickedMesh_ >= 0) {
            if (pickedMaterial_.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Mesh %d has no editable material values.", pickedMesh_);
            else ImGui::Text("Picked: %s", pickedMaterial_.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Show all materials")) {
                pickedMesh_ = -1;
                pickedMaterial_.clear();
            }
        } else {
            ImGui::TextDisabled("Click the model to pick the material under the cursor.");
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        bool searched = inputTextWithHint("##matsearch", "Search materials, parameters or textures", matFilter_);
        ImGui::Checkbox("Colours only", &matColoursOnly_);
        ImGui::SameLine();
        ImGui::Checkbox("Include hidden parts and detail levels", &allColours_);
        bool picked = !pickedMaterial_.empty();
        std::string f = picked ? std::string() : lower(matFilter_);
        int listed = 0;
        for (size_t mi = 0; mi < materials_.size(); ++mi) {
            const MaterialEntry& e = materials_[mi];
            if (picked && e.label != pickedMaterial_) continue;
            if (!f.empty() && e.search.find(f) == std::string::npos) continue;
            bool shown = allColours_ || picked;
            for (size_t m : e.meshes) shown |= meshShown(m);
            if (!shown) continue;
            // A search that names parameters of this material (not the material itself) lists only those.
            bool narrow = !f.empty() && lower(e.label).find(f) == std::string::npos;
            if (narrow) {
                bool paramHit = false;
                for (size_t gi : e.groups) paramHit |= lower(colourGroups_[gi].name).find(f) != std::string::npos;
                narrow = paramHit;
            }
            std::vector<size_t> rows;
            size_t edited = 0;
            for (size_t gi : e.groups) {
                const ColourGroup& g = colourGroups_[gi];
                edited += colourEdited_.count(gi);
                if (matColoursOnly_ && !g.colour) continue;
                if (narrow && lower(g.name).find(f) == std::string::npos) continue;
                bool used = allColours_ || picked;
                for (auto [m, k] : g.uses) used |= meshShown(m);
                if (used) rows.push_back(gi);
            }
            if (rows.empty()) continue;
            ++listed;
            // Every mesh with it draws a colour map, which takes the place of its base colour.
            std::set<int> maps;
            for (size_t m : e.meshes) maps.insert(model_.meshes[m].texture);
            const bool textured = !maps.empty() && !maps.count(-1);
            ImGui::PushID(int(mi));
            if ((searched && !f.empty()) || (picked && scrollToPicked_)) ImGui::SetNextItemOpen(true);
            std::string head = e.label + "  (" + std::to_string(e.meshes.size()) + " mesh" + (e.meshes.size() == 1 ? "" : "es") + ")" +
                               (textured ? "  textured" : "") + (edited ? "  *" : "") + "###mat";
            bool open = ImGui::TreeNodeEx(head.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
            if (ImGui::IsItemHovered()) hoveredMaterial_ = e.label;
            if (picked && scrollToPicked_) {
                ImGui::SetScrollHereY(0.2f);
                scrollToPicked_ = false;
            }
            if (ImGui::BeginItemTooltip()) {
                ImGui::TextUnformatted("Meshes:");
                for (size_t m : e.meshes) {
                    const auto& mesh = model_.meshes[m];
                    ImGui::BulletText("mesh %zu in %s, %zu tris", m, groupLabel(size_t(mesh.group)).c_str(), mesh.indices.size() / 3);
                }
                if (textured) {
                    ImGui::TextUnformatted("Textured: its meshes draw");
                    for (int t : maps) ImGui::BulletText("%s", shortTex(model_.textures[size_t(t)]).c_str());
                    ImGui::TextUnformatted("so its base colour isn't drawn; textureGain and textureOffset tint the texture.");
                }
                ImGui::EndTooltip();
            }
            if (edited) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Revert all"))
                    for (size_t gi : e.groups) setColour(gi, colourGroups_[gi].original);
            }
            if (open) {
                for (size_t gi : rows) {
                    const ColourGroup& g = colourGroups_[gi];
                    std::string pn = startsWith(g.name, "c:") ? g.name.substr(2) : g.name;
                    ImGui::PushID(int(gi));
                    std::array<float, 4> v = colourOf(g);
                    bool changed;
                    ImGui::SetNextItemWidth(em(260));
                    if (g.colour) changed = ImGui::ColorEdit3("##v", v.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                    else changed = ImGui::DragFloat4("##v", v.data(), 0.01f, 0, 0, "%.3g");
                    if (changed) setColour(gi, v);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(pn.c_str());
                    if (ImGui::BeginItemTooltip()) {
                        ImGui::Text("%s\nnow %.4g %.4g %.4g %.4g\nwas %.4g %.4g %.4g %.4g\n%zu mesh(es)", g.name.c_str(), v[0], v[1], v[2], v[3],
                                    g.original[0], g.original[1], g.original[2], g.original[3], g.uses.size());
                        ImGui::EndTooltip();
                    }
                    if (textured && g.name == "c:basecolour") {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "(not drawn: textured)");
                        if (ImGui::BeginItemTooltip()) {
                            ImGui::TextUnformatted("Every mesh with this material draws a texture in place of this colour, so changing "
                                                   "it shows nothing. Tint the texture with textureGain and textureOffset instead.");
                            ImGui::EndTooltip();
                        }
                    }
                    if (colourEdited_.count(gi)) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Revert")) setColour(gi, g.original);
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (!listed) ImGui::TextDisabled(f.empty() ? "No editable material values on the shown parts." : "Nothing matches the search.");
    }

    // ---- replacing geometry ---------------------------------------------------------
    // Imports and swaps are applied to a copy of the first bundle for the preview, and replayed on
    // every copy of the model when saving.
    void drawReplace(App& app) {
        ImGui::SeparatorText("Replace");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Import OBJ / FBX: objects named like the export's (group0_mesh3) edit that mesh in place when the vertex "
                           "and triangle counts match, otherwise replace its geometry (skinning comes from the FBX's weights on "
                           "bones named like this model's, else from the nearest original vertex); an object with no faces removes "
                           "the mesh. Replace copies another game model into this one's place.");
        ImGui::PopStyleColor();
        bool coloursPending = !colourEdited_.empty();
        ImGui::BeginDisabled(coloursPending);
        if (ImGui::Button("Import OBJ / FBX...")) {
            app.openFileDialog("Import OBJ / FBX", {{"OBJ or FBX", "obj;fbx"}}, [this, &app](const fs::path& p) {
                Bytes text;
                try {
                    text = readFile(p);
                } catch (std::exception& e) {
                    app.error(e.what());
                    return;
                }
                std::string name = toUtf8(p.filename());
                bool rig = importRig_;
                deferred_ = [this, &app, name, text, rig] { addChange(app, {Change::Kind::Import, name, text, rig}); };
            });
        }
        ImGui::SameLine();
        ImGui::Checkbox("Override rig", &importRig_);
        helpMarker("FBX only: also moves this model's joints to the file's bones with the same names, so its animations bend "
                   "where your rig's joints are. Bone orientations, the hierarchy and the bone count stay the game's (its "
                   "animations need them); bones the file lacks follow their parent, extra bones are ignored. The file may "
                   "hold just the skeleton.");
        ImGui::SameLine();
        if (ImGui::Button("Replace with another model...")) {
            swapFilter_.clear();
            ImGui::OpenPopup("##swap");
        }
        ImGui::EndDisabled();
        if (coloursPending) ImGui::TextDisabled("Save or revert the material changes first.");
        if (ImGui::BeginPopup("##swap")) {
            ImGui::TextUnformatted("Replace this model with:");
            ImGui::SetNextItemWidth(em(320));
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            inputText("##filter", swapFilter_);
            std::string f = lower(swapFilter_);
            if (ImGui::BeginChild("##models", ImVec2(em(420), em(300)))) {
                int listed = 0;
                if (catalog_)
                    for (auto& m : catalog_->models) {
                        if (m.name == info_.name || (!f.empty() && lower(m.name).find(f) == std::string::npos)) continue;
                        if (++listed > 500) {
                            ImGui::TextDisabled("... type to narrow the list");
                            break;
                        }
                        if (ImGui::Selectable(shortModelName(m.name).c_str())) {
                            std::string name = m.name;
                            deferred_ = [this, &app, name] { addChange(app, {Change::Kind::Swap, name, {}, false, 0}); };
                            ImGui::CloseCurrentPopup();
                        }
                    }
                if (!catalog_) ImGui::TextDisabled("The model list is still being built.");
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
        bool backup = false;
        for (auto& l : info_.locs) {
            fs::path bak = l.file;
            bak += ".bak";
            std::error_code ec;
            backup |= fs::exists(bak, ec);
        }
        ImGui::BeginDisabled(coloursPending || !backup);
        if (ImGui::Button("Restore the original model")) deferred_ = [this, &app] { addChange(app, {Change::Kind::Restore, {}, {}, false, 0}); };
        ImGui::EndDisabled();
        helpMarker(backup ? "Brings back the model as the game shipped it, from the .bak NutTool kept of its bundle before first saving "
                            "over it: its meshes, materials, rig and textures. Only this model: the bundle's other models keep their "
                            "changes. Ctrl+S writes it."
                          : "Needs the .bak NutTool keeps of a bundle the first time it saves over it (File > Make .bak backups when "
                            "saving); this model's bundle has none.");
        // Each change not saved yet, with its own undo: the rest are applied again without it.
        for (size_t i = 0; i < changes_.size(); ++i) {
            ImGui::PushID(int(i) + 20000);
            ImGui::BulletText("%s", changeLabel(changes_[i]).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Undo")) {
                std::vector<Change> rest = changes_;
                rest.erase(rest.begin() + std::ptrdiff_t(i));
                deferred_ = [this, &app, rest] { rebuild(app, rest); };
            }
            ImGui::PopID();
        }
        if (!replaceNotes_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            for (auto& n : replaceNotes_) ImGui::TextWrapped("%s", n.c_str());
            ImGui::PopStyleColor();
        }
        if (dirty()) {
            if (ImGui::Button("Discard changes")) deferred_ = [this] { discard(); };
            ImGui::SameLine();
            ImGui::TextDisabled("Not saved yet: Ctrl+S writes them into the bundle(s).");
        }
    }

    std::string changeLabel(const Change& ch) const {
        switch (ch.kind) {
        case Change::Kind::Swap: return "Replaced by " + shortModelName(ch.name);
        case Change::Kind::Restore: return "Restored the original model";
        case Change::Kind::Import: return "Imported " + ch.name + (ch.rig ? " (with its rig)" : "");
        case Change::Kind::Bind: return "Mesh " + std::to_string(ch.mesh) + " draws " + shortTex(ch.name);
        case Change::Kind::Fur: return std::string("Fur ") + (ch.on ? "on" : "off") + " in " + ch.name;
        case Change::Kind::Rig: return "Rig and weights edited";
        }
        return {};
    }

    // Applies one change to the model at `symbol` in `c` (a copy of the model at `at`); returns the streamed texture
    // levels it needs in the destination's stream bundle.
    std::vector<streambundle::Entry> applyChange(caff::Caff& c, uint32_t symbol, const AssetLoc& at, const Change& ch,
                                                 std::vector<std::string>* notes) {
        auto note = [&](const std::vector<std::string>& n) {
            if (notes) notes->insert(notes->end(), n.begin(), n.end());
        };
        switch (ch.kind) {
        case Change::Kind::Swap:
        case Change::Kind::Restore: {
            model::CopyResult r = ch.kind == Change::Kind::Swap ? copyFrom(c, symbol, at, ch.name) : restoreFrom(c, symbol, at);
            note(r.notes);
            return r.streamEntries;
        }
        case Change::Kind::Import: {
            model::ImportReport r = model::importMeshFile(c, symbol, ch.file, ch.rig);
            note({ch.name + ": " + std::to_string(r.edited) + " mesh(es) edited, " + std::to_string(r.rebuilt) + " rebuilt, " +
                  std::to_string(r.hidden) + " hidden"});
            note(r.notes);
            return {};
        }
        case Change::Kind::Bind: model::bindColourMap(c, symbol, ch.mesh, ch.name); return {};
        case Change::Kind::Fur: model::setFur(c, symbol, ch.block, ch.on); return {};
        case Change::Kind::Rig: note(rigedit::write(c, symbol, *ch.rigEdit)); return {};
        }
        return {};
    }

    // The rig editor's edits, made a change of the model (it is loaded again with them).
    void applyPendingRig(App& app) {
        if (!pendingRig_) return;
        auto r = std::move(pendingRig_);
        Change ch{Change::Kind::Rig, {}, {}, false, 0, 0, true, r};
        if (!addChange(app, std::move(ch))) rig_.markDirty();  // the edits stay in the editor to fix and apply again
    }

    // The models the rig editor can copy weights from: this one as saved (before the changes listed) and the game's own.
    std::vector<RigEditor::Source> rigSources() {
        std::vector<RigEditor::Source> out;
        const AssetLoc& loc = info_.locs.front();
        auto addFrom = [&](const caff::Caff& c, uint32_t symbol, const std::string& label) {
            try {
                auto m = std::make_shared<model::Model>(model::parse(c, symbol));
                auto r = std::make_shared<rigedit::Rig>(rigedit::read(c, symbol, *m));
                if (!r->bones.empty()) out.push_back({label, m, r});
            } catch (const std::exception&) {
            }
        };
        if (preview_) addFrom(cache_.get(loc), loc.symbol, "This model as saved (before the changes above)");
        fs::path bak = loc.file;
        bak += ".bak";
        std::error_code ec;
        if (fs::exists(bak, ec)) {
            try {
                Loaded l = loadAsset(bak);
                std::shared_ptr<caff::Caff> original;
                if (loc.entry < 0 && l.caff) original = std::make_shared<caff::Caff>(std::move(*l.caff));
                else if (loc.entry >= 0 && l.sb && size_t(loc.entry) < l.sb->entries.size())
                    original = std::make_shared<caff::Caff>(caff::Caff::parse(l.sb->entries[size_t(loc.entry)].data));
                if (original)
                    for (size_t s = 0; s < original->symbols.size(); ++s)
                        if (model::isModelSymbol(*original, s) && assetdb::baseAssetName(original->symbolName(s)) == info_.name) {
                            addFrom(*original, uint32_t(s), "The game's original (" + toUtf8(bak.filename()) + ")");
                            break;
                        }
            } catch (const std::exception&) {
            }
        }
        return out;
    }

    // Applies `ch` to the preview and lists it; with `reload` the model is read again from it. A whole model copied in
    // (Swap, Restore) starts from the disk copy, replacing what the earlier changes did.
    bool addChange(App& app, Change ch, bool reload = true) {
        const AssetLoc& loc = info_.locs.front();
        const bool whole = ch.kind == Change::Kind::Swap || ch.kind == Change::Kind::Restore;
        try {
            caff::Caff trial = preview_ && !whole ? *preview_ : cache_.get(loc);
            std::vector<std::string> notes;
            applyChange(trial, loc.symbol, loc, ch, &notes);
            preview_ = std::make_unique<caff::Caff>(std::move(trial));
            if (whole) changes_.clear(), replaceNotes_.clear();
            app.status(notes.empty() ? changeLabel(ch) : notes.front());
            changes_.push_back(std::move(ch));
            replaceNotes_.insert(replaceNotes_.end(), notes.begin(), notes.end());
            if (reload) loadModel(*preview_, loc.symbol);
            return true;
        } catch (std::exception& e) {
            app.error(std::string("could not apply it: ") + e.what());
            return false;
        }
    }

    // The preview made again from the disk copy with only `changes` (an undo); nothing changes when one fails.
    bool rebuild(App& app, const std::vector<Change>& changes) {
        const AssetLoc& loc = info_.locs.front();
        try {
            caff::Caff trial = cache_.get(loc);
            std::vector<std::string> notes;
            for (const Change& ch : changes) applyChange(trial, loc.symbol, loc, ch, &notes);
            preview_ = changes.empty() ? nullptr : std::make_unique<caff::Caff>(std::move(trial));
            changes_ = changes;
            replaceNotes_ = notes;
            loadModel(preview_ ? *preview_ : cache_.get(loc), loc.symbol);
            app.status("Change undone");
            return true;
        } catch (std::exception& e) {
            app.error(std::string("could not undo it: ") + e.what());
            return false;
        }
    }

    // The model as the game shipped it: the same model in the .bak NutTool kept of its file before first saving over it.
    model::CopyResult restoreFrom(caff::Caff& c, uint32_t symbol, const AssetLoc& at) {
        fs::path bak = at.file;
        bak += ".bak";
        std::error_code ec;
        if (!fs::exists(bak, ec)) fail(toUtf8(at.file.filename()) + " has no .bak backup to restore the model from");
        Loaded l = loadAsset(bak);
        std::shared_ptr<caff::Caff> original;
        if (at.entry < 0) {
            if (!l.caff) fail(toUtf8(bak) + " is not a bundle");
            original = std::make_shared<caff::Caff>(std::move(*l.caff));
        } else {
            if (!l.sb || size_t(at.entry) >= l.sb->entries.size()) fail(toUtf8(bak) + ": stream bundle entry missing");
            original = std::make_shared<caff::Caff>(caff::Caff::parse(l.sb->entries[size_t(at.entry)].data));
        }
        for (size_t s = 0; s < original->symbols.size(); ++s)
            if (model::isModelSymbol(*original, s) && assetdb::baseAssetName(original->symbolName(s)) == info_.name)
                return model::copyModel(c, symbol, *original, s,
                                        model::texturesIn(original, at.entry < 0 ? model::pairedStreamBundle(at.file) : fs::path()));
        fail(info_.name + " is not in " + toUtf8(bak));
    }

    // Makes mesh `mesh` draw texture `t` in place of its colour map, in the preview and the view (no reload needed).
    void bindTexture(App& app, size_t mesh, int t) {
        const model::Mesh& me = model_.meshes[mesh];
        uint32_t record = 0;
        for (size_t k = 0; k < me.textures.size() && k < me.source.samplers.size(); ++k)
            if (me.textures[k] == me.texture) record = me.source.samplers[k];
        if (!addChange(app, {Change::Kind::Bind, model_.textures[size_t(t)], {}, false, mesh}, false)) return;
        // Draws sharing the sampler record change with it.
        for (size_t j = 0; j < model_.meshes.size(); ++j) {
            model::Mesh& mj = model_.meshes[j];
            for (size_t k = 0; k < mj.textures.size() && k < mj.source.samplers.size(); ++k) {
                if (mj.source.samplers[k] != record) continue;
                if (mj.textures[k] == mj.texture) mj.texture = t;
                mj.textures[k] = t;
                meshTex_[j] = t;
            }
        }
        applyMeshState();
    }

    // Copies the model `name` over the one at `symbol` in `c` (a copy of the model at `at`).
    model::CopyResult copyFrom(caff::Caff& c, uint32_t symbol, const AssetLoc& at, const std::string& name) {
        const ModelInfo* src = catalog_ ? catalog_->model(name) : nullptr;
        if (!src || src->locs.empty()) fail(name + " is not in the model list");
        // A copy in the same file shares its pools: copy within the CAFF.
        for (auto& l : src->locs)
            if (l.file == at.file && l.entry == at.entry) return model::copyModel(c, symbol, c, l.symbol, catalogTextures());
        caff::Caff& sc = cache_.get(src->locs.front());
        return model::copyModel(c, symbol, sc, src->locs.front().symbol, catalogTextures());
    }

    // Textures for copyModel, from wherever the catalog found them (whole or mip half in a bundle,
    // the streamed top level in a stream bundle). Files are read once per copy, not through cache_,
    // whose entries could be dropped while copyModel still uses them.
    model::FindTexture catalogTextures() const {
        auto catalog = catalog_;
        auto caffs = std::make_shared<std::map<std::pair<fs::path, int>, std::shared_ptr<const caff::Caff>>>();
        auto indexes = std::make_shared<std::map<fs::path, std::vector<streambundle::IndexEntry>>>();
        auto indexOf = [indexes](const fs::path& f) -> const std::vector<streambundle::IndexEntry>& {
            auto it = indexes->find(f);
            if (it == indexes->end()) {
                std::vector<streambundle::IndexEntry> idx;
                streambundle::readIndex(f, idx);
                it = indexes->emplace(f, std::move(idx)).first;
            }
            return it->second;
        };
        return [catalog, caffs, indexOf](const std::string& name) -> std::optional<model::TextureSource> {
            const TextureInfo* ti = catalog ? catalog->texture(name) : nullptr;
            if (!ti || ti->full.empty()) return std::nullopt;
            const AssetLoc& loc = ti->full.front();
            auto& c = (*caffs)[{loc.file, loc.entry}];
            if (!c) {
                if (loc.entry < 0) {
                    Loaded l = loadAsset(loc.file);
                    if (!l.caff) return std::nullopt;
                    c = std::make_shared<caff::Caff>(std::move(*l.caff));
                } else {
                    auto& idx = indexOf(loc.file);
                    if (size_t(loc.entry) >= idx.size()) return std::nullopt;
                    c = std::make_shared<caff::Caff>(caff::Caff::parse(streambundle::readEntry(loc.file, idx[size_t(loc.entry)])));
                }
            }
            model::TextureSource src{c, loc.symbol, std::nullopt};
            if (!ti->tops.empty()) {
                const AssetLoc& t = ti->tops.front();
                auto& idx = indexOf(t.file);
                if (size_t(t.entry) >= idx.size()) return std::nullopt;
                src.top = streambundle::Entry{idx[size_t(t.entry)].id, streambundle::readEntry(t.file, idx[size_t(t.entry)])};
            }
            return src;
        };
    }

    // Returns the stream bundle entries (streamed texture levels) the destination's stream bundle needs.
    std::vector<streambundle::Entry> applyReplacements(caff::Caff& c, uint32_t symbol, const AssetLoc& at) {
        std::vector<streambundle::Entry> entries;
        for (const Change& ch : changes_) {
            auto e = applyChange(c, symbol, at, ch, nullptr);
            entries.insert(entries.end(), e.begin(), e.end());
        }
        return entries;
    }

    // Back to what is on disk.
    void discard() {
        preview_.reset();
        changes_.clear();
        replaceNotes_.clear();
        motions_.clear();
        cache_ = CaffCache();
        loadModel(cache_.get(info_.locs.front()), info_.locs.front().symbol);
    }

    // Writes the edited colours into one copy of the model (`symbol` is its .data part in `c`).
    void patchColours(caff::Caff& c, uint32_t symbol) const {
        model::Model m = model::parse(c, symbol);
        if (m.meshes.size() != model_.meshes.size()) fail("the model there differs from the one shown");
        std::map<int, Bytes> data;
        for (size_t gi : colourEdited_)
            for (auto [mi, ki] : colourGroups_[gi].uses) {
                const auto& k = m.meshes[mi].constants.at(ki);
                if (k.name != colourGroups_[gi].name || k.symbol < 0) fail("the model there differs from the one shown");
                Bytes& d = data.try_emplace(k.symbol, c.symbolData(size_t(k.symbol))).first->second;
                if (k.offset + 16 > d.size()) fail("material constant outside its symbol");
                const auto& v = model_.meshes[mi].constants[ki].value;
                for (int q = 0; q < 4; ++q) {
                    uint32_t bits;
                    std::memcpy(&bits, &v[size_t(q)], 4);
                    putBe32(&d[k.offset + 4 * uint32_t(q)], bits);
                }
            }
        for (auto& [s, d] : data) c.replaceSymbolData(size_t(s), d);
    }

    bool modelDirty() const { return !colourEdited_.empty() || !changes_.empty() || rig_.dirty(); }

    // Writes the new motions into every copy of their animations; "save as" writes only those in the model's
    // first file, into the copy. Returns the copies written.
    int saveMotions(App& app, const fs::path* saveAs, std::vector<std::string>& skipped) {
        int written = 0;
        for (auto& [name, edit] : motions_) {
            const ModelInfo* info = catalog_ ? catalog_->anim(name) : nullptr;
            if (!info) fail(name + " is not in the animation list");
            for (const AssetLoc& loc : info->locs) {
                fs::path target = loc.file;
                if (saveAs) {
                    if (loc.file != info_.locs.front().file) {
                        skipped.push_back(shortAnimName(name, animPrefix_) + " in " + toUtf8(loc.file.filename()));
                        continue;
                    }
                    target = *saveAs;
                }
                CaffCache baseCache;
                std::vector<model::Key> keys = model::keysFromJson(motionBase(loc, name, edit.fromOriginal, baseCache), edit.json);
                Loaded l = loadAsset(target);
                auto write = [&](caff::Caff& c) {
                    auto sym = findAnimation(c, name, loc.symbol);
                    if (!sym) fail(name + " is not in " + toUtf8(target));
                    model::writeAnimationKeys(c, *sym, keys);
                };
                if (loc.entry < 0) {
                    if (!l.caff) fail(toUtf8(target) + " is not a bundle");
                    write(*l.caff);
                    if (!saveAs) app.backupBeforeWrite(target);
                    saveCaff(target, *l.caff);
                } else {
                    if (!l.sb || size_t(loc.entry) >= l.sb->entries.size()) fail(toUtf8(target) + ": stream bundle entry missing");
                    auto& e = l.sb->entries[size_t(loc.entry)];
                    caff::Caff c = caff::Caff::parse(e.data);
                    write(c);
                    e.data = c.serialize();
                    if (!saveAs) app.backupBeforeWrite(target);
                    writeFile(target, l.sb->serialize());
                }
                ++written;
            }
        }
        return written;
    }

public:
    bool dirty() const override { return modelDirty() || !motions_.empty(); }
    bool canSave() const override { return dirty(); }

    // Every bundle holding the model gets the change (some models ship in several); "save as"
    // writes only the first.
    bool save(App& app, const fs::path* saveAs) override {
        // Rig edits still being made go in with the rest.
        rig_.commit();
        applyPendingRig(app);
        if (rig_.dirty()) return false;
        try {
            std::vector<AssetLoc> locs = info_.locs;
            if (saveAs) locs.resize(1);
            // Only new motions: the model's files stay as they are.
            if (!modelDirty() && !saveAs) locs.clear();
            int streamFiles = 0;
            for (const AssetLoc& loc : locs) {
                fs::path target = saveAs ? *saveAs : loc.file;
                Loaded l = loadAsset(loc.file);
                if (loc.entry < 0) {
                    if (!l.caff) fail(toUtf8(loc.file) + " is not a bundle");
                    auto streamed = applyReplacements(*l.caff, loc.symbol, loc);
                    patchColours(*l.caff, loc.symbol);
                    // Copied textures' streamed levels go into the bundle's stream bundle, written first.
                    if (!streamed.empty() && saveAs)
                        fail("the copied textures' full-size levels go into the bundle's stream bundle, which Save as can't write: "
                             "use Save, or Tools > Import into a file > Model swap with Write the stream bundle to");
                    if (!streamed.empty() && !saveAs) {
                        fs::path sp = model::pairedStreamBundle(loc.file);
                        Loaded sl = loadAsset(sp);
                        if (!sl.sb) fail(toUtf8(sp) + " (the bundle's stream bundle) is missing; the copied textures need it");
                        if (model::addStreamEntries(*sl.sb, streamed)) {
                            app.backupBeforeWrite(sp);
                            writeFile(sp, sl.sb->serialize());
                            ++streamFiles;
                        }
                    }
                    if (!saveAs) app.backupBeforeWrite(loc.file);
                    saveCaff(target, *l.caff);
                } else {
                    if (!l.sb || size_t(loc.entry) >= l.sb->entries.size()) fail(toUtf8(loc.file) + ": stream bundle entry missing");
                    auto& e = l.sb->entries[size_t(loc.entry)];
                    caff::Caff c = caff::Caff::parse(e.data);
                    auto streamed = applyReplacements(c, loc.symbol, loc);
                    patchColours(c, loc.symbol);
                    e.data = c.serialize();
                    model::addStreamEntries(*l.sb, streamed);
                    if (!saveAs) app.backupBeforeWrite(loc.file);
                    writeFile(target, l.sb->serialize());
                }
            }
            bool modelSaved = !locs.empty();
            bool coloured = !colourEdited_.empty();
            for (size_t gi : colourEdited_) colourGroups_[gi].original = colourOf(colourGroups_[gi]);
            colourEdited_.clear();
            bool replaced = !changes_.empty();
            std::vector<std::string> skipped;
            int motionCopies = motions_.empty() ? 0 : saveMotions(app, saveAs, skipped);
            size_t motionCount = motions_.size();
            if (!saveAs) {
                // What is on disk now matches the preview.
                preview_.reset();
                changes_.clear();
                replaceNotes_.clear();
                motions_.clear();
                cache_ = CaffCache();
            }
            for (auto& s : skipped) app.log("Save As: the new motion of " + s + " is not in the saved file; Save writes it there");
            std::string what = replaced ? "the model" : coloured ? "material changes" : modelSaved ? "the model" : "";
            std::string msg = what.empty() ? "" : "Saved " + what + " to " + std::to_string(locs.size() + size_t(streamFiles)) + " file(s)" +
                                                      (streamFiles ? " (with the textures it needs)" : "");
            if (motionCount)
                msg += (msg.empty() ? "Saved " : "; ") + std::to_string(motionCount) + " new motion(s) into " + std::to_string(motionCopies) +
                       " animation cop" + (motionCopies == 1 ? "y" : "ies");
            app.status(msg);
            return true;
        } catch (std::exception& e) {
            app.error(std::string("save failed: ") + e.what());
            return false;
        }
    }

private:
    std::vector<ColourGroup> colourGroups_;
    std::set<size_t> colourEdited_;
    std::vector<MaterialEntry> materials_;
    std::string matFilter_;
    std::string pickedMaterial_, hoveredMaterial_; // MaterialEntry labels: picked on the model, hovered in the list
    int pickedMesh_ = -1;
    bool scrollToPicked_ = false;
    bool allColours_ = false, matColoursOnly_ = false;
    // Changes not saved yet, in order: the preview is the disk copy with them applied, and saving replays them on every copy.
    std::unique_ptr<caff::Caff> preview_;
    std::vector<Change> changes_;
    bool importRig_ = false;
    std::vector<std::string> replaceNotes_;
    std::string swapFilter_;
    std::function<void()> deferred_; // runs at the start of the next frame

    ModelInfo info_;
    std::shared_ptr<const Catalog> catalog_;
    fs::path root_;
    CaffCache cache_;
    model::Model model_;
    std::vector<LoadedTex> textures_;
    std::vector<int> meshTex_;
    std::vector<bool> meshVisible_;
    std::vector<bool> groupShown_;
    bool hasFur_ = false;
    std::vector<model::FurBlock> furBlocks_;  // the render program's furred blocks (modelimport.hpp)
    int lod_ = 0;
    std::map<int, int> switchState_;
    std::unique_ptr<ModelView> view_;
    ModelView::Options opt_;
    RigEditor rig_;
    std::shared_ptr<const rigedit::Rig> pendingRig_;  // edits the rig editor handed over, made a change next frame
    // Animation.
    std::string animPrefix_;
    std::vector<std::string> ownAnims_;
    int fbxAnims_ = 1; // FBX export: 0 none, 1 named after the model, 2 the one playing
    bool fbxFur_ = true;
    bool fbxReorient_ = false;
    int fbxAxis_ = 1; // fbx::Options::boneAxis
    bool fbxEnds_ = false;
    float fbxEndPct_ = 1.5f; // end bone length, percent of the skeleton's size
    std::string animFilter_, animName_, pendingName_, animError_;
    std::future<AnimLoad> pending_;
    std::optional<MotionEdit> pendingMotion_;   // the motion pending_ applies
    std::map<std::string, MotionEdit> motions_; // animation -> new motion not saved yet
    bool motionFromOriginal_ = true;
    std::shared_ptr<model::Animation> anim_;
    std::vector<int> trackOf_;
    int matched_ = 0;
    float animTime_ = 0, speed_ = 1;
    bool playing_ = false, loop_ = true;
    std::vector<Mat4> bindWorld_, bindInverse_;
    std::vector<float> skin_, boneWorld_;
    bool startupChecked_ = false;
    float panelFrac_ = 0.28f;
};

} // namespace

std::unique_ptr<Document> makeModelDocument(const ModelInfo& m, std::shared_ptr<const Catalog> catalog, const fs::path& root) {
    return std::make_unique<ModelDocument>(m, std::move(catalog), root);
}

} // namespace nut::gui
