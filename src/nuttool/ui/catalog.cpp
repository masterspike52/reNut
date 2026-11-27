#include "catalog.hpp"

#include "app.hpp"
#include "assetdb.hpp"
#include "container.hpp"
#include "json.hpp"
#include "media.hpp"
#include "model.hpp"
#include "objparams.hpp"
#include "streambundle.hpp"
#include "texture.hpp"
#include "xctd.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <thread>

namespace nut::gui {
namespace {

constexpr int kCacheVersion = 7;

struct Found {
    struct Tex {
        std::string name;
        AssetLoc loc;
        uint32_t w, h, mips;
        std::string format;
    };
    std::vector<Tex> textures;
    std::vector<std::pair<std::string, AssetLoc>> models;
    std::vector<std::pair<std::string, AssetLoc>> anims;
    std::vector<uint32_t> uiIds;
    std::vector<AudioBankInfo> audio;
    bool menus = false;
    std::vector<std::pair<std::string, AssetLoc>> scripts;    // game script .data symbols
    std::vector<std::pair<std::string, AssetLoc>> parts;      // vehicle part objparams
    std::vector<std::pair<uint32_t, std::string>> scriptRefs; // {word that may be a script id, asset holding it}
    std::vector<std::string> names;                           // every asset name in the file
};

// Words carrying a game script type byte: "used by" candidates, kept once every script is known.
void scanScriptRefs(const Bytes& d, const std::string& user, Found& out) {
    static const std::vector<bool> scriptType = [] {
        std::vector<bool> t(256, false);
        for (const char* n : {"script", "actorgoals", "scripttable", "statetable"})
            if (auto x = assetdb::typeIndex(n)) t[*x] = true;
        return t;
    }();
    std::set<uint32_t> seen;
    for (size_t k = 0; k + 4 <= d.size(); k += 4) {
        uint32_t v = be32(&d[k]);
        if (scriptType[v >> 24] && seen.insert(v).second) out.scriptRefs.emplace_back(v, user);
    }
}

// A wave bank's tables (`d` may stop after them: the wave data is not needed).
void scanWaveBank(const Bytes& d, const fs::path& file, int entry, uint32_t id, Found& out) {
    media::WaveBank wb = media::parseWaveBank(d, false);
    AudioBankInfo b;
    b.name = wb.name.empty() ? "wavebank_" + hex32(id) : wb.name;
    b.locs.push_back({file, entry, 0});
    for (auto& w : wb.waves)
        b.waves.push_back({w.name, uint8_t(w.formatTag), uint8_t(w.channels), w.sampleRate, w.durationSamples, w.loopLength != 0});
    out.audio.push_back(std::move(b));
}

void scanCaff(caff::Caff& c, const fs::path& file, int entry, Found& out) {
    for (auto& t : ops::findTextures(c)) {
        try {
            texture::Header h = texture::parseHeader(c.symbolData(t.header));
            out.textures.push_back({t.name, {file, entry, uint32_t(t.header)}, h.width, h.height, h.mipLevels, texture::describeFormat(h.d3dFormat).name});
        } catch (...) {
        }
    }
    for (size_t i = 0; i < c.symbols.size(); ++i) {
        if (model::isModelSymbol(c, i)) out.models.emplace_back(assetdb::baseAssetName(c.symbolName(i)), AssetLoc{file, entry, uint32_t(i)});
        else if (model::isAnimationSymbol(c, i)) out.anims.emplace_back(assetdb::baseAssetName(c.symbolName(i)), AssetLoc{file, entry, uint32_t(i)});
        std::string n = c.symbolName(i);
        std::string base = assetdb::baseAssetName(n);
        if (startsWith(base, "aid_")) {
            out.names.push_back(base);
            if (ops::validSection(c, c.symbols[i]) && c.sections[c.symbols[i].section - 1].name == ".data") {
                try {
                    Bytes d = c.symbolData(i);
                    if (gamescript::kindOf(base)) out.scripts.emplace_back(base, AssetLoc{file, entry, uint32_t(i)});
                    else if (startsWith(base, "aid_objparams_") && objparams::isVehiclePart(d)) out.parts.emplace_back(base, AssetLoc{file, entry, uint32_t(i)});
                    scanScriptRefs(d, base, out);
                } catch (...) {
                }
            }
        }
        if (entry < 0 && startsWith(n, "aid_xuipackage_")) out.menus = true;
        if (startsWith(n, "aid_xuicachefile_")) {
            // Dependency list of {u32 1, u32 12, u32 asset id} records.
            try {
                Bytes d = c.symbolData(i);
                for (size_t k = 0; k + 12 <= d.size(); k += 12)
                    if (d[k + 8] == assetdb::kTypeTexture) out.uiIds.push_back(be32(&d[k + 8]));
            } catch (...) {
            }
        }
    }
}

// Stream bundles are scanned entry by entry, reading only each CAFF's tables and .data section:
// the texture data that makes up most of their size is never read.
bool scanStreamBundle(const fs::path& f, Found& out) {
    std::vector<streambundle::IndexEntry> index;
    if (!streambundle::readIndex(f, index)) return false;
    std::ifstream in(longPath(readPath(f)), std::ios::binary);
    if (!in) return false;
    auto read = [&](const streambundle::IndexEntry& e, size_t len) {
        Bytes b(std::min<size_t>(len, e.size));
        in.clear();
        in.seekg(std::streamoff(e.offset));
        in.read(reinterpret_cast<char*>(b.data()), std::streamsize(b.size()));
        b.resize(size_t(in.gcount()));
        return b;
    };
    const std::vector<std::string> keep = {".data"};
    for (size_t e = 0; e < index.size(); ++e) {
        if (index[e].size < 4) continue;
        try {
            Bytes head = read(index[e], 64 * 1024);
            if (media::isWaveBank(head)) {
                size_t need = media::waveBankHeaderBytes(head);
                if (need > head.size() && need <= index[e].size) head = read(index[e], need);
                scanWaveBank(head, f, int(e), index[e].id, out);
                continue;
            }
            if (!caff::isCaff(head)) continue;
            size_t need = 0;
            std::optional<caff::Caff> c = caff::Caff::parsePartial(head, keep, need);
            for (int tries = 0; !c && need > head.size() && need <= index[e].size && tries < 3; ++tries) {
                head = read(index[e], need);
                c = caff::Caff::parsePartial(head, keep, need);
            }
            if (c) scanCaff(*c, f, int(e), out);
        } catch (...) {
        }
    }
    return true;
}

// Bundles keep .data first: decompress only the XCTD blocks up to its end, skipping the GPU data.
bool scanBundle(const fs::path& f, Found& out) {
    std::ifstream in(longPath(readPath(f)), std::ios::binary);
    if (!in) return false;
    size_t fileSize = size_t(fs::file_size(longPath(readPath(f))));
    Bytes raw;
    auto readTo = [&](size_t n) {
        n = std::min(n, fileSize);
        if (n <= raw.size()) return;
        size_t old = raw.size();
        raw.resize(n);
        in.clear();
        in.seekg(std::streamoff(old));
        in.read(reinterpret_cast<char*>(raw.data() + old), std::streamsize(n - old));
        raw.resize(old + size_t(in.gcount()));
    };
    readTo(256 * 1024);
    bool compressed = xctd::isXctd(raw);
    xctd::Info info;
    if (compressed) info = xctd::parse(raw);
    else if (!caff::isCaff(raw)) return false;
    const std::vector<std::string> keep = {".data"};
    size_t want = 256 * 1024;
    for (int tries = 0; tries < 4; ++tries) {
        Bytes plain;
        if (compressed) {
            readTo(xctd::compressedBytesFor(info, want, fileSize));
            plain = xctd::decompressPrefix(raw, want);
        } else {
            readTo(want);
            plain = raw;
        }
        size_t need = 0;
        std::optional<caff::Caff> c = caff::Caff::parsePartial(plain, keep, need);
        if (c) {
            scanCaff(*c, f, -1, out);
            return true;
        }
        if (need <= plain.size()) return false;
        want = need;
    }
    return false;
}

Found scanFile(const fs::path& f) {
    Found out;
    try {
        if (scanStreamBundle(f, out) || scanBundle(f, out)) return out;
    } catch (...) {
        out = Found{};
    }
    Loaded l = loadAsset(f);
    if (l.caff) scanCaff(*l.caff, f, -1, out);
    else if (l.sb) {
        for (size_t e = 0; e < l.sb->entries.size(); ++e) {
            const Bytes& d = l.sb->entries[e].data;
            if (media::isWaveBank(d)) {
                try {
                    scanWaveBank(d, f, int(e), l.sb->entries[e].id, out);
                } catch (...) {
                }
                continue;
            }
            if (!caff::isCaff(d)) continue;
            try {
                caff::Caff c = caff::Caff::parse(d);
                scanCaff(c, f, int(e), out);
            } catch (...) {
            }
        }
    }
    return out;
}

Json locJson(const AssetLoc& l, const fs::path& root) {
    Json o = Json::array();
    o.push(toUtf8Generic(fs::relative(l.file, root)));
    o.push(l.entry);
    o.push(l.symbol);
    return o;
}

} // namespace

const TextureInfo* Catalog::texture(const std::string& name) const {
    auto it = textureIndex.find(name);
    return it == textureIndex.end() ? nullptr : &textures[it->second];
}

const ModelInfo* Catalog::model(const std::string& name) const {
    auto it = modelIndex.find(name);
    return it == modelIndex.end() ? nullptr : &models[it->second];
}

const AudioBankInfo* Catalog::audioBank(const std::string& name) const {
    auto it = audioIndex.find(name);
    return it == audioIndex.end() ? nullptr : &audio[it->second];
}

double AudioBankInfo::seconds() const {
    double s = 0;
    for (auto& w : waves) s += w.seconds();
    return s;
}

const ModelInfo* Catalog::anim(const std::string& name) const {
    auto it = animIndex.find(name);
    return it == animIndex.end() ? nullptr : &anims[it->second];
}

const ScriptInfo* Catalog::script(const std::string& name) const {
    auto it = scriptIndex.find(name);
    return it == scriptIndex.end() ? nullptr : &scripts[it->second];
}

const ScriptInfo* Catalog::part(const std::string& name) const {
    auto it = partIndex.find(name);
    return it == partIndex.end() ? nullptr : &parts[it->second];
}

std::string Catalog::nameOf(uint32_t id) const {
    auto it = names.find(id);
    return it == names.end() ? std::string() : it->second;
}

namespace {

// Sorts each list by name and makes its index again.
void reindex(Catalog& c) {
    auto sortIndex = [](auto& list, std::map<std::string, size_t>& index) {
        std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.name < b.name; });
        index.clear();
        for (size_t i = 0; i < list.size(); ++i) index[list[i].name] = i;
    };
    sortIndex(c.textures, c.textureIndex);
    sortIndex(c.models, c.modelIndex);
    sortIndex(c.anims, c.animIndex);
    sortIndex(c.audio, c.audioIndex);
    sortIndex(c.scripts, c.scriptIndex);
    sortIndex(c.parts, c.partIndex);
}

} // namespace

