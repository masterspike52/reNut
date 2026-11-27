// Game script tab: the text of a scene script, actor goal list, script table or state table, with
// what it points at and what points at it. Saving writes every copy in the game's bundles.
#include "docs.hpp"

#include "assetdb.hpp"
#include "container.hpp"
#include "modelimport.hpp"
#include "objparams.hpp"
#include "renutmake.hpp"
#include "renutpart.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>

namespace nut::gui {

void ScriptEditor::load(std::string initial, Build b) {
    build = std::move(b);
    text = saved = std::move(initial);
    check();
}

void ScriptEditor::loadAsset(const std::string& assetName, const Bytes& data, const gamescript::NameLookup& names) {
    if (auto kind = gamescript::kindOf(assetName)) {
        gamescript::Kind k = *kind;
        load(gamescript::toText(k, data, names), [k](const std::string& t) { return gamescript::fromText(k, t); });
    } else if (objparams::isObjParams(data)) {
        load(objparams::toText(data, names), [data](const std::string& t) { return objparams::fromText(data, t); });
    } else {
        fail(assetName + " is neither a game script nor an objparams record");
    }
}

void ScriptEditor::check() {
    try {
        bytes = build(text).size();
        problem.clear();
    } catch (std::exception& e) {
        problem = e.what();
    }
}

bool ScriptEditor::draw(const char* id, const ImVec2& size) {
    ImVec2 box(size.x, std::max(em(80), size.y - ImGui::GetTextLineHeightWithSpacing()));
    bool changed = inputTextMultiline(id, text, box, ImGuiInputTextFlags_AllowTabInput);
    if (changed) check();
    if (!problem.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", problem.c_str());
    else ImGui::TextDisabled("%s%zu bytes", modified() ? "edited, builds to " : "", bytes);
    return changed;
}

void assetList(App& app, const char* id, const std::vector<std::string>& names, float height) {
    ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_Borders);
    if (names.empty()) ImGui::TextDisabled("none found");
    auto cat = app.catalog();
    ImGuiListClipper clip;
    clip.Begin(int(names.size()));
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const std::string& n = names[size_t(i)];
            ImGui::PushID(i);
            bool known = cat && (cat->script(n) || cat->part(n) || cat->model(n) || cat->texture(ops::fullName(n)) || cat->audioBank(n));
            if (!known) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(n.c_str()) && known) app.openAsset(n);
            if (!known) ImGui::PopStyleColor();
            if (ImGui::BeginItemTooltip()) {
                auto aid = assetdb::idFromName(n);
                ImGui::Text("%s\n%s%s", n.c_str(), aid ? (assetdb::typeName(uint8_t(*aid >> 24)) + ", id " + hex32(*aid)).c_str() : "",
                            known ? "\nClick to open it" : "\nNo tab for this kind of asset: open its bundle to see it");
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

namespace {

// Choices offered for a reNut part's collision and attachable faces (any other text works too).
const std::vector<std::string> kCollisions = {"none", "body_light_cube", "body_light_wedge", "body_light_corner"};
const std::vector<std::string> kAttachable = {"all", "none", "top", "bottom", "sides", "top+sides"};

class ScriptDocument : public Document {
public:
    ScriptDocument(ScriptInfo info, std::shared_ptr<const Catalog> cat, const fs::path& root) : info_(std::move(info)), cat_(std::move(cat)), root_(root) {
        if (info_.locs.empty()) fail("no copies of " + info_.name + " were found");
        path = info_.locs.front().file;
        CaffCache cache;
        caff::Caff& c = cache.get(info_.locs.front());
        uint32_t si = info_.locs.front().symbol;
        if (si >= c.symbols.size() || assetdb::baseAssetName(c.symbolName(si)) != info_.name)
            fail(toUtf8(path) + " changed since it was indexed; reopen the assets folder to index it again");
        Bytes data = c.symbolData(si);
        editor_.loadAsset(info_.name, data, lookup());
        findUses(data);
        original_ = std::move(data);
        part_.id = "mymod_" + renutpart::baseName(info_.name);
    }

    std::string title() const override { return info_.name; }
    bool dirty() const override { return editor_.modified(); }
    bool canSave() const override { return true; }

    void draw(App& app) override {
        ImGui::TextUnformatted(info_.name.c_str());
        ImGui::SameLine();
        size_t files = copies();
        ImGui::TextDisabled("%s, %zu %s", info_.objParams ? "vehicle part" : gamescript::kindLabel(info_.kind), files, files == 1 ? "bundle" : "bundles");
        if (ImGui::BeginItemTooltip()) {
            for (auto& l : info_.locs) ImGui::TextUnformatted((toUtf8Generic(fs::relative(l.file, root_)) + (l.entry >= 0 ? "  entry " + std::to_string(l.entry + 1) : "")).c_str());
            ImGui::EndTooltip();
        }
        ImGui::BeginDisabled(!editor_.problem.empty() || !editor_.modified());
        if (ImGui::Button("Save to every copy")) save(app, nullptr);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!editor_.modified());
        if (ImGui::Button("Revert")) {
            editor_.text = editor_.saved;
            editor_.check();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Export text...")) {
            std::string text = "# " + info_.name + "\n" + editor_.text;
            app.saveFileDialog("Export script text", {{"Text", "txt"}}, [&app, text](const fs::path& p) {
                writeText(p, text);
                app.status("Exported " + toUtf8(p));
            }, fromUtf8(info_.name + ".txt"));
        }
        ImGui::SameLine();
        if (ImGui::Button("Import text...")) {
            app.openFileDialog("Import script text", {{"Text", "txt"}, {"All files", "*"}}, [this, &app](const fs::path& p) {
                editor_.text = readText(p);
                editor_.check();
                app.status(editor_.problem.empty() ? "Loaded " + toUtf8(p) + " (save to apply it)" : "Loaded " + toUtf8(p) + ": " + editor_.problem);
            });
        }
        if (info_.objParams) {
            ImGui::SameLine();
            ImGui::BeginDisabled(!editor_.problem.empty());
            bool requested = app.takePartDialog(info_.name) && editor_.problem.empty(); // reNut > New part
            if (ImGui::Button("Save as reNut part...") || requested) ImGui::OpenPopup("Save as reNut part");
            ImGui::EndDisabled();
            drawPartPopup(app);
        }
        helpMarker(info_.objParams
                       ? "One field per line, name = value. Fields without a name go by offset; words that are 0 are not listed, "
                         "and adding a line such as 0x3C4 = 1.5 sets one. Values: 12 or -3 (int), 1.5 (float), 0x1A2B3C4D (hex), "
                         "@aid_name (an asset, by name), \"text\" (a string field), <0a0b> (raw bytes), # starts a comment.\n"
                         "The ratings are the parts shop's 0-5 stars, separate from the values the game simulates with (mass, "
                         "health, power). Saving writes every bundle that holds this part (with .bak backups when enabled)."
                       : "One record per line. Values: 12 or -3 (int), 1.5 (float), 0x1A2B3C4D (hex), @aid_name (an asset, by name), "
                         "\"text\" (a string field), <0a0b> (raw bytes), # starts a comment.\n"
                         "Known opcodes and goals must keep their argument count and types' sizes; the line under the editor says "
                         "what is wrong. Saving writes every bundle that holds this asset (with .bak backups when enabled).");

        float h = ImGui::GetContentRegionAvail().y, w = ImGui::GetContentRegionAvail().x;
        float lw = leftFrac_ * w;
        ImGui::BeginChild("##scripttext", ImVec2(lw, h));
        editor_.draw("##script", ImGui::GetContentRegionAvail());
        ImGui::EndChild();
        splitter("##scriptsplit", lw, em(300), w - em(200), h);
        leftFrac_ = lw / w;
        ImGui::BeginChild("##scriptrefs", ImVec2(0, h));
        float lh = (ImGui::GetContentRegionAvail().y - 2 * ImGui::GetTextLineHeightWithSpacing()) / 2;
        ImGui::Text("Uses (%zu)", uses_.size());
        helpMarker("Assets this one points at: every word of its data that is the id of a known asset.");
        assetList(app, "##uses", uses_, lh);
        ImGui::Text("Used by (%zu)", info_.usedBy.size());
        helpMarker("Assets whose data holds this one's id. Scripts that nothing points at are started by the game's code, or not used.");
        assetList(app, "##usedby", info_.usedBy, lh);
        ImGui::EndChild();
    }

    bool save(App& app, const fs::path* saveAs) override {
        try {
            if (saveAs) {
                writeText(*saveAs, "# " + info_.name + "\n" + editor_.text);
                return true;
            }
            Bytes data = editor_.build(editor_.text);
            // Every copy is patched before any file is written, so a failure leaves them all as they were.
            std::map<fs::path, std::vector<AssetLoc>> byFile;
            for (auto& l : info_.locs) byFile[l.file].push_back(l);
            std::vector<std::pair<fs::path, Loaded>> patched;
            for (auto& [file, locs] : byFile) {
                Loaded l = loadAsset(file);
                for (auto& loc : locs) {
                    std::optional<caff::Caff> entry;
                    caff::Caff* c = nullptr;
                    if (loc.entry < 0 && l.caff) {
                        c = &*l.caff;
                    } else if (loc.entry >= 0 && l.sb && size_t(loc.entry) < l.sb->entries.size()) {
                        entry = caff::Caff::parse(l.sb->entries[size_t(loc.entry)].data);
                        c = &*entry;
                    }
                    if (!c || loc.symbol >= c->symbols.size() || assetdb::baseAssetName(c->symbolName(loc.symbol)) != info_.name)
                        fail(toUtf8(file) + " changed since it was indexed; reopen the assets folder to index it again");
                    c->replaceSymbolData(loc.symbol, data);
                    if (entry) l.sb->entries[size_t(loc.entry)].data = entry->serialize();
                }
                l.raw = Bytes();
                patched.emplace_back(file, std::move(l));
            }
            for (auto& [file, l] : patched) {
                app.backupBeforeWrite(file);
                if (l.caff) saveCaff(file, *l.caff);
                else writeFile(file, l.sb->serialize());
            }
            editor_.loadAsset(info_.name, data, lookup());
            findUses(data);
            original_ = data;
            app.status("Saved " + info_.name + " to " + std::to_string(patched.size()) + (patched.size() == 1 ? " bundle" : " bundles"));
            return true;
        } catch (std::exception& e) {
            app.error(info_.name + ": " + e.what());
            return false;
        }
    }

private:
    // A new part for reNut from this one and the edits in the editor (renutpart.hpp); the shipped
    // part stays as it is.
    void drawPartPopup(App& app) {
        if (!ImGui::BeginPopupModal("Save as reNut part", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        ImGui::PushTextWrapPos(em(560));
        ImGui::TextUnformatted("Makes a new part for reNut, starting from this one, in a RexFabric Lua mod: a script calling "
                               "renut.parts.add, with what you changed in the editor as its fields. This part itself is not "
                               "changed. Put the mod folder in reNut's mods folder (mods\\<your mod>).");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::SetNextItemWidth(em(420));
        inputText("Mod folder", part_.mod);
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) app.openFolderDialog("Mod folder", [this](const fs::path& p) { part_.mod = toUtf8(p); });
        ImGui::SetNextItemWidth(em(260));
        inputText("Part id", part_.id);
        helpMarker("1-40 characters of a-z, 0-9 and _, unique across every mod: start it with your mod's name. Vehicles keep parts by "
                   "id, so don't change it once players have the part.");
        ImGui::SetNextItemWidth(em(260));
        inputTextWithHint("Garage name", "the part's own", part_.name);
        ImGui::SetNextItemWidth(em(120));
        ImGui::InputInt("How many the player owns", &part_.count);
        part_.count = std::clamp(part_.count, 1, 99);
        ImGui::Checkbox("A new kind of part (callbacks to fill in)", &part_.behaviour);
        helpMarker("Adds every callback (on_use, on_tick, on_physics...) to the script, empty, for you to fill in with what the part "
                   "does. Without it the part is a variant: the shipped part with your values.");
        ImGui::Checkbox("Replace the script if it exists", &part_.overwrite);
        helpMarker("Off: an existing script is kept. With a model or footprint of its own, those are then made anew and the script "
                   "you may have edited stays.");

        ImGui::SeparatorText("Its own model and footprint");
        ImGui::Checkbox("A model of its own", &part_.ownModel);
        helpMarker("The base's model (or the template's) with your mesh file's geometry, as model import applies it: OBJ or FBX "
                   "objects named like the export's (group1_mesh8), or a node of a .glb. It goes, with a copy of the collision, "
                   "into the mod's own stream bundle (<mod folder>\\bundles), and the script gets model = \"<id>\".");
        if (part_.ownModel) {
            ImGui::Indent();
            pathField(app, "Mesh file##part", part_.model, PathPick::OpenFile, {{"OBJ, FBX or glTF", "obj;fbx;glb"}});
            glbNodes(part_.model, part_.nodesSeen, part_.nodes, part_.nodesError);
            bool glb = lower(toUtf8(fromUtf8(trim(part_.model)).extension())) == ".glb";
            if (glb) {
                comboField("Node##part", part_.mesh, part_.nodes, "the node whose mesh it takes");
                if (!part_.nodesError.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", part_.nodesError.c_str());
                ImGui::SetNextItemWidth(em(260));
                inputTextWithHint("Into meshes##part", "8,0,17,25", part_.into);
                helpMarker("The template's meshes that get the node's mesh, by number (as Export from a file > Models with Every draw "
                           "group names them: group0_mesh8 is 8); every other mesh is removed.");
                ImGui::SetNextItemWidth(em(260));
                inputTextWithHint("Flat colour at##part", "u,v or u,v,u2,v2 (optional)", part_.flat);
                helpMarker("Sets every vertex to one spot of the textures (from the image's top left; u2, v2 for the second set, the "
                           "ambient occlusion) for one flat colour. Empty projects the textures along each face's main axis.");
            }
            ImGui::SetNextItemWidth(em(260));
            inputTextWithHint("Template##part", "this part's own model", part_.templateName);
            helpMarker("Another part model to start from, such as wheel_standard (aid_model_banjox_vehicleparts_ may be left out).");
            comboField("Collision##part", part_.collision, kCollisions, "the template's", em(260));
            helpMarker("The collision is the template's, keeping its size, or another part model's (body_light_wedge...), or none.");
            ImGui::Unindent();
        }
        ImGui::Checkbox("A footprint of its own", &part_.ownFootprint);
        helpMarker("The base's footprint (its cells and attach points) with only some faces taking other parts. top makes a part that "
                   "goes under a block and hangs down. The script gets attach = \"<id>\".");
        if (part_.ownFootprint) {
            ImGui::Indent();
            comboField("Attachable faces##part", part_.attachable, kAttachable, "top", em(260));
            helpMarker("all, none, top, bottom, sides, top+sides, or a list of directions such as -x,-y,+z.");
            ImGui::Unindent();
        }
        if (part_.ownModel || part_.ownFootprint) {
            std::string hint;
            try {
                if (!trim(part_.mod).empty()) hint = renutmake::defaultBundle(fromUtf8(trim(part_.mod)));
            } catch (std::exception&) {
            }
            ImGui::SetNextItemWidth(em(260));
            inputTextWithHint("Stream bundle name##part", hint.empty() ? "the mod folder's name" : hint.c_str(), part_.bundle);
        }

        ImGui::SeparatorText("mod.toml (when the folder has none)");
        ImGui::SetNextItemWidth(em(260));
        inputTextWithHint("Mod name##part", "the folder's name", part_.modName);
        ImGui::SetNextItemWidth(em(260));
        inputTextWithHint("Author##part", "none", part_.author);
        ImGui::Spacing();
        bool ready = !part_.mod.empty() && renutpart::validId(part_.id) && (!part_.ownModel || !trim(part_.model).empty());
        ImGui::BeginDisabled(!ready || app.busy());
        if (ImGui::Button("Write")) {
            try {
                renutpart::Part p;
                p.id = part_.id;
                p.base = renutpart::baseName(info_.name);
                p.name = part_.name;
                p.count = part_.count;
                p.behaviour = part_.behaviour;
                p.original = original_;
                p.edited = editor_.build(editor_.text);
                // The model and footprint come from the game's stream bundle paired with the part's bundle.
                bool assets = part_.ownModel || part_.ownFootprint;
                renutmake::PartAssets pa;
                if (part_.ownModel) {
                    pa.model = fromUtf8(trim(part_.model));
                    pa.templateName = trim(part_.templateName);
                    pa.glb.node = part_.mesh;
                    for (auto& s : split(part_.into, ','))
                        if (!trim(s).empty()) pa.glb.into.push_back(std::atoi(trim(s).c_str()));
                    for (auto& s : split(part_.flat, ','))
                        if (!trim(s).empty()) pa.glb.flat.push_back(float(std::atof(trim(s).c_str())));
                    pa.collision = trim(part_.collision);
                }
                if (part_.ownFootprint) pa.attachable = trim(part_.attachable).empty() ? std::string("all") : trim(part_.attachable);
                pa.bundle = trim(part_.bundle);
                fs::path bundle = info_.locs.front().file;
                for (auto& l : info_.locs)
                    if (l.entry < 0) {
                        bundle = l.file;
                        break;
                    }
                fs::path stream = model::pairedStreamBundle(bundle), mod = fromUtf8(part_.mod);
                std::string modName = trim(part_.modName), author = trim(part_.author);
                bool force = part_.overwrite;
                auto names = lookup();
                ImGui::CloseCurrentPopup();
                app.runTask("Making reNut part " + p.id, true,
                            [&app, p, pa, assets, stream, mod, modName, author, force, names](Task&) mutable -> std::function<void()> {
                                auto say = [&app](const std::string& s) { app.log(s); };
                                fs::path file;
                                if (!assets || renutmake::addPartAssets(mod, stream, p, pa, force, say))
                                    file = renutpart::write(mod, p, modName, author, force, names);
                                return [&app, file, mod] {
                                    if (!file.empty()) app.status("Wrote " + toUtf8(file));
                                    else app.status("Made the part's model and footprint in " + toUtf8(mod) + " and kept its script (see the log)");
                                };
                            });
            } catch (std::exception& e) {
                app.error(std::string("Save as reNut part: ") + e.what());
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    gamescript::NameLookup lookup() const {
        auto cat = cat_;
        return [cat](uint32_t id) { return cat ? cat->nameOf(id) : std::string(); };
    }

    void findUses(const Bytes& data) {
        uses_.clear();
        auto names = lookup();
        for (uint32_t id : gamescript::referencedIds(data, names, assetdb::idFromName(info_.name).value_or(0))) uses_.push_back(names(id));
    }

    size_t copies() const {
        std::set<fs::path> files;
        for (auto& l : info_.locs) files.insert(l.file);
        return files.size();
    }

    ScriptInfo info_;
    std::shared_ptr<const Catalog> cat_;
    fs::path root_;
    ScriptEditor editor_;
    std::vector<std::string> uses_;
    float leftFrac_ = 0.68f;
    Bytes original_;  // the asset as loaded or last saved
    struct {
        std::string mod, id, name, modName, author;
        int count = 1;
        bool behaviour = false, overwrite = false;
        // Its own model (a mesh file, a .glb node and the template's meshes it goes into) and footprint.
        bool ownModel = false, ownFootprint = false;
        std::string model, mesh, into, flat, templateName, collision, attachable = "top", bundle;
        std::vector<std::string> nodes;
        std::string nodesSeen, nodesError;
    } part_;  // Save as reNut part
};

} // namespace

std::unique_ptr<Document> makeScriptDocument(const ScriptInfo& s, std::shared_ptr<const Catalog> catalog, const fs::path& root) {
    return std::make_unique<ScriptDocument>(s, std::move(catalog), root);
}

} // namespace nut::gui
