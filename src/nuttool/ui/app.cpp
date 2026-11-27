#include "app.hpp"
#include "docs.hpp"

#include "assetdb.hpp"
#include "container.hpp"
#include "loctext.hpp"
#include "nbpatch.hpp"
#include "ops.hpp"
#include "texture_edit.hpp"

#include <imgui_internal.h>

#include <rexfabric/rexfabric.h>

#include "nuttool/workspace.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"
#include "renut_engine/mods/mod_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace nut::gui {

// ---- helpers -----------------------------------------------------------------

std::string toUtf8(const fs::path& p) {
    auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }

std::string toUtf8Generic(const fs::path& p) {
    auto u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string formatSize(uint64_t b) {
    char t[32];
    if (b < 1024) std::snprintf(t, sizeof t, "%llu B", (unsigned long long)b);
    else if (b < 1024 * 1024) std::snprintf(t, sizeof t, "%.1f KB", b / 1024.0);
    else std::snprintf(t, sizeof t, "%.1f MB", b / (1024.0 * 1024.0));
    return t;
}

GpuImage::~GpuImage() {
    if (tex != ImTextureID_Invalid) renut::gpu::overlay::RemoveImage(uint64_t(tex));
}

namespace {

int inputTextResize(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(data->UserData);
        s->resize(size_t(data->BufTextLen));
        data->Buf = s->data();
    }
    return 0;
}

} // namespace

bool inputText(const char* label, std::string& s, ImGuiInputTextFlags flags) {
    return ImGui::InputText(label, s.data(), s.capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize, inputTextResize, &s);
}

namespace {
bool g_collapsing = false;
}

bool collapsingAll() { return g_collapsing; }

void collapseWindowTree() {
    ImGuiWindow* root = ImGui::GetCurrentWindow();
    for (ImGuiWindow* w : GImGui->Windows)
        if (w == root || ImGui::IsWindowChildOf(w, root, false)) w->StateStorage.SetAllInt(0);
}

bool inputTextWithHint(const char* label, const char* hint, std::string& s, ImGuiInputTextFlags flags) {
    return ImGui::InputTextWithHint(label, hint, s.data(), s.capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize, inputTextResize, &s);
}

bool inputTextMultiline(const char* label, std::string& s, const ImVec2& size, ImGuiInputTextFlags flags) {
    return ImGui::InputTextMultiline(label, s.data(), s.capacity() + 1, size, flags | ImGuiInputTextFlags_CallbackResize, inputTextResize, &s);
}

fs::path exportName(const std::string& asset, const std::string& ext) {
    std::string n = asset;
    size_t slash = n.find_last_of("/\\");
    if (slash != std::string::npos) n = n.substr(slash + 1);
    if (startsWith(n, "aid_")) {
        size_t type = n.find('_', 4);
        if (type != std::string::npos) n = n.substr(type + 1);
    }
    for (const char* p : {"banjoxdlc_", "banjox_"})
        if (startsWith(n, p) && n.size() > std::strlen(p)) n = n.substr(std::strlen(p));
    n = ops::safeName(n, 150);
    return fromUtf8(ext.empty() ? n : n + "." + ext);
}

void helpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void splitter(const char* id, float& width, float minWidth, float maxWidth, float height) {
    ImGui::SameLine(0, 0);
    ImGui::InvisibleButton(id, ImVec2(ImGui::GetFontSize() * 0.4f, height));
    if (ImGui::IsItemActive()) width += ImGui::GetIO().MouseDelta.x;
    width = std::clamp(width, minWidth, std::max(minWidth, maxWidth));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2((a.x + b.x) * 0.5f - 1, a.y), ImVec2((a.x + b.x) * 0.5f + 1, b.y), ImGui::GetColorU32(ImGuiCol_SeparatorHovered));
    }
    ImGui::SameLine(0, 0);
}

void hexView(const char* id, const Bytes& data, float height) {
    if (!ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::EndChild();
        return;
    }
    int rows = int((data.size() + 15) / 16);
    ImGuiListClipper clip;
    clip.Begin(rows);
    char line[128];
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            size_t base = size_t(r) * 16;
            int n = std::snprintf(line, sizeof line, "%08zx  ", base);
            for (size_t k = 0; k < 16; ++k) {
                if (base + k < data.size()) n += std::snprintf(line + n, sizeof line - size_t(n), "%02x ", data[base + k]);
                else n += std::snprintf(line + n, sizeof line - size_t(n), "   ");
                if (k == 7) line[n++] = ' ';
            }
            line[n++] = ' ';
            for (size_t k = 0; k < 16 && base + k < data.size(); ++k) {
                uint8_t c = data[base + k];
                line[n++] = (c >= 32 && c < 127) ? char(c) : '.';
            }
            line[n] = 0;
            ImGui::TextUnformatted(line);
        }
    }
    ImGui::EndChild();
}

// ---- construction / settings ------------------------------------------------------

App::App(SDL_Window* window, const fs::path& assets, const fs::path& settings) : window_(window) {
    instance_ = this;
    if (!settings.empty()) settingsPath_ = settings / "settings.txt";
    loadSettings();
    // In game the assets are the game's own, and edits go to the open mod (never a .bak beside a game file).
    backups_ = false;
    uiScale_ = 0;
    updateScale();
    if (!assets.empty() && fs::exists(assets)) setAssetsRoot(assets);
}

App::~App() {
    // Background work stops at its next check.
    for (auto& t : tasks_) t->cancel = true;
    saveSettings();
    saveScriptText();
    // Work that writes files is waited for. Read-only work can be stuck in one long read (a network
    // drive): give it a moment, then leave it running on its own.
    uint64_t deadline = SDL_GetTicks() + 1500;
    for (auto& t : tasks_) {
        if (t->readOnly)
            while (!t->finished && SDL_GetTicks() < deadline) SDL_Delay(10);
        if (!t->thread.joinable()) continue;
        if (t->readOnly && !t->finished) {
            t->thread.detach();
            (void)t.release();  // the stuck work still holds its task
        } else {
            t->thread.join();
        }
    }
    instance_ = nullptr;
}

void App::loadSettings() {
    if (settingsPath_.empty() || !fs::exists(settingsPath_)) return;
    try {
        for (auto& line : split(readText(settingsPath_), '\n')) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
            if (k == "assets") root_ = fromUtf8(v);
            else if (k == "update") updateDir_ = fromUtf8(v);
            else if (k == "backups") backups_ = v == "1";
            else if (k == "dark") darkTheme_ = v == "1";
            else if (k == "browserWidth") browserWidth_ = std::max(150.0f, std::stof(v));
            else if (k == "extractOut") extractOut_ = v;
            else if (k == "lastDir") lastDir_ = fromUtf8(v);
            else if (k == "modAuthor") mod_.author = v;
            else if (k == "modOriginal") modOriginal_ = v;
            else if (k == "bulkFolder") bulkFolder_ = v;
            else if (k == "zoom") userZoom_ = std::clamp(std::stof(v), 0.5f, 3.0f);
            else if (k == "modelPanels") {
                bool* p[] = {&modelPanels.parts, &modelPanels.materials, &modelPanels.fur, &modelPanels.meshes,
                             &modelPanels.textures, &modelPanels.skeleton, &modelPanels.materialNames, &modelPanels.warnings};
                for (size_t i = 0; i < sizeof p / sizeof *p && i < v.size(); ++i) *p[i] = v[i] == '1';
            } else
                for (auto& [key, field] : tools_.remembered())
                    if (k == "tools." + key) *field = v;
        }
    } catch (...) {
    }
}

void App::saveSettings() {
    if (settingsPath_.empty()) return;
    std::string s = "assets=" + toUtf8(root_) + "\nupdate=" + toUtf8(updateDir_) + "\nbackups=" + (backups_ ? "1" : "0") + "\ndark=" + (darkTheme_ ? "1" : "0") +
                    "\nbrowserWidth=" + std::to_string(int(browserWidth_)) + "\nextractOut=" + extractOut_ + "\nlastDir=" + toUtf8(lastDir_) + "\nzoom=" + std::to_string(userZoom_) +
                    "\nmodAuthor=" + mod_.author + "\nmodOriginal=" + modOriginal_ + "\nbulkFolder=" + bulkFolder_ + "\nmodelPanels=";
    for (bool b : {modelPanels.parts, modelPanels.materials, modelPanels.fur, modelPanels.meshes, modelPanels.textures, modelPanels.skeleton,
                   modelPanels.materialNames, modelPanels.warnings})
        s += b ? '1' : '0';
    s += '\n';
    for (auto& [key, field] : tools_.remembered())
        if (!field->empty() && field->find_first_of("\r\n") == std::string::npos) s += "tools." + key + "=" + *field + "\n";
    try { writeText(settingsPath_, s); } catch (...) {}
}

bool App::reloadForWorkspace() {
    for (auto& d : docs_)
        if (d->dirty()) return false;
    docs_.clear();
    if (!root_.empty()) setAssetsRoot(root_);
    return true;
}

fs::path App::updateRoot() const { return ops::updateFolder(root_, updateDir_); }

void App::setAssetsRoot(const fs::path& p) {
    root_ = p;
    catalog_.reset();
    scanAssets();
    collectGlyphs();
    buildCatalog();
    saveSettings();
    status("Assets folder: " + toUtf8(root_));
}

// ---- asset scanning ---------------------------------------------------------------

