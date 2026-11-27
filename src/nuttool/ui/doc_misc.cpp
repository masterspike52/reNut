// Document factory and the simpler document types (video, ini, image, index, hex).
#include "docs.hpp"

#include "assetdb.hpp"
#include "loctext.hpp"
#include "media.hpp"
#include "ops.hpp"
#include "video_player.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

namespace nut::gui {

std::string friendlyName(const fs::path& root, const fs::path& p) {
    static std::mutex m;
    static std::map<std::string, assetdb::Names> cache;
    auto id = assetdb::idFromPath(p);
    if (!id || root.empty()) return toUtf8(p.filename());
    std::lock_guard<std::mutex> lk(m);
    std::string key = toUtf8(root);
    auto it = cache.find(key);
    if (it == cache.end()) {
        assetdb::Names names;
        try {
            for (auto& e : assetdb::readDbIndex(root / "Debug" / "db_index.txt")) names.add(e.name);
        } catch (...) {
        }
        it = cache.emplace(key, std::move(names)).first;
    }
    std::string n = it->second.lookup(*id);
    std::string t = assetdb::typeName(uint8_t(*id >> 24));
    if (startsWith(n, "aid_" + t + "_")) n = n.substr(5 + t.size());
    return n.empty() ? hex32(*id) : n;
}

namespace {

// ---- video -----------------------------------------------------------------------------

class VideoDocument : public Document {
public:
    VideoDocument(const fs::path& p, Bytes data) : data_(std::move(data)) {
        path = p;
        title_ = "video " + toUtf8(p.filename());
        info_ = media::describeAsf(data_);
    }
    std::string title() const override { return title_; }
    bool dirty() const override { return dirty_; }
    bool canSave() const override { return true; }

    void draw(App& app) override {
        ImGui::TextDisabled("XMV movies are plain Windows Media (WMV/ASF) files.  %s", formatSize(data_.size()).c_str());
        if (ImGui::Button("Export as .wmv...")) {
            Bytes d = data_;
            app.saveFileDialog("Export video", {{"Windows Media Video", "wmv"}}, [&app, d](const fs::path& p) {
                writeFile(p, d);
                app.status("Exported " + toUtf8(p));
            }, fromUtf8(toUtf8(path.stem()) + ".wmv"));
        }
        ImGui::SameLine();
        if (ImGui::Button("Replace with...")) {
            app.openFileDialog("Replacement video", {{"Windows Media Video", "wmv;asf;xmv"}}, [this, &app](const fs::path& p) {
                Bytes d = readFile(p);
                if (!media::isAsf(d)) {
                    app.error("That file is not WMV/ASF; the game expects WMV video");
                    return;
                }
                data_ = std::move(d);
                info_ = media::describeAsf(data_);
                dirty_ = true;
                player_.reset(); // reopen with the new movie
                app.status("Replaced video; save to write it");
            });
        }
        helpMarker("The replacement should be WMV (WMV3 video) like the original; the console's player decodes nothing else.");
        if (ImGui::CollapsingHeader("Streams")) ImGui::TextUnformatted(info_.c_str());
        drawPlayer();
    }

    ~VideoDocument() override = default;

