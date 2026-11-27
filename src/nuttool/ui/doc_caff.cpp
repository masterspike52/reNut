// Bundle / CAFF and stream bundle documents.
#include "audio_view.hpp"
#include "docs.hpp"

#include "assetdb.hpp"
#include "media.hpp"
#include "objparams.hpp"
#include "ops.hpp"
#include "xctd.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace nut::gui {

// ---- texture preview -----------------------------------------------------------

void TexturePreview::draw(App& app, const texture::Header& h, const std::vector<texture::Level>& levels, float height) {
    if (levels.empty()) return;
    int slices = int(levels[0].slices.size());
    level = std::clamp(level, 0, int(levels.size()) - 1);
    slice = std::clamp(slice, 0, slices - 1);
    ImGui::SetNextItemWidth(em(140));
    if (ImGui::SliderInt("Mip", &level, 0, int(levels.size()) - 1)) dirty = true;
    if (slices > 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em(140));
        if (ImGui::SliderInt(h.kind == texture::Kind::Cube ? "Face" : "Slice", &slice, 0, slices - 1)) dirty = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em(110));
    if (ImGui::Combo("##channel", &channel, "RGBA\0RGB only\0Alpha only\0")) dirty = true;
    ImGui::SameLine();
    ImGui::Checkbox("Fit", &fit);
    if (!fit) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em(120));
        ImGui::SliderFloat("Zoom", &zoom, 0.125f, 8.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
    }
    if (dirty || !image) {
        image::Rgba img = texture::toRgba(h, levels[size_t(level)], uint32_t(slice));
        for (size_t i = 0; i < img.pixels.size(); i += 4) {
            if (channel == 1) img.pixels[i + 3] = 255;
            else if (channel == 2) { img.pixels[i] = img.pixels[i + 1] = img.pixels[i + 2] = img.pixels[i + 3]; img.pixels[i + 3] = 255; }
        }
        image = app.upload(img);
        dirty = false;
    }
    ImGui::Text("%u x %u", levels[size_t(level)].width, levels[size_t(level)].height);
    ImGui::BeginChild("##texview", ImVec2(0, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    if (image && image->tex) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float s = zoom;
        if (fit) s = std::min({avail.x / float(image->width), avail.y / float(image->height), 8.0f});
        ImVec2 size(float(image->width) * s, float(image->height) * s);
        ImVec2 p = ImGui::GetCursorScreenPos();
        // Checkerboard behind transparent pixels.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float cs = 8;
        ImU32 c1 = IM_COL32(90, 90, 90, 255), c2 = IM_COL32(130, 130, 130, 255);
        ImVec2 clipMin = ImGui::GetWindowPos(), clipMax(clipMin.x + ImGui::GetWindowWidth(), clipMin.y + ImGui::GetWindowHeight());
        for (float y = 0; y < size.y; y += cs) {
            if (p.y + y > clipMax.y || p.y + y + cs < clipMin.y) continue;
            for (float x = 0; x < size.x; x += cs) {
                if (p.x + x > clipMax.x) break;
                dl->AddRectFilled(ImVec2(p.x + x, p.y + y), ImVec2(std::min(p.x + x + cs, p.x + size.x), std::min(p.y + y + cs, p.y + size.y)),
                                  (int(x / cs) + int(y / cs)) % 2 ? c1 : c2);
            }
        }
        ImGui::Image(image->id(), size);
    }
    ImGui::EndChild();
}

namespace {

std::string kindLabel(texture::Kind k) {
    switch (k) {
        case texture::Kind::Tex2D: return "2D";
        case texture::Kind::Cube: return "cube map";
        case texture::Kind::Array: return "array";
        case texture::Kind::Volume: return "volume (3D)";
    }
    return "?";
}

// ---- CAFF symbol browser ----------------------------------------------------------

struct CaffContext {
    std::vector<caff::Caff*> streams;                  // texture top levels for merging
    streambundle::StreamBundle* pairedStream = nullptr; // where split texture top levels live
    std::function<void()> onChanged;
    std::function<void()> onStreamChanged;
};

class CaffView {
public:
    void invalidate() {
        namesLower_.clear();
        texFor_ = -2;
        hexFor_ = -1;
        refsFor_ = -2;
        localNamesFor_ = size_t(-1);
    }