void App::scanAssets() {
    groups_.clear();
    labels_.clear();
    if (root_.empty() || !fs::exists(root_)) return;
    assetdb::Names names;
    try {
        for (auto& e : assetdb::readDbIndex(root_ / "Debug" / "db_index.txt")) names.add(e.name);
    } catch (...) {
    }
    auto label = [&](const fs::path& f) {
        auto id = assetdb::idFromPath(f);
        if (!id) return toUtf8(f.filename());
        std::string n = names.lookup(*id);
        std::string t = assetdb::typeName(uint8_t(*id >> 24));
        if (startsWith(n, "aid_" + t + "_")) n = n.substr(5 + t.size());
        return n.empty() ? hex32(*id) : n;
    };
    auto list = [&](const fs::path& dir, bool recursive) {
        std::vector<BrowserItem> items;
        if (!fs::exists(dir)) return items;
        auto add = [&](const fs::path& f) {
            BrowserItem it;
            it.path = f;
            it.label = label(f);
            it.tooltip = toUtf8(fs::relative(f, root_));
            if (auto id = assetdb::idFromPath(f)) it.tooltip += "\nid " + hex32(*id);
            labels_[toUtf8(f)] = it.label;
            it.lower = lower(it.label + "\n" + it.tooltip);
            items.push_back(std::move(it));
        };
        if (recursive) {
            for (auto& e : fs::recursive_directory_iterator(dir))
                if (e.is_regular_file() && e.path().extension() != ".bak") add(e.path());
        } else {
            for (auto& e : fs::directory_iterator(dir))
                if (e.is_regular_file() && e.path().extension() != ".bak") add(e.path());
        }
        std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return a.label < b.label; });
        return items;
    };
    // Bundles aren't listed, but their tabs still take their names.
    list(root_ / "Bundle" / "4f", false);
    list(root_ / "Bundle" / "50", false);
    BrowserGroup text{"Text", {}, {}};
    text.children.push_back({"english", list(root_ / "Debug" / "11", true), {}});
    if (fs::exists(root_ / "loctext")) {
        std::vector<fs::path> langs;
        for (auto& d : fs::directory_iterator(root_ / "loctext")) if (d.is_directory()) langs.push_back(d.path());
        std::sort(langs.begin(), langs.end());
        for (auto& d : langs) text.children.push_back({toUtf8(d.filename()), list(d, true), {}});
    }
    groups_.push_back(std::move(text));
    groups_.push_back({"Videos", list(root_ / "Debug" / "36", true), {}});
    // The title update's bundles, named from its own db_index.txt, for their tabs too.
    if (fs::path update = updateRoot(); !update.empty()) {
        try {
            for (auto& e : assetdb::readDbIndex(update / "db_index.txt")) names.add(e.name);
        } catch (...) {
        }
        for (auto& f : ops::updateBundles(update)) labels_[toUtf8(f)] = label(f);
    }
    if (catalog_) addCatalogGroups();
}

// Indexes every texture and model in the background; the browser gains Textures, UI icons
// and Models groups when it finishes.
void App::buildCatalog() {
    if (root_.empty() || !fs::exists(root_)) return;
    fs::path root = root_;
    fs::path cache = settingsPath_.empty() ? fs::path() : settingsPath_.parent_path() / "catalog.json";
    fs::path update = updateRoot();
    indexing_ = true;
    const fs::path views = settingsPath_.empty() ? fs::path() : settingsPath_.parent_path() / "mod_view";
    runReadOnlyTask("Indexing textures and models", false, [this, root, cache, update, views](Task& t) -> std::function<void()> {
        ops::Progress pr;
        pr.log = [this](const std::string& s) { log(s); };
        pr.step = [&t](double f, const std::string& s) { t.report(f, s); };
        pr.cancel = &t.cancel;
        std::shared_ptr<const Catalog> cat = Catalog::build(root, cache, pr, update);
        // The Mods group: each mod's game-layout folders (bundles, worlds, NutTool's workspace) and its .nbpatch
        // (unpacked) indexed on their own, and its other files listed.
        auto mods = std::make_shared<std::vector<ModView>>();
        if (cat && !views.empty()) {
            for (auto& m : renut::nuttool::ListMods(renut::mods::ModsFolder())) {
                if (t.cancel) break;
                ModView v;
                v.name = m.name;
                v.root = m.location;
                t.report(1, "Reading the mod " + m.name);
                std::error_code ec;
                std::vector<std::pair<fs::path, std::string>> roots;  // folders in the game's layout, then their index's cache name
                try {
                    const fs::path unpacked = views / fromUtf8(m.name);
                    std::string unpackError;
                    if (renut::nuttool::UnpackMod(root, m.location, unpacked, &unpackError)) roots.push_back({unpacked, m.name + ".json"});
                    else if (unpackError != "it has no .nbpatch") v.error = unpackError;
                    if (!m.loose) {
                        for (const char* sub : {"bundles", "worlds", "nuttool"})
                            if (fs::is_directory(m.location / sub / "Bundle", ec)) roots.push_back({m.location / sub, m.name + "." + sub + ".json"});
                        for (auto it = fs::recursive_directory_iterator(m.location, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                            if (ec) break;
                            if (it->is_directory(ec) && it.depth() == 0 && it->path().filename() == "nuttool") it.disable_recursion_pending();
                            else if (it->is_regular_file(ec)) v.files.push_back(it->path());
                        }
                        std::sort(v.files.begin(), v.files.end());
                    }
                    std::shared_ptr<const Catalog> modCat;
                    for (auto& [r, cacheName] : roots) {
                        std::shared_ptr<const Catalog> c = Catalog::build(r, views / fromUtf8(cacheName), pr);
                        if (c) modCat = modCat ? std::shared_ptr<const Catalog>(Catalog::overlay(*modCat, *c)) : c;
                    }
                    if (modCat) {
                        v.catalog = modCat;
                        v.merged = Catalog::overlay(*cat, *modCat);
                    }
                } catch (const std::exception& e) {
                    v.error = e.what();
                }
                if (v.catalog || !v.files.empty() || !v.error.empty()) mods->push_back(std::move(v));
            }
        }
        return [this, cat, root, mods] {
            indexing_ = false;
            if (!cat || root != root_) return;
            catalog_ = cat;
            modViews_ = std::move(*mods);
            addCatalogGroups();
            for (auto& k : std::exchange(pendingOpen_, {})) openWhenIndexed(k);
            size_t ui = 0;
            for (auto& x : cat->textures) ui += x.ui;
            status("Indexed " + std::to_string(cat->textures.size()) + " textures (" + std::to_string(ui) + " UI icons), " +
                   std::to_string(cat->models.size()) + " models and " + std::to_string(cat->scripts.size()) + " scripts");
        };
    });
}

void App::addCatalogGroups() {
    groups_.erase(std::remove_if(groups_.begin(), groups_.end(),
                                 [](const BrowserGroup& g) {
                                     return g.name == "Textures" || g.name == "UI icons" || g.name == "Models" || g.name == "Audio" || g.name == "Scripts" ||
                                            g.name == "Vehicle parts" || g.name == "Mods";
                                 }),
                  groups_.end());
    if (!catalog_) return;
    auto strip = [](const std::string& n, std::initializer_list<const char*> prefixes) {
        for (const char* p : prefixes)
            if (startsWith(n, p)) return n.substr(std::string(p).size());
        return n;
    };
    BrowserGroup textures{"Textures", {}, {}}, ui{"UI icons", {}, {}}, models{"Models", {}, {}};
    for (auto& t : catalog_->textures) {
        BrowserItem it;
        it.kind = BrowserItem::Texture;
        it.key = t.name;
        it.label = strip(t.name, {"aid_texture_banjox_", "aid_texture_banjoxdlc_", "aid_texture_"});
        size_t copies = std::max(t.full.size(), t.tops.size());
        it.tooltip = t.name + "\n" + std::to_string(t.width) + "x" + std::to_string(t.height) + " " + t.format + ", " +
                     std::to_string(t.mips) + " mip(s), " + std::to_string(copies) + (copies == 1 ? " copy" : " copies");
        it.lower = lower(t.name);
        if (t.ui) ui.items.push_back(it);
        textures.items.push_back(std::move(it));
    }
    // The worlds' models (aid_model_banjox_background_...) aren't listed: the studio can't load a world.
    for (auto& m : catalog_->models) {
        if (startsWith(m.name, "aid_model_banjox_background_") || startsWith(m.name, "aid_model_banjoxdlc_background_")) continue;
        BrowserItem it;
        it.kind = BrowserItem::Model;
        it.key = m.name;
        it.label = strip(m.name, {"aid_model_banjox_", "aid_model_banjoxdlc_", "aid_model_"});
        it.tooltip = m.name + "\n" + std::to_string(m.locs.size()) + (m.locs.size() == 1 ? " copy" : " copies");
        it.lower = lower(m.name);
        models.items.push_back(std::move(it));
    }
    // Audio: wave banks grouped by the first word of their names (ImpactsPropBigAlBox -> Impacts).
    // Searching also finds the banks holding a matching wave.
    BrowserGroup audio{"Audio", {}, {}};
    {
        static const std::map<std::string, std::string> rename = {
            {"Ambi", "Ambience"}, {"Bolt", "Bolt-ons"}, {"Object", "Objects"}, {"Footstep", "Footsteps"}, {"Engine", "Engines"},
            {"Tyre", "Tyres"}, {"Cutscene", "Cutscenes"}, {"Impact", "Impacts"}, {"Task", "Tasks"}, {"Vocal", "Vocals"}};
        auto category = [](const std::string& n) {
            size_t e = 1;
            while (e < n.size() && !std::isupper(uint8_t(n[e])) && n[e] != '_') ++e;
            std::string c = n.substr(0, e);
            auto it = rename.find(c);
            return it != rename.end() ? it->second : c;
        };
        std::map<std::string, std::vector<BrowserItem>> byCat;
        for (auto& b : catalog_->audio) {
            BrowserItem it;
            it.kind = BrowserItem::Audio;
            it.key = b.name;
            it.label = b.name;
            char line[96];
            std::snprintf(line, sizeof line, "%zu wave%s, %d:%02d", b.waves.size(), b.waves.size() == 1 ? "" : "s", int(b.seconds()) / 60,
                          int(b.seconds()) % 60);
            it.tooltip = b.name + "\n" + line;
            it.lower = lower(b.name);
            for (auto& w : b.waves) it.lower += "\n" + lower(w.name);
            byCat[category(b.name)].push_back(std::move(it));
        }
        std::vector<BrowserItem> other;
        for (auto& [c, items] : byCat) {
            if (items.size() < 2) {
                for (auto& it : items) other.push_back(std::move(it));
                continue;
            }
            audio.children.push_back({c, std::move(items), {}});
        }
        if (!other.empty()) {
            std::sort(other.begin(), other.end(), [](auto& a, auto& b) { return a.label < b.label; });
            audio.children.push_back({"Other", std::move(other), {}});
        }
    }
    // Scripts by kind; searching also matches the assets that use them.
    BrowserGroup scripts{"Scripts", {}, {}};
    {
        const std::pair<gamescript::Kind, const char*> kinds[] = {{gamescript::Kind::Script, "Scene scripts"},
                                                                  {gamescript::Kind::ActorGoals, "Actor goals"},
                                                                  {gamescript::Kind::ScriptTable, "Script tables"},
                                                                  {gamescript::Kind::StateTable, "State tables"}};
        for (auto& [kind, label] : kinds) {
            BrowserGroup g{label, {}, {}};
            for (auto& s : catalog_->scripts) {
                if (s.kind != kind) continue;
                BrowserItem it;
                it.kind = BrowserItem::Script;
                it.key = s.name;
                it.label = strip(s.name, {"aid_script_banjox_", "aid_script_banjoxdlc_", "aid_actorgoals_banjox_", "aid_scripttable_banjox_",
                                          "aid_statetable_banjox_", "aid_script_", "aid_actorgoals_", "aid_scripttable_", "aid_statetable_"});
                it.tooltip = s.name + "\n" + std::to_string(s.locs.size()) + (s.locs.size() == 1 ? " copy" : " copies") + ", used by " +
                             std::to_string(s.usedBy.size()) + (s.usedBy.size() == 1 ? " asset" : " assets");
                it.lower = lower(s.name);
                for (auto& u : s.usedBy) it.lower += "\n" + lower(u);
                g.items.push_back(std::move(it));
            }
            if (!g.items.empty()) scripts.children.push_back(std::move(g));
        }
    }
    // Vehicle parts by the category in their names (aid_objparams_banjox_vehicleblock_wheels_monster -> wheels).
    BrowserGroup parts{"Vehicle parts", {}, {}};
    {
        std::map<std::string, std::vector<BrowserItem>> byCat;
        for (auto& p : catalog_->parts) {
            BrowserItem it;
            it.kind = BrowserItem::Script;
            it.key = p.name;
            std::string rest = strip(p.name, {"aid_objparams_banjox_vehicleblock_", "aid_objparams_banjoxdlc_vehicleblock_", "aid_objparams_"});
            size_t us = rest.find('_');
            std::string cat = us == std::string::npos ? std::string("other") : rest.substr(0, us);
            it.label = us == std::string::npos ? rest : rest.substr(us + 1);
            it.tooltip = p.name + "\n" + std::to_string(p.locs.size()) + (p.locs.size() == 1 ? " copy" : " copies");
            it.lower = lower(p.name);
            byCat[cat].push_back(std::move(it));
        }
        for (auto& [c, items] : byCat) parts.children.push_back({c, std::move(items), {}});
    }
    // Before Text and Videos.
    auto at = groups_.begin();
    std::vector<BrowserGroup> add;
    add.push_back(std::move(models));
    add.push_back(std::move(textures));
    add.push_back(std::move(ui));
    if (!audio.children.empty()) add.push_back(std::move(audio));
    if (!scripts.children.empty()) add.push_back(std::move(scripts));
    if (!parts.children.empty()) add.push_back(std::move(parts));
    groups_.insert(at, std::make_move_iterator(add.begin()), std::make_move_iterator(add.end()));

    // Mods, last: each mod's assets (from its folders and .nbpatch) and its files.
    BrowserGroup mods{"Mods", {}, {}};
    for (const ModView& v : modViews_) {
        BrowserGroup g{v.error.empty() ? v.name : v.name + " (its .nbpatch couldn't be read: " + v.error + ")", {}, {}};
        if (v.catalog) {
            auto list = [&](const char* name, BrowserItem::Kind kind, const auto& assets, std::initializer_list<const char*> prefixes) {
                BrowserGroup kindGroup{name, {}, {}};
                for (auto& a : assets) {
                    BrowserItem it;
                    it.kind = kind;
                    it.key = a.name;
                    it.mod = v.name;
                    it.label = strip(a.name, prefixes);
                    it.tooltip = a.name + "\nin the mod " + v.name;
                    it.lower = lower(a.name + "\n" + v.name);
                    kindGroup.items.push_back(std::move(it));
                }
                if (!kindGroup.items.empty()) g.children.push_back(std::move(kindGroup));
            };
            list("Models", BrowserItem::Model, v.catalog->models, {"aid_model_banjox_", "aid_model_banjoxdlc_", "aid_model_"});
            list("Textures", BrowserItem::Texture, v.catalog->textures, {"aid_texture_banjox_", "aid_texture_banjoxdlc_", "aid_texture_"});
            list("Audio", BrowserItem::Audio, v.catalog->audio, {});
            list("Scripts", BrowserItem::Script, v.catalog->scripts, {"aid_script_banjox_", "aid_script_"});
            list("Vehicle parts", BrowserItem::Script, v.catalog->parts, {"aid_objparams_banjox_vehicleblock_", "aid_objparams_"});
        }
        // Its other files (scripts, music, data), opened as files are.
        BrowserGroup files{"Files", {}, {}};
        for (const fs::path& f : v.files) {
            BrowserItem it;
            it.path = f;
            it.label = toUtf8Generic(f.lexically_relative(v.root));
            it.tooltip = toUtf8(f);
            it.lower = lower(it.label + "\n" + v.name);
            files.items.push_back(std::move(it));
        }
        if (!files.items.empty()) g.children.push_back(std::move(files));
        mods.children.push_back(std::move(g));
    }
    if (!mods.children.empty()) groups_.push_back(std::move(mods));
}

void App::collectGlyphs() {
    // The game text uses thousands of CJK characters; only those are added to the UI font.
    fs::path root = root_;
    runReadOnlyTask("Loading text glyphs", false, [this, root](Task& t) -> std::function<void()> {
        auto glyphs = std::make_shared<std::set<uint32_t>>();
        std::vector<fs::path> files;
        auto addDir = [&](const fs::path& d) {
            if (!fs::exists(d)) return;
            for (auto& e : fs::recursive_directory_iterator(d)) if (e.is_regular_file()) files.push_back(e.path());
        };
        addDir(root / "Debug" / "11");
        addDir(root / "loctext");
        for (size_t i = 0; i < files.size() && !t.cancel; ++i) {
            t.report(double(i) / files.size(), toUtf8(files[i].filename()));
            try {
                auto id = assetdb::idFromPath(files[i]);
                if (!id || (*id >> 24) != assetdb::kTypeLoctext) continue;
                Bytes d = readFile(files[i]);
                if (!caff::isCaff(d)) continue;
                for (auto& e : loctext::read(caff::Caff::parse(d)).entries)
                    for (char16_t ch : utf8ToUtf16(e.text)) glyphs->insert(ch);
            } catch (...) {
            }
        }
        return [this, glyphs] {
            glyphs_ = std::move(*glyphs);
            fontsDirty_ = true;
        };
    });
}

// ---- fonts -------------------------------------------------------------------------

void App::setZoom(float z) {
    userZoom_ = std::clamp(z, 0.5f, 3.0f);
    updateScale();
    saveSettings();
}

void App::updateScale() {
    // The window scale includes the monitor's content scale (125%, 150%, 200%...).
    float s = SDL_GetWindowDisplayScale(window_);
    float content = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window_));
    s = std::max({s, content, 1.0f}) * userZoom_;
    if (std::fabs(s - uiScale_) < 0.01f) return;
    uiScale_ = s;
    ImGuiStyle& style = style_;
    style = ImGuiStyle();
    if (darkTheme_) ImGui::StyleColorsDark(&style);
    else ImGui::StyleColorsLight(&style);
    style.FrameRounding = 3;
    style.TabRounding = 3;
    style.ScaleAllSizes(uiScale_);
    fontsDirty_ = true;
}

