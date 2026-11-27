#include "texture_edit.hpp"

#include "app.hpp"

#include <algorithm>
#include <set>

namespace nut::gui {
namespace {

// The streamed level 0 of a split texture copy normally lives in the stream bundle with the same file name.
const AssetLoc* topFor(const TextureInfo& t, const AssetLoc& loc) {
    for (auto& tp : t.tops)
        if (tp.file.filename() == loc.file.filename()) return &tp;
    return nullptr;
}

} // namespace

TextureEdits::File& TextureEdits::load(const fs::path& p) {
    auto it = files_.find(p);
    if (it != files_.end()) return it->second;
    File f;
    f.l = loadAsset(p);
    loadedBytes_ += f.l.raw.size();
    f.l.raw = Bytes(); // the parsed copy is all that is needed
    return files_.emplace(p, std::move(f)).first->second;
}

void TextureEdits::eachCopy(const TextureInfo& t, bool withTops,
                            const std::function<bool(caff::Caff&, const ops::TexRef&, File*)>& change) {
    std::vector<std::function<void()>> undo;
    try {
        for (auto& loc : t.full) {
            File& f = load(loc.file);
            if (loc.entry < 0) {
                if (!f.l.caff) continue;
                caff::Caff& c = *f.l.caff;
                auto ref = ops::textureForSymbol(c, loc.symbol);
                if (!ref) continue;
                const AssetLoc* top = withTops ? topFor(t, loc) : nullptr;
                File* tf = top ? &load(top->file) : nullptr;
                std::optional<Bytes> oldTop;
                bool topWasDirty = tf && tf->dirty;
                if (tf && tf->l.sb && top->entry >= 0 && size_t(top->entry) < tf->l.sb->entries.size())
                    oldTop = tf->l.sb->entries[size_t(top->entry)].data;
                undo.push_back([&c, &f, header = ref->header, gpu = ref->gpu, oldHeader = c.symbolData(ref->header),
                                oldGpu = c.symbolData(ref->gpu), wasDirty = f.dirty, tf, entry = top ? top->entry : -1, oldTop, topWasDirty] {
                    c.replaceSymbolData(header, oldHeader);
                    c.replaceSymbolData(gpu, oldGpu);
                    f.dirty = wasDirty;
                    if (oldTop) {
                        tf->l.sb->entries[size_t(entry)].data = *oldTop;
                        tf->dirty = topWasDirty;
                    }
                });
                if (change(c, *ref, tf)) f.dirty = true;
            } else if (f.l.sb && size_t(loc.entry) < f.l.sb->entries.size()) {
                auto& e = f.l.sb->entries[size_t(loc.entry)];
                if (!caff::isCaff(e.data)) continue;
                caff::Caff c = caff::Caff::parse(e.data);
                auto ref = ops::textureForSymbol(c, loc.symbol);
                if (!ref || !change(c, *ref, nullptr)) continue;
                undo.push_back([&f, entry = size_t(loc.entry), old = e.data, wasDirty = f.dirty] {
                    f.l.sb->entries[entry].data = old;
                    f.dirty = wasDirty;
                });
                e.data = c.serialize();
                f.dirty = true;
            }
        }
    } catch (...) {
        for (auto it = undo.rbegin(); it != undo.rend(); ++it) (*it)();
        throw;
    }
}

std::string TextureEdits::import(const TextureInfo& t, const ops::TextureImage& img) {
    std::string summary;
    eachCopy(t, true, [&](caff::Caff& c, const ops::TexRef& ref, File* top) {
        bool streamChanged = false;
        texture::Header before = texture::parseHeader(c.symbolData(ref.header));
        std::string s = ops::importTexture(c, ref, img, top && top->l.sb ? &*top->l.sb : nullptr, &streamChanged);
        if (summary.empty()) summary = s;
        texture::Header after = texture::parseHeader(c.symbolData(ref.header));
        resized_ |= before.width != after.width || before.height != after.height;
        if (streamChanged) top->dirty = true;
        return true;
    });
    return summary;
}

bool TextureEdits::removeMips(const TextureInfo& t) {
    bool any = false;
    eachCopy(t, false, [&](caff::Caff& c, const ops::TexRef& ref, File*) {
        bool changed = false;
        ops::removeMipmaps(c, ref, &changed);
        any |= changed;
        return changed;
    });
    return any;
}

image::Rgba TextureEdits::currentImage(const TextureInfo& t) {
    if (t.full.empty()) fail("only the streamed level 0 of " + t.name + " was found");
    const AssetLoc& main = t.full.front();
    // A CAFF held by a loaded file: the bundle itself, or a parsed copy of a stream bundle entry.
    auto caffAt = [this](const AssetLoc& loc, std::optional<caff::Caff>& entry) -> caff::Caff* {
        File& f = load(loc.file);
        if (loc.entry < 0) return f.l.caff ? &*f.l.caff : nullptr;
        if (!f.l.sb || size_t(loc.entry) >= f.l.sb->entries.size()) return nullptr;
        entry = caff::Caff::parse(f.l.sb->entries[size_t(loc.entry)].data);
        return &*entry;
    };
    std::optional<caff::Caff> mainEntry, topEntry;
    caff::Caff* c = caffAt(main, mainEntry);
    std::optional<ops::TexRef> ref;
    if (c) ref = ops::textureForSymbol(*c, main.symbol);
    if (!ref) fail("texture not found in " + toUtf8Generic(main.file));
    std::vector<caff::Caff*> streams;
    const AssetLoc* top = topFor(t, main);
    if (!top && !t.tops.empty()) top = &t.tops.front();
    if (top)
        if (caff::Caff* tc = caffAt(*top, topEntry)) streams.push_back(tc);
    ops::DecodedTexture d = ops::decodeTexture(*ref, streams);
    return texture::toRgba(d.header, d.levels.at(0));
}

int TextureEdits::write(App& app, std::set<fs::path>* written) {
    int n = 0;
    for (auto& [p, f] : files_) {
        if (!f.dirty) continue;
        app.backupBeforeWrite(p);
        if (f.l.caff) saveCaff(p, *f.l.caff);
        else if (f.l.sb) writeFile(p, f.l.sb->serialize());
        if (written) written->insert(p);
        ++n;
    }
    files_.clear();
    loadedBytes_ = 0;
    return n;
}

BulkImportResult bulkImport(App& app, const Catalog& cat, const fs::path& folder, Task& task) {
    BulkImportResult r;
    std::set<std::string> exts;
    for (const std::string& e : split(ops::textureImagePattern(), ';')) exts.insert("." + e);
    std::map<std::string, size_t> byName;
    for (size_t i = 0; i < cat.textures.size(); ++i) byName.emplace(lower(ops::safeName(cat.textures[i].name, 150)), i);

    std::vector<fs::path> images;
    for (auto& e : fs::directory_iterator(folder))
        if (e.is_regular_file() && exts.count(lower(e.path().extension().string()))) images.push_back(e.path());
    std::sort(images.begin(), images.end());
    struct Job {
        size_t tex;
        fs::path image;                     // slice 0, or the whole texture
        std::map<uint32_t, fs::path> slices; // array textures: every slice given, 0 included
    };
    // An image is named after a texture, or after one slice of an array texture (<name>_slice1, as exported).
    std::map<size_t, std::vector<std::pair<fs::path, uint32_t>>> byTexture;
    for (auto& img : images) {
        std::string stem = lower(toUtf8(img.stem()));
        uint32_t slice = 0;
        auto it = byName.find(stem);
        if (it == byName.end()) {
            size_t at = stem.rfind("_slice");
            std::string digits = at == std::string::npos ? std::string() : stem.substr(at + 6);
            if (!digits.empty() && digits.size() <= 3 && std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                slice = uint32_t(std::stoul(digits));
                it = byName.find(stem.substr(0, at));
            }
        }
        if (it == byName.end()) ++r.unmatched;
        else byTexture[it->second].emplace_back(img, slice);
    }
    // Two images for one texture or slice (a .png and a .jpg, say): which is meant is unknown, so none is used.
    std::vector<Job> jobs;
    for (auto& [tex, imgs] : byTexture) {
        std::map<uint32_t, fs::path> slices;
        bool clash = false;
        for (auto& [p, s] : imgs) clash |= !slices.emplace(s, p).second;
        std::string names;
        for (auto& [p, s] : imgs) names += (names.empty() ? "" : ", ") + toUtf8(p.filename());
        if (clash || !slices.count(0)) {
            app.log("Not imported: " + names + (clash ? ": more than one image for the same texture or slice" : ": no image for slice 0"));
            r.failed += int(imgs.size());
            continue;
        }
        jobs.push_back({tex, slices[0], slices});
    }
    // Textures of the same files together, so each file is loaded and written once.
    auto fileOf = [&](const Job& j) { const auto& t = cat.textures[j.tex]; return t.full.empty() ? fs::path() : t.full.front().file; };
    std::stable_sort(jobs.begin(), jobs.end(), [&](const Job& a, const Job& b) { return fileOf(a) < fileOf(b); });

    TextureEdits edits;
    std::set<fs::path> written;
    for (size_t i = 0; i < jobs.size() && !task.cancel; ++i) {
        // Keep memory in check: stream bundles can be close to a gigabyte each.
        if (edits.loadedBytes() > (uint64_t(3) << 30)) edits.write(app, &written);
        const TextureInfo& t = cat.textures[jobs[i].tex];
        std::string file = toUtf8(jobs[i].image.filename());
        task.report(double(i) / double(jobs.size()), file);
        try {
            ops::TextureImage img = ops::readTextureImage(jobs[i].image);
            // Slices 1, 2... of an array texture; a missing one repeats slice 0.
            uint32_t lastSlice = jobs[i].slices.rbegin()->first;
            if (lastSlice > 0 && img.dds) fail("array slices can't be given as .dds; use PNG images");
            for (uint32_t s = 1; s <= lastSlice; ++s) {
                auto it = jobs[i].slices.find(s);
                img.extraSlices.push_back(it == jobs[i].slices.end() ? img.rgba : ops::readTextureImage(it->second).rgba);
                if (it != jobs[i].slices.end()) file += ", " + toUtf8(it->second.filename());
            }
            if (!img.dds && img.extraSlices.empty()) {
                image::Rgba cur = edits.currentImage(t);
                if (cur.width == img.width && cur.height == img.height && cur.pixels == img.rgba.pixels) {
                    ++r.unchanged;
                    continue;
                }
            }
            std::string summary = edits.import(t, img);
            ++r.imported;
            app.log("Imported " + file + ": " + summary);
        } catch (std::exception& e) {
            ++r.failed;
            app.log("Not imported: " + file + ": " + e.what());
        }
    }
    edits.write(app, &written);
    r.files = int(written.size());
    if (edits.resizedAny()) r.menuElements = fixMenus(app, cat, &task);
    return r;
}

int fixMenus(App& app, const Catalog& cat, Task* task) {
    ops::Progress pr;
    pr.log = [&app](const std::string& s) { app.log(s); };
    if (task) {
        pr.step = [task](double f, const std::string& s) { task->report(f, s); };
        pr.cancel = &task->cancel;
    }
    pr.at(-1, "looking for resized textures");
    auto resized = ops::resizedTextures(app.assetsRoot(), pr, app.updateRoot());
    int n = 0;
    for (size_t i = 0; i < cat.menuBundles.size(); ++i) {
        pr.at(double(i) / double(cat.menuBundles.size()), "fitting menus: " + toUtf8(cat.menuBundles[i].filename()));
        try {
            n += ops::adaptMenus(cat.menuBundles[i], resized, pr);
        } catch (std::exception& e) {
            app.log("Menus in " + toUtf8(cat.menuBundles[i].filename()) + " not changed: " + e.what());
        }
    }
    return n;
}

} // namespace nut::gui