    void draw(App& app, caff::Caff& c, CaffContext& ctx) {
        if (namesLower_.size() != c.symbols.size()) {
            namesLower_.clear();
            isTexture_.assign(c.symbols.size(), 0);
            for (size_t i = 0; i < c.symbols.size(); ++i) namesLower_.push_back(lower(c.symbolName(i)));
            for (auto& t : ops::findTextures(c)) isTexture_[t.header] = isTexture_[t.gpu] = 1;
        }
        float h = ImGui::GetContentRegionAvail().y, w = ImGui::GetContentRegionAvail().x;
        float lw = leftFrac_ * w;
        ImGui::BeginChild("##symbols", ImVec2(lw, h), ImGuiChildFlags_Borders);
        drawTable(c);
        ImGui::EndChild();
        splitter("##symsplit", lw, em(250), w - em(300), h);
        leftFrac_ = lw / w;
        ImGui::BeginChild("##detail", ImVec2(0, h));
        drawDetail(app, c, ctx);
        ImGui::EndChild();
    }

private:
    void drawTable(caff::Caff& c) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Textures only").x - ImGui::GetFrameHeight() * 2);
        inputText("##symfilter", filter_);
        if (filter_.empty() && !ImGui::IsItemActive())
            ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetItemRectMin().x + ImGui::GetStyle().FramePadding.x, ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y),
                                                ImGui::GetColorU32(ImGuiCol_TextDisabled), "Filter by name...");
        ImGui::SameLine();
        ImGui::Checkbox("Textures only", &texturesOnly_);
        std::string needle = lower(filter_);
        std::vector<int> rows;
        for (size_t i = 0; i < c.symbols.size(); ++i) {
            if (texturesOnly_ && !(isTexture_[i] && ops::validSection(c, c.symbols[i]) && c.sections[c.symbols[i].section - 1].name == ".data")) continue;
            if (!needle.empty() && namesLower_[i].find(needle) == std::string::npos) continue;
            rows.push_back(int(i));
        }
        ImGui::TextDisabled("%zu of %zu symbols", rows.size(), c.symbols.size());
        ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;
        if (!ImGui::BeginTable("##symtable", 4, tf)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, em(30));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthFixed, em(72));
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, em(56));
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(int(rows.size()));
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                int i = rows[size_t(r)];
                const auto& s = c.symbols[size_t(i)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char id[16];
                std::snprintf(id, sizeof id, "%d", i + 1);
                if (ImGui::Selectable(id, selected_ == i, ImGuiSelectableFlags_SpanAllColumns)) selected_ = i;
                ImGui::TableNextColumn();
                std::string n = assetdb::baseAssetName(c.symbolName(size_t(i)));
                if (startsWith(n, "aid_")) n = n.substr(4);
                ImGui::TextUnformatted(n.empty() ? "(unnamed)" : n.c_str());
                if (ImGui::IsItemHovered() && ImGui::CalcTextSize(n.c_str()).x > ImGui::GetColumnWidth()) ImGui::SetTooltip("%s", n.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(ops::validSection(c, s) ? c.sections[s.section - 1].name.c_str() : "-");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatSize(s.size).c_str());
            }
        }
        ImGui::EndTable();
    }

    void drawDetail(App& app, caff::Caff& c, CaffContext& ctx) {
        if (selected_ < 0 || size_t(selected_) >= c.symbols.size()) {
            ImGui::TextDisabled("Select a symbol to see its contents.");
            ImGui::TextWrapped("Each symbol is one asset (or one part of an asset: models and textures have a .data header plus GPU data).");
            return;
        }
        size_t si = size_t(selected_);
        const auto& s = c.symbols[si];
        std::string full = c.symbolName(si);
        std::string base = assetdb::baseAssetName(full);
        ImGui::TextWrapped("%s", full.empty() ? "(unnamed)" : full.c_str());
        ImGui::TextDisabled("symbol #%zu   section %s   offset 0x%x   size %s (%u bytes)   kind %u", si + 1,
                            ops::validSection(c, s) ? c.sections[s.section - 1].name.c_str() : "-", s.offset, formatSize(s.size).c_str(), s.size, s.kind);
        if (auto id = assetdb::idFromName(base)) ImGui::TextDisabled("asset id %s (%s)", hex32(*id).c_str(), assetdb::typeName(uint8_t(*id >> 24)).c_str());

        bool valid = ops::validSection(c, s) && s.size > 0;
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Export raw...")) {
            std::string def = ops::safeName(base.empty() ? "symbol" + std::to_string(si + 1) : base) + "." + ops::sectionTag(c, s) + ".bin";
            Bytes data = c.symbolData(si);
            app.saveFileDialog("Export symbol", {{"Binary", "bin"}, {"All files", "*"}}, [&app, data](const fs::path& p) {
                writeFile(p, data);
                app.status("Exported " + toUtf8(p));
            }, fromUtf8(def));
        }
        ImGui::SameLine();
        if (ImGui::Button("Replace raw...")) {
            caff::Caff* cp = &c;
            CaffContext* cx = &ctx;
            app.openFileDialog("Replace symbol data", {{"All files", "*"}}, [this, &app, cp, cx, si](const fs::path& p) {
                Bytes data = readFile(p);
                uint32_t old = cp->symbols[si].size;
                cp->replaceSymbolData(si, data);
                invalidate();
                if (cx->onChanged) cx->onChanged();
                app.status("Replaced symbol #" + std::to_string(si + 1) + " (" + std::to_string(old) + " -> " + std::to_string(data.size()) + " bytes)");
            });
        }
        helpMarker("Replaces this symbol's bytes. If the size changes, later symbols move; pointers stay valid because they are relative to their target symbol. "
                   "If you change the internal layout of an asset, its relocation offsets must match the new data.");
        ImGui::EndDisabled();
        // Shortcuts into the game-wide Models / Textures tabs.
        if (auto cat = app.catalog()) {
            if (cat->model(base)) {
                ImGui::SameLine();
                if (ImGui::Button("Open in model viewer")) app.openModel(base);
            } else if (isTexture_[si] && cat->texture(ops::fullName(base))) {
                ImGui::SameLine();
                if (ImGui::Button("Open texture tab")) app.openTexture(ops::fullName(base));
                helpMarker("Shows every copy of this texture across the game files; importing there updates all of them.");
            }
        }

        drawScriptAndRefs(app, c, ctx, si, base, valid);

        // Texture preview.
        if (texFor_ != selected_) {
            texFor_ = selected_;
            tex_.reset();
            texRef_.reset();
            texError_.clear();
            preview_ = TexturePreview{};
            if (isTexture_[si]) {
                texRef_ = ops::textureForSymbol(c, si);
                if (texRef_) {
                    try {
                        texture::Header h = texture::parseHeader(c.symbolData(texRef_->header));
                        if (!texture::describeFormat(h.d3dFormat).supported()) texError_ = "Unsupported texture format " + hex32(h.d3dFormat);
                        else tex_ = ops::decodeTexture(*texRef_, ctx.streams);
                    } catch (std::exception& e) {
                        texError_ = e.what();
                    }
                }
            }
        }
        if (texRef_) {
            ImGui::SeparatorText("Texture");
            if (!texError_.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", texError_.c_str());
            if (tex_) {
                const auto& h = tex_->header;
                ImGui::Text("%s  %ux%u  %s  %zu mip level(s)%s", texture::describeFormat(h.d3dFormat).name, h.width, h.height, kindLabel(h.kind).c_str(),
                            tex_->levels.size(), tex_->merged ? "  (level 0 from the stream bundle)" : "");
                bool splitNoStream = !tex_->merged && endsWith(texRef_->name, "mip");
                if (splitNoStream) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Level 0 of this texture lives in the stream bundle, which was not found.");
                if (ImGui::Button("Export PNG...")) {
                    image::Rgba img = texture::toRgba(h, tex_->levels[size_t(preview_.level)], uint32_t(preview_.slice));
                    app.saveFileDialog("Export PNG", {{"PNG image", "png"}}, [&app, img](const fs::path& p) {
                        writeFile(p, image::encodePng(img));
                        app.status("Exported " + toUtf8(p));
                    }, fromUtf8(ops::safeName(tex_->name, 150) + ".png"));
                }
                ImGui::SameLine();
                if (ImGui::Button("Export DDS...")) {
                    Bytes dds = image::encodeDds(texture::toDds(h, tex_->levels, uint32_t(preview_.slice)));
                    app.saveFileDialog("Export DDS", {{"DDS texture", "dds"}}, [&app, dds](const fs::path& p) {
                        writeFile(p, dds);
                        app.status("Exported " + toUtf8(p));
                    }, fromUtf8(ops::safeName(tex_->name, 150) + ".dds"));
                }
                ImGui::SameLine();
                bool canImport = h.kind == texture::Kind::Tex2D || h.kind == texture::Kind::Array;
                ImGui::BeginDisabled(!canImport);
                if (ImGui::Button("Import image...")) {
                    caff::Caff* cp = &c;
                    CaffContext* cx = &ctx;
                    ops::TexRef ref = *texRef_;
                    app.openFileDialog("Import texture image", {{"Images", ops::textureImagePattern()}}, [this, &app, cp, cx, ref](const fs::path& p) {
                        bool streamChanged = false;
                        std::string summary = ops::importTexture(*cp, ref, p, cx->pairedStream, &streamChanged);
                        invalidate();
                        if (cx->onChanged) cx->onChanged();
                        if (streamChanged && cx->onStreamChanged) cx->onStreamChanged();
                        app.status(ref.name + ": " + summary);
                    });
                }
                ImGui::EndDisabled();
                helpMarker("PNG/JPG/BMP/TGA: compressed to the texture's format, and the mip chain is regenerated. An image of another "
                           "size (a higher resolution, say) resizes the texture; for a split texture that needs the matching stream bundle.\n"
                           "DDS: must use the same format; its data is used as-is.\nArray textures (several slices, such as the animated "
                           "action buttons) take one image for every slice here; Bulk import takes one per slice (<name>_slice0.png, "
                           "<name>_slice1.png...). Cube maps and volumes cannot be imported.");
                ImGui::SameLine();
                // Levels stored in this bundle: a split texture keeps level 1 here, a whole one level 0.
                size_t stored = tex_->levels.size() - (tex_->merged ? 1 : 0);
                ImGui::BeginDisabled(stored <= 1);
                if (ImGui::Button("Remove mipmaps")) {
                    bool changed = false;
                    std::string summary = ops::removeMipmaps(c, *texRef_, &changed);
                    std::string name = texRef_->name;
                    if (changed) {
                        invalidate();
                        if (ctx.onChanged) ctx.onChanged();
                    }
                    app.status(name + ": " + summary);
                }
                ImGui::EndDisabled();
                helpMarker("Drops the mip chain (the smaller copies the game shows at a distance), keeping the full-size picture as "
                           "it is. A split texture keeps level 1 too: the game streams level 0 into it, and a split texture needs two "
                           "levels; its stream bundle is not changed.");
                float ph = std::max(em(160), ImGui::GetContentRegionAvail().y - em(140));
                preview_.draw(app, h, tex_->levels, ph);
            }
        }

        // Hex view of the raw bytes.
        if (valid) {
            ImGui::SeparatorText("Data");
            if (hexFor_ != selected_) {
                hexFor_ = selected_;
                hex_ = c.symbolData(si);
                if (hex_.size() > 4 * 1024 * 1024) hex_.resize(4 * 1024 * 1024);
            }
            hexView("##hex", hex_, std::max(em(120), ImGui::GetContentRegionAvail().y));
        }
    }

    // Asset names for ids: the game-wide index once it is built, else this bundle's own symbols.
    gamescript::NameLookup nameLookup(App& app, const caff::Caff& c) {
        if (auto cat = app.catalog()) return [cat](uint32_t id) { return cat->nameOf(id); };
        if (localNamesFor_ != c.symbols.size()) {
            localNames_ = assetdb::Names();
            for (size_t i = 0; i < c.symbols.size(); ++i) localNames_.add(c.symbolName(i));
            localNamesFor_ = c.symbols.size();
        }
        return [this](uint32_t id) { return localNames_.lookup(id); };
    }

    // A game script symbol gets a text editor; every .data symbol lists the scripts and other assets it points at.
    void drawScriptAndRefs(App& app, caff::Caff& c, CaffContext& ctx, size_t si, const std::string& base, bool valid) {
        const auto& s = c.symbols[si];
        bool isData = valid && c.sections[s.section - 1].name == ".data";
        auto kind = gamescript::kindOf(base);
        bool params = startsWith(base, "aid_objparams_");
        // The index finishing adds names; reload then, unless that would drop edits.
        if (refsFor_ != selected_ || (refsCatalog_ != app.catalog().get() && !(script_ && script_->modified()))) {
            refsFor_ = selected_;
            refsCatalog_ = app.catalog().get();
            script_.reset();
            scriptError_.clear();
            usesScripts_.clear();
            usesOther_.clear();
            if (isData && s.size <= 4 * 1024 * 1024) {
                Bytes d = c.symbolData(si);
                auto names = nameLookup(app, c);
                if (kind || (params && objparams::isObjParams(d))) {
                    try {
                        script_.emplace();
                        script_->loadAsset(base, d, names);
                    } catch (std::exception& e) {
                        script_.reset();
                        scriptError_ = e.what();
                    }
                }
                for (uint32_t id : gamescript::referencedIds(d, names, assetdb::idFromName(base).value_or(0)))
                    (gamescript::kindOfId(id) ? usesScripts_ : usesOther_).push_back(names(id));
            }
        }
        if (!script_ && scriptError_.empty() && usesScripts_.empty() && usesOther_.empty()) return;
        ImGui::SeparatorText(kind ? "Game script" : script_ ? "Object parameters" : "Uses");
        if (!scriptError_.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Cannot show it as text: %s", scriptError_.c_str());
        if (script_) {
            ImGui::BeginDisabled(!script_->problem.empty() || !script_->modified());
            if (ImGui::Button("Apply")) {
                try {
                    c.replaceSymbolData(si, script_->build(script_->text));
                    invalidate();
                    if (ctx.onChanged) ctx.onChanged();
                    app.status("Applied " + base + " (save the bundle to keep it)");
                } catch (std::exception& e) {
                    app.error(base + ": " + e.what());
                }
            }
            ImGui::EndDisabled();
            if (auto cat = app.catalog(); cat && (cat->script(base) || cat->part(base))) {
                ImGui::SameLine();
                if (ImGui::Button("Open in its own tab")) app.openScript(base);
            }
            helpMarker(kind ? "Apply changes this bundle only (save it to keep the change). The script tab edits every bundle that holds "
                              "this asset and shows what uses it. One record per line; # starts a comment."
                            : "Apply changes this bundle only (save it to keep the change). One field per line, name = value; fields "
                              "without a name go by offset, and words that are 0 are not listed (add a line such as 0x3C4 = 1.5 to set one). "
                              "Vehicle parts have their own tabs under Vehicle parts, which edit every copy.");
            if (script_) script_->draw("##scripttext", ImVec2(0, std::min(em(320), std::max(em(140), ImGui::GetContentRegionAvail().y * 0.5f))));
        }
        if (!usesScripts_.empty()) {
            ImGui::Text("Scripts it uses (%zu)", usesScripts_.size());
            assetList(app, "##usesscripts", usesScripts_, std::min(em(120), ImGui::GetTextLineHeightWithSpacing() * float(usesScripts_.size() + 1)));
        }
        if (!usesOther_.empty() && ImGui::TreeNode("##usesother", "Other assets it points at (%zu)", usesOther_.size())) {
            assetList(app, "##usesotherlist", usesOther_, std::min(em(160), ImGui::GetTextLineHeightWithSpacing() * float(usesOther_.size() + 1)));
            ImGui::TreePop();
        }
    }

    int refsFor_ = -2;
    const Catalog* refsCatalog_ = nullptr;
    std::optional<ScriptEditor> script_;
    std::string scriptError_;
    std::vector<std::string> usesScripts_, usesOther_;
    assetdb::Names localNames_;
    size_t localNamesFor_ = size_t(-1);

    std::string filter_;
    bool texturesOnly_ = false;
    int selected_ = -1;
    float leftFrac_ = 0.5f; // symbol list share of the width
    std::vector<std::string> namesLower_;
    std::vector<uint8_t> isTexture_;
    int texFor_ = -2;
    std::optional<ops::TexRef> texRef_;
    std::optional<ops::DecodedTexture> tex_;
    std::string texError_;
    TexturePreview preview_;
    int hexFor_ = -1;
    Bytes hex_;
};