// The overlay's atlas loads glyphs as they're drawn, so the fonts are added once (CJK and Hangul merged in) and
// drawn at any size; the overlay's own fonts stay.
void App::rebuildFonts() {
    if (font_) return;
    ImGuiIO& io = ImGui::GetIO();
    fs::path fonts;
    if (const char* windir = SDL_getenv("WINDIR")) fonts = fromUtf8(windir) / "Fonts";
    const fs::path mainFont = fonts / "segoeui.ttf", cjkFont = fonts / "msyh.ttc", hangulFont = fonts / "malgun.ttf";
    std::error_code ec;
    if (!fonts.empty() && fs::is_regular_file(mainFont, ec)) font_ = io.Fonts->AddFontFromFileTTF(toUtf8(mainFont).c_str(), 16.0f);
    if (!font_) return;
    ImFontConfig merge;
    merge.MergeMode = true;
    if (fs::is_regular_file(cjkFont, ec)) io.Fonts->AddFontFromFileTTF(toUtf8(cjkFont).c_str(), 16.0f, &merge);
    if (fs::is_regular_file(hangulFont, ec)) io.Fonts->AddFontFromFileTTF(toUtf8(hangulFont).c_str(), 16.0f, &merge);
}

// ---- services -----------------------------------------------------------------------

std::shared_ptr<GpuImage> App::upload(const image::Rgba& img, bool nearest) {
    (void)nearest;  // the overlay filters linearly
    auto g = std::make_shared<GpuImage>();
    if (img.width == 0 || img.height == 0) return g;
    // NutTool draws in reNut's overlay, which only shows its own pictures.
    g->tex = ImTextureID(renut::gpu::overlay::AddImage(img.width, img.height, img.pixels));
    g->width = int(img.width);
    g->height = int(img.height);
    return g;
}

void App::status(const std::string& msg) {
    status_ = msg;
    statusError_ = false;
    log(msg);
}

void App::error(const std::string& msg) {
    status_ = msg;
    statusError_ = true;
    log("error: " + msg);
}

void App::log(const std::string& msg) {
    std::lock_guard<std::mutex> lk(logMutex_);
    log_.push_back(msg);
    while (log_.size() > 2000) log_.pop_front();
}

void App::backupBeforeWrite(const fs::path& p) {
    // A game file goes to the open mod's workspace: the game's own file stays its original.
    if (!backups_ || !fs::exists(p) || writePath(p) != p) return;
    fs::path bak = p;
    bak += ".bak";
    if (!fs::exists(bak)) fs::copy_file(p, bak);
}

void App::openDocument(const std::string& key, const std::string& label, std::function<std::unique_ptr<Document>()> load) {
    for (auto& d : docs_) {
        if (d->key == key) {
            selectTab_ = d->uid;
            return;
        }
    }
    runReadOnlyTask("Opening " + label, true, [this, key, load = std::move(load)](Task&) -> std::function<void()> {
        auto holder = std::make_shared<std::unique_ptr<Document>>(load());
        return [this, holder, key] {
            auto& d = *holder;
            d->key = key;
            d->uid = "doc" + std::to_string(++uidCounter_);
            selectTab_ = d->uid;
            status("Opened " + d->title());
            docs_.push_back(std::move(d));
        };
    });
}

void App::openPath(const fs::path& p) {
    fs::path root = root_;
    std::string name = labels_.count(toUtf8(p)) ? labels_[toUtf8(p)] : toUtf8(p.filename());
    openDocument(toUtf8(p), name, [p, root] { return loadDocument(p, root); });
}

void App::openTexture(const std::string& name) {
    const TextureInfo* t = catalog_ ? catalog_->texture(name) : nullptr;
    if (!t) {
        error(indexing_ ? "Still indexing textures, try again in a moment" : "Texture not found: " + name);
        return;
    }
    TextureInfo copy = *t;
    fs::path root = root_;
    openDocument("tex:" + name, name, [copy, root] { return makeTextureDocument(copy, root); });
}