    bool save(App& app, const fs::path* saveAs) override {
        try {
            fs::path target = saveAs ? *saveAs : path;
            if (!saveAs) app.backupBeforeWrite(path);
            writeFile(target, data_);
            dirty_ = false;
            return true;
        } catch (std::exception& e) {
            app.error(std::string("save failed: ") + e.what());
            return false;
        }
    }

private:
    void drawPlayer() {
        if (!player_) {
            player_ = std::make_unique<VideoPlayer>();
            player_->setLoop(loop_);
            player_->setVolume(volume_);
            if (player_->open(data_) && autoplay_) player_->play();
        }
        VideoPlayer& p = *player_;
        if (!p.isOpen()) {
            ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Cannot play: %s", p.error().c_str());
            return;
        }
        const GpuImage* frame = p.update();
        // Controls.
        if (ImGui::Button(p.playing() ? "Pause" : "Play", ImVec2(em(70), 0)) || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                                                                    !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false))) {
            if (p.playing()) p.pause();
            else p.play();
            autoplay_ = p.playing();
        }
        ImGui::SameLine();
        float t = float(scrubbing_ ? scrubTo_ : p.position());
        ImGui::SetNextItemWidth(std::max(em(120), ImGui::GetContentRegionAvail().x - em(330)));
        char label[64];
        std::snprintf(label, sizeof label, "%d:%05.2f / %d:%05.2f", int(t) / 60, std::fmod(t, 60.0f), int(p.duration()) / 60, std::fmod(float(p.duration()), 60.0f));
        if (ImGui::SliderFloat("##pos", &t, 0, float(p.duration()), label)) {
            scrubbing_ = true;
            scrubTo_ = t;
        }
        if (scrubbing_ && !ImGui::IsItemActive()) {
            // Seek once the slider is released (seeking restarts decoding).
            bool was = p.playing();
            p.seek(scrubTo_);
            if (was) p.play();
            scrubbing_ = false;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!p.hasAudio());
        ImGui::SetNextItemWidth(em(90));
        if (ImGui::SliderFloat("Volume", &volume_, 0, 1, "%.2f")) p.setVolume(volume_);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Checkbox("Loop", &loop_)) p.setLoop(loop_);
        // Picture, fitted to the space left.
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (frame && p.width() > 0 && p.height() > 0 && avail.x > 8 && avail.y > 8) {
            float s = std::min(avail.x / float(p.width()), avail.y / float(p.height()));
            ImVec2 size(float(p.width()) * s, float(p.height()) * s);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - size.x) / 2);
            ImGui::Image(frame->id(), size);
            if (ImGui::IsItemClicked()) {
                if (p.playing()) p.pause();
                else p.play();
                autoplay_ = p.playing();
            }
        } else if (!frame) {
            ImGui::TextDisabled("Loading...");
        }
    }

    Bytes data_;
    std::string title_, info_;
    bool dirty_ = false;
    std::unique_ptr<VideoPlayer> player_;
    bool autoplay_ = true, loop_ = false, scrubbing_ = false;
    float volume_ = 0.8f, scrubTo_ = 0;
};

// ---- ini / text ----------------------------------------------------------------------------

class IniDocument : public Document {
public:
    IniDocument(const fs::path& p, Bytes data) {
        path = p;
        title_ = toUtf8(p.filename());
        utf16_ = data.size() >= 2 && data[0] == 0xFE && data[1] == 0xFF;
        Bytes u = utf16_ ? ops::precacheToUtf8(data) : data;
        text_.assign(u.begin(), u.end());
    }
    std::string title() const override { return title_; }
    bool dirty() const override { return dirty_; }
    bool canSave() const override { return true; }

    void draw(App& app) override {
        ImGui::TextDisabled(utf16_ ? "UTF-16 (big-endian) character list: the glyphs the game pre-renders for this language."
                                   : "Plain text font configuration.");
        // As the command line's ini-export and ini-import: the text as UTF-8, and text files read back.
        if (ImGui::Button("Export text...")) {
            std::string text = text_;
            app.saveFileDialog("Export as UTF-8 text", {{"Text", "txt"}}, [&app, text](const fs::path& p) {
                writeText(p, text);
                app.status("Exported " + toUtf8(p));
            }, fromUtf8(toUtf8(path.stem()) + ".txt"));
        }
        ImGui::SameLine();
        if (ImGui::Button("Import text...")) {
            app.openFileDialog("Import UTF-8 text", {{"Text", "txt"}, {"All files", "*"}}, [this, &app](const fs::path& p) {
                text_ = readText(p);
                dirty_ = true;
                app.status("Loaded " + toUtf8(p) + " (save to write it)");
            });
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Save as UTF-16 character list", &utf16_)) dirty_ = true;
        helpMarker("Character lists (precache files) are UTF-16 big-endian with a byte order mark; the other font metadata is "
                   "plain text. Saving writes the text in the form ticked here (the command line's ini-import --utf16).");
        if (inputTextMultiline("##text", text_, ImGui::GetContentRegionAvail(), ImGuiInputTextFlags_AllowTabInput)) dirty_ = true;
    }

    bool save(App& app, const fs::path* saveAs) override {
        try {
            fs::path target = saveAs ? *saveAs : path;
            if (!saveAs) app.backupBeforeWrite(path);
            if (utf16_) writeFile(target, ops::utf8ToPrecache(text_));
            else writeText(target, text_);
            dirty_ = false;
            return true;
        } catch (std::exception& e) {
            app.error(std::string("save failed: ") + e.what());
            return false;
        }
    }

private:
    std::string title_, text_;
    bool utf16_ = false, dirty_ = false;
};

// ---- BMP image -------------------------------------------------------------------------------

image::Rgba decodeBmp(const Bytes& d) {
    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') fail("not a BMP file");
    uint32_t off = le32(&d[10]);
    int32_t w = int32_t(le32(&d[18])), h = int32_t(le32(&d[22]));
    uint16_t bpp = le16(&d[28]);
    uint32_t comp = le32(&d[30]);
    if ((comp != 0 && comp != 3) || (bpp != 8 && bpp != 24 && bpp != 32)) fail("unsupported BMP variant");
    bool topDown = h < 0;
    h = std::abs(h);
    size_t stride = (size_t(w) * bpp / 8 + 3) & ~size_t(3);
    if (off + stride * size_t(h) > d.size()) fail("truncated BMP");
    image::Rgba img;
    img.width = uint32_t(w);
    img.height = uint32_t(h);
    img.pixels.resize(size_t(w) * size_t(h) * 4);
    size_t pal = 14 + le32(&d[14]);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = &d[off + stride * size_t(topDown ? y : h - 1 - y)];
        for (int x = 0; x < w; ++x) {
            uint8_t* o = &img.pixels[(size_t(y) * size_t(w) + size_t(x)) * 4];
            const uint8_t* s = bpp == 8 ? &d[pal + size_t(row[x]) * 4] : &row[size_t(x) * bpp / 8];
            o[0] = s[2];
            o[1] = s[1];
            o[2] = s[0];
            o[3] = 255;
        }
    }
    return img;
}

