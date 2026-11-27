#include "ops.hpp"

#include "audio.hpp"

#include "assetdb.hpp"
#include "container.hpp"
#include "font.hpp"
#include "gamescript.hpp"
#include "image.hpp"
#include "json.hpp"
#include "loctext.hpp"
#include "media.hpp"
#include "nbpatch.hpp"
#include "objparams.hpp"
#include "xctd.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

namespace nut::ops {

// ---- naming ------------------------------------------------------------------

std::string safeName(std::string s, size_t maxLen) {
    for (char& c : s)
        if (!(std::isalnum(uint8_t(c)) || c == '_' || c == '-' || c == '.')) c = '_';
    if (s.size() > maxLen) s = s.substr(0, maxLen);
    return s.empty() ? "unnamed" : s;
}

bool validSection(const caff::Caff& c, const caff::Symbol& s) { return s.section >= 1 && s.section <= c.sections.size(); }

std::string sectionTag(const caff::Caff& c, const caff::Symbol& s) {
    if (!validSection(c, s)) return "nosection";
    std::string n = c.sections[s.section - 1].name;
    return n.empty() ? "data" : (n[0] == '.' ? n.substr(1) : n);
}

std::vector<size_t> selectSymbols(const caff::Caff& c, const std::string& sel, const std::string& section) {
    std::vector<size_t> out;
    if (!sel.empty() && sel[0] == '#') {
        size_t i = std::stoul(sel.substr(1));
        if (i == 0 || i > c.symbols.size()) fail("symbol index out of range");
        out.push_back(i - 1);
        return out;
    }
    std::string needle = lower(sel);
    for (size_t i = 0; i < c.symbols.size(); ++i) {
        if (!validSection(c, c.symbols[i]) || c.symbols[i].size == 0) continue;
        if (!section.empty() && c.sections[c.symbols[i].section - 1].name != section) continue;
        std::string n = lower(c.symbolName(i));
        if (n == needle || n.find(needle) != std::string::npos) out.push_back(i);
    }
    // Prefer exact base-name matches when the substring is ambiguous.
    std::vector<size_t> exact;
    for (size_t i : out)
        if (lower(assetdb::baseAssetName(c.symbolName(i))) == needle) exact.push_back(i);
    return exact.empty() ? out : exact;
}

// ---- textures ------------------------------------------------------------------

std::vector<TexRef> findTextures(caff::Caff& c) {
    std::vector<TexRef> out;
    for (size_t i = 0; i < c.symbols.size(); ++i) {
        const auto& s = c.symbols[i];
        if (!validSection(c, s) || c.sections[s.section - 1].name != ".data" || s.size < 0x38) continue;
        if (std::memcmp(&c.sections[s.section - 1].data[s.offset], "texture\0", 8) != 0) continue;
        for (size_t k = 0; k < c.symbols.size(); ++k) {
            const auto& g = c.symbols[k];
            if (k != i && g.nameIndex == s.nameIndex && s.nameIndex && validSection(c, g) && c.sections[g.section - 1].name == ".texturegpu") {
                out.push_back({&c, i, k, assetdb::baseAssetName(c.symbolName(i))});
                break;
            }
        }
    }
    return out;
}

std::optional<TexRef> textureForSymbol(caff::Caff& c, size_t symbol) {
    for (auto& t : findTextures(c))
        if (t.header == symbol || t.gpu == symbol) return t;
    return std::nullopt;
}

std::string partnerName(const std::string& n, const char* from, const char* to) {
    if (!endsWith(n, from)) return {};
    return n.substr(0, n.size() - 3) + to;
}

std::string fullName(const std::string& n) {
    if (endsWith(n, "mip") || endsWith(n, "top")) {
        size_t p = n.rfind("_0x");
        if (p != std::string::npos && n.size() - p == 3 + 8 + 3) return n.substr(0, n.size() - 3);
    }
    return n;
}

texture::Header asFullChain(texture::Header h) {
    h.baseOffset = 0;
    h.mipOffset = 0xFFFFFFFFu;
    return h;
}

DecodedTexture decodeTexture(const TexRef& t, const std::vector<caff::Caff*>& streams) {
    DecodedTexture d;
    d.header = texture::parseHeader(t.caff->symbolData(t.header));
    d.levels = texture::decode(d.header, t.caff->symbolData(t.gpu));
    d.name = t.name;
    std::string topName = partnerName(t.name, "mip", "top");
    if (topName.empty()) return d;
    for (auto* sc : streams) {
        for (auto& top : findTextures(*sc)) {
            if (top.name != topName) continue;
            texture::Header th = texture::parseHeader(sc->symbolData(top.header));
            auto topLevels = texture::decode(th, sc->symbolData(top.gpu));
            if (th.width != d.header.width || th.height != d.header.height || topLevels[0].slices.size() != d.levels[0].slices.size()) return d;
            d.levels.insert(d.levels.begin(), topLevels[0]);
            d.header = asFullChain(d.header);
            d.name = fullName(t.name);
            d.merged = true;
            return d;
        }
    }
    return d;
}

void writeTextureFiles(const fs::path& dir, const std::string& name, const texture::Header& h, const std::vector<texture::Level>& levels,
                       bool dds, bool png) {
    uint32_t slices = uint32_t(levels.at(0).slices.size());
    for (uint32_t s = 0; s < slices; ++s) {
        std::string suffix = slices == 1 ? "" : (h.kind == texture::Kind::Cube ? "_face" : "_slice") + std::to_string(s);
        if (dds) writeFile(dir / (safeName(name + suffix, 150) + ".dds"), image::encodeDds(texture::toDds(h, levels, s)));
        if (png) writeFile(dir / (safeName(name + suffix, 150) + ".png"), image::encodePng(texture::toRgba(h, levels[0], s)));
    }
}

TexStats exportTextures(const std::vector<caff::Caff*>& mains, const std::vector<caff::Caff*>& streams, const fs::path& dir, bool dds, bool png,
                        std::set<std::string>* mergedTops, const Progress* progress, const std::string& filter,
                        std::set<std::string>* seen) {
    TexStats st;
    for (auto* c : mains) {
        for (auto& t : findTextures(*c)) {
            std::string key = lower(fullName(t.name));
            if (!filter.empty() && key.find(filter) == std::string::npos) continue;
            if (seen && !seen->insert(key).second) continue;
            try {
                Bytes hd = c->symbolData(t.header);
                texture::Header h = texture::parseHeader(hd);
                if (!texture::describeFormat(h.d3dFormat).supported()) {
                    writeFile(dir / "raw" / (safeName(t.name, 150) + ".header.bin"), hd);
                    writeFile(dir / "raw" / (safeName(t.name, 150) + ".gpu.bin"), c->symbolData(t.gpu));
                    ++st.skipped;
                    continue;
                }
                DecodedTexture d = decodeTexture(t, streams);
                if (d.merged) {
                    ++st.merged;
                    if (mergedTops) mergedTops->insert(partnerName(t.name, "mip", "top"));
                }
                writeTextureFiles(dir, d.name, d.header, d.levels, dds, png);
                ++st.written;
            } catch (std::exception& e) {
                if (progress) progress->info("warning: texture " + t.name + ": " + e.what());
                ++st.skipped;
            }
        }
    }
    return st;
}

namespace {

std::vector<texture::Level> levelsFrom(const texture::Header& h, const texture::Contents& c, const TextureImage& src) {
    if (src.dds) return texture::fromDds(h, c, src.ddsFile);
    if (c.slices != 1 && h.kind != texture::Kind::Array) fail("importing cube and volume textures is not supported");
    auto dims = texture::levelDims(h, c);
    // One image per slice; slices without their own image repeat slice 0.
    if (src.extraSlices.size() + 1 > c.slices)
        fail("the texture has " + std::to_string(c.slices) + " slice(s), but images were given for " + std::to_string(src.extraSlices.size() + 1));
    std::vector<const image::Rgba*> images(c.slices, &src.rgba);
    for (size_t s = 1; s < c.slices && s - 1 < src.extraSlices.size(); ++s) images[s] = &src.extraSlices[s - 1];
    for (size_t s = 0; s < images.size(); ++s)
        if (images[s]->width != dims[0].first || images[s]->height != dims[0].second)
            fail("the image" + (c.slices > 1 ? " of slice " + std::to_string(s) : std::string()) + " is " + std::to_string(images[s]->width) +
                 "x" + std::to_string(images[s]->height) + " but the texture needs " + std::to_string(dims[0].first) + "x" +
                 std::to_string(dims[0].second));
    texture::Format f = texture::describeFormat(h.d3dFormat);
    std::vector<texture::Level> levels;
    for (size_t i = 0; i < dims.size(); ++i) {
        texture::Level lv;
        lv.width = dims[i].first;
        lv.height = dims[i].second;
        for (const image::Rgba* img : images) lv.slices.push_back(texture::fromRgba(f, i == 0 ? *img : texture::downscale(*img, lv.width, lv.height)));
        levels.push_back(std::move(lv));
    }
    return levels;
}

std::string sizeText(uint32_t w, uint32_t h) { return std::to_string(w) + "x" + std::to_string(h); }

} // namespace

TextureImage readTextureImage(const fs::path& img) {
    TextureImage s;
    Bytes file = readFile(img);
    if (lower(img.extension().string()) == ".dds") {
        image::Dds d = image::decodeDds(file);
        s.dds = true;
        s.width = d.width;
        s.height = d.height;
        s.ddsFile = std::move(file);
    } else {
        s.rgba = image::decodeImage(file);
        s.width = s.rgba.width;
        s.height = s.rgba.height;
    }
    return s;
}

std::vector<texture::Level> levelsFromImage(const texture::Header& h, const texture::Contents& c, const fs::path& img) {
    return levelsFrom(h, c, readTextureImage(img));
}

const char* textureImagePattern() { return image::canDecodeImages() ? "png;jpg;jpeg;bmp;tga;dds" : "png;dds"; }

std::string importTexture(caff::Caff& c, const TexRef& t, const fs::path& img, streambundle::StreamBundle* stream, bool* streamChanged) {
    return importTexture(c, t, readTextureImage(img), stream, streamChanged);
}

std::string importTexture(caff::Caff& c, const TexRef& t, const TextureImage& src, streambundle::StreamBundle* stream, bool* streamChanged) {
    if (streamChanged) *streamChanged = false;
    Bytes headerData = c.symbolData(t.header);
    texture::Header h = texture::parseHeader(headerData);
    texture::Contents cont = texture::describeContents(h, c.symbolData(t.gpu).size());
    bool split = cont.firstLevel == 1;

    // An image of another size resizes the texture: the header gets the new size (and mip count)
    // and the GPU data is laid out again, so it can grow.
    texture::Header nh = h;
    texture::Contents ncont = cont;
    bool resize = src.width != h.width || src.height != h.height;
    // Array textures (the animated action buttons, say) list every slice's size in a table after the header.
    bool array = h.kind == texture::Kind::Array && texture::hasArrayTable(headerData, cont.slices);
    if (resize) {
        if ((h.kind != texture::Kind::Tex2D || cont.slices != 1) && !array)
            fail("only 2D and array textures can change size; the image must be " + sizeText(h.width, h.height));
        if (h.baseOffset != 0 || (h.mipOffset != 0xFFFFFFFFu && !split) || (array && split))
            fail("this texture's layout cannot change size; the image must be " + sizeText(h.width, h.height));
        if (split && !stream)
            fail("this texture is split for streaming, and changing its size needs the paired stream bundle too (the image must be " +
                 sizeText(h.width, h.height) + " without it)");
        nh = texture::resized(h, src.width, src.height);
        ncont = texture::describeContents(nh, 0);
        // The slice table is checked against the header's old size, so it goes first.
        if (array) {
            ncont.slices = cont.slices;
            texture::Contents one = ncont;
            one.slices = 1;
            texture::writeArrayTable(headerData, nh, cont.slices, uint32_t(texture::gpuSize(nh, one)));
        }
        texture::writeHeaderDims(headerData, nh.width, nh.height, nh.mipLevels);
    }
    std::string sized = resize ? ", resized from " + sizeText(h.width, h.height) + " to " + sizeText(nh.width, nh.height) : "";

    // Levels are always made for the whole chain; a split texture's halves take their part.
    texture::Header full = split ? asFullChain(nh) : nh;
    texture::Contents fullCont = split ? texture::describeContents(full, 0) : ncont;
    if (split && stream) {
        std::string topName = partnerName(t.name, "mip", "top");
        for (auto& e : stream->entries) {
            if (!caff::isCaff(e.data)) continue;
            caff::Caff sc = caff::Caff::parse(e.data);
            auto tops = findTextures(sc);
            auto it = std::find_if(tops.begin(), tops.end(), [&](const TexRef& r) { return r.name == topName; });
            if (it == tops.end()) continue;
            Bytes topData = sc.symbolData(it->header);
            texture::Header th = texture::parseHeader(topData);
            texture::Contents tc = texture::describeContents(th, sc.symbolData(it->gpu).size());
            auto levels = levelsFrom(full, fullCont, src);
            if (resize) {
                th.width = nh.width;
                th.height = nh.height;
                texture::writeHeaderDims(topData, th.width, th.height, th.mipLevels);
                sc.replaceSymbolData(it->header, topData);
            }
            std::vector<texture::Level> topLevels(levels.begin(), levels.begin() + 1), mipLevels(levels.begin() + 1, levels.end());
            sc.replaceSymbolData(it->gpu, texture::encode(th, tc, topLevels));
            c.replaceSymbolData(t.gpu, texture::encode(nh, ncont, mipLevels));
            if (resize) c.replaceSymbolData(t.header, headerData);
            e.data = sc.serialize();
            if (streamChanged) *streamChanged = true;
            return "imported " + std::to_string(levels.size()) + " levels (level 0 into the stream bundle)" + sized;
        }
        fail("the streamed top level '" + topName + "' was not found in the paired stream bundle");
    }
    auto levels = levelsFrom(full, fullCont, src);
    if (split) levels.erase(levels.begin());
    c.replaceSymbolData(t.gpu, texture::encode(nh, ncont, levels));
    if (resize) c.replaceSymbolData(t.header, headerData);
    std::string s = "imported " + std::to_string(levels.size()) + " levels" + sized;
    if (ncont.slices > 1 && !src.dds) {
        s += ", " + std::to_string(ncont.slices) + " slices";
        if (src.extraSlices.size() + 1 < ncont.slices) s += " (slices without their own image repeat slice 0)";
    }
    if (split) s += " (mips only: the streamed top level was not updated)";
    return s;
}

std::string removeMipmaps(caff::Caff& c, const TexRef& t, bool* changed) {
    if (changed) *changed = false;
    Bytes headerData = c.symbolData(t.header);
    texture::Header h = texture::parseHeader(headerData);
    Bytes gpu = c.symbolData(t.gpu);
    texture::Contents cont = texture::describeContents(h, gpu.size());
    bool split = cont.firstLevel == 1;
    uint32_t keep = split ? 2 : 1;
    if (h.mipLevels <= keep) return split ? "no mipmaps to remove (a split texture keeps level 1 beside its streamed level 0)" : "no mipmaps to remove";
    if (!split && (h.baseOffset != 0 || (h.mipOffset != 0xFFFFFFFFu && h.mipOffset != 0)))
        fail("this texture's layout (explicit level offsets) is not supported for removing mipmaps");
    // The stored levels are re-tiled as they are, without recompressing.
    auto levels = texture::decode(h, gpu);
    texture::Header nh = h;
    nh.mipLevels = keep;
    nh.mipOffset = split ? 0 : 0xFFFFFFFFu;
    texture::Contents nc = cont;
    nc.levelCount = keep - cont.firstLevel;
    levels.resize(nc.levelCount);
    c.replaceSymbolData(t.gpu, texture::encode(nh, nc, levels));
    texture::writeHeaderDims(headerData, h.width, h.height, keep);
    if (!split) putBe32(&headerData[0x2C], nh.mipOffset);
    c.replaceSymbolData(t.header, headerData);
    if (changed) *changed = true;
    uint32_t removed = h.mipLevels - keep;
    return "removed " + std::to_string(removed) + " mip level(s)" + (split ? " (kept level 1 beside the streamed level 0)" : "");
}

namespace {

// removeMipmaps for the textures of `c` that differ from the same-named texture in `original`.
int removeChangedMips(caff::Caff& c, caff::Caff* original, const std::string& where, const Progress& progress) {
    std::map<std::string, std::pair<Bytes, Bytes>> before;
    if (original)
        for (auto& t : findTextures(*original)) before[t.name] = {original->symbolData(t.header), original->symbolData(t.gpu)};
    int n = 0;
    for (auto& t : findTextures(c)) {
        auto it = before.find(t.name);
        if (it != before.end() && it->second.first == c.symbolData(t.header) && it->second.second == c.symbolData(t.gpu)) continue;
        try {
            bool changed = false;
            std::string s = removeMipmaps(c, t, &changed);
            if (!changed) continue;
            ++n;
            progress.info(where + ": " + t.name + ": " + s);
        } catch (std::exception& e) {
            progress.info("warning: " + where + ": " + t.name + ": " + e.what());
        }
    }
    return n;
}

} // namespace

int removeChangedMipmaps(const fs::path& file, const fs::path& original, const Progress& progress) {
    std::string where = (file.parent_path().filename() / file.filename()).generic_string();
    Loaded l = loadAsset(file);
    int n = 0;
    if (l.caff) {
        std::optional<Loaded> o;
        if (!original.empty()) o = loadAsset(original);
        n = removeChangedMips(*l.caff, o && o->caff ? &*o->caff : nullptr, where, progress);
        if (n) {
            prepareOutput(file, {});
            saveCaff(file, *l.caff);
        }
    } else if (l.sb) {
        // Stream bundles can be close to a gigabyte, so a plain original is read entry by entry.
        std::vector<streambundle::IndexEntry> index;
        std::map<uint32_t, streambundle::IndexEntry> before;
        std::optional<Loaded> whole;
        std::map<uint32_t, const Bytes*> wholeEntries;
        if (!original.empty()) {
            if (streambundle::readIndex(original, index)) {
                for (auto& e : index) before[e.id] = e;
            } else {
                whole = loadAsset(original);
                if (whole->sb)
                    for (auto& e : whole->sb->entries) wholeEntries[e.id] = &e.data;
            }
        }
        for (auto& e : l.sb->entries) {
            if (!caff::isCaff(e.data)) continue;
            std::optional<Bytes> od;
            if (auto it = before.find(e.id); it != before.end()) od = streambundle::readEntry(original, it->second);
            else if (auto w = wholeEntries.find(e.id); w != wholeEntries.end()) od = *w->second;
            std::optional<caff::Caff> oc;
            if (od) {
                if (*od == e.data) continue;
                if (caff::isCaff(*od)) oc = caff::Caff::parse(*od);
            }
            caff::Caff c = caff::Caff::parse(e.data);
            int k = removeChangedMips(c, oc ? &*oc : nullptr, where, progress);
            if (!k) continue;
            e.data = c.serialize();
            n += k;
        }
        if (n) {
            prepareOutput(file, {});
            writeFile(file, l.sb->serialize());
        }
    }
    return n;
}

// ---- menus -------------------------------------------------------------------------

namespace {

using Sizes = std::map<std::string, std::pair<uint32_t, uint32_t>>;

void textureSizes(caff::Caff& c, Sizes& out) {
    for (auto& t : findTextures(c)) {
        try {
            texture::Header h = texture::parseHeader(c.symbolData(t.header));
            out[lower(fullName(t.name))] = {h.width, h.height};
        } catch (...) {
        }
    }
}

void compareSizes(const Sizes& now, const Sizes& before, std::map<std::string, xui::Resize>& out) {
    for (auto& [name, wh] : now) {
        auto it = before.find(name);
        if (it != before.end() && it->second != wh) out[name] = {it->second.first, it->second.second, wh.first, wh.second};
    }
}

} // namespace

std::map<std::string, xui::Resize> resizedTextures(const fs::path& root, const Progress& progress, const fs::path& updateDir) {
    std::map<std::string, xui::Resize> out;
    auto changes = nbpatch::findChanges(root, {}, progress);
    if (!updateDir.empty() && fs::is_directory(updateDir))
        for (auto& ch : nbpatch::findChanges(updateDir, {}, progress)) changes.push_back(ch);
    for (auto& ch : changes) {
        if (ch.original.empty()) continue;
        try {
            std::vector<streambundle::IndexEntry> now, before;
            if (streambundle::readIndex(ch.modified, now) && streambundle::readIndex(ch.original, before)) {
                // Stream bundles can be close to a gigabyte: only the entries that differ are read.
                std::map<uint32_t, streambundle::IndexEntry> old;
                for (auto& e : before) old[e.id] = e;
                for (auto& e : now) {
                    auto it = old.find(e.id);
                    if (it == old.end()) continue;
                    Bytes a = streambundle::readEntry(ch.modified, e), b = streambundle::readEntry(ch.original, it->second);
                    if (a == b || !caff::isCaff(a) || !caff::isCaff(b)) continue;
                    caff::Caff ca = caff::Caff::parse(a), cb = caff::Caff::parse(b);
                    Sizes sa, sb;
                    textureSizes(ca, sa);
                    textureSizes(cb, sb);
                    compareSizes(sa, sb, out);
                }
                continue;
            }
            Loaded a = loadAsset(ch.modified), b = loadAsset(ch.original);
            if (!a.caff || !b.caff) continue;
            Sizes sa, sb;
            textureSizes(*a.caff, sa);
            textureSizes(*b.caff, sb);
            compareSizes(sa, sb, out);
        } catch (std::exception& e) {
            progress.info("warning: " + ch.path + ": " + e.what());
        }
    }
    return out;
}

fs::path updateFolder(const fs::path& root, const fs::path& explicitDir) {
    if (!explicitDir.empty()) return fs::is_directory(explicitDir) ? explicitDir : fs::path();
    if (root.empty()) return {};
    for (const fs::path& d : {fs::absolute(root).lexically_normal().parent_path() / "update", root / "update"})
        if (fs::is_directory(d) && !updateBundles(d).empty()) return d;
    return {};
}

std::vector<fs::path> updateBundles(const fs::path& updateDir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (updateDir.empty() || !fs::is_directory(updateDir, ec)) return out;
    for (auto& e : fs::directory_iterator(updateDir, ec)) {
        if (!e.is_regular_file() || e.path().has_extension()) continue;
        auto id = assetdb::idFromPath(e.path().filename());
        if (id && ((*id >> 24) == assetdb::kTypeBundle || (*id >> 24) == assetdb::kTypeStreamBundle)) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<fs::path> menuBundles(const fs::path& root, const Progress& progress, const fs::path& updateDir) {
    std::vector<fs::path> files, out;
    if (fs::exists(root / "Bundle" / "4f"))
        for (auto& e : fs::directory_iterator(root / "Bundle" / "4f"))
            if (e.is_regular_file() && e.path().extension() != ".bak") files.push_back(e.path());
    for (auto& f : updateBundles(updateDir))
        if ((assetdb::idFromPath(f.filename()).value_or(0) >> 24) == assetdb::kTypeBundle) files.push_back(f);
    std::sort(files.begin(), files.end());
    for (size_t i = 0; i < files.size() && !progress.cancelled(); ++i) {
        progress.at(double(i) / double(files.size()), files[i].filename().string());
        try {
            // The symbol names come before the GPU data: decompress only up to the end of .data.
            Bytes raw = readFile(files[i]);
            bool compressed = xctd::isXctd(raw);
            size_t want = 256 * 1024;
            for (int tries = 0; tries < 4; ++tries) {
                Bytes plain = compressed ? xctd::decompressPrefix(raw, want) : raw;
                size_t need = 0;
                if (auto c = caff::Caff::parsePartial(plain, {".data"}, need)) {
                    for (size_t s = 0; s < c->symbols.size(); ++s)
                        if (startsWith(c->symbolName(s), "aid_xuipackage_")) {
                            out.push_back(files[i]);
                            break;
                        }
                    break;
                }
                if (need <= plain.size()) break;
                want = need;
            }
        } catch (std::exception& e) {
            progress.info("warning: " + files[i].filename().string() + ": " + e.what());
        }
    }
    return out;
}

int adaptMenus(const fs::path& bundle, const std::map<std::string, xui::Resize>& resized, const Progress& progress) {
    Loaded now = loadAsset(bundle);
    if (!now.caff) return 0;
    fs::path bak = bundle;
    bak += ".bak";
    std::optional<Loaded> orig;
    if (fs::exists(bak)) orig = loadAsset(bak);
    std::string where = (bundle.parent_path().filename() / bundle.filename()).generic_string();
    int total = 0;
    bool dirty = false;
    caff::Caff& c = *now.caff;
    for (size_t i = 0; i < c.symbols.size(); ++i) {
        std::string name = c.symbolName(i);
        if (!startsWith(name, "aid_xuipackage_")) continue;
        Bytes current = c.symbolData(i), from = current;
        if (orig && orig->caff)
            for (size_t k = 0; k < orig->caff->symbols.size(); ++k)
                if (orig->caff->symbolName(k) == name) from = orig->caff->symbolData(k);
        std::vector<std::string> notes;
        int n = 0;
        Bytes out = xui::adaptPackage(from, resized, notes, &n);
        for (auto& s : notes) progress.info(where + ": " + s);
        if (n) progress.info(where + ": " + assetdb::baseAssetName(name) + ": adjusted " + std::to_string(n) + " element(s)");
        total += n;
        if (out != current) {
            c.replaceSymbolData(i, out);
            dirty = true;
        }
    }
    if (dirty) {
        prepareOutput(bundle, {});
        saveCaff(bundle, c);
    }
    return total;
}

// ---- containers --------------------------------------------------------------------

void unpackCaff(const caff::Caff& c, bool wasXctd, const fs::path& dir) {
    fs::create_directories(longPath(dir));
    Json j = c.toJson();
    j["xctd"] = wasXctd;
    Json files = Json::array();
    std::vector<std::vector<uint8_t>> covered(c.sections.size());
    for (size_t i = 0; i < c.sections.size(); ++i) covered[i].assign(c.sections[i].data.size(), 0);
    bool overlap = false;
    for (size_t i = 0; i < c.symbols.size(); ++i) {
        const auto& s = c.symbols[i];
        if (!validSection(c, s) || s.size == 0 || uint64_t(s.offset) + s.size > c.sections[s.section - 1].data.size()) {
            files.push(Json());
            continue;
        }
        char prefix[16];
        std::snprintf(prefix, sizeof prefix, "%04zu_", i + 1);
        std::string fn = "assets/" + std::string(prefix) + safeName(assetdb::baseAssetName(c.symbolName(i))) + "." + sectionTag(c, s) + ".bin";
        writeFile(dir / fn, c.symbolData(i));
        files.push(fn);
        auto& cov = covered[s.section - 1];
        for (uint32_t k = 0; k < s.size; ++k) {
            if (cov[s.offset + k]) overlap = true;
            cov[s.offset + k] = 1;
        }
    }
    j["symbolFiles"] = files;
    // Sections are only stored when the symbol files cannot reproduce them exactly.
    Json secFiles = Json::array();
    for (size_t i = 0; i < c.sections.size(); ++i) {
        bool needed = overlap;
        for (size_t k = 0; k < c.sections[i].data.size() && !needed; ++k)
            if (!covered[i][k] && c.sections[i].data[k]) needed = true;
        if (needed) {
            std::string fn = "sections/" + std::to_string(i + 1) + "_" + safeName(c.sections[i].name) + ".bin";
            writeFile(dir / fn, c.sections[i].data);
            secFiles.push(fn);
        } else {
            secFiles.push(Json());
        }
    }
    j["sectionFiles"] = secFiles;
    writeText(dir / "caff.json", j.dump());
}

caff::Caff packCaff(const fs::path& dir, bool* wasXctd, int* changed) {
    Json j = Json::parse(readText(dir / "caff.json"));
    caff::Caff c = caff::Caff::fromJson(j);
    if (wasXctd) *wasXctd = j.has("xctd") && j["xctd"].asBool();
    const auto& secJ = j["sections"].items();
    for (size_t i = 0; i < c.sections.size(); ++i) {
        const Json& f = j["sectionFiles"][i];
        if (!f.isNull()) c.sections[i].data = readFile(dir / f.asString());
        else c.sections[i].data.assign(secJ[i]["size"].asU32(), 0);
    }
    // First place unchanged-size symbol files, then apply resized ones (which shift later data).
    std::vector<std::pair<size_t, Bytes>> resized;
    int nChanged = 0;
    const Json& sf = j["symbolFiles"];
    for (size_t i = 0; i < c.symbols.size() && i < sf.size(); ++i) {
        if (sf[i].isNull()) continue;
        Bytes data = readFile(dir / sf[i].asString());
        auto& s = c.symbols[i];
        auto& sec = c.sections.at(s.section - 1);
        if (data.size() == s.size) {
            if (!std::equal(data.begin(), data.end(), sec.data.begin() + s.offset)) {
                if (!j["sectionFiles"][s.section - 1].isNull()) ++nChanged; // edited relative to stored section
                std::copy(data.begin(), data.end(), sec.data.begin() + s.offset);
            }
        } else {
            resized.emplace_back(i, std::move(data));
        }
    }
    for (auto& [i, data] : resized) {
        c.replaceSymbolData(i, data);
        ++nChanged;
    }
    if (changed) *changed = nChanged;
    return c;
}

std::string entryName(const streambundle::Entry& e) {
    if (caff::isCaff(e.data)) {
        try {
            caff::Caff c = caff::Caff::parse(e.data);
            for (size_t i = 0; i < c.symbols.size(); ++i) {
                std::string n = assetdb::baseAssetName(c.symbolName(i));
                if (startsWith(n, "aid_")) return n;
            }
            if (!c.symbolNames.empty()) return assetdb::baseAssetName(c.symbolNames[0]);
        } catch (...) {
        }
    }
    if (media::isWaveBank(e.data)) {
        try { return "wavebank_" + media::parseWaveBank(e.data).name; } catch (...) {}
    }
    return {};
}

std::string entryExt(const streambundle::Entry& e) {
    if (caff::isCaff(e.data)) return ".caff";
    if (media::isWaveBank(e.data)) return ".xwb";
    return ".bin";
}

void unpackStreamBundle(const streambundle::StreamBundle& sb, const fs::path& dir) {
    fs::create_directories(longPath(dir));
    Json j = Json::object();
    j["version"] = double(sb.version);
    j["timestamp"] = sb.timestamp;
    Json deps = Json::array();
    for (uint32_t d : sb.dependencies) deps.push("0x" + hex32(d));
    j["dependencies"] = deps;
    Json entries = Json::array();
    for (size_t i = 0; i < sb.entries.size(); ++i) {
        const auto& e = sb.entries[i];
        Json o = Json::object();
        o["id"] = "0x" + hex32(e.id);
        o["type"] = assetdb::typeName(uint8_t(e.id >> 24));
        if (e.data.empty()) {
            o["file"] = Json();
        } else {
            std::string name = entryName(e);
            o["name"] = name;
            char prefix[16];
            std::snprintf(prefix, sizeof prefix, "%04zu_", i + 1);
            std::string fn = std::string(prefix) + hex32(e.id) + (name.empty() ? "" : "_" + safeName(name)) + entryExt(e);
            writeFile(dir / fn, e.data);
            o["file"] = fn;
        }
        entries.push(o);
    }
    j["entries"] = entries;
    writeText(dir / "streambundle.json", j.dump());
}

streambundle::StreamBundle packStreamBundle(const fs::path& dir) {
    Json j = Json::parse(readText(dir / "streambundle.json"));
    streambundle::StreamBundle sb;
    sb.version = float(j["version"].asNumber());
    sb.timestamp = j["timestamp"].asU32();
    for (auto& d : j["dependencies"].items()) sb.dependencies.push_back(d.asU32());
    for (auto& o : j["entries"].items()) {
        streambundle::Entry e;
        e.id = o["id"].asU32();
        if (!o["file"].isNull()) e.data = readFile(dir / o["file"].asString());
        sb.entries.push_back(std::move(e));
    }
    return sb;
}

int exportWaveBank(const Bytes& data, const fs::path& dir, const std::string& base, bool wav) {
    media::WaveBank wb = media::parseWaveBank(data);
    writeFile(dir / (safeName(base) + ".xwb"), data);
    int decoded = 0;
    for (size_t i = 0; i < wb.waves.size(); ++i) {
        std::string ext;
        Bytes f;
        if (wav && audio::canDecode(wb.waves[i])) {
            try {
                f = audio::toWav(audio::decode(wb.waves[i]));
                ext = ".wav";
                ++decoded;
            } catch (std::exception&) {
            }
        }
        if (f.empty()) f = media::waveToFile(wb.waves[i], ext);
        char prefix[16];
        std::snprintf(prefix, sizeof prefix, "%03zu_", i);
        writeFile(dir / safeName(base) / (std::string(prefix) + safeName(wb.waves[i].name) + ext), f);
    }
    return decoded;
}

Bytes precacheToUtf8(const Bytes& d) {
    if (d.size() < 2 || d[0] != 0xFE || d[1] != 0xFF) fail("expected a UTF-16BE file with BOM");
    std::u16string s;
    for (size_t i = 2; i + 1 < d.size(); i += 2) s += char16_t(be16(&d[i]));
    std::string u = utf16ToUtf8(s);
    return Bytes(u.begin(), u.end());
}

Bytes utf8ToPrecache(const std::string& text) {
    Bytes out{0xFE, 0xFF};
    for (char16_t ch : utf8ToUtf16(text)) { out.push_back(uint8_t(ch >> 8)); out.push_back(uint8_t(ch)); }
    return out;
}

// ---- whole-folder jobs -----------------------------------------------------------

namespace {

std::vector<fs::path> filesIn(const fs::path& dir, bool recursive) {
    std::vector<fs::path> out;
    if (!fs::exists(dir)) return out;
    // NutTool's .bak backups are not game files.
    auto add = [&](const fs::directory_entry& e) {
        if (e.is_regular_file() && lower(e.path().extension().string()) != ".bak") out.push_back(e.path());
    };
    if (recursive) {
        for (auto& e : fs::recursive_directory_iterator(dir)) add(e);
    } else {
        for (auto& e : fs::directory_iterator(dir)) add(e);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

int extractAll(const fs::path& root, const fs::path& out, const ExtractOptions& opt, const Progress& progress) {
    auto want = [&](const char* cat) {
        if (opt.texturesOnly) return std::string(cat) == "bundles" || std::string(cat) == "streams";
        return opt.only.empty() || opt.only.count(cat) != 0;
    };
    const std::string filter = lower(opt.filter);
    auto named = [&](const std::string& s) { return filter.empty() || lower(s).find(filter) != std::string::npos; };
    std::set<std::string> seen; // textures written, in textures-only mode
    fs::path textureDir = out / "textures";
    bool dds = !opt.texturesOnly || opt.texturesDds, png = opt.texturesOnly || opt.png;
    assetdb::Names names;
    auto entries = assetdb::readDbIndex(root / "Debug" / "db_index.txt");
    for (auto& e : entries) names.add(e.name);
    auto label = [&](uint32_t id) {
        std::string n = names.lookup(id);
        std::string t = assetdb::typeName(uint8_t(id >> 24));
        if (startsWith(n, "aid_" + t + "_")) n = n.substr(5 + t.size());
        return n.empty() ? hex32(id) : safeName(n);
    };
    int failures = 0;
    auto guarded = [&](const std::string& what, auto&& fn) {
        try { fn(); } catch (std::exception& e) { ++failures; progress.info("error: " + what + ": " + e.what()); }
    };

    // Work list, so progress can be reported as a fraction.
    std::vector<fs::path> bundles = want("bundles") ? filesIn(root / "Bundle" / "4f", false) : std::vector<fs::path>{};
    std::vector<fs::path> streams = want("streams") ? filesIn(root / "Bundle" / "50", false) : std::vector<fs::path>{};
    std::vector<std::pair<fs::path, std::string>> texts;
    if (want("text")) {
        for (auto& f : filesIn(root / "Debug" / "11", true)) texts.emplace_back(f, "english");
        if (fs::exists(root / "loctext"))
            for (auto& d : fs::directory_iterator(root / "loctext"))
                if (d.is_directory()) for (auto& f : filesIn(d.path(), true)) texts.emplace_back(f, d.path().filename().string());
    }
    std::vector<fs::path> videos = want("video") ? filesIn(root / "Debug" / "36", true) : std::vector<fs::path>{};
    std::vector<fs::path> fonts;
    if (want("fonts")) for (auto& f : filesIn(root / "xuifontcachefont", false)) if (lower(f.extension().string()) == ".sbp") fonts.push_back(f);
    std::vector<fs::path> inis = want("ini") ? filesIn(root / "xuifontcachemeta", false) : std::vector<fs::path>{};
    // Weights roughly track the time each item takes.
    double total = bundles.size() * 10.0 + streams.size() * 10.0 + texts.size() * 0.05 + videos.size() * 0.5 + fonts.size() * 20.0 + inis.size() * 0.01 + 1;
    double done = 0;
    auto advance = [&](double w, const std::string& label) { done += w; progress.at(std::min(1.0, done / total), label); };

    if (!opt.texturesOnly) {
        Json arr = Json::array();
        for (auto& e : entries) {
            if (!named(e.name)) continue;
            Json o = Json::object();
            o["id"] = "0x" + hex32(e.id);
            o["name"] = e.name;
            o["path"] = fs::relative(assetdb::pathForEntry(root, e), root).generic_string();
            arr.push(o);
        }
        writeText(out / "index.json", arr.dump());
    }

    std::set<std::string> mergedTops;
    for (size_t n = 0; n < bundles.size(); ++n) {
        if (progress.cancelled()) return failures;
        const fs::path& f = bundles[n];
        uint32_t id = assetdb::idFromPath(f).value_or(0);
        advance(10, "bundle " + std::to_string(n + 1) + "/" + std::to_string(bundles.size()) + ": " + label(id));
        guarded(f.string(), [&] {
            Loaded l = loadAsset(f);
            if (!l.caff) fail("not a CAFF bundle");
            fs::path dir = out / "bundles" / label(id);
            if (!opt.texturesOnly) {
                if (filter.empty()) {
                    unpackCaff(*l.caff, l.xctd, dir);
                } else {
                    const caff::Caff& c = *l.caff;
                    for (size_t i = 0; i < c.symbols.size(); ++i) {
                        const auto& s = c.symbols[i];
                        std::string name = assetdb::baseAssetName(c.symbolName(i));
                        if (!named(name) || !validSection(c, s) || s.size == 0 || uint64_t(s.offset) + s.size > c.sections[s.section - 1].data.size())
                            continue;
                        char prefix[16];
                        std::snprintf(prefix, sizeof prefix, "%04zu_", i + 1);
                        writeFile(dir / "assets" / (std::string(prefix) + safeName(name) + "." + sectionTag(c, s) + ".bin"), c.symbolData(i));
                    }
                }
            }
            std::vector<caff::Caff> streamCaffs;
            fs::path sbPath = root / "Bundle" / "50" / f.filename();
            if (fs::exists(sbPath)) {
                Loaded sl = loadAsset(sbPath);
                if (sl.sb) for (auto& e : sl.sb->entries) if (caff::isCaff(e.data)) streamCaffs.push_back(caff::Caff::parse(e.data));
            }
            std::vector<caff::Caff*> ptrs;
            for (auto& sc : streamCaffs) ptrs.push_back(&sc);
            exportTextures({&*l.caff}, ptrs, opt.texturesOnly ? textureDir : dir / "textures", dds, png, &mergedTops, &progress, filter,
                           opt.texturesOnly ? &seen : nullptr);
        });
    }
    for (size_t n = 0; n < streams.size(); ++n) {
        if (progress.cancelled()) return failures;
        const fs::path& f = streams[n];
        uint32_t id = assetdb::idFromPath(f).value_or(0);
        advance(10, "stream bundle " + std::to_string(n + 1) + "/" + std::to_string(streams.size()) + ": " + label(id));
        guarded(f.string(), [&] {
            Loaded l = loadAsset(f);
            if (!l.sb) fail("not a stream bundle");
            fs::path dir = out / "streambundles" / label(id);
            if (!opt.texturesOnly) {
                if (filter.empty()) {
                    unpackStreamBundle(*l.sb, dir);
                } else {
                    for (size_t i = 0; i < l.sb->entries.size(); ++i) {
                        const auto& e = l.sb->entries[i];
                        std::string name = entryName(e);
                        if (e.data.empty() || name.empty() || !named(name)) continue;
                        char prefix[16];
                        std::snprintf(prefix, sizeof prefix, "%04zu_", i + 1);
                        writeFile(dir / (std::string(prefix) + hex32(e.id) + "_" + safeName(name) + entryExt(e)), e.data);
                    }
                }
            }
            std::vector<caff::Caff> caffs;
            for (auto& e : l.sb->entries) {
                if (caff::isCaff(e.data)) {
                    caff::Caff c = caff::Caff::parse(e.data);
                    auto tex = findTextures(c);
                    bool alreadyMerged = !tex.empty() && std::all_of(tex.begin(), tex.end(), [&](const TexRef& t) { return mergedTops.count(t.name) != 0; });
                    if (!tex.empty() && !alreadyMerged) caffs.push_back(std::move(c));
                } else if (media::isWaveBank(e.data) && !opt.texturesOnly) {
                    std::string name = entryName(e);
                    if (named(name)) exportWaveBank(e.data, dir / "waves", hex32(e.id) + "_" + name);
                }
            }
            std::vector<caff::Caff*> ptrs;
            for (auto& c : caffs) ptrs.push_back(&c);
            if (!ptrs.empty())
                exportTextures(ptrs, {}, opt.texturesOnly ? textureDir : dir / "textures", dds, png, nullptr, &progress, filter,
                               opt.texturesOnly ? &seen : nullptr);
        });
    }
    for (auto& [f, lang] : texts) {
        if (progress.cancelled()) return failures;
        advance(0.05, "text: " + lang);
        auto id = assetdb::idFromPath(f);
        if (!id || (*id >> 24) != assetdb::kTypeLoctext || !named(label(*id))) continue;
        guarded(f.string(), [&] {
            Loaded l = loadAsset(f);
            if (!l.caff) fail("not a CAFF file");
            Json j = loctext::toJson(loctext::read(*l.caff));
            j["asset"] = assetdb::baseAssetName(l.caff->symbolName(0));
            writeText(out / "text" / lang / (label(*id) + ".json"), j.dump());
        });
    }
    for (auto& f : videos) {
        if (progress.cancelled()) return failures;
        advance(0.5, "video");
        auto id = assetdb::idFromPath(f);
        if (!id || !named(label(*id))) continue;
        guarded(f.string(), [&] {
            fs::path dst = longPath(out / "video" / (label(*id) + ".wmv"));
            fs::create_directories(dst.parent_path());
            fs::copy_file(longPath(f), dst, fs::copy_options::overwrite_existing);
        });
    }
    for (auto& f : fonts) {
        if (progress.cancelled()) return failures;
        advance(20, "font: " + f.filename().string());
        if (!named(f.stem().string())) continue;
        guarded(f.string(), [&] { font::exportTo(font::Font::parse(readFile(f)), out / "fonts" / f.stem()); });
    }
    for (auto& f : inis) {
        advance(0.01, "font metadata");
        if (!named(f.stem().string())) continue;
        guarded(f.string(), [&] {
            Bytes d = readFile(f);
            bool utf16 = d.size() >= 2 && d[0] == 0xFE && d[1] == 0xFF;
            writeFile(out / "fontmeta" / (f.stem().string() + (utf16 ? ".utf8.txt" : ".ini")), utf16 ? precacheToUtf8(d) : d);
        });
    }
    if (want("misc") && named("tile.bmp") && fs::exists(root / "tile.bmp")) {
        guarded("tile.bmp", [&] {
            fs::create_directories(longPath(out / "misc"));
            fs::copy_file(longPath(root / "tile.bmp"), longPath(out / "misc" / "tile.bmp"), fs::copy_options::overwrite_existing);
        });
    }
    progress.at(1.0, "done");
    return failures;
}

VerifyResult verifyAll(const fs::path& root, const Progress& progress) {
    VerifyResult r;
    auto report = [&](const std::string& what, bool good, const std::string& why = "") {
        if (good) ++r.passed;
        else {
            ++r.failed;
            progress.info("FAIL " + what + " " + why);
        }
    };
    auto checkCaff = [&](const Bytes& d, const std::string& what) {
        try {
            if (caff::headerChecksum(d.data()) != be32(&d[0x18])) return report(what, false, "header checksum");
            caff::Caff c = caff::Caff::parse(d);
            if (c.serialize() != d) return report(what, false, "CAFF re-serialisation differs");
            if (loctext::isLoctext(c)) {
                caff::Caff c2 = c;
                loctext::write(c2, loctext::read(c));
                if (c2.serialize() != d) return report(what, false, "loctext rebuild differs");
            }
            // Game scripts and objparams must survive the trip through their editable text unchanged.
            for (size_t i = 0; i < c.symbols.size(); ++i) {
                std::string name = assetdb::baseAssetName(c.symbolName(i));
                auto kind = gamescript::kindOf(name);
                bool params = startsWith(name, "aid_objparams_");
                if ((!kind && !params) || !validSection(c, c.symbols[i]) || c.sections[c.symbols[i].section - 1].name != ".data") continue;
                Bytes data = c.symbolData(i);
                try {
                    if (kind && gamescript::fromText(*kind, gamescript::toText(*kind, data)) != data)
                        return report(what, false, name + ": script text round trip differs");
                    if (params && objparams::isObjParams(data) && objparams::fromText(data, objparams::toText(data)) != data)
                        return report(what, false, name + ": objparams text round trip differs");
                } catch (std::exception& e) { return report(what, false, name + ": " + e.what()); }
            }
            report(what, true);
        } catch (std::exception& e) { report(what, false, e.what()); }
    };
    std::vector<fs::path> files = filesIn(root, true);
    for (size_t i = 0; i < files.size(); ++i) {
        if (progress.cancelled()) break;
        const fs::path& p = files[i];
        std::string s = p.generic_string();
        std::string ext = lower(p.extension().string());
        if (ext == ".xex" || ext == ".xexp" || ext == ".bak") continue;
        progress.at(double(i) / files.size(), std::to_string(r.passed + r.failed) + " checked");
        try {
            Bytes raw = readFile(p);
            Bytes d = xctd::isXctd(raw) ? xctd::decompress(raw) : raw;
            FileKind k = detect(d, p);
            if (k == FileKind::Caff) checkCaff(d, s);
            else if (k == FileKind::StreamBundle) {
                auto sb = streambundle::StreamBundle::parse(d);
                report(s, sb.serialize() == d, "stream bundle re-serialisation differs");
                for (auto& en : sb.entries) {
                    if (caff::isCaff(en.data)) checkCaff(en.data, s + ":" + hex32(en.id));
                    else if (media::isWaveBank(en.data)) {
                        try { media::parseWaveBank(en.data); report(s + ":" + hex32(en.id), true); } catch (std::exception& ex) { report(s + ":" + hex32(en.id), false, ex.what()); }
                    }
                }
            } else if (k == FileKind::Font) {
                auto f = font::Font::parse(d);
                report(s, f.serialize() == d, "font re-serialisation differs");
            }
        } catch (std::exception& ex) { report(s, false, ex.what()); }
    }
    progress.at(1.0, "done");
    return r;
}

} // namespace nut::ops