void App::openWhenIndexed(const std::string& key) {
    if (!catalog_) {
        pendingOpen_.push_back(key);
        return;
    }
    if (startsWith(key, "tex:")) openTexture(key.substr(4));
    else if (startsWith(key, "model:")) openModel(key.substr(6));
    else if (startsWith(key, "audio:")) openAudio(key.substr(6));
    else if (startsWith(key, "script:")) openScript(key.substr(7));
}

void App::openScript(const std::string& name) {
    const ScriptInfo* s = catalog_ ? catalog_->script(name) : nullptr;
    if (!s && catalog_) s = catalog_->part(name);
    if (!s) {
        error(indexing_ ? "Still indexing, try again in a moment" : "Script not found: " + name);
        return;
    }
    ScriptInfo copy = *s;
    auto cat = catalog_;
    fs::path root = root_;
    openDocument("script:" + name, name, [copy, cat, root] { return makeScriptDocument(copy, cat, root); });
}

namespace {

// A Mods group item's tab key: its mod's, apart from the game's asset of the same name.
std::string modItemKey(const BrowserItem& it) {
    const char* kind = it.kind == BrowserItem::Texture ? "tex:" : it.kind == BrowserItem::Model ? "model:" : it.kind == BrowserItem::Audio ? "audio:" : "script:";
    return "mod:" + it.mod + ":" + kind + it.key;
}

} // namespace

void App::openModAsset(const BrowserItem& it) {
    auto v = std::find_if(modViews_.begin(), modViews_.end(), [&](const ModView& m) { return m.name == it.mod; });
    if (v == modViews_.end() || !v->catalog) return;
    const std::string key = modItemKey(it), label = it.mod + ": " + it.label;
    const fs::path root = v->root;
    const std::shared_ptr<const Catalog> merged = v->merged;
    if (it.kind == BrowserItem::Texture) {
        if (const TextureInfo* t = v->catalog->texture(it.key)) {
            TextureInfo copy = *t;
            openDocument(key, label, [copy, root] { return makeTextureDocument(copy, root); });
        }
    } else if (it.kind == BrowserItem::Model) {
        if (const ModelInfo* m = v->catalog->model(it.key)) {
            if (!closeOtherModels(key, [this, it] { openModAsset(it); })) return;
            ModelInfo copy = *m;
            openDocument(key, label, [copy, merged, root] { return makeModelDocument(copy, merged, root); });
        }
    } else if (it.kind == BrowserItem::Audio) {
        if (const AudioBankInfo* a = v->catalog->audioBank(it.key)) {
            AudioBankInfo copy = *a;
            openDocument(key, label, [copy, root] { return makeAudioDocument(copy, root, std::string()); });
        }
    } else if (it.kind == BrowserItem::Script) {
        const ScriptInfo* s = v->catalog->script(it.key);
        if (!s) s = v->catalog->part(it.key);
        if (s) {
            ScriptInfo copy = *s;
            openDocument(key, label, [copy, merged, root] { return makeScriptDocument(copy, merged, root); });
        }
    }
}

bool App::openAsset(const std::string& name) {
    if (!catalog_) return false;
    if (catalog_->script(name) || catalog_->part(name)) openScript(name);
    else if (catalog_->model(name)) openModel(name);
    else if (catalog_->texture(ops::fullName(name))) openTexture(ops::fullName(name));
    else if (catalog_->audioBank(name)) openAudio(name);
    else return false;
    return true;
}

void App::openAudio(const std::string& name, const std::string& filter) {
    const AudioBankInfo* a = catalog_ ? catalog_->audioBank(name) : nullptr;
    if (!a) {
        error(indexing_ ? "Still indexing, try again in a moment" : "Wave bank not found: " + name);
        return;
    }
    AudioBankInfo copy = *a;
    fs::path root = root_;
    openDocument("audio:" + name, name, [copy, root, filter] { return makeAudioDocument(copy, root, filter); });
}

void App::openModel(const std::string& name) {
    const ModelInfo* m = catalog_ ? catalog_->model(name) : nullptr;
    if (!m) {
        error(indexing_ ? "Still indexing models, try again in a moment" : "Model not found: " + name);
        return;
    }
    if (!closeOtherModels("model:" + name, [this, name] { openModel(name); })) return;
    ModelInfo copy = *m;
    auto cat = catalog_;
    fs::path root = root_;
    openDocument("model:" + name, name, [copy, cat, root] { return makeModelDocument(copy, cat, root); });
}

void App::openAny(const fs::path& p) {
    if (fs::is_directory(p)) {
        if (fs::exists(p / "Debug" / "db_index.txt") || fs::exists(p / "Bundle")) setAssetsRoot(p);
        else if (fs::exists(p / "assets" / "Debug" / "db_index.txt")) setAssetsRoot(p / "assets");
        else error(toUtf8(p) + " does not look like the game's assets folder");
        return;
    }
    if (fs::exists(p)) openPath(p);
    else error("No such file: " + toUtf8(p));
}

// ---- dialogs --------------------------------------------------------------------------

int App::newDialog(FileCallback cb, std::vector<Filter> filters, const fs::path& start) {
    auto req = std::make_unique<DialogRequest>();
    req->cb = std::move(cb);
    req->strings.reserve(filters.size() * 2);
    for (auto& f : filters) {
        req->strings.push_back(f.name);
        req->strings.push_back(f.pattern);
    }
    for (size_t i = 0; i < filters.size(); ++i) req->filters.push_back({req->strings[i * 2].c_str(), req->strings[i * 2 + 1].c_str()});
    // Start where the user last was: a bare file name (a suggested save name) goes into that folder.
    fs::path dir = !lastDir_.empty() && fs::is_directory(lastDir_) ? lastDir_ : root_;
    fs::path where = start;
    if (start.empty()) where = dir;
    else if (!start.has_parent_path() && !dir.empty()) where = dir / start;
    req->start = where.empty() ? std::string() : toUtf8(where);
    std::lock_guard<std::mutex> lk(dialogMutex_);
    int id = ++dialogCounter_;
    dialogs_[id] = std::move(req);
    return id;
}

void SDLCALL App::dialogCallback(void* userdata, const char* const* files, int) {
    // SDL may call this from another thread; results are handed to the UI thread.
    App* app = instance_;
    if (!app) return;
    std::string path = (files && files[0]) ? files[0] : "";
    std::lock_guard<std::mutex> lk(app->dialogMutex_);
    app->dialogResults_.emplace_back(int(intptr_t(userdata)), path);
}

void App::openFileDialog(const std::string&, std::vector<Filter> filters, FileCallback cb, const fs::path& start) {
    int id = newDialog(std::move(cb), std::move(filters), start);
    auto& r = *dialogs_[id];
    SDL_ShowOpenFileDialog(dialogCallback, reinterpret_cast<void*>(intptr_t(id)), window_, r.filters.empty() ? nullptr : r.filters.data(),
                           int(r.filters.size()), r.start.empty() ? nullptr : r.start.c_str(), false);
}

void App::saveFileDialog(const std::string&, std::vector<Filter> filters, FileCallback cb, const fs::path& start) {
    int id = newDialog(std::move(cb), std::move(filters), start);
    auto& r = *dialogs_[id];
    SDL_ShowSaveFileDialog(dialogCallback, reinterpret_cast<void*>(intptr_t(id)), window_, r.filters.empty() ? nullptr : r.filters.data(),
                           int(r.filters.size()), r.start.empty() ? nullptr : r.start.c_str());
}

void App::openFolderDialog(const std::string&, FileCallback cb, const fs::path& start) {
    int id = newDialog(std::move(cb), {}, start);
    auto& r = *dialogs_[id];
    SDL_ShowOpenFolderDialog(dialogCallback, reinterpret_cast<void*>(intptr_t(id)), window_, r.start.empty() ? nullptr : r.start.c_str(), false);
}

void App::pumpDialogs() {
    std::deque<std::pair<int, std::string>> results;
    std::map<int, std::unique_ptr<DialogRequest>> done;
    {
        std::lock_guard<std::mutex> lk(dialogMutex_);
        results.swap(dialogResults_);
        for (auto& [id, path] : results) {
            auto it = dialogs_.find(id);
            if (it != dialogs_.end()) {
                done[id] = std::move(it->second);
                dialogs_.erase(it);
            }
        }
    }
    for (auto& [id, path] : results) {
        auto it = done.find(id);
        if (it == done.end() || path.empty()) continue;
        fs::path chosen = fromUtf8(path);
        std::error_code ec;
        fs::path dir = fs::is_directory(chosen, ec) ? chosen : chosen.parent_path();
        if (!dir.empty() && dir != lastDir_) {
            lastDir_ = dir;
            saveSettings();
        }
        try {
            it->second->cb(fromUtf8(path));
        } catch (std::exception& e) {
            error(e.what());
        }
    }
}

// ---- tasks ------------------------------------------------------------------------------

void App::runReadOnlyTask(const std::string& label, bool modal, std::function<std::function<void()>(Task&)> work) {
    runTask(label, modal, std::move(work), true);
}

void App::runTask(const std::string& label, bool modal, std::function<std::function<void()>(Task&)> work, bool readOnly) {
    auto t = std::make_unique<Task>();
    t->label = label;
    t->modal = modal;
    t->readOnly = readOnly;
    Task* raw = t.get();
    tasks_.push_back(std::move(t));
    raw->thread = std::thread([raw, work = std::move(work)] {
        try {
            raw->onDone = work(*raw);
        } catch (std::exception& e) {
            raw->error = e.what();
        } catch (...) {
            raw->error = "unknown error";
        }
        raw->finished = true;
    });
}

void App::pumpTasks() {
    for (size_t i = 0; i < tasks_.size();) {
        Task& t = *tasks_[i];
        if (!t.finished) { ++i; continue; }
        t.thread.join();
        std::unique_ptr<Task> keep = std::move(tasks_[i]);
        tasks_.erase(tasks_.begin() + long(i));
        if (!keep->error.empty()) error(keep->label + ": " + keep->error);
        else if (keep->onDone) {
            try { keep->onDone(); } catch (std::exception& e) { error(e.what()); }
        }
    }
}

// ---- frame --------------------------------------------------------------------------------

void App::frame() {
    pumpDialogs();
    pumpTasks();
    if (fontsDirty_) {
        fontsDirty_ = false;
        rebuildFonts();
    }
    const ImGuiStyle overlayStyle = ImGui::GetStyle();
    ImGui::GetStyle() = style_;
    ImGui::PushFont(font_, std::round(16.0f * uiScale_));
    drawFrame();
    ImGui::PopFont();
    style_ = ImGui::GetStyle();
    ImGui::GetStyle() = overlayStyle;
}