std::shared_ptr<Catalog> Catalog::overlay(const Catalog& base, const Catalog& over) {
    auto cat = std::make_shared<Catalog>(base);
    auto put = [](auto& list, const std::map<std::string, size_t>& index, const auto& items) {
        for (auto& x : items) {
            auto it = index.find(x.name);
            if (it != index.end()) list[it->second] = x;
            else list.push_back(x);
        }
    };
    put(cat->textures, cat->textureIndex, over.textures);
    put(cat->models, cat->modelIndex, over.models);
    put(cat->anims, cat->animIndex, over.anims);
    put(cat->audio, cat->audioIndex, over.audio);
    put(cat->scripts, cat->scriptIndex, over.scripts);
    put(cat->parts, cat->partIndex, over.parts);
    cat->menuBundles.insert(cat->menuBundles.end(), over.menuBundles.begin(), over.menuBundles.end());
    for (auto& [id, name] : over.names) cat->names[id] = name;
    reindex(*cat);
    return cat;
}

std::shared_ptr<Catalog> Catalog::build(const fs::path& root, const fs::path& cacheFile, const ops::Progress& progress, const fs::path& updateDir) {
    std::vector<fs::path> files;
    for (const char* sub : {"4f", "50"}) {
        fs::path d = root / "Bundle" / sub;
        if (!fs::exists(d)) continue;
        for (auto& e : fs::directory_iterator(d))
            if (e.is_regular_file() && e.path().extension() != ".bak") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (auto& f : ops::updateBundles(updateDir)) files.push_back(f);
    // Signature of the scanned files, to validate the cache.
    std::string signature;
    for (auto& f : files) {
        signature += toUtf8Generic(fs::relative(f, root)) + ":" + std::to_string(fs::file_size(f)) + ":" +
                     std::to_string(fs::last_write_time(f).time_since_epoch().count()) + ";";
    }
    std::string sigHash = hex32(crc32(signature.data(), signature.size()));

    auto cat = std::make_shared<Catalog>();
    auto finish = [&] {
        reindex(*cat);
        return cat;
    };
    auto readLoc = [&](const Json& j) { return AssetLoc{root / fromUtf8Generic(j[size_t(0)].asString()), int(j[size_t(1)].asNumber()), j[size_t(2)].asU32()}; };

    // Cached result?
    try {
        if (!cacheFile.empty() && fs::exists(cacheFile)) {
            Json j = Json::parse(readText(cacheFile));
            if (j["version"].asNumber() == kCacheVersion && j["signature"].asString() == sigHash) {
                for (auto& t : j["textures"].items()) {
                    TextureInfo ti;
                    ti.name = t["n"].asString();
                    for (auto& l : t["f"].items()) ti.full.push_back(readLoc(l));
                    for (auto& l : t["t"].items()) ti.tops.push_back(readLoc(l));
                    ti.width = t["w"].asU32();
                    ti.height = t["h"].asU32();
                    ti.mips = t["m"].asU32();
                    ti.format = t["fmt"].asString();
                    ti.ui = t["ui"].asBool();
                    cat->textures.push_back(std::move(ti));
                }
                for (const char* key : {"models", "anims"}) {
                    for (auto& m : j[key].items()) {
                        ModelInfo mi;
                        mi.name = m["n"].asString();
                        for (auto& l : m["l"].items()) mi.locs.push_back(readLoc(l));
                        (std::string(key) == "models" ? cat->models : cat->anims).push_back(std::move(mi));
                    }
                }
                for (auto& a : j["audio"].items()) {
                    AudioBankInfo b;
                    b.name = a["n"].asString();
                    for (auto& l : a["l"].items()) b.locs.push_back(readLoc(l));
                    for (const Json& w : a["w"].items())
                        b.waves.push_back({w[size_t(0)].asString(), uint8_t(w[size_t(1)].asU32()), uint8_t(w[size_t(2)].asU32()), w[size_t(3)].asU32(),
                                           w[size_t(4)].asU32(), w[size_t(5)].asBool()});
                    cat->audio.push_back(std::move(b));
                }
                for (auto& m : j["menus"].items()) cat->menuBundles.push_back(root / fromUtf8Generic(m.asString()));
                for (auto& s : j["scripts"].items()) {
                    ScriptInfo si;
                    si.name = s["n"].asString();
                    auto kind = gamescript::kindOf(si.name);
                    if (!kind) continue;
                    si.kind = *kind;
                    for (auto& l : s["l"].items()) si.locs.push_back(readLoc(l));
                    for (auto& u : s["u"].items()) si.usedBy.push_back(u.asString());
                    cat->scripts.push_back(std::move(si));
                }
                for (auto& n : j["names"].items())
                    if (auto id = assetdb::idFromName(n.asString())) cat->names.emplace(*id, n.asString());
                for (auto& p : j["parts"].items()) {
                    ScriptInfo si;
                    si.name = p["n"].asString();
                    si.objParams = true;
                    for (auto& l : p["l"].items()) si.locs.push_back(readLoc(l));
                    cat->parts.push_back(std::move(si));
                }
                progress.at(1.0, "loaded from cache");
                return finish();
            }
        }
    } catch (...) {
        cat = std::make_shared<Catalog>();
    }

    // Scan with a few worker threads (LZX decompression dominates).
    std::vector<Found> results(files.size());
    std::atomic<size_t> next{0}, done{0};
    unsigned nthreads = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nthreads; ++t)
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < files.size();) {
                if (progress.cancelled()) return;
                try {
                    results[i] = scanFile(files[i]);
                } catch (std::exception& e) {
                    progress.info("index: " + toUtf8Generic(files[i]) + ": " + e.what());
                }
                size_t d = ++done;
                progress.at(double(d) / double(files.size()), "indexing " + std::to_string(d) + "/" + std::to_string(files.size()));
            }
        });
    for (auto& t : pool) t.join();
    if (progress.cancelled()) return nullptr;

    std::set<uint32_t> uiIds;
    for (auto& r : results) uiIds.insert(r.uiIds.begin(), r.uiIds.end());
    std::map<std::string, TextureInfo> texMap;
    std::map<std::string, ModelInfo> modelMap, animMap;
    std::map<std::string, AudioBankInfo> audioMap;
    for (size_t i = 0; i < results.size(); ++i)
        if (results[i].menus) cat->menuBundles.push_back(files[i]);
    for (auto& r : results) {
        for (auto& t : r.textures) {
            bool top = endsWith(t.name, "top") && ops::fullName(t.name) != t.name;
            std::string merged = ops::fullName(t.name);
            TextureInfo& ti = texMap[merged];
            ti.name = merged;
            (top ? ti.tops : ti.full).push_back(t.loc);
            // Dimensions/format describe the full texture; the top half has the same size.
            if (ti.width == 0 || top) {
                ti.width = t.w;
                ti.height = t.h;
                ti.format = t.format;
            }
            ti.mips = std::max(ti.mips, t.mips);
            auto id = assetdb::idFromName(t.name);
            if ((id && uiIds.count(*id)) || t.name.find("_ui_") != std::string::npos) ti.ui = true;
        }
        for (auto& [n, loc] : r.models) {
            ModelInfo& mi = modelMap[n];
            mi.name = n;
            mi.locs.push_back(loc);
        }
        for (auto& [n, loc] : r.anims) {
            ModelInfo& ai = animMap[n];
            ai.name = n;
            ai.locs.push_back(loc);
        }
        for (auto& b : r.audio) {
            AudioBankInfo& ab = audioMap[b.name];
            if (ab.name.empty()) ab = b;
            else ab.locs.insert(ab.locs.end(), b.locs.begin(), b.locs.end());
        }
    }
    for (auto& [n, t] : texMap) cat->textures.push_back(std::move(t));
    for (auto& [n, m] : modelMap) cat->models.push_back(std::move(m));
    for (auto& [n, a] : animMap) cat->anims.push_back(std::move(a));
    for (auto& [n, a] : audioMap) cat->audio.push_back(std::move(a));

    // Game scripts, and which assets point at them.
    std::map<std::string, ScriptInfo> scriptMap;
    std::map<uint32_t, std::set<std::string>> refs;
    for (auto& r : results) {
        for (auto& [n, loc] : r.scripts) {
            ScriptInfo& s = scriptMap[n];
            s.name = n;
            s.kind = *gamescript::kindOf(n);
            s.locs.push_back(loc);
        }
        for (auto& [id, user] : r.scriptRefs) refs[id].insert(user);
        for (auto& n : r.names)
            if (auto id = assetdb::idFromName(n)) cat->names.emplace(*id, n);
    }
    for (const fs::path& idx : {root / "Debug" / "db_index.txt", updateDir.empty() ? fs::path() : updateDir / "db_index.txt"}) {
        try {
            if (!idx.empty() && fs::exists(idx))
                for (auto& e : assetdb::readDbIndex(idx))
                    if (auto id = assetdb::idFromName(e.name)) cat->names.emplace(*id, assetdb::baseAssetName(e.name));
        } catch (...) {
        }
    }
    std::map<std::string, ScriptInfo> partMap;
    for (auto& r : results)
        for (auto& [n, loc] : r.parts) {
            ScriptInfo& p = partMap[n];
            p.name = n;
            p.objParams = true;
            p.locs.push_back(loc);
        }
    for (auto& [n, p] : partMap) cat->parts.push_back(std::move(p));
    for (auto& [n, s] : scriptMap) {
        auto id = assetdb::idFromName(n);
        auto it = id ? refs.find(*id) : refs.end();
        if (it != refs.end())
            for (auto& u : it->second)
                if (u != n) s.usedBy.push_back(u);
        cat->scripts.push_back(std::move(s));
    }

    // Save the cache.
    try {
        Json j = Json::object();
        j["version"] = kCacheVersion;
        j["signature"] = sigHash;
        Json texs = Json::array();
        for (auto& t : cat->textures) {
            Json o = Json::object();
            o["n"] = t.name;
            Json f = Json::array(), tp = Json::array();
            for (auto& l : t.full) f.push(locJson(l, root));
            for (auto& l : t.tops) tp.push(locJson(l, root));
            o["f"] = f;
            o["t"] = tp;
            o["w"] = t.width;
            o["h"] = t.height;
            o["m"] = t.mips;
            o["fmt"] = t.format;
            o["ui"] = t.ui;
            texs.push(o);
        }
        j["textures"] = texs;
        auto named = [&](const std::vector<ModelInfo>& list) {
            Json arr = Json::array();
            for (auto& m : list) {
                Json o = Json::object();
                o["n"] = m.name;
                Json l = Json::array();
                for (auto& x : m.locs) l.push(locJson(x, root));
                o["l"] = l;
                arr.push(o);
            }
            return arr;
        };
        j["models"] = named(cat->models);
        j["anims"] = named(cat->anims);
        Json audio = Json::array();
        for (auto& b : cat->audio) {
            Json o = Json::object();
            o["n"] = b.name;
            Json l = Json::array(), w = Json::array();
            for (auto& x : b.locs) l.push(locJson(x, root));
            for (auto& x : b.waves) {
                Json e = Json::array();
                e.push(x.name);
                e.push(uint32_t(x.format));
                e.push(uint32_t(x.channels));
                e.push(x.rate);
                e.push(x.samples);
                e.push(x.loops);
                w.push(e);
            }
            o["l"] = l;
            o["w"] = w;
            audio.push(o);
        }
        j["audio"] = audio;
        Json menus = Json::array();
        for (auto& f : cat->menuBundles) menus.push(toUtf8Generic(fs::relative(f, root)));
        j["menus"] = menus;
        Json scripts = Json::array();
        for (auto& s : cat->scripts) {
            Json o = Json::object();
            o["n"] = s.name;
            Json l = Json::array(), u = Json::array();
            for (auto& x : s.locs) l.push(locJson(x, root));
            for (auto& x : s.usedBy) u.push(x);
            o["l"] = l;
            o["u"] = u;
            scripts.push(o);
        }
        j["scripts"] = scripts;
        Json parts = Json::array();
        for (auto& p : cat->parts) {
            Json o = Json::object();
            o["n"] = p.name;
            Json l = Json::array();
            for (auto& x : p.locs) l.push(locJson(x, root));
            o["l"] = l;
            parts.push(o);
        }
        j["parts"] = parts;
        Json names = Json::array();
        for (auto& [id, n] : cat->names) names.push(n);
        j["names"] = names;
        if (!cacheFile.empty()) writeText(cacheFile, j.dump(0));
    } catch (std::exception& e) {
        progress.info(std::string("could not save the index cache: ") + e.what());
    }
    return finish();
}