// ---- bundle / CAFF document ------------------------------------------------------------

class CaffDocument : public Document {
public:
    CaffDocument(const fs::path& p, const fs::path& root, Loaded l) : l_(std::move(l)) {
        path = p;
        auto id = assetdb::idFromPath(p);
        title_ = toUtf8(p.filename());
        if (!root.empty()) {
            try {
                assetdb::Names names;
                for (auto& e : assetdb::readDbIndex(root / "Debug" / "db_index.txt")) names.add(e.name);
                if (id && !names.lookup(*id).empty()) title_ = names.lookup(*id);
            } catch (...) {
            }
        }
        // A bundle's streamed texture levels live in the stream bundle with the same name.
        if (id && (*id >> 24) == assetdb::kTypeBundle && !root.empty()) {
            fs::path sp = root / "Bundle" / "50" / p.filename();
            if (fs::exists(sp)) {
                stream_ = loadAsset(sp);
                streamPath_ = sp;
                parseStreams();
            }
        }
        textures_ = ops::findTextures(*l_.caff).size();
    }

    std::string title() const override { return title_; }
    bool dirty() const override { return dirty_ || streamDirty_; }
    bool canSave() const override { return true; }

    void draw(App& app) override {
        caff::Caff& c = *l_.caff;
        ImGui::Text("%zu symbols, %zu textures, %zu sections", c.symbols.size(), textures_, c.sections.size());
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", l_.xctd ? "XCTD-compressed on disk" : "plain CAFF on disk",
                            stream_ ? ", paired stream bundle loaded" : "");
        if (ImGui::Button("Export all textures...")) {
            app.openFolderDialog("Export textures to", [this, &app](const fs::path& dir) {
                app.runTask("Exporting textures", true, [this, dir, &app](Task&) -> std::function<void()> {
                    ops::TexStats st = ops::exportTextures({&*l_.caff}, streamPtrs_, dir, true, true, nullptr);
                    return [&app, st, dir] {
                        app.status("Exported " + std::to_string(st.written) + " textures as DDS + PNG to " + toUtf8(dir) +
                                   (st.skipped ? " (" + std::to_string(st.skipped) + " unsupported saved raw)" : ""));
                    };
                });
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("Unpack to folder...")) {
            app.openFolderDialog("Unpack into", [this, &app](const fs::path& picked) {
                // Into a folder named after the bundle file, unless that is the folder picked.
                fs::path dir = path.empty() || picked.filename() == path.filename() ? picked : picked / path.filename();
                app.runTask("Unpacking", true, [this, dir, &app](Task&) -> std::function<void()> {
                    ops::unpackCaff(*l_.caff, l_.xctd, dir);
                    return [&app, dir] { app.status("Unpacked to " + toUtf8(dir) + " (rebuild it with Tools > Pack folders and convert XCTD)"); };
                });
            });
        }
        helpMarker("Writes caff.json plus one file per symbol. Edit the files and rebuild the bundle with Tools > Pack folders and "
                   "convert XCTD (or 'nuttool caff-pack <folder> <file>').");
        ImGui::SameLine();
        if (ImGui::Button("More exports...")) app.openExportTools(path);
        helpMarker("Tools > Export from a file, for this bundle: its textures as DDS, PNG or both, every model as OBJ with a bone "
                   "list, or its wave banks.");
        ctx_.streams = streamPtrs_;
        ctx_.pairedStream = stream_ ? &*stream_->sb : nullptr;
        ctx_.onChanged = [this] { dirty_ = true; };
        ctx_.onStreamChanged = [this] {
            streamDirty_ = true;
            parseStreams();
        };
        ImGui::Separator();
        view_.draw(app, c, ctx_);
    }

    bool save(App& app, const fs::path* saveAs) override {
        try {
            fs::path target = saveAs ? *saveAs : path;
            if (!saveAs) app.backupBeforeWrite(path);
            saveCaff(target, *l_.caff);
            if (streamDirty_ && stream_) {
                app.backupBeforeWrite(streamPath_);
                writeFile(streamPath_, stream_->sb->serialize());
                streamDirty_ = false;
            }
            if (!saveAs) l_.xctd = false;
            dirty_ = false;
            return true;
        } catch (std::exception& e) {
            app.error(std::string("save failed: ") + e.what());
            return false;
        }
    }

private:
    void parseStreams() {
        streamCaffs_.clear();
        streamPtrs_.clear();
        if (!stream_ || !stream_->sb) return;
        for (auto& e : stream_->sb->entries)
            if (caff::isCaff(e.data)) streamCaffs_.push_back(caff::Caff::parse(e.data));
        for (auto& c : streamCaffs_) streamPtrs_.push_back(&c);
    }

    Loaded l_;
    std::string title_;
    size_t textures_ = 0;
    std::optional<Loaded> stream_;
    fs::path streamPath_;
    std::vector<caff::Caff> streamCaffs_;
    std::vector<caff::Caff*> streamPtrs_;
    bool dirty_ = false, streamDirty_ = false;
    CaffContext ctx_;
    CaffView view_;
};

// ---- stream bundle document ------------------------------------------------------------

class StreamBundleDocument : public Document {
public:
    StreamBundleDocument(const fs::path& p, const fs::path& root, Loaded l) : l_(std::move(l)) {
        path = p;
        title_ = friendlyName(root, p) + " (stream)";
        for (auto& e : l_.sb->entries) names_.push_back(e.data.empty() ? std::string("(empty slot)") : ops::entryName(e));
    }