void App::drawFrame() {
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput || io.KeyCtrl) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            for (auto& d : docs_) if (d->uid == currentTabUid_) { saveDocument(*d, io.KeyShift); break; }
        }
        if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Equal, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false))) setZoom(userZoom_ + 0.125f);
        if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false))) setZoom(userZoom_ - 0.125f);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0, false)) setZoom(1.0f);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    // See-through: its parts draw their own backgrounds, and a tab showing the game leaves its view clear.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(2);
    drawMenu();

    float statusH = ImGui::GetFrameHeightWithSpacing();
    float logH = showLog_ ? 160 * uiScale_ : 0;
    float avail = ImGui::GetContentRegionAvail().y - statusH - logH;
    const ImU32 panel = ImGui::GetColorU32(ImGuiCol_WindowBg);
    // browserWidth_ is stored at 100% scale so it survives DPI changes.
    float bw = browserWidth_ * uiScale_;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, panel);
    ImGui::BeginChild("##browser", ImVec2(bw, avail), ImGuiChildFlags_Borders);
    drawBrowser(bw);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    splitter("##split", bw, em(150), vp->WorkSize.x - em(300), avail);
    browserWidth_ = bw / uiScale_;
    bool game = false;
    for (auto& d : docs_)
        if (d->uid == currentTabUid_) game = d->showsGame();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, game ? ImU32(0) : panel);
    ImGui::BeginChild("##docs", ImVec2(0, avail));
    ImGui::PopStyleColor();
    drawDocuments();
    ImGui::EndChild();
    if (showLog_) drawLog();
    drawStatusBar();
    ImGui::End();

    drawTasks();
    drawExtractDialog();
    drawModExport();
    drawRestore();
    drawBulkImport();
    drawChangedMips();
    drawToolWindows();
    drawScript();
    drawIdTool();
    drawAbout();
    drawCloseConfirm();
}

void App::drawMenu() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open file..."))openFileDialog("Open asset file", {}, [this](const fs::path& p) { openPath(p); }, root_);
        if (ImGui::MenuItem("Rescan assets folder", nullptr, false, !root_.empty())) scanAssets();
        if (ImGui::BeginMenu("Title update folder")) {
            fs::path cur = updateRoot();
            ImGui::TextDisabled("%s", cur.empty() ? "none found" : toUtf8(cur).c_str());
            if (ImGui::MenuItem("Choose..."))
                openFolderDialog("Choose the title update folder (with its 4f... bundles)", [this](const fs::path& p) {
                    updateDir_ = p;
                    setAssetsRoot(root_);
                }, cur.empty() ? root_ : cur);
            if (ImGui::MenuItem("Find it beside the assets folder", nullptr, updateDir_.empty())) {
                updateDir_.clear();
                setAssetsRoot(root_);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        Document* cur = nullptr;
        for (auto& d : docs_) if (d->uid == currentTabUid_) cur = d.get();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, cur && cur->canSave())) saveDocument(*cur, false);
        if (ImGui::MenuItem("Save as...", "Ctrl+Shift+S", false, cur && cur->canSave())) saveDocument(*cur, true);
        if (ImGui::MenuItem("Save all", nullptr, false, !docs_.empty()))
            for (auto& d : docs_) if (d->dirty()) saveDocument(*d, false);
        if (ImGui::MenuItem("Close tab", nullptr, false, cur != nullptr)) closeDocuments({cur->uid});
        if (ImGui::MenuItem("Close all tabs", nullptr, false, !docs_.empty())) {
            std::vector<std::string> uids;
            for (auto& d : docs_) uids.push_back(d->uid);
            closeDocuments(uids);
        }
        ImGui::Separator();
        ImGui::MenuItem("Make .bak backups when saving", nullptr, &backups_);
        ImGui::Separator();
        if (ImGui::MenuItem("Back to the main menu")) requestQuit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Extract everything...", nullptr, false, !root_.empty())) showExtract_ = true;
        if (ImGui::MenuItem("Export mod package (.nbpatch)...", nullptr, false, !root_.empty())) showModExport_ = true;
        if (ImGui::MenuItem("Bulk import textures...", nullptr, false, !root_.empty())) showBulkImport_ = true;
        if (ImGui::MenuItem("Remove mipmaps from changed textures...", nullptr, false, !root_.empty())) showChangedMips_ = true;
        if (ImGui::MenuItem("Fit menus to resized textures", nullptr, false, catalog_ && !busy())) {
            auto cat = catalog_;
            docs_.clear(); // a bundle tab may show a menu that is about to change
            runTask("Fitting menus to resized textures", true, [this, cat](Task& t) -> std::function<void()> {
                int n = fixMenus(*this, *cat, &t);
                return [this, n] {
                    status(n ? "Fitted " + std::to_string(n) + " menu element(s) to their textures' new sizes (see the log)"
                             : "No menu element needed fitting");
                    if (n) buildCatalog();
                };
            });
        }
        if (ImGui::MenuItem("Restore original files (revert all)...", nullptr, false, !root_.empty())) {
            showRestore_ = true;
            restoreScanned_ = false;
        }
        if (ImGui::MenuItem("Verify all files", nullptr, false, !root_.empty())) {
            fs::path root = root_;
            runReadOnlyTask("Verifying all files", false, [this, root](Task& t) -> std::function<void()> {
                ops::Progress pr;
                pr.log = [this](const std::string& s) { log(s); };
                pr.step = [&t](double f, const std::string& s) { t.report(f, s); };
                pr.cancel = &t.cancel;
                auto r = ops::verifyAll(root, pr);
                return [this, r] {
                    if (r.failed) error("Verify: " + std::to_string(r.passed) + " passed, " + std::to_string(r.failed) + " failed (see log)");
                    else status("Verify: all " + std::to_string(r.passed) + " checks passed");
                };
            });
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Export from a file (textures, models, wave banks)...")) tools_.showExport = true;
        if (ImGui::MenuItem("Export a model (FBX / OBJ)...")) tools_.showModel = true;
        if (ImGui::MenuItem("Import into a file (texture, model, animation keys)...")) tools_.showImport = true;
        if (ImGui::MenuItem("Game scripts and objparams (export / import)...")) tools_.showScripts = true;
        if (ImGui::MenuItem("Replace an actor (model, rig and animations)...", nullptr, false, !root_.empty())) tools_.showActor = true;
        if (ImGui::MenuItem("Pack folders and convert XCTD...")) tools_.showPack = true;
        if (ImGui::MenuItem("Inspect, extract and replace (info, list, symbols, menus, fonts)...")) tools_.showFileJobs = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Script (Lua)...")) showScript_ = true;
        if (ImGui::MenuItem("Run a nuttool command...")) tools_.showConsole = true;
        if (ImGui::MenuItem("Asset ID calculator...")) showIdTool_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("reNut")) {
        if (ImGui::MenuItem("New part from a shipped part...")) tools_.showPartPicker = true;
        if (ImGui::MenuItem("New block from a .glb...")) tools_.showBlock = true;
        if (ImGui::MenuItem("New world from a model...")) tools_.showWorld = true;
        if (ImGui::MenuItem("Vehicle blueprint...")) tools_.showVehicle = true;
        if (ImGui::MenuItem("Script graphs (node editor)...")) rexfabric::ShowNodeEditor(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Export mod package (.nbpatch)...", nullptr, false, !root_.empty())) showModExport_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Log", nullptr, &showLog_);
        if (ImGui::MenuItem("Collapse all in this tab", nullptr, false, !docs_.empty())) collapseDoc_ = true;
        if (ImGui::MenuItem("Collapse the asset tree", nullptr, false, !root_.empty())) collapseBrowser_ = true;
        if (ImGui::BeginMenu("Model tab sections")) {
            bool c = false;
            c |= ImGui::MenuItem("Parts (detail level, switches)", nullptr, &modelPanels.parts);
            c |= ImGui::MenuItem("Material values", nullptr, &modelPanels.materials);
            c |= ImGui::MenuItem("Fur", nullptr, &modelPanels.fur);
            c |= ImGui::MenuItem("Meshes", nullptr, &modelPanels.meshes);
            c |= ImGui::MenuItem("Textures", nullptr, &modelPanels.textures);
            c |= ImGui::MenuItem("Skeleton", nullptr, &modelPanels.skeleton);
            c |= ImGui::MenuItem("Material names", nullptr, &modelPanels.materialNames);
            c |= ImGui::MenuItem("Warnings", nullptr, &modelPanels.warnings);
            if (c) saveSettings();
            ImGui::EndMenu();
        }
        static float zoomPct = 100;
        if (ImGui::IsWindowAppearing()) zoomPct = userZoom_ * 100;
        ImGui::SetNextItemWidth(em(160));
        ImGui::SliderFloat("UI size", &zoomPct, 50, 300, "%.0f%%");
        if (ImGui::IsItemDeactivatedAfterEdit()) setZoom(zoomPct / 100);
        ImGui::TextDisabled("Ctrl+= / Ctrl+- / Ctrl+0");
        if (ImGui::MenuItem("Dark theme", nullptr, &darkTheme_)) {
            if (darkTheme_) ImGui::StyleColorsDark();
            else ImGui::StyleColorsLight();
        }
        ImGui::EndMenu();
    }
    if (hostMenus) hostMenus();
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About")) showAbout_ = true;
        ImGui::EndMenu();
    }
    if (hostMenusRight) hostMenusRight();
    ImGui::EndMenuBar();
}

void App::drawBrowser(float) {
    if (root_.empty()) {
        ImGui::TextWrapped("reNut hasn't found the game's files.");
        return;
    }
    ImGui::TextDisabled("%s", toUtf8(root_).c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    browserFilterChanged_ = inputText("##filter", browserFilter_);
    if (browserFilter_.empty() && !ImGui::IsItemActive()) {
        ImVec2 pos = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + ImGui::GetStyle().FramePadding.x, pos.y + ImGui::GetStyle().FramePadding.y),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled), "Search assets...");
    }
    if (ImGui::SmallButton("Collapse all")) collapseBrowser_ = true;
    if (indexing_) {
        ImGui::SameLine();
        ImGui::TextDisabled("Indexing textures, models and audio...");
    }
    ImGui::Separator();
    ImGui::BeginChild("##tree");
    if (collapseBrowser_) collapseWindowTree();
    collapseBrowser_ = false;
    for (auto& g : groups_) drawBrowserGroup(g, true);
    ImGui::EndChild();
}