caff::Caff& CaffCache::get(const AssetLoc& loc) {
    auto it = items_.find(loc.file);
    if (it == items_.end()) {
        // Keep a handful of decompressed bundles around; stream bundles only keep their entry
        // table and read single entries (some are close to a gigabyte).
        if (order_.size() >= 6) {
            items_.erase(order_.front());
            order_.erase(order_.begin());
        }
        Item item;
        if (loc.entry >= 0 && streambundle::readIndex(loc.file, item.index)) {
            item.indexed = true;
        } else {
            Loaded l = loadAsset(loc.file);
            if (l.caff) item.bundle = std::make_unique<caff::Caff>(std::move(*l.caff));
            else if (l.sb) item.sb = std::make_unique<streambundle::StreamBundle>(std::move(*l.sb));
            else fail(toUtf8Generic(loc.file) + " is not a bundle");
        }
        it = items_.emplace(loc.file, std::move(item)).first;
        order_.push_back(loc.file);
    }
    Item& item = it->second;
    if (loc.entry < 0) {
        if (!item.bundle) fail("expected a bundle: " + toUtf8Generic(loc.file));
        return *item.bundle;
    }
    auto& e = item.entries[loc.entry];
    if (!e) {
        if (item.indexed) {
            if (size_t(loc.entry) >= item.index.size()) fail("bad stream bundle entry");
            e = std::make_unique<caff::Caff>(caff::Caff::parse(streambundle::readEntry(loc.file, item.index[size_t(loc.entry)])));
        } else {
            if (!item.sb || size_t(loc.entry) >= item.sb->entries.size()) fail("bad stream bundle entry");
            e = std::make_unique<caff::Caff>(caff::Caff::parse(item.sb->entries[size_t(loc.entry)].data));
        }
    }
    return *e;
}

ops::DecodedTexture decodeCatalogTexture(const TextureInfo& t, CaffCache& cache) {
    const AssetLoc& main = !t.full.empty() ? t.full.front() : t.tops.front();
    caff::Caff& c = cache.get(main);
    auto ref = ops::textureForSymbol(c, main.symbol);
    if (!ref) fail("texture not found in " + toUtf8Generic(main.file));
    // The streamed top half normally lives in the stream bundle with the same file name.
    std::vector<caff::Caff*> streams;
    const AssetLoc* top = nullptr;
    for (auto& tp : t.tops)
        if (tp.file.filename() == main.file.filename()) top = &tp;
    if (!top && !t.tops.empty()) top = &t.tops.front();
    if (top && top != &main) streams.push_back(&cache.get(*top));
    return ops::decodeTexture(*ref, streams);
}

} // namespace nut::gui