class ImageDocument : public Document {
public:
    ImageDocument(const fs::path& p, Bytes data) : img_(decodeBmp(data)) {
        path = p;
        title_ = toUtf8(p.filename());
    }
    std::string title() const override { return title_; }
    void draw(App& app) override {
        if (!gpu_) gpu_ = app.upload(img_);
        ImGui::Text("%u x %u", img_.width, img_.height);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em(160));
        ImGui::SliderFloat("Zoom", &zoom_, 1, 16, "%.0fx");
        ImGui::SameLine();
        if (ImGui::Button("Export PNG...")) {
            image::Rgba img = img_;
            app.saveFileDialog("Export PNG", {{"PNG image", "png"}}, [&app, img](const fs::path& p) {
                writeFile(p, image::encodePng(img));
                app.status("Exported " + toUtf8(p));
            }, fromUtf8(toUtf8(path.stem()) + ".png"));
        }
        if (gpu_ && gpu_->tex) ImGui::Image(gpu_->id(), ImVec2(float(img_.width) * zoom_, float(img_.height) * zoom_));
    }

private:
    image::Rgba img_;
    std::shared_ptr<GpuImage> gpu_;
    std::string title_;
    float zoom_ = 4;
};

// ---- asset index ---------------------------------------------------------------------------------

class IndexDocument : public Document {
public:
    IndexDocument(const fs::path& p, const fs::path& root) : root_(root) {
        path = p;
        entries_ = assetdb::readDbIndex(p);
        for (auto& e : entries_) {
            fs::path f = assetdb::pathForEntry(root, e);
            files_.push_back(f);
            exists_.push_back(fs::exists(f));
        }
    }
    std::string title() const override { return "asset index"; }
    void draw(App& app) override {
        ImGui::SetNextItemWidth(em(300));
        inputText("##filter", filter_);
        ImGui::SameLine();
        ImGui::TextDisabled("%zu assets listed in db_index.txt (double-click to open)", entries_.size());
        ImGui::SameLine();
        if (ImGui::Button("Export JSON...")) {
            // The command line reads <root>\Debug\db_index.txt.
            fs::path root = path.parent_path().filename() == "Debug" ? path.parent_path().parent_path() : root_;
            app.saveFileDialog("Export the asset index", {{"JSON", "json"}}, [&app, root](const fs::path& out) {
                // The command line's index --json writes the list; its output is kept as the file.
                app.runCommand("Exporting the asset index", {"index", cliPath(root), "--json"}, [&app, out](int code, const std::string& text) {
                    if (code) {
                        std::string why = trim(text);
                        if (startsWith(why, "error: ")) why = why.substr(7);
                        app.error("The asset index was not exported: " + why);
                        return;
                    }
                    writeText(out, text);
                    app.status("Exported the asset index to " + toUtf8(out));
                }, false);
            }, fromUtf8("db_index.json"));
        }
        helpMarker("Every asset in db_index.txt with its id, file (relative to the assets folder), whether that file exists, "
                   "timestamp and version, as the command line's index --json writes them.");
        std::string needle = lower(filter_);
        if (!ImGui::BeginTable("##index", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, em(80));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 0.3f);
        ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, em(70));
        ImGui::TableHeadersRow();
        std::vector<size_t> rows;
        for (size_t i = 0; i < entries_.size(); ++i)
            if (needle.empty() || lower(entries_[i].name).find(needle) != std::string::npos) rows.push_back(i);
        ImGuiListClipper clip;
        clip.Begin(int(rows.size()));
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                size_t i = rows[size_t(r)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(int(i));
                if (ImGui::Selectable(hex32(entries_[i].id).c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(0) && exists_[i])
                    app.openPath(files_[i]);
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entries_[i].name.c_str());
                ImGui::TableNextColumn();
                std::string rel = toUtf8(fs::relative(files_[i], root_));
                if (exists_[i]) ImGui::TextUnformatted(rel.c_str());
                else ImGui::TextDisabled("%s (missing)", rel.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entries_[i].version.c_str());
            }
        }
        ImGui::EndTable();
    }