void App::drawBrowserGroup(const BrowserGroup& g, bool topLevel) {
    std::string needle = lower(browserFilter_);
    auto matches = [&](const BrowserItem& it) { return needle.empty() || it.lower.find(needle) != std::string::npos; };
    std::function<size_t(const BrowserGroup&)> count = [&](const BrowserGroup& x) {
        size_t n = 0;
        for (auto& it : x.items) n += matches(it);
        for (auto& c : x.children) n += count(c);
        return n;
    };
    size_t n = count(g);
    if (n == 0 && !needle.empty()) return;
    // Searching opens the groups with matches each time the search text changes; they can be
    // collapsed in between. Search results keep their own open state (another id), so clearing
    // the search brings the tree back as it was.
    bool searching = !needle.empty();
    if (searching) ImGui::SetNextItemOpen(true, browserFilterChanged_ ? ImGuiCond_Always : ImGuiCond_Once);
    char head[256];
    std::snprintf(head, sizeof head, "%s (%zu)###%s%s%p", g.name.c_str(), n, searching ? "search:" : "", g.name.c_str(), static_cast<const void*>(&g));
    if (!ImGui::TreeNodeEx(head, topLevel ? ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth : ImGuiTreeNodeFlags_SpanAvailWidth)) return;
    for (auto& c : g.children) drawBrowserGroup(c, false);
    std::vector<const BrowserItem*> shown;
    for (auto& it : g.items) if (matches(it)) shown.push_back(&it);
    ImGuiListClipper clip;
    clip.Begin(int(shown.size()));
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const BrowserItem& it = *shown[size_t(i)];
            std::string key = !it.mod.empty()                 ? modItemKey(it)
                              : it.kind == BrowserItem::Texture ? "tex:" + it.key
                              : it.kind == BrowserItem::Model   ? "model:" + it.key
                              : it.kind == BrowserItem::Audio   ? "audio:" + it.key
                              : it.kind == BrowserItem::Script  ? "script:" + it.key
                                                                : toUtf8(it.path);
            bool open = false;
            for (auto& d : docs_) if (d->key == key) open = true;
            ImGui::PushID(&it);
            if (ImGui::Selectable(it.label.c_str(), open)) {
                if (!it.mod.empty()) openModAsset(it);
                else if (it.kind == BrowserItem::Texture) openTexture(it.key);
                else if (it.kind == BrowserItem::Model) openModel(it.key);
                else if (it.kind == BrowserItem::Audio) openAudio(it.key, needle.empty() || lower(it.key).find(needle) != std::string::npos ? std::string() : needle);
                else if (it.kind == BrowserItem::Script) openScript(it.key);
                else openPath(it.path);
            }
            if (ImGui::BeginItemTooltip()) {
                ImGui::TextUnformatted(it.tooltip.c_str());
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
    }
    ImGui::TreePop();
}

void App::drawDocuments() {
    if (docs_.empty()) {
        collapseDoc_ = false;
        drawWelcome();
        return;
    }
    if (!ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_AutoSelectNewTabs)) return;
    std::vector<std::string> toClose; // closed after the tab bar
    auto allUids = [&] {
        std::vector<std::string> u;
        for (auto& d : docs_) u.push_back(d->uid);
        return u;
    };
    if (ImGui::TabItemButton("Close all", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) toClose = allUids();
    ImGui::SetItemTooltip("Close every tab (asks about unsaved changes)");
    if (ImGui::TabItemButton("Collapse all", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) collapseDoc_ = true;
    ImGui::SetItemTooltip("Collapse every section of this tab");
    for (size_t i = 0; i < docs_.size(); ++i) {
        Document& d = *docs_[i];
        bool open = true;
        ImGuiTabItemFlags flags = d.dirty() ? ImGuiTabItemFlags_UnsavedDocument : 0;
        if (selectTab_ == d.uid && ImGui::GetFrameCount() > 1) flags |= ImGuiTabItemFlags_SetSelected;
        std::string label = d.title() + "###" + d.uid;
        bool shown = ImGui::BeginTabItem(label.c_str(), &open, flags);
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Close")) toClose = {d.uid};
            if (ImGui::MenuItem("Close other tabs", nullptr, false, docs_.size() > 1))
                for (auto& o : docs_)
                    if (o->uid != d.uid) toClose.push_back(o->uid);
            if (ImGui::MenuItem("Close tabs to the right", nullptr, false, i + 1 < docs_.size()))
                for (size_t k = i + 1; k < docs_.size(); ++k) toClose.push_back(docs_[k]->uid);
            if (ImGui::MenuItem("Close all tabs")) toClose = allUids();
            ImGui::EndPopup();
        }
        if (shown) {
            if (selectTab_ == d.uid) selectTab_.clear();
            currentTabUid_ = d.uid;
            ImGui::BeginChild("##doc");
            if (collapseDoc_) collapseWindowTree();
            g_collapsing = collapseDoc_;
            try {
                d.draw(*this);
            } catch (std::exception& e) {
                error(d.title() + ": " + e.what());
            }
            g_collapsing = false;
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (!open) toClose = {d.uid};
    }
    collapseDoc_ = false;
    ImGui::EndTabBar();
    if (!toClose.empty()) closeDocuments(toClose);
}

void App::drawWelcome() {
    ImGui::Dummy(ImVec2(0, 20 * uiScale_));
    ImGui::Indent(20 * uiScale_);
    ImGui::TextUnformatted("NutTool - Banjo-Kazooie: Nuts & Bolts asset editor");
    ImGui::Spacing();
    if (root_.empty()) {
        ImGui::TextWrapped("Start by choosing the game's assets folder (the one with Bundle, Debug, loctext...).");
        if (ImGui::Button("Choose assets folder..."))
            openFolderDialog("Choose the assets folder", [this](const fs::path& p) { setAssetsRoot(p); });
    } else {
        ImGui::BulletText("Pick an asset on the left to open it. Use the search box to filter by name.");
        ImGui::BulletText("Bundles: browse every asset inside, preview and replace textures, export or replace raw data.");
        ImGui::BulletText("Text: edit strings for any language side by side with English.");
        ImGui::BulletText("Fonts: view glyphs, edit metrics, export/import glyph pages, export as TTF / BMFont.");
        ImGui::BulletText("Scripts: read and edit scene scripts, actor goals and script tables as text, and see what uses each one.");
        ImGui::BulletText("Tools > Extract everything... unpacks the whole game into a folder.");
        ImGui::Spacing();
        ImGui::TextDisabled("Saving writes plain (uncompressed) files and keeps a .bak of the original.");
    }
    ImGui::Unindent();
}

void App::drawStatusBar() {
    ImGui::Separator();
    for (auto& t : tasks_) {
        if (t->modal) continue;
        double p;
        std::string step;
        {
            std::lock_guard<std::mutex> lk(t->m);
            p = t->progress;
            step = t->step;
        }
        ImGui::PushID(t.get());
        ImGui::ProgressBar(p < 0 ? float(-1.0 * ImGui::GetTime()) : float(p), ImVec2(180 * uiScale_, 0), t->label.c_str());
        if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted(step.c_str()); ImGui::EndTooltip(); }
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) t->cancel = true;
        ImGui::SameLine();
        ImGui::PopID();
    }
    if (statusError_) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", status_.c_str());
    else ImGui::TextUnformatted(status_.c_str());
}

