// A single texture from the catalog, with every copy of it across bundles.
#include "docs.hpp"

#include "ops.hpp"
#include "texture_edit.hpp"

#include <algorithm>
#include <set>

namespace nut::gui {
namespace {

std::string shortName(const std::string& n) {
    for (const char* p : {"aid_texture_banjox_", "aid_texture_banjoxdlc_", "aid_texture_"})
        if (startsWith(n, p)) return n.substr(std::string(p).size());
    return n;
}

class TextureDocument : public Document {
public:
    TextureDocument(const TextureInfo& t, const fs::path& root) : t_(t), root_(root) {
        path = !t.full.empty() ? t.full.front().file : t.tops.front().file;
        reload();
    }

    std::string title() const override { return "tex " + shortName(t_.name); }

    void draw(App& app) override {
        ImGui::TextUnformatted(t_.name.c_str());
        ImGui::TextDisabled("%s  %ux%u  %u mip level(s)%s   %zu cop%s in the game files", t_.format.c_str(), t_.width, t_.height, t_.mips,
                            t_.ui ? "   UI icon" : "", copies(), copies() == 1 ? "y" : "ies");
        if (!error_.empty()) {
            ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", error_.c_str());
            return;
        }
        if (ImGui::Button("Export PNG...")) {
            image::Rgba img = texture::toRgba(tex_.header, tex_.levels[size_t(preview_.level)], uint32_t(preview_.slice));
            app.saveFileDialog("Export PNG", {{"PNG image", "png"}}, [&app, img](const fs::path& p) {
                writeFile(p, image::encodePng(img));
                app.status("Exported " + toUtf8(p));
            }, fromUtf8(ops::safeName(tex_.name, 150) + ".png"));
        }
        ImGui::SameLine();
        if (ImGui::Button("Export DDS...")) {
            Bytes dds = image::encodeDds(texture::toDds(tex_.header, tex_.levels, uint32_t(preview_.slice)));
            app.saveFileDialog("Export DDS", {{"DDS texture", "dds"}}, [&app, dds](const fs::path& p) {
                writeFile(p, dds);
                app.status("Exported " + toUtf8(p));
            }, fromUtf8(ops::safeName(tex_.name, 150) + ".dds"));
        }
        ImGui::SameLine();
        bool canImport = tex_.header.kind == texture::Kind::Tex2D || tex_.header.kind == texture::Kind::Array;
        ImGui::BeginDisabled(!canImport);
        if (ImGui::Button("Import image...")) {
            app.openFileDialog("Import texture image", {{"Images", ops::textureImagePattern()}}, [this, &app](const fs::path& img) {
                app.runTask("Importing " + shortName(t_.name), true, [this, img, &app](Task&) -> std::function<void()> {
                    int menus = 0;
                    int written = importAll(app, ops::readTextureImage(img), &menus);
                    return [this, &app, written, menus] {
                        reload();
                        preview_.dirty = true;
                        app.status("Imported into " + std::to_string(written) + " file(s) (originals kept as .bak)" +
                                   (menus ? "; fitted " + std::to_string(menus) + " menu element(s) to the new size" : ""));
                    };
                });
            });
        }
        ImGui::EndDisabled();
        helpMarker("Writes the image into every copy of this texture (each bundle, and the streamed top level of split textures). "
                   "PNG/JPG/BMP/TGA are compressed to the texture's format with a new mip chain. An image of another size "
                   "(a higher resolution, say) resizes the texture, and the menus that show it are fitted so it keeps its size "
                   "on screen. DDS must use the same format. An array texture (several slices, such as the animated action "
                   "buttons) gets the image in every slice; Bulk import takes one image per slice. Changed files are saved "
                   "immediately with a .bak backup.");
        ImGui::SameLine();
        bool split = !t_.tops.empty();
        ImGui::BeginDisabled(tex_.levels.size() <= (split ? 2u : 1u));
        if (ImGui::Button("Remove mipmaps")) {
            app.runTask("Removing mipmaps from " + shortName(t_.name), true, [this, &app](Task&) -> std::function<void()> {
                int written = removeMipsAll(app);
                return [this, &app, written] {
                    reload();
                    preview_.level = 0;
                    preview_.dirty = true;
                    app.status("Removed the mipmaps in " + std::to_string(written) + " file(s) (originals kept as .bak)");
                };
            });
        }
        ImGui::EndDisabled();
        helpMarker("Drops the mip chain (the smaller copies the game shows at a distance) from every copy of this texture, "
                   "keeping the full-size picture as it is. A split texture keeps level 1 too: the game streams the full-size "
                   "level into it, and a split texture needs two levels. Without mipmaps, distant surfaces can shimmer and "
                   "the texture is read at full size everywhere. Changed files are saved immediately with a .bak backup.");
        if (ImGui::CollapsingHeader("Where it lives")) {
            auto row = [&](const AssetLoc& l, const char* what) {
                std::string s = toUtf8(fs::relative(l.file, root_));
                if (l.entry >= 0) s += "  entry " + std::to_string(l.entry + 1);
                ImGui::PushID(&l);
                if (ImGui::SmallButton("Open")) app.openPath(l.file);
                ImGui::SameLine();
                ImGui::Text("%s  %s", what, s.c_str());
                ImGui::PopID();
            };
            for (auto& l : t_.full) row(l, t_.tops.empty() ? "texture" : "mips   ");
            for (auto& l : t_.tops) row(l, "level 0");
        }
        preview_.draw(app, tex_.header, tex_.levels, std::max(em(160), ImGui::GetContentRegionAvail().y - em(40)));
    }

private:
    size_t copies() const {
        std::set<fs::path> files;
        for (auto& l : t_.full) files.insert(l.file);
        for (auto& l : t_.tops) if (t_.full.empty()) files.insert(l.file);
        return files.size();
    }

    void reload() {
        error_.clear();
        try {
            CaffCache cache;
            tex_ = decodeCatalogTexture(t_, cache);
            // An import may have resized it.
            t_.width = tex_.header.width;
            t_.height = tex_.header.height;
            t_.mips = uint32_t(tex_.levels.size());
        } catch (std::exception& e) {
            error_ = e.what();
        }
    }

    // Writes the image into every copy. Runs on a worker thread (the UI is blocked by a modal task).
    // A copy that cannot take the image (one that would need resizing without its stream bundle,
    // say) leaves all the files untouched.
    int importAll(App& app, const ops::TextureImage& img, int* menuElements) {
        TextureEdits edits;
        edits.import(t_, img);
        int written = edits.write(app);
        auto cat = app.catalog();
        if (edits.resizedAny() && cat) *menuElements = fixMenus(app, *cat, nullptr);
        return written;
    }

    // Removes the mipmaps from every copy (worker thread); streamed top levels are not touched.
    int removeMipsAll(App& app) {
        TextureEdits edits;
        edits.removeMips(t_);
        return edits.write(app);
    }

    TextureInfo t_;
    fs::path root_;
    ops::DecodedTexture tex_;
    std::string error_;
    TexturePreview preview_;
};

} // namespace

std::unique_ptr<Document> makeTextureDocument(const TextureInfo& t, const fs::path& root) { return std::make_unique<TextureDocument>(t, root); }

} // namespace nut::gui