    std::string title() const override { return title_; }
    bool dirty() const override { return dirty_ || entryDirty_; }
    bool canSave() const override { return true; }

    void draw(App& app) override {
        auto& sb = *l_.sb;
        ImGui::Text("%zu entries, %zu dependencies", sb.entries.size(), sb.dependencies.size());
        ImGui::SameLine();
        if (ImGui::Button("Unpack all...")) {
            commitEntry();
            app.openFolderDialog("Unpack into", [this, &app](const fs::path& picked) {
                fs::path dir = path.empty() || picked.filename() == path.filename() ? picked : picked / path.filename();
                app.runTask("Unpacking", true, [this, dir, &app](Task&) -> std::function<void()> {
                    ops::unpackStreamBundle(*l_.sb, dir);
                    return [&app, dir] { app.status("Unpacked to " + toUtf8(dir) + " (rebuild it with Tools > Pack folders and convert XCTD)"); };
                });
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("Export all audio...")) {
            app.openFolderDialog("Export wave banks to", [this, &app](const fs::path& dir) {
                app.runTask("Exporting audio", true, [this, dir, &app](Task&) -> std::function<void()> {
                    int n = 0;
                    for (auto& e : l_.sb->entries)
                        if (media::isWaveBank(e.data)) { ops::exportWaveBank(e.data, dir, hex32(e.id) + "_" + ops::entryName(e)); ++n; }
                    return [&app, n, dir] { app.status("Exported " + std::to_string(n) + " wave bank(s) to " + toUtf8(dir)); };
                });
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("More exports...")) app.openExportTools(path);
        helpMarker("Tools > Export from a file, for this stream bundle: wave banks decoded to WAV or picked by name, the textures "
                   "of its entries, or their models as OBJ. Pack folders and convert XCTD rebuilds an unpacked stream bundle.");
        ImGui::SetNextItemWidth(-FLT_MIN);
        inputText("##entryfilter", filter_);
        if (filter_.empty() && !ImGui::IsItemActive())
            ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetItemRectMin().x + ImGui::GetStyle().FramePadding.x, ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y),
                                                ImGui::GetColorU32(ImGuiCol_TextDisabled), "Filter entries by name or id...");
        std::string needle = lower(filter_);
        float tableH = ImGui::GetContentRegionAvail().y * 0.32f;
        ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("##entries", 5, tf, ImVec2(0, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, em(40));
            ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, em(80));
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, em(110));
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, em(80));
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            std::vector<int> rows;
            for (size_t i = 0; i < sb.entries.size(); ++i)
                if (needle.empty() || lower(names_[i]).find(needle) != std::string::npos || hex32(sb.entries[i].id).find(needle) != std::string::npos)
                    rows.push_back(int(i));
            ImGuiListClipper clip;
            clip.Begin(int(rows.size()));
            while (clip.Step()) {
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                    int i = rows[size_t(r)];
                    const auto& e = sb.entries[size_t(i)];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    char lbl[16];
                    std::snprintf(lbl, sizeof lbl, "%d", i + 1);
                    if (ImGui::Selectable(lbl, selected_ == i, ImGuiSelectableFlags_SpanAllColumns)) select(i);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(hex32(e.id).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(assetdb::typeName(uint8_t(e.id >> 24)).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(formatSize(e.data.size()).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(names_[size_t(i)].c_str());
                }
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
        drawEntry(app);
    }

    bool save(App& app, const fs::path* saveAs) override {
        try {
            commitEntry();
            fs::path target = saveAs ? *saveAs : path;
            if (!saveAs) app.backupBeforeWrite(path);
            writeFile(target, l_.sb->serialize());
            dirty_ = false;
            return true;
        } catch (std::exception& e) {
            app.error(std::string("save failed: ") + e.what());
            return false;
        }
    }

private:
    void select(int i) {
        if (i == selected_) return;
        commitEntry();
        selected_ = i;
        entryCaff_.reset();
        hasWaves_ = false;
        wavesView_.setBank(nullptr, "");
        view_ = CaffView{};
        const auto& e = l_.sb->entries[size_t(i)];
        try {
            if (caff::isCaff(e.data)) entryCaff_ = caff::Caff::parse(e.data);
            else if (media::isWaveBank(e.data)) {
                auto raw = std::make_shared<const Bytes>(e.data);
                wavesView_.setBank(std::make_shared<const media::WaveBank>(media::parseWaveBank(*raw)), hex32(e.id) + "_" + names_[size_t(i)], raw);
                hasWaves_ = true;
            }
        } catch (std::exception& ex) {
            entryError_ = ex.what();
        }
    }

    // Edits to the selected entry's CAFF are written back into the bundle when switching entries or saving.
    void commitEntry() {
        if (entryDirty_ && entryCaff_ && selected_ >= 0) {
            l_.sb->entries[size_t(selected_)].data = entryCaff_->serialize();
            dirty_ = true;
        }
        entryDirty_ = false;
    }

    void drawEntry(App& app) {
        if (selected_ < 0) {
            ImGui::TextDisabled("Select an entry. Entries are standalone assets (textures, cutscenes, animations) or audio banks.");
            return;
        }
        auto& e = l_.sb->entries[size_t(selected_)];
        ImGui::Text("Entry %d: %s", selected_ + 1, names_[size_t(selected_)].c_str());
        ImGui::BeginDisabled(e.data.empty());
        if (ImGui::Button("Export entry...")) {
            commitEntry();
            Bytes data = l_.sb->entries[size_t(selected_)].data;
            std::string def = hex32(e.id) + "_" + ops::safeName(names_[size_t(selected_)]) + ops::entryExt(e);
            app.saveFileDialog("Export entry", {{"All files", "*"}}, [&app, data](const fs::path& p) {
                writeFile(p, data);
                app.status("Exported " + toUtf8(p));
            }, fromUtf8(def));
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Replace entry...")) {
            int idx = selected_;
            app.openFileDialog("Replace entry", {{"All files", "*"}}, [this, &app, idx](const fs::path& p) {
                l_.sb->entries[size_t(idx)].data = readFile(p);
                names_[size_t(idx)] = ops::entryName(l_.sb->entries[size_t(idx)]);
                entryDirty_ = false;
                dirty_ = true;
                selected_ = -1;
                select(idx);
                app.status("Replaced entry " + std::to_string(idx + 1));
            });
        }
        if (entryCaff_) {
            ctx_.streams.clear();
            ctx_.pairedStream = nullptr;
            ctx_.onChanged = [this] { entryDirty_ = true; };
            ctx_.onStreamChanged = nullptr;
            view_.draw(app, *entryCaff_, ctx_);
        } else if (hasWaves_) {
            wavesView_.draw(app);
        } else {
            if (!entryError_.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "%s", entryError_.c_str());
            hexView("##entryhex", e.data, ImGui::GetContentRegionAvail().y);
        }
    }

    Loaded l_;
    std::string title_;
    std::vector<std::string> names_;
    std::string filter_;
    int selected_ = -1;
    std::optional<caff::Caff> entryCaff_;
    bool hasWaves_ = false;
    WaveBankView wavesView_;
    std::string entryError_;
    bool dirty_ = false, entryDirty_ = false;
    CaffContext ctx_;
    CaffView view_;
};

} // namespace

std::unique_ptr<Document> makeCaffDocument(const fs::path& p, const fs::path& root, Loaded l) {
    return std::make_unique<CaffDocument>(p, root, std::move(l));
}

std::unique_ptr<Document> makeStreamBundleDocument(const fs::path& p, const fs::path& root, Loaded l) {
    return std::make_unique<StreamBundleDocument>(p, root, std::move(l));
}

} // namespace nut::gui