void App::drawLog() {
    ImGui::BeginChild("##log", ImVec2(0, 160 * uiScale_ - ImGui::GetStyle().ItemSpacing.y), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    std::lock_guard<std::mutex> lk(logMutex_);
    ImGuiListClipper clip;
    clip.Begin(int(log_.size()));
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) ImGui::TextUnformatted(log_[size_t(i)].c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void App::drawTasks() {
    Task* modal = nullptr;
    for (auto& t : tasks_) if (t->modal) { modal = t.get(); break; }
    if (modal && !ImGui::IsPopupOpen("Working")) ImGui::OpenPopup("Working");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Working", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        if (!modal) ImGui::CloseCurrentPopup();
        else {
            double p;
            std::string step;
            {
                std::lock_guard<std::mutex> lk(modal->m);
                p = modal->progress;
                step = modal->step;
            }
            ImGui::TextUnformatted(modal->label.c_str());
            ImGui::ProgressBar(p < 0 ? float(-1.0 * ImGui::GetTime()) : float(p), ImVec2(320 * uiScale_, 0), step.empty() ? "" : step.c_str());
        }
        ImGui::EndPopup();
    }
}

void App::drawExtractDialog() {
    if (showExtract_) ImGui::OpenPopup("Extract everything");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Extract everything", &showExtract_, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Unpack every asset into editable files (textures as DDS, text as JSON, ...).");
    ImGui::SetNextItemWidth(420 * uiScale_);
    inputText("Output folder", extractOut_);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) openFolderDialog("Output folder", [this](const fs::path& p) { extractOut_ = toUtf8(p); });
    ImGui::Checkbox("Textures only", &extractTexturesOnly_);
    ImGui::SameLine();
    helpMarker("Just the textures, from every bundle and stream bundle, as PNG in one \"textures\" folder (one copy of each; "
               "split textures merged). These are the names Bulk import textures reads back.");
    static const char* cats[] = {"bundles", "streams", "text", "video", "fonts", "ini", "misc"};
    static const char* catLabels[] = {"Bundles", "Stream bundles (incl. audio)", "Text", "Videos", "Fonts", "Font metadata", "Other files"};
    if (extractTexturesOnly_) {
        ImGui::Checkbox("Also write DDS", &extractDds_);
    } else {
        for (int i = 0; i < 7; ++i) {
            ImGui::Checkbox(catLabels[i], &extractCats_[i]);
            if (i % 3 != 2 && i != 6) ImGui::SameLine();
        }
        ImGui::Checkbox("Also write PNG copies of textures", &extractPng_);
    }
    ImGui::SetNextItemWidth(260 * uiScale_);
    inputText("Only names containing", extractFilter_);
    ImGui::SameLine();
    helpMarker("Leave empty for everything. With text such as ui_, only what is named with it is written: textures, the "
               "symbols of bundles and entries of stream bundles, wave banks, text tables, videos and fonts. A filtered bundle "
               "is written as loose files, without the caff.json needed to pack it back.");
    if (!extractTexturesOnly_ && extractFilter_.empty()) ImGui::TextDisabled("A full extraction writes about 8 GB and takes a few minutes.");
    // With no kind ticked, the extraction would take every kind.
    bool anyKind = extractTexturesOnly_;
    for (bool c : extractCats_) anyKind |= c;
    if (!anyKind) ImGui::TextDisabled("Tick at least one kind of file.");
    ImGui::BeginDisabled(extractOut_.empty() || !anyKind);
    if (ImGui::Button("Start")) {
        ops::ExtractOptions opt;
        opt.png = extractPng_;
        opt.texturesOnly = extractTexturesOnly_;
        opt.texturesDds = extractDds_;
        opt.filter = trim(extractFilter_);
        for (int i = 0; i < 7; ++i) if (extractCats_[i]) opt.only.insert(cats[i]);
        fs::path root = root_, out = fromUtf8(extractOut_);
        saveSettings();
        runTask("Extracting", false, [this, root, out, opt](Task& t) -> std::function<void()> {
            ops::Progress pr;
            pr.log = [this](const std::string& s) { log(s); };
            pr.step = [&t](double f, const std::string& s) { t.report(f, s); };
            pr.cancel = &t.cancel;
            int failures = ops::extractAll(root, out, opt, pr);
            bool cancelled = t.cancel;
            return [this, failures, cancelled, out] {
                if (cancelled) status("Extraction cancelled");
                else if (failures) error("Extraction finished with " + std::to_string(failures) + " errors (see log)");
                else status("Extraction finished: " + toUtf8(out));
            };
        });
        showExtract_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close")) { showExtract_ = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void App::drawModExport() {
    if (!showModExport_) return;
    ImGui::SetNextWindowSize(ImVec2(em(660), em(600)), ImGuiCond_Appearing);
    if (!ImGui::Begin("Export mod package", &showModExport_)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Packages the game files you changed as a .nbpatch mod (NB Studio's format). reNut applies it at startup "
                       "to an unmodified game folder, without writing to that folder; put it in reNut's mods folder.");

    ImGui::SeparatorText("Mod");
    const float w = em(380);
    ImGui::SetNextItemWidth(w);
    inputText("Name", mod_.name);
    ImGui::SetNextItemWidth(w);
    inputText("Version", mod_.version);
    ImGui::SetNextItemWidth(w);
    inputText("Author", mod_.author);
    ImGui::SetNextItemWidth(w);
    inputText("Category", mod_.category);
    ImGui::SetNextItemWidth(w);
    inputText("Id", mod_.id);
    ImGui::SameLine();
    helpMarker("Leave empty to use the name in lower case with dashes. Other mods name this one by id when they need it "
               "or cannot be combined with it.");
    if (mod_.id.empty() && !mod_.name.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", nbpatch::slug(mod_.name).c_str());
    }
    inputTextMultiline("Description", mod_.description, ImVec2(w, em(52)));

    ImGui::SeparatorText("Changed files");
    bool rescan = false;
    rescan |= ImGui::RadioButton("Files that have a .bak backup (saved by NutTool with backups on)", &modSource_, 0);
    rescan |= ImGui::RadioButton("Files that differ from a clean copy of the game folder", &modSource_, 1);
    if (modSource_ == 1) {
        ImGui::SetNextItemWidth(em(460));
        rescan |= inputText("##original", modOriginal_);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##original"))
            openFolderDialog("Clean game folder", [this](const fs::path& p) { modOriginal_ = toUtf8(p); modScanned_ = false; });
    }
    if (rescan) modScanned_ = false;
    ImGui::BeginDisabled(busy() || (modSource_ == 1 && modOriginal_.empty()));
    if (ImGui::Button("Find changed files")) {
        fs::path root = root_, orig = modSource_ == 1 ? fromUtf8(modOriginal_) : fs::path();
        fs::path update = updateRoot(), origUpdate = orig.empty() ? fs::path() : ops::updateFolder(orig);
        saveSettings();
        runReadOnlyTask("Looking for changed files", true, [this, root, orig, update, origUpdate](Task& t) -> std::function<void()> {
            ops::Progress pr;
            pr.log = [this](const std::string& s) { log(s); };
            pr.step = [&t](double f, const std::string& s) { t.report(f, s); };
            pr.cancel = &t.cancel;
            auto found = std::make_shared<std::vector<nbpatch::Change>>(nbpatch::findChanges(root, orig, pr));
            // The title update's edits go in as update/<file>, which reNut serves as UPDATE: files.
            nbpatch::addUpdateChanges(*found, root, update, origUpdate, pr);
            return [this, found] {
                modChanges_ = std::move(*found);
                modPick_.assign(modChanges_.size(), 1);
                modScanned_ = true;
                status(std::to_string(modChanges_.size()) + " changed file(s)");
            };
        });
    }
    ImGui::EndDisabled();
    if (modScanned_) {
        ImGui::SameLine();
        if (modChanges_.empty()) ImGui::TextDisabled("No changed files.");
        else ImGui::Text("%zu changed file(s)", modChanges_.size());
    }
    if (ImGui::BeginChild("##modfiles", ImVec2(0, em(150)), ImGuiChildFlags_Borders)) {
        for (size_t i = 0; i < modChanges_.size(); ++i) {
            const auto& c = modChanges_[i];
            bool on = modPick_[i] != 0;
            if (ImGui::Checkbox(c.path.c_str(), &on)) modPick_[i] = on;
            ImGui::SameLine();
            ImGui::TextDisabled(c.original.empty() ? "new file" : "changed");
        }
    }
    ImGui::EndChild();

    ImGui::SeparatorText("Package");
    ImGui::SetNextItemWidth(em(460));
    inputText("##out", modOut_);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##out")) {
        std::string name = (mod_.id.empty() ? nbpatch::slug(mod_.name) : mod_.id);
        saveFileDialog("Save mod package", {{"Mod package", "nbpatch"}}, [this](const fs::path& p) {
            fs::path q = p;
            if (lower(q.extension().string()) != ".nbpatch") q += ".nbpatch";
            modOut_ = toUtf8(q);
        }, lastDir_ / (name.empty() ? "mod.nbpatch" : name + ".nbpatch"));
    }
    size_t picked = size_t(std::count(modPick_.begin(), modPick_.end(), 1));
    ImGui::BeginDisabled(busy() || trim(mod_.name).empty() || modOut_.empty() || picked == 0);
    if (ImGui::Button("Build package")) {
        std::vector<nbpatch::Change> changes;
        for (size_t i = 0; i < modChanges_.size(); ++i)
            if (modPick_[i]) changes.push_back(modChanges_[i]);
        nbpatch::Info info = mod_;
        fs::path out = fromUtf8(modOut_);
        saveSettings();
        runTask("Building mod package", true, [this, changes, info, out](Task& t) -> std::function<void()> {
            ops::Progress pr;
            pr.log = [this](const std::string& s) { log(s); };
            pr.step = [&t](double f, const std::string& s) { t.report(f, s); };
            pr.cancel = &t.cancel;
            log("Building " + toUtf8(out));
            bool ok = nbpatch::write(out, info, changes, pr);
            return [this, ok, out] {
                if (ok) status("Wrote " + toUtf8(out));
                else {
                    error("The mod package was not written (see the log)");
                    showLog_ = true;
                }
            };
        });
    }
    ImGui::EndDisabled();
    if (trim(mod_.name).empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Give the mod a name.");
    }
    ImGui::End();
}