private:
    fs::path root_;
    std::vector<assetdb::IndexEntry> entries_;
    std::vector<fs::path> files_;
    std::vector<bool> exists_;
    std::string filter_;
};

// ---- fallback hex view ---------------------------------------------------------------------------

class HexDocument : public Document {
public:
    HexDocument(const fs::path& p, Bytes data, std::string kind) : data_(std::move(data)), kind_(std::move(kind)) {
        path = p;
        title_ = toUtf8(p.filename());
    }
    std::string title() const override { return title_; }
    void draw(App& app) override {
        ImGui::Text("%s, %s", kind_.c_str(), formatSize(data_.size()).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Export...")) {
            Bytes d = data_;
            app.saveFileDialog("Export", {{"All files", "*"}}, [&app, d](const fs::path& p) {
                writeFile(p, d);
                app.status("Exported " + toUtf8(p));
            }, path.filename());
        }
        hexView("##hex", data_, ImGui::GetContentRegionAvail().y);
    }

private:
    Bytes data_;
    std::string kind_, title_;
};

// A text file (a mod's Lua, JSON, TOML...), shown to read.
class PlainTextDocument : public Document {
public:
    PlainTextDocument(const fs::path& p, Bytes data) : text_(data.begin(), data.end()) {
        path = p;
        title_ = toUtf8(p.filename());
    }
    std::string title() const override { return title_; }
    void draw(App&) override {
        ImGui::TextDisabled("%s, %s (read only)", toUtf8(path).c_str(), formatSize(text_.size()).c_str());
        ImGui::InputTextMultiline("##text", text_.data(), text_.size() + 1, ImGui::GetContentRegionAvail(), ImGuiInputTextFlags_ReadOnly);
    }

private:
    std::string text_, title_;
};

} // namespace

std::unique_ptr<Document> makeVideoDocument(const fs::path& p, Bytes data) { return std::make_unique<VideoDocument>(p, std::move(data)); }
std::unique_ptr<Document> makeIniDocument(const fs::path& p, Bytes data) { return std::make_unique<IniDocument>(p, std::move(data)); }
std::unique_ptr<Document> makeImageDocument(const fs::path& p, Bytes data) { return std::make_unique<ImageDocument>(p, std::move(data)); }
std::unique_ptr<Document> makeIndexDocument(const fs::path& p, const fs::path& root) { return std::make_unique<IndexDocument>(p, root); }
std::unique_ptr<Document> makeHexDocument(const fs::path& p, Bytes data, const std::string& kind) {
    return std::make_unique<HexDocument>(p, std::move(data), kind);
}

std::unique_ptr<Document> loadDocument(const fs::path& p, const fs::path& root) {
    if (p.filename() == "db_index.txt") return makeIndexDocument(p, root);
    std::string ext = lower(p.extension().string());
    if (ext == ".ini") return makeIniDocument(p, readFile(p));
    if (ext == ".lua" || ext == ".json" || ext == ".toml" || ext == ".txt" || ext == ".md" || ext == ".cfg" || ext == ".xml")
        return std::make_unique<PlainTextDocument>(p, readFile(p));
    Loaded l = loadAsset(p);
    switch (l.kind) {
        case FileKind::Caff:
            if (loctext::isLoctext(*l.caff)) return makeTextDocument(p, root, std::move(l));
            return makeCaffDocument(p, root, std::move(l));
        case FileKind::StreamBundle: return makeStreamBundleDocument(p, root, std::move(l));
        case FileKind::Font: return makeFontDocument(p, std::move(l.raw));
        case FileKind::Asf: return makeVideoDocument(p, std::move(l.raw));
        case FileKind::Bmp: return makeImageDocument(p, std::move(l.raw));
        case FileKind::WaveBank: return makeWaveBankDocument(p, std::move(l.raw));
        default: return makeHexDocument(p, std::move(l.raw), kindName(l.kind));
    }
}

} // namespace nut::gui
