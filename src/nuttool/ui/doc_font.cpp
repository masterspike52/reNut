// XUI glyph cache (.sbp) viewer/editor.
#include "docs.hpp"

#include "font.hpp"
#include "texture.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace nut::gui {
namespace {

class FontDocument : public Document {
public:
    FontDocument(const fs::path& p, Bytes data) : font_(font::Font::parse(data)) {
        path = p;
        title_ = "font " + toUtf8(p.stem());
    }

    std::string title() const override { return title_; }
    bool dirty() const override { return dirty_; }
    bool canSave() const override { return true; }

    void draw(App& app) override {
        ImGui::Text("%zu glyphs, %ux%u cells, line height %u", font_.glyphs.size(), font_.cellSize, font_.cellSize, font_.header[2]);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em(160));
        bool go = inputText("##find", search_, ImGuiInputTextFlags_EnterReturnsTrue);
        if (search_.empty() && !ImGui::IsItemActive())
            ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetItemRectMin().x + ImGui::GetStyle().FramePadding.x, ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                                "char or U+XXXX");
        ImGui::SameLine();
        if (ImGui::Button("Find") || go) find(app);
        ImGui::SameLine();
        if (ImGui::Button("Export...")) {
            app.openFolderDialog("Export glyph pages and font to", [this, &app](const fs::path& picked) {
                // Into a folder named after the font, which also names the .ttf and its family.
                fs::path dir = picked.filename() == path.stem() ? picked : picked / path.stem();
                app.runTask("Exporting font", true, [this, dir, &app](Task&) -> std::function<void()> {
                    fs::create_directories(dir);
                    font::exportTo(font_, dir);
                    return [&app, dir] { app.status("Exported glyph pages, font.json, font.fnt and the .ttf to " + toUtf8(dir)); };
                });
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("Import pages...")) {
            app.openFolderDialog("Folder with font.json and page_NNN.png", [this, &app](const fs::path& dir) {
                app.runTask("Importing glyph pages", true, [this, dir, &app](Task&) -> std::function<void()> {
                    auto f = std::make_shared<font::Font>(font::importFrom(dir, &font_));
                    return [this, f, &app, dir] {
                        font_ = std::move(*f);
                        cache_.clear();
                        dirty_ = true;
                        app.status("Imported glyphs from " + toUtf8(dir));
                    };
                });
            });
        }
        helpMarker("Export writes PNG pages (32x32 glyphs each, white with alpha = coverage) plus font.json with codes and metrics. "
                   "Edit them and import the folder back. It also writes the font itself: <folder name>.ttf (TrueType, one square per "
                   "pixel; set it to the line height in pixels for an exact match) and font.fnt (BMFont over the pages, keeps the anti-aliasing).");

        float h = ImGui::GetContentRegionAvail().y, w = ImGui::GetContentRegionAvail().x;
        float gw = gridFrac_ * w;
        ImGui::BeginChild("##grid", ImVec2(gw, h), ImGuiChildFlags_Borders);
        drawGrid(app);
        ImGui::EndChild();
        splitter("##fontsplit", gw, em(200), w - em(260), h);
        gridFrac_ = gw / w;
        ImGui::BeginChild("##glyph", ImVec2(0, h));
        drawGlyph(app);
        ImGui::EndChild();
    }

    bool save(App& app, const fs::path* saveAs) override {
        try {
            fs::path target = saveAs ? *saveAs : path;
            if (!saveAs) app.backupBeforeWrite(path);
            writeFile(target, font_.serialize());
            dirty_ = false;
            return true;
        } catch (std::exception& e) {
            app.error(std::string("save failed: ") + e.what());
            return false;
        }
    }

private:
    image::Rgba glyphImage(const font::Glyph& g) const {
        image::Rgba img;
        img.width = img.height = font_.cellSize;
        img.pixels.resize(size_t(img.width) * img.height * 4);
        for (size_t i = 0; i < g.pixels.size(); ++i) {
            img.pixels[i * 4] = img.pixels[i * 4 + 1] = img.pixels[i * 4 + 2] = 255;
            img.pixels[i * 4 + 3] = uint8_t(g.pixels[i] * 17);
        }
        return img;
    }

    GpuImage* image(App& app, size_t i) {
        auto it = cache_.find(i);
        if (it != cache_.end()) return it->second.get();
        if (cache_.size() > 6000) cache_.clear();
        auto img = app.upload(glyphImage(font_.glyphs[i]), false);
        return (cache_[i] = img).get();
    }

    void find(App& app) {
        std::string s = trim(search_);
        if (s.empty()) return;
        uint32_t code = 0;
        try {
            if (startsWith(s, "U+") || startsWith(s, "u+")) code = parseHex(s.substr(2));
            else if (startsWith(s, "0x")) code = parseHex(s);
            else code = utf8ToUtf16(s)[0];
        } catch (...) {
            app.error("Enter a character or a code like U+00E9");
            return;
        }
        for (size_t i = 0; i < font_.glyphs.size(); ++i) {
            if (font_.glyphs[i].code == code) {
                selected_ = int(i);
                scrollTo_ = int(i);
                return;
            }
        }
        app.error("No glyph for U+" + hex32(code).substr(4));
    }

    void drawGrid(App& app) {
        float cell = em(44);
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        int cols = std::max(1, int((ImGui::GetContentRegionAvail().x + spacing) / (cell + spacing)));
        int rows = int((font_.glyphs.size() + size_t(cols) - 1) / size_t(cols));
        float rowH = cell + ImGui::GetStyle().ItemSpacing.y;
        if (scrollTo_ >= 0) {
            ImGui::SetScrollY(float(scrollTo_ / cols) * rowH);
            scrollTo_ = -1;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiListClipper clip;
        clip.Begin(rows, rowH);
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                for (int c = 0; c < cols; ++c) {
                    size_t i = size_t(r) * size_t(cols) + size_t(c);
                    if (i >= font_.glyphs.size()) break;
                    if (c) ImGui::SameLine();
                    ImGui::PushID(int(i));
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    if (ImGui::Selectable("##g", selected_ == int(i), 0, ImVec2(cell, cell))) selected_ = int(i);
                    if (ImGui::BeginItemTooltip()) {
                        ImGui::Text("U+%04X", font_.glyphs[i].code);
                        ImGui::EndTooltip();
                    }
                    if (GpuImage* g = image(app, i); g && g->tex) dl->AddImage(g->id(), ImVec2(pos.x + 2, pos.y + 2), ImVec2(pos.x + cell - 2, pos.y + cell - 2));
                    ImGui::PopID();
                }
            }
        }
    }