// Puts the .bak backups back over the files saved since, undoing the edits.
void App::drawRestore() {
    if (!showRestore_) return;
    ImGui::SetNextWindowSize(ImVec2(em(600), em(420)), ImGuiCond_Appearing);
    if (!ImGui::Begin("Restore original files", &showRestore_)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("NutTool backs a file up as <file>.bak the first time it saves over it. Restoring copies that backup "
                       "back, undoing every change saved to the file since. The backups are kept. Open tabs are closed "
                       "and the assets re-indexed afterwards.");
    ImGui::BeginDisabled(busy());
    // Look as soon as the window opens, and again on request.
    if (ImGui::Button("Look again") || (!restoreScanned_ && !busy())) {
        fs::path root = root_, update = updateRoot();
        runReadOnlyTask("Looking for edited files", true, [this, root, update](Task& t) -> std::function<void()> {
            ops::Progress pr;
            pr.log = [this](const std::string& s) { log(s); };
            pr.step = [&t](double f, const std::string& s) { t.report(f, s); };
            pr.cancel = &t.cancel;
            auto found = std::make_shared<std::vector<nbpatch::Change>>(nbpatch::findChanges(root, {}, pr));
            nbpatch::addUpdateChanges(*found, root, update, {}, pr);
            return [this, found] {
                restoreChanges_ = std::move(*found);
                restorePick_.assign(restoreChanges_.size(), 1);
                restoreScanned_ = true;
            };
        });
    }
    ImGui::EndDisabled();
    if (restoreScanned_) {
        ImGui::SameLine();
        if (restoreChanges_.empty()) ImGui::TextDisabled("No file differs from its backup.");
        else ImGui::Text("%zu edited file(s)", restoreChanges_.size());
    }
    float listH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginChild("##restorefiles", ImVec2(0, std::max(listH, em(60))), ImGuiChildFlags_Borders)) {
        for (size_t i = 0; i < restoreChanges_.size(); ++i) {
            bool on = restorePick_[i] != 0;
            if (ImGui::Checkbox(restoreChanges_[i].path.c_str(), &on)) restorePick_[i] = on;
        }
    }
    ImGui::EndChild();
    size_t picked = size_t(std::count(restorePick_.begin(), restorePick_.end(), 1));
    ImGui::BeginDisabled(busy() || picked == 0);
    std::string label = "Restore " + std::to_string(picked) + " file(s)...";
    if (ImGui::Button(label.c_str())) ImGui::OpenPopup("Restore?");
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("Restore?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Undo every saved change to %zu file(s)? This cannot be undone.", picked);
        bool anyDirty = false;
        for (auto& d : docs_) anyDirty |= d->dirty();
        if (anyDirty) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Unsaved changes in open tabs are discarded.");
        if (ImGui::Button("Restore")) {
            std::vector<nbpatch::Change> todo;
            for (size_t i = 0; i < restoreChanges_.size(); ++i)
                if (restorePick_[i]) todo.push_back(restoreChanges_[i]);
            docs_.clear(); // tabs may show what is about to change
            runTask("Restoring original files", true, [this, todo](Task& t) -> std::function<void()> {
                int done = 0;
                std::string failed;
                for (size_t i = 0; i < todo.size(); ++i) {
                    t.report(double(i) / double(todo.size()), todo[i].path);
                    std::error_code ec;
                    fs::copy_file(longPath(todo[i].original), longPath(todo[i].modified), fs::copy_options::overwrite_existing, ec);
                    if (ec) failed += "\n" + todo[i].path + ": " + ec.message();
                    else ++done;
                }
                return [this, done, failed] {
                    restoreScanned_ = false;
                    restoreChanges_.clear();
                    restorePick_.clear();
                    if (!failed.empty()) error("Some files were not restored:" + failed);
                    status("Restored " + std::to_string(done) + " file(s) from their backups");
                    buildCatalog();
                };
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}

// Imports a folder of images into the textures they are named after.
void App::drawBulkImport() {
    if (!showBulkImport_) return;
    ImGui::SetNextWindowSize(ImVec2(em(600), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Bulk import textures", &showBulkImport_)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Imports every image in a folder into the texture it is named after, as Export PNG and Extract everything "
                       "name them (aid_texture_banjox_particles_spanglewhite.png). Each image goes into every copy of its texture, as "
                       "Import image in a texture tab does: an image of another size resizes the texture, and a DDS must use the "
                       "texture's format. Images that match their texture exactly are skipped, so a folder of exports with a few "
                       "edited ones changes only those. Changed files are saved with a .bak backup. Open tabs are closed first.");
    ImGui::SetNextItemWidth(em(460));
    inputText("##bulkfolder", bulkFolder_);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##bulk"))
        openFolderDialog("Folder of texture images", [this](const fs::path& p) { bulkFolder_ = toUtf8(p); }, fromUtf8(bulkFolder_));
    bool anyDirty = false;
    for (auto& d : docs_) anyDirty |= d->dirty();
    if (anyDirty) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Unsaved changes in open tabs are discarded.");
    auto cat = catalog_;
    if (!cat) ImGui::TextDisabled("Waiting for the asset index...");
    ImGui::BeginDisabled(busy() || !cat || bulkFolder_.empty());
    if (ImGui::Button("Import")) {
        fs::path folder = fromUtf8(bulkFolder_);
        saveSettings();
        docs_.clear(); // tabs may show what is about to change
        runTask("Importing textures", true, [this, cat, folder](Task& t) -> std::function<void()> {
            BulkImportResult r = bulkImport(*this, *cat, folder, t);
            return [this, r] {
                std::string s = "Imported " + std::to_string(r.imported) + " texture(s) into " + std::to_string(r.files) + " file(s); " +
                                std::to_string(r.unchanged) + " unchanged, " + std::to_string(r.unmatched) + " not named after a texture";
                if (r.menuElements) s += "; fitted " + std::to_string(r.menuElements) + " menu element(s)";
                if (r.failed) {
                    error(s + ", " + std::to_string(r.failed) + " not imported (see the log)");
                    showLog_ = true;
                } else {
                    status(s);
                }
                if (r.files) buildCatalog();
            };
        });
    }
    ImGui::EndDisabled();
    ImGui::End();
}

// Removes the mipmaps of every texture that differs from its file's .bak backup.
void App::drawChangedMips() {
    if (!showChangedMips_) return;
    ImGui::SetNextWindowSize(ImVec2(em(560), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Remove mipmaps from changed textures", &showChangedMips_)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Removes the mipmaps of every texture you changed: the textures that differ from their file's .bak backup "
                       "(imported, resized...). The other textures in those files are left alone. As with Remove mipmaps in a "
                       "texture tab, the full-size picture is kept as it is and a split texture keeps level 1. Open tabs are closed first.");
    bool anyDirty = false;
    for (auto& d : docs_) anyDirty |= d->dirty();
    if (anyDirty) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Unsaved changes in open tabs are discarded.");
    if (ImGui::RadioButton("Compare with the .bak backups##mips", !mipsFromOriginal_)) mipsFromOriginal_ = false;
    if (ImGui::RadioButton("Compare with a clean copy of the game folder##mips", mipsFromOriginal_)) mipsFromOriginal_ = true;
    if (mipsFromOriginal_) {
        ImGui::SetNextItemWidth(em(400));
        inputText("##mipsoriginal", modOriginal_);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##mipsoriginal"))
            openFolderDialog("Clean game folder", [this](const fs::path& p) { modOriginal_ = toUtf8(p); }, fromUtf8(modOriginal_));
        ImGui::SameLine();
        helpMarker("The textures that differ from this untouched copy, or that it lacks, lose their mipmaps (as the command line's "
                   "--original). The title update's files are compared with their backups only without it.");
    }
    ImGui::BeginDisabled(busy() || (mipsFromOriginal_ && trim(modOriginal_).empty()));
    if (ImGui::Button("Remove mipmaps")) {
        fs::path root = root_, update = updateRoot();
        fs::path original = mipsFromOriginal_ ? fromUtf8(trim(modOriginal_)) : fs::path();
        saveSettings();
        docs_.clear();
        runTask("Removing mipmaps from changed textures", true, [this, root, update, original](Task& t) -> std::function<void()> {
            ops::Progress pr;
            pr.log = [this](const std::string& s) { log(s); };
            pr.step = [&t](double f, const std::string& s) { t.report(f, s); };
            pr.cancel = &t.cancel;
            auto changes = nbpatch::findChanges(root, original, pr);
            if (original.empty())
                for (auto& c : nbpatch::findChanges(update, {}, pr)) changes.push_back(c);
            int textures = 0, files = 0;
            for (size_t i = 0; i < changes.size() && !t.cancel; ++i) {
                t.report(double(i) / double(changes.size()), changes[i].path);
                try {
                    int n = ops::removeChangedMipmaps(changes[i].modified, changes[i].original, pr);
                    textures += n;
                    files += n ? 1 : 0;
                } catch (std::exception& e) {
                    log("warning: " + changes[i].path + ": " + e.what());
                }
            }
            return [this, textures, files] {
                status("Removed the mipmaps of " + std::to_string(textures) + " changed texture(s) in " + std::to_string(files) + " file(s)");
                if (files) buildCatalog();
            };
        });
    }
    ImGui::EndDisabled();
    ImGui::End();
}

void App::drawIdTool() {
    if (!showIdTool_) return;
    ImGui::SetNextWindowSize(ImVec2(520 * uiScale_, 0), ImGuiCond_Appearing);
    if (ImGui::Begin("Asset ID calculator", &showIdTool_)) {
        ImGui::TextWrapped("Type an asset name such as aid_loctext_banjox_frontend or aid_texture_banjox_particles_spanglewhite, "
                           "or several, one per line.");
        inputTextMultiline("##name", idToolInput_, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4 + ImGui::GetStyle().FramePadding.y * 2));
        int row = 0;
        for (auto& line : split(idToolInput_, '\n')) {
            std::string name = trim(line);
            if (name.empty()) continue;
            ImGui::PushID(row++);
            if (row > 1) ImGui::Separator();
            if (auto id = assetdb::idFromName(name)) {
                ImGui::Text("%s\nid: %s   type: %s", name.c_str(), hex32(*id).c_str(), assetdb::typeName(uint8_t(*id >> 24)).c_str());
                fs::path p = assetdb::pathForId(root_, *id);
                ImGui::Text("file: %s", toUtf8(root_.empty() ? assetdb::pathForId("", *id) : fs::relative(p, root_)).c_str());
                if (!root_.empty()) {
                    if (fs::exists(p)) {
                        if (ImGui::Button("Open")) openPath(p);
                    } else {
                        ImGui::TextDisabled("(no such file; the asset may live inside a bundle)");
                    }
                }
            } else {
                ImGui::TextDisabled("%s: unknown asset type prefix.", name.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}

void App::drawAbout() {
    if (showAbout_) ImGui::OpenPopup("About NutTool");
    if (ImGui::BeginPopupModal("About NutTool", &showAbout_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("NutTool - Banjo-Kazooie: Nuts & Bolts asset tool");
        ImGui::Spacing();
        ImGui::TextUnformatted("Reads, unpacks and edits bundles, stream bundles, text, textures,");
        ImGui::TextUnformatted("fonts, audio banks and videos. See README.md for the formats.");
        ImGui::Spacing();
        ImGui::TextDisabled("Dear ImGui %s, SDL %d.%d.%d", IMGUI_VERSION, SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
        if (ImGui::Button("Close")) { showAbout_ = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void App::saveDocument(Document& d, bool saveAs) {
    if (!d.canSave()) return;
    if (saveAs) {
        std::string uid = d.uid;
        saveFileDialog("Save as", {}, [this, uid](const fs::path& p) {
            for (auto& doc : docs_)
                if (doc->uid == uid && doc->save(*this, &p)) status("Saved " + toUtf8(p));
        }, d.path);
        return;
    }
    if (d.save(*this, nullptr)) status("Saved " + toUtf8(d.path));
}

bool App::closeOtherModels(const std::string& key, std::function<void()> retry) {
    // The studio shows every open model in the same place.
    std::vector<std::string> others;
    bool dirty = false;
    for (auto& d : docs_)
        if (d->showsGame() && d->key != key) {
            others.push_back(d->uid);
            dirty |= d->dirty();
        }
    if (dirty) {
        closeUids_ = others;
        afterClose_ = std::move(retry);
        return false;
    }
    removeDocuments(others);
    return true;
}

void App::closeDocuments(const std::vector<std::string>& uids) {
    for (auto& d : docs_)
        if (d->dirty() && std::find(uids.begin(), uids.end(), d->uid) != uids.end()) {
            closeUids_ = uids;
            return;
        }
    removeDocuments(uids);
}

void App::removeDocuments(const std::vector<std::string>& uids) {
    docs_.erase(std::remove_if(docs_.begin(), docs_.end(), [&](const std::unique_ptr<Document>& d) {
                    return std::find(uids.begin(), uids.end(), d->uid) != uids.end();
                }),
                docs_.end());
}

void App::requestQuit() {
    bool anyDirty = false;
    for (auto& d : docs_) anyDirty |= d->dirty();
    if (anyDirty) quitPending_ = true;
    else quit_ = true;
}

void App::drawCloseConfirm() {
    bool want = !closeUids_.empty() || quitPending_;
    if (want && !ImGui::IsPopupOpen("Unsaved changes")) ImGui::OpenPopup("Unsaved changes");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    std::vector<size_t> which;
    for (size_t i = 0; i < docs_.size(); ++i)
        if (docs_[i]->dirty() && (quitPending_ || std::find(closeUids_.begin(), closeUids_.end(), docs_[i]->uid) != closeUids_.end()))
            which.push_back(i);
    ImGui::TextUnformatted("These documents have unsaved changes:");
    for (size_t i : which) ImGui::BulletText("%s", docs_[i]->title().c_str());
    auto finish = [&](bool proceed) {
        std::function<void()> then = std::exchange(afterClose_, nullptr);
        if (proceed) {
            if (quitPending_) quit_ = true;
            else removeDocuments(closeUids_);
        }
        const bool open = proceed && !quitPending_ && then;
        quitPending_ = false;
        closeUids_.clear();
        ImGui::CloseCurrentPopup();
        if (open) then();
    };
    if (ImGui::Button("Save")) {
        bool ok = true;
        for (size_t i : which) ok &= docs_[i]->save(*this, nullptr);
        finish(ok);
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) finish(true);
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) finish(false);
    ImGui::EndPopup();
}

} // namespace nut::gui