    void drawGlyph(App& app) {
        if (selected_ < 0 || size_t(selected_) >= font_.glyphs.size()) {
            ImGui::TextDisabled("Select a glyph.");
            return;
        }
        auto& g = font_.glyphs[size_t(selected_)];
        std::u16string u(1, char16_t(g.code));
        ImGui::Text("U+%04X  '%s'", g.code, utf16ToUtf8(u).c_str());
        float size = std::min(ImGui::GetContentRegionAvail().x, 5.0f * float(font_.cellSize));
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + size, p.y + size), IM_COL32(20, 20, 24, 255));
        if (GpuImage* img = image(app, size_t(selected_)); img && img->tex) ImGui::Image(img->id(), ImVec2(size, size));
        else ImGui::Dummy(ImVec2(size, size));
        ImGui::SeparatorText("Metrics");
        int m[4] = {g.metrics[0], g.metrics[1], g.metrics[2], g.metrics[3]};
        ImGui::SetNextItemWidth(em(220));
        if (ImGui::InputInt4("##metrics", m)) {
            for (int k = 0; k < 4; ++k) g.metrics[k] = int8_t(std::clamp(m[k], -128, 127));
            dirty_ = true;
        }
        helpMarker("Four signed bytes per glyph from the game's font cache: unused, A, B, C. The game draws the cell A pixels right of the pen "
                   "and advances the pen by A + B + C.");
        ImGui::SeparatorText("Bitmap");
        if (ImGui::Button("Export PNG...")) {
            image::Rgba img = glyphImage(g);
            char def[32];
            std::snprintf(def, sizeof def, "glyph_U%04X.png", g.code);
            app.saveFileDialog("Export glyph", {{"PNG image", "png"}}, [&app, img](const fs::path& out) {
                writeFile(out, image::encodePng(img));
                app.status("Exported " + toUtf8(out));
            }, fromUtf8(def));
        }
        ImGui::SameLine();
        if (ImGui::Button("Replace from PNG...")) {
            size_t idx = size_t(selected_);
            app.openFileDialog("Glyph image", {{"PNG image", "png"}}, [this, &app, idx](const fs::path& in) {
                image::Rgba img = image::decodePng(readFile(in));
                if (img.width != font_.cellSize || img.height != font_.cellSize) {
                    if (img.width < font_.cellSize || img.height < font_.cellSize) {
                        app.error("Glyph images must be " + std::to_string(font_.cellSize) + "x" + std::to_string(font_.cellSize) + " (or larger, to be scaled down)");
                        return;
                    }
                    img = texture::downscale(img, font_.cellSize, font_.cellSize);
                }
                auto& gl = font_.glyphs[idx];
                for (size_t i = 0; i < gl.pixels.size(); ++i) {
                    const uint8_t* px = &img.pixels[i * 4];
                    uint8_t a = px[3] == 255 ? uint8_t((px[0] + px[1] + px[2]) / 3) : px[3];
                    gl.pixels[i] = uint8_t((a + 8) / 17);
                }
                gl.packed.clear();
                cache_.erase(idx);
                dirty_ = true;
                app.status("Replaced glyph U+" + hex32(gl.code).substr(4));
            });
        }
        helpMarker("PNG with white glyph on transparent background (alpha = coverage), or white on black without alpha.");
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            std::fill(g.pixels.begin(), g.pixels.end(), uint8_t(0));
            g.packed.clear();
            cache_.erase(size_t(selected_));
            dirty_ = true;
        }
    }

    font::Font font_;
    std::string title_;
    std::unordered_map<size_t, std::shared_ptr<GpuImage>> cache_;
    std::string search_;
    int selected_ = -1, scrollTo_ = -1;
    float gridFrac_ = 0.62f; // glyph grid share of the width
    bool dirty_ = false;
};

} // namespace

std::unique_ptr<Document> makeFontDocument(const fs::path& p, Bytes data) { return std::make_unique<FontDocument>(p, std::move(data)); }

} // namespace nut::gui
