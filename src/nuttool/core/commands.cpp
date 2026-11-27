#include "commands.hpp"
#include "script.hpp"

#include "actorswap.hpp"
#include "animkeys.hpp"
#include "assetdb.hpp"
#include "blockmodel.hpp"
#include "container.hpp"
#include "audio.hpp"
#include "fbx.hpp"
#include "font.hpp"
#include "gamescript.hpp"
#include "gltfobj.hpp"
#include "texture.hpp"
#include "json.hpp"
#include "loctext.hpp"
#include "media.hpp"
#include "model.hpp"
#include "modelimport.hpp"
#include "nbpatch.hpp"
#include "objparams.hpp"
#include "renutchars.hpp"
#include "renutmake.hpp"
#include "renutworld.hpp"
#include "renutmodel.hpp"
#include "renutpart.hpp"
#include "renutvehicle.hpp"
#include "scriptgraph.hpp"
#include "streambundle.hpp"
#include "ops.hpp"
#include "xctd.hpp"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <functional>
#include <cstdio>
#include <memory>
#include <optional>

namespace nut::cli {

namespace {
thread_local const Sink* sink = nullptr;
} // namespace

void print(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::string s;
    if (n >= int(sizeof buf)) {
        s.resize(size_t(n) + 1);
        va_start(ap, fmt);
        std::vsnprintf(s.data(), s.size(), fmt, ap);
        va_end(ap);
        s.resize(size_t(n));
    } else if (n > 0) {
        s.assign(buf, size_t(n));
    }
    if (sink) (*sink)(s);
    else std::fwrite(s.data(), 1, s.size(), stdout);
}

SinkScope::SinkScope(const Sink& s) : prev_(sink) { sink = &s; }
SinkScope::~SinkScope() { sink = prev_; }

using ops::safeName;
using ops::validSection;

// ---- argument helpers ------------------------------------------------------

const std::string& Args::at(size_t i, const char* what) const {
    if (i >= pos.size()) fail(std::string("missing argument: ") + what);
    return pos[i];
}

std::string Args::opt(const std::string& k, const std::string& def) const {
    auto it = opts.find(k);
    return it == opts.end() ? def : it->second;
}

namespace {

caff::Caff& requireCaff(Loaded& l, const fs::path& p) {
    if (!l.caff) fail(p.string() + " is not a CAFF/bundle file (" + kindName(l.kind) + ")");
    return *l.caff;
}

void printCaffSummary(const caff::Caff& c) {
    print("sections:\n");
    for (auto& s : c.sections) print("  %-12s %10zu bytes (align %u)\n", s.name.c_str(), s.data.size(), 1u << s.alignLog2);
    size_t relocs = 0;
    for (int k = 0; k < 2; ++k) for (auto& r : c.relocs[k]) relocs += r.offsets.size();
    print("symbols: %zu, names: %zu, relocations: %zu, string pools: %zu\n", c.symbols.size(), c.symbolNames.size(), relocs, c.pools.size());
}

ops::Progress consoleProgress() {
    ops::Progress p;
    p.log = [](const std::string& s) { print("  %s\n", s.c_str()); };
    auto last = std::make_shared<std::string>();
    p.step = [last](double, const std::string& label) {
        if (label == *last) return;
        *last = label;
        print("[%s]\n", label.c_str());
        std::fflush(stdout);
    };
    return p;
}

} // namespace

// ---- commands --------------------------------------------------------------

int info(const Args& a) {
    fs::path p = a.at(0, "file");
    Loaded l = loadAsset(p);
    print("%s: %s%s, %zu bytes\n", p.string().c_str(), kindName(l.kind), l.xctd ? " (XCTD-compressed)" : "", l.raw.size());
    if (auto id = assetdb::idFromPath(p))
        print("asset id %s (type %s)\n", hex32(*id).c_str(), assetdb::typeName(uint8_t(*id >> 24)).c_str());
    switch (l.kind) {
        case FileKind::Caff: {
            auto& c = *l.caff;
            printCaffSummary(c);
            if (loctext::isLoctext(c)) print("content: loctext with %zu strings\n", loctext::read(c).entries.size());
            size_t tex = ops::findTextures(c).size();
            if (tex) print("textures: %zu\n", tex);
            for (size_t i = 0; i < c.symbols.size() && i < 8; ++i) print("  #%zu %s\n", i + 1, c.symbolName(i).c_str());
            if (c.symbols.size() > 8) print("  ... (use 'list' for all)\n");
            break;
        }
        case FileKind::StreamBundle:
            print("entries: %zu, dependencies: %zu, version %.2f\n", l.sb->entries.size(), l.sb->dependencies.size(), l.sb->version);
            break;
        case FileKind::Font: {
            auto f = font::Font::parse(l.raw);
            print("glyphs: %zu, cell %ux%u, line height %u, %u bpp\n", f.glyphs.size(), f.cellSize, f.cellSize, f.header[2], f.header[3]);
            break;
        }
        case FileKind::Asf: print("%s", media::describeAsf(l.raw).c_str()); break;
        case FileKind::WaveBank: {
            auto wb = media::parseWaveBank(l.raw);
            print("wave bank '%s' v%u, %zu waves\n", wb.name.c_str(), wb.version, wb.waves.size());
            break;
        }
        case FileKind::DbIndex: print("entries: %zu\n", assetdb::readDbIndex(p).size()); break;
        default: break;
    }
    return 0;
}

int hash(const Args& a) {
    if (a.pos.empty()) fail("usage: hash <asset name>...");
    for (auto& n : a.pos) {
        auto id = assetdb::idFromName(n);
        if (!id) { print("%s: unknown asset type\n", n.c_str()); continue; }
        print("%s  %s  %s\n", hex32(*id).c_str(), n.c_str(), assetdb::pathForId("", *id).generic_string().c_str());
    }
    return 0;
}

int index(const Args& a) {
    fs::path root = a.at(0, "assets dir");
    auto entries = assetdb::readDbIndex(root / "Debug" / "db_index.txt");
    Json arr = Json::array();
    for (auto& e : entries) {
        fs::path p = assetdb::pathForEntry(root, e);
        bool exists = fs::exists(p);
        if (a.flag("--json")) {
            Json o = Json::object();
            o["id"] = "0x" + hex32(e.id);
            o["name"] = e.name;
            o["path"] = fs::relative(p, root).generic_string();
            o["exists"] = exists;
            o["timestamp"] = e.timestamp;
            o["version"] = e.version;
            arr.push(o);
        } else {
            print("%s  %-60s %s%s\n", hex32(e.id).c_str(), e.name.c_str(), fs::relative(p, root).generic_string().c_str(), exists ? "" : "  (missing)");
        }
    }
    if (a.flag("--json")) print("%s", arr.dump().c_str());
    return 0;
}

int list(const Args& a) {
    fs::path p = a.at(0, "file");
    Loaded l = loadAsset(p);
    if (l.caff) {
        auto& c = *l.caff;
        print("%5s %-11s %10s %10s %4s  %s\n", "#", "section", "offset", "size", "kind", "name");
        for (size_t i = 0; i < c.symbols.size(); ++i) {
            const auto& s = c.symbols[i];
            print("%5zu %-11s %10u %10u %4u  %s\n", i + 1, validSection(c, s) ? c.sections[s.section - 1].name.c_str() : "-", s.offset, s.size,
                        s.kind, c.symbolName(i).c_str());
        }
    } else if (l.sb) {
        print("%5s %-10s %-16s %10s  %s\n", "#", "id", "type", "size", "name");
        for (size_t i = 0; i < l.sb->entries.size(); ++i) {
            const auto& e = l.sb->entries[i];
            print("%5zu %s %-16s %10zu  %s\n", i + 1, hex32(e.id).c_str(), assetdb::typeName(uint8_t(e.id >> 24)).c_str(), e.data.size(),
                        ops::entryName(e).c_str());
        }
        print("dependencies:");
        for (uint32_t d : l.sb->dependencies) print(" %s", hex32(d).c_str());
        print("\n");
    } else if (l.kind == FileKind::WaveBank) {
        auto wb = media::parseWaveBank(l.raw);
        static const char* fmts[] = {"PCM", "XMA", "ADPCM", "WMA"};
        for (size_t i = 0; i < wb.waves.size(); ++i) {
            const auto& w = wb.waves[i];
            print("%4zu %-5s %u ch %6u Hz %8u samples %8zu bytes  %s\n", i, fmts[w.formatTag], w.channels, w.sampleRate, w.durationSamples, w.data.size(),
                        w.name.c_str());
        }
    } else {
        fail("cannot list contents of a " + std::string(kindName(l.kind)) + " file");
    }
    return 0;
}

int decompress(const Args& a) {
    Bytes d = readFile(a.at(0, "input"));
    if (!xctd::isXctd(d)) fail("input is not XCTD-compressed");
    Bytes out = xctd::decompress(d);
    writeFile(a.at(1, "output"), out);
    print("decompressed %zu -> %zu bytes\n", d.size(), out.size());
    return 0;
}

int compress(const Args& a) {
    Bytes d = readFile(a.at(0, "input"));
    if (xctd::isXctd(d)) fail("input is already XCTD-wrapped");
    writeFile(a.at(1, "output"), xctd::wrapStored(d));
    print("wrapped %zu bytes in an uncompressed XCTD container\n", d.size());
    return 0;
}

int extract(const Args& a) {
    fs::path p = a.at(0, "file");
    std::string sel = a.at(1, "symbol");
    fs::path out = a.at(2, "output");
    Loaded l = loadAsset(p);
    if (l.sb) {
        // For stream bundles the selector is an entry id or index.
        for (size_t i = 0; i < l.sb->entries.size(); ++i) {
            const auto& e = l.sb->entries[i];
            if (sel == "#" + std::to_string(i + 1) || lower(sel) == hex32(e.id) || lower(sel) == "0x" + hex32(e.id) ||
                (!e.data.empty() && lower(ops::entryName(e)).find(lower(sel)) != std::string::npos)) {
                writeFile(out, e.data);
                print("wrote entry %s (%zu bytes) to %s\n", hex32(e.id).c_str(), e.data.size(), out.string().c_str());
                return 0;
            }
        }
        fail("no stream bundle entry matches '" + sel + "'");
    }
    auto& c = requireCaff(l, p);
    auto hits = ops::selectSymbols(c, sel, a.opt("--section"));
    if (hits.empty()) fail("no symbol matches '" + sel + "'");
    if (hits.size() == 1) {
        writeFile(out, c.symbolData(hits[0]));
        print("wrote symbol #%zu (%u bytes) to %s\n", hits[0] + 1, c.symbols[hits[0]].size, out.string().c_str());
    } else {
        for (size_t i : hits) {
            fs::path f = out / (std::to_string(i + 1) + "_" + safeName(assetdb::baseAssetName(c.symbolName(i))) + "." + ops::sectionTag(c, c.symbols[i]) + ".bin");
            writeFile(f, c.symbolData(i));
            print("wrote symbol #%zu to %s\n", i + 1, f.string().c_str());
        }
    }
    return 0;
}

int replace(const Args& a) {
    fs::path p = a.at(0, "file");
    std::string sel = a.at(1, "symbol");
    Bytes data = readFile(a.at(2, "data file"));
    Loaded l = loadAsset(p);
    fs::path out = prepareOutput(p, a.opt("-o"));
    if (l.sb) {
        for (size_t i = 0; i < l.sb->entries.size(); ++i) {
            auto& e = l.sb->entries[i];
            if (sel == "#" + std::to_string(i + 1) || lower(sel) == hex32(e.id) || lower(sel) == "0x" + hex32(e.id)) {
                e.data = data;
                writeFile(out, l.sb->serialize());
                print("replaced entry %s; wrote %s\n", hex32(e.id).c_str(), out.string().c_str());
                return 0;
            }
        }
        fail("no stream bundle entry matches '" + sel + "' (use #index or the hex id)");
    }
    auto& c = requireCaff(l, p);
    auto hits = ops::selectSymbols(c, sel, a.opt("--section"));
    if (hits.size() != 1) fail(hits.empty() ? "no symbol matches '" + sel + "'" : "'" + sel + "' matches " + std::to_string(hits.size()) + " symbols; use #index or --section");
    uint32_t oldSize = c.symbols[hits[0]].size;
    c.replaceSymbolData(hits[0], data);
    saveCaff(out, c, a.flag("--xctd"));
    print("replaced symbol #%zu (%u -> %zu bytes); wrote %s\n", hits[0] + 1, oldSize, data.size(), out.string().c_str());
    if (data.size() != oldSize) print("note: relocation offsets inside this symbol were kept; update them if its layout changed\n");
    return 0;
}

int caffUnpack(const Args& a) {
    fs::path p = a.at(0, "file"), dir = a.at(1, "output dir");
    Loaded l = loadAsset(p);
    auto& c = requireCaff(l, p);
    ops::unpackCaff(c, l.xctd, dir);
    print("unpacked %zu symbols from %s into %s\n", c.symbols.size(), p.string().c_str(), dir.string().c_str());
    return 0;
}

int caffPack(const Args& a) {
    fs::path dir = a.at(0, "unpacked dir"), out = a.at(1, "output file");
    bool wasXctd = false;
    int changed = 0;
    caff::Caff c = ops::packCaff(dir, &wasXctd, &changed);
    saveCaff(out, c, a.flag("--xctd"));
    print("packed %s (%d resized/edited symbols)%s\n", out.string().c_str(), changed, wasXctd && !a.flag("--xctd") ? " as plain CAFF" : "");
    return 0;
}

int sbUnpack(const Args& a) {
    fs::path p = a.at(0, "file"), dir = a.at(1, "output dir");
    Loaded l = loadAsset(p);
    if (!l.sb) fail(p.string() + " is not a stream bundle");
    ops::unpackStreamBundle(*l.sb, dir);
    print("unpacked %zu entries into %s\n", l.sb->entries.size(), dir.string().c_str());
    return 0;
}

int sbPack(const Args& a) {
    fs::path dir = a.at(0, "unpacked dir"), out = a.at(1, "output file");
    auto sb = ops::packStreamBundle(dir);
    writeFile(out, sb.serialize());
    print("packed %zu entries into %s\n", sb.entries.size(), out.string().c_str());
    return 0;
}

int texExport(const Args& a) {
    fs::path p = a.at(0, "file"), dir = a.at(1, "output dir");
    bool png = a.flag("--png") || a.flag("--png-only"), dds = !a.flag("--png-only");
    Loaded l = loadAsset(p);
    if (!l.caff && !l.sb) fail("texture export needs a CAFF, bundle or stream bundle");
    // Stream bundle entries are parsed into owned CAFFs; pointers are taken once the vectors are final.
    auto parseEntries = [](const streambundle::StreamBundle& sb) {
        std::vector<caff::Caff> v;
        for (auto& e : sb.entries) if (caff::isCaff(e.data)) v.push_back(caff::Caff::parse(e.data));
        return v;
    };
    std::vector<caff::Caff> mainEntries, streamEntries;
    if (l.sb) mainEntries = parseEntries(*l.sb);
    if (!a.opt("--stream").empty()) {
        Loaded s = loadAsset(a.opt("--stream"));
        if (!s.sb) fail("--stream must be a stream bundle");
        streamEntries = parseEntries(*s.sb);
    }
    std::vector<caff::Caff*> mains, streams;
    if (l.caff) mains.push_back(&*l.caff);
    for (auto& c : mainEntries) mains.push_back(&c);
    for (auto& c : streamEntries) streams.push_back(&c);
    ops::Progress pr = consoleProgress();
    ops::TexStats st = ops::exportTextures(mains, streams, dir, dds, png, nullptr, &pr);
    print("exported %d textures (%d merged with streamed top mips, %d skipped) to %s\n", st.written, st.merged, st.skipped, dir.string().c_str());
    return 0;
}

int modelExport(const Args& a) {
    fs::path p = a.at(0, "file"), dir = a.at(1, "output dir");
    bool allGroups = a.flag("--all-groups");
    Loaded l = loadAsset(p);
    std::vector<caff::Caff> entries;
    if (l.sb)
        for (auto& e : l.sb->entries) if (caff::isCaff(e.data)) entries.push_back(caff::Caff::parse(e.data));
    std::vector<caff::Caff*> caffs;
    if (l.caff) caffs.push_back(&*l.caff);
    for (auto& c : entries) caffs.push_back(&c);
    if (caffs.empty()) fail("model export needs a CAFF, bundle or stream bundle");
    int written = 0;
    for (caff::Caff* c : caffs) {
        for (size_t i = 0; i < c->symbols.size(); ++i) {
            if (!model::isModelSymbol(*c, i)) continue;
            model::Model m;
            try {
                m = model::parse(*c, i);
            } catch (std::exception& e) {
                print("warning: %s: %s\n", c->symbolName(i).c_str(), e.what());
                continue;
            }
            std::vector<bool> include(m.meshes.size());
            size_t verts = 0, tris = 0, shown = 0;
            for (size_t k = 0; k < m.meshes.size(); ++k) {
                auto& mesh = m.meshes[k];
                include[k] = allGroups || m.groups[size_t(mesh.group)].shown;
                if (include[k]) verts += mesh.vertices.size(), tris += mesh.indices.size() / 3;
            }
            for (auto& g : m.groups) shown += allGroups || g.shown;
            std::string base = safeName(m.name, 150);
            fs::create_directories(dir);
            writeText(dir / (base + ".obj"), model::toObj(m, base + ".mtl", {}, include));
            writeText(dir / (base + ".mtl"), model::toMtl(m));
            std::string skel = "# bone parent local_xyz world_xyz rotation_xyzw\n";
            for (size_t b = 0; b < m.bones.size(); ++b) {
                auto& bn = m.bones[b];
                char line[512];
                std::snprintf(line, sizeof line, "%zu %s %d  %g %g %g  %g %g %g  %g %g %g %g\n", b, bn.name.c_str(), bn.parent, bn.local[0], bn.local[1],
                              bn.local[2], bn.world[0], bn.world[1], bn.world[2], bn.rotation[0], bn.rotation[1], bn.rotation[2], bn.rotation[3]);
                skel += line;
            }
            if (!m.bones.empty()) writeText(dir / (base + ".bones.txt"), skel);
            print("%s: %zu of %zu draw groups, %zu vertices, %zu triangles, %zu bones, %zu textures\n", m.name.c_str(), shown, m.groups.size(), verts,
                        tris, m.bones.size(), m.textures.size());
            for (auto& w : m.warnings) print("  warning: %s\n", w.c_str());
            ++written;
        }
    }
    if (written && a.flag("--png")) {
        ops::Progress pr = consoleProgress();
        ops::exportTextures(caffs, {}, dir, false, true, nullptr, &pr);
    }
    print("exported %d model(s) to %s\n", written, dir.string().c_str());
    return 0;
}

namespace {

// The .data symbol of the model called `name` (with or without "aid_model_"), or SIZE_MAX.
size_t findModel(const caff::Caff& c, const std::string& name) {
    std::string want = lower(name);
    for (size_t i = 0; i < c.symbols.size(); ++i) {
        if (!model::isModelSymbol(c, i)) continue;
        std::string n = lower(c.symbolName(i));
        if (n == want || n == "aid_model_" + want) return i;
    }
    return SIZE_MAX;
}

} // namespace

// nuttool model-fbx <file> <model> <out.fbx>: the model with its skeleton, textures (from the
// bundle and its paired stream bundle) and the animations named after it.
int modelFbx(const Args& a) {
    fs::path p = a.at(0, "file"), out = a.at(2, "output .fbx or .obj");
    bool obj = lower(out.extension().string()) == ".obj";
    std::string name = a.at(1, "model name");
    Loaded l = loadAsset(p);
    if (!l.caff) fail("model-fbx needs a bundle or CAFF");
    caff::Caff& c = *l.caff;
    size_t sym = findModel(c, name);
    if (sym == SIZE_MAX) fail("no model '" + name + "' in " + p.string());
    model::Model m = model::parse(c, sym);

    fbx::Options opt;
    if (a.flag("--all-groups")) opt.include.assign(m.meshes.size(), true);
    opt.tint = !a.flag("--no-tint");
    std::string axis = lower(a.opt("--bone-axis"));
    opt.reorient = a.flag("--reorient-bones") || !axis.empty();
    if (!axis.empty()) {
        static const char* axes[6] = {"x", "y", "z", "-x", "-y", "-z"};
        auto it = std::find(std::begin(axes), std::end(axes), axis.front() == '+' ? axis.substr(1) : axis);
        if (it == std::end(axes)) fail("--bone-axis takes X, Y, Z, -X, -Y or -Z");
        opt.boneAxis = int(it - std::begin(axes));
    }
    opt.boneEnds = a.flag("--bone-ends") || !a.opt("--bone-end-size").empty();
    if (!a.opt("--bone-end-size").empty()) opt.boneEndSize = std::clamp(std::stof(a.opt("--bone-end-size")) / 100, 0.0001f, 1.0f);

    // Textures: whole ones and mip halves here, streamed top levels in the paired stream bundle.
    std::vector<caff::Caff> streamCaffs;
    fs::path sbPath = model::pairedStreamBundle(p);
    if (!a.flag("--no-textures") && !sbPath.empty() && fs::exists(longPath(sbPath))) {
        try {
            Loaded sl = loadAsset(sbPath);
            if (sl.sb)
                for (auto& e : sl.sb->entries)
                    if (caff::isCaff(e.data)) streamCaffs.push_back(caff::Caff::parse(e.data));
        } catch (std::exception& e) {
            print("warning: %s: %s\n", sbPath.string().c_str(), e.what());
        }
    }
    std::vector<caff::Caff*> streams;
    for (auto& s : streamCaffs) streams.push_back(&s);
    std::map<std::string, ops::TexRef> textures;
    for (auto& t : ops::findTextures(c)) textures.emplace(ops::fullName(t.name), t);
    for (auto& sc : streamCaffs)
        for (auto& t : ops::findTextures(sc)) textures.emplace(ops::fullName(t.name), t);
    if (!a.flag("--no-textures"))
        opt.texture = [&](const std::string& tex) -> std::optional<Bytes> {
            auto it = textures.find(tex);
            if (it == textures.end()) return std::nullopt;
            try {
                ops::DecodedTexture d = ops::decodeTexture(it->second, streams);
                return image::encodePng(texture::toRgba(d.header, d.levels[0], 0));
            } catch (std::exception& e) {
                print("warning: texture %s: %s\n", tex.c_str(), e.what());
                return std::nullopt;
            }
        };
    // Fur shells: every slice of the fur volumes and colour maps; length in percent of the model's radius, as the viewer shows it.
    if (!a.flag("--no-fur")) {
        opt.fur = true;
        if (!a.opt("--fur-length").empty()) opt.furLength = std::stof(a.opt("--fur-length")) / 100;
        if (!a.opt("--fur-tiling").empty()) opt.furTiling = std::stof(a.opt("--fur-tiling"));
        if (!a.opt("--fur-size").empty()) opt.furSize = uint32_t(std::clamp(std::stoi(a.opt("--fur-size")), 16, 4096));
        opt.pixels = [&](const std::string& tex) -> std::optional<std::vector<image::Rgba>> {
            auto it = textures.find(tex);
            if (it == textures.end()) return std::nullopt;
            try {
                ops::DecodedTexture d = ops::decodeTexture(it->second, streams);
                std::vector<image::Rgba> slices;
                for (uint32_t s = 0; s < d.levels[0].slices.size(); ++s) slices.push_back(texture::toRgba(d.header, d.levels[0], s));
                return slices;
            } catch (std::exception& e) {
                print("warning: texture %s: %s\n", tex.c_str(), e.what());
                return std::nullopt;
            }
        };
    }

    // Animations: "aid_anim_<model>_*" in --anims <file> (default: the same bundle), or all of them with --all-anims.
    std::vector<model::Animation> anims;
    std::vector<std::string> animNames;
    if (!obj && !a.flag("--no-anims")) {
        std::optional<Loaded> other;
        caff::Caff* ac = &c;
        if (!a.opt("--anims").empty()) {
            other = loadAsset(a.opt("--anims"));
            if (!other->caff) fail("--anims needs a bundle or CAFF");
            ac = &*other->caff;
        }
        std::string prefix = startsWith(m.name, "aid_model_") ? "aid_anim_" + m.name.substr(10) + "_" : std::string();
        for (size_t i = 0; i < ac->symbols.size(); ++i) {
            if (!model::isAnimationSymbol(*ac, i)) continue;
            std::string an = assetdb::baseAssetName(ac->symbolName(i));
            bool own = !prefix.empty() && startsWith(an, prefix);
            if (!own && !a.flag("--all-anims")) continue;
            try {
                anims.push_back(model::parseAnimation(*ac, i));
                animNames.push_back(own ? an.substr(prefix.size()) : an);
            } catch (std::exception& e) {
                print("warning: %s: %s\n", an.c_str(), e.what());
            }
        }
    }
    for (size_t i = 0; i < anims.size(); ++i) opt.animations.push_back({animNames[i], &anims[i]});

    fbx::Report r;
    if (out.has_parent_path()) fs::create_directories(longPath(out.parent_path()));
    if (obj) {
        std::string base = out.stem().string();
        fbx::ObjFiles files = fbx::writeObj(m, opt, base, &r);
        fs::path dir = out.parent_path();
        writeText(out, files.obj);
        writeText(dir / (base + ".mtl"), files.mtl);
        for (auto& [file, bytes] : files.files) writeFile(dir / file, bytes);
        print("%s: %zu meshes, %zu vertices, %zu triangles, %zu materials, %zu textures, %zu fur shells -> %s (+ %zu files)\n", m.name.c_str(),
              r.meshes, r.vertices, r.triangles, r.materials, r.textures, r.furShells, out.string().c_str(), files.files.size() + 1);
        for (auto& n : r.notes) print("  note: %s\n", n.c_str());
        return 0;
    }
    Bytes data = fbx::write(m, opt, &r);
    writeFile(out, data);
    print("%s: %zu meshes, %zu vertices, %zu triangles, %zu bones, %zu materials, %zu textures (%zu embedded), %zu animations, %zu fur shells -> %s (%zu KB)\n",
          m.name.c_str(), r.meshes, r.vertices, r.triangles, r.bones, r.materials, r.textures, r.embedded, r.animations, r.furShells,
          out.string().c_str(), data.size() / 1024);
    for (auto& n : r.notes) print("  note: %s\n", n.c_str());
    return 0;
}

namespace {

// Runs `edit` on every copy of a model in a bundle or stream bundle and writes the result. `edit`
// may return stream bundle entries (streamed texture levels): they go into the same stream bundle,
// or for a bundle into its paired one (written to --stream-out, or in place).
using ModelEdit = std::function<std::vector<streambundle::Entry>(caff::Caff&, size_t)>;
void editModel(const Args& a, const fs::path& p, const std::string& name, const ModelEdit& edit) {
    Loaded l = loadAsset(p);
    int copies = 0;
    std::vector<streambundle::Entry> extra;
    auto run = [&](caff::Caff& c, size_t s) {
        auto e = edit(c, s);
        extra.insert(extra.end(), e.begin(), e.end());
        ++copies;
    };
    if (l.caff) {
        size_t s = findModel(*l.caff, name);
        if (s != SIZE_MAX) run(*l.caff, s);
    } else if (l.sb) {
        for (auto& e : l.sb->entries) {
            if (!caff::isCaff(e.data)) continue;
            caff::Caff c = caff::Caff::parse(e.data);
            size_t s = findModel(c, name);
            if (s == SIZE_MAX) continue;
            run(c, s);
            e.data = c.serialize();
        }
    } else {
        fail(p.string() + " is not a bundle or stream bundle (" + kindName(l.kind) + ")");
    }
    if (!copies) fail("no model '" + name + "' in " + p.string());
    // The stream bundle first: if it cannot be written, the bundle stays as it was.
    if (!extra.empty()) {
        if (l.sb) {
            model::addStreamEntries(*l.sb, extra);
        } else {
            if (!a.opt("-o").empty() && a.opt("--stream-out").empty())
                fail("the copied textures also go into the stream bundle; with -o, give --stream-out <file> too");
            fs::path sp = model::pairedStreamBundle(p);
            Loaded sl = loadAsset(sp);
            if (!sl.sb) fail(sp.string() + " (the bundle's stream bundle) is missing; the copied textures need it");
            int n = model::addStreamEntries(*sl.sb, extra);
            fs::path sout = prepareOutput(sp, a.opt("--stream-out"));
            writeFile(sout, sl.sb->serialize());
            print("wrote %s (%d streamed texture level(s) added)\n", sout.string().c_str(), n);
        }
    }
    fs::path out = prepareOutput(p, a.opt("-o"));
    if (l.caff) saveCaff(out, *l.caff, a.flag("--xctd"));
    else writeFile(out, l.sb->serialize());
    print("wrote %s (%d cop%s of the model)\n", out.string().c_str(), copies, copies == 1 ? "y" : "ies");
}

} // namespace

// nuttool anim-import <file> <anim> <keys.json>: new keys for every copy of an animation in a bundle
// or stream bundle (animkeys.hpp has the file format).
int animImport(const Args& a) {
    fs::path p = a.at(0, "file");
    std::string name = lower(a.at(1, "animation name"));
    std::string text = readText(a.at(2, "keys .json"));
    Loaded l = loadAsset(p);
    int copies = 0;
    auto apply = [&](caff::Caff& c) {
        for (size_t i = 0; i < c.symbols.size(); ++i) {
            if (!model::isAnimationSymbol(c, i)) continue;
            std::string n = lower(assetdb::baseAssetName(c.symbolName(i)));
            if (n != name && n != "aid_anim_" + name) continue;
            model::Animation an = model::parseAnimation(c, i);
            model::KeysReport r;
            std::vector<model::Key> keys = model::keysFromJson(an, text, &r);
            model::writeAnimationKeys(c, i, keys);
            print("%s: %zu bones keyed from %zu timeline entries, %d frames over %.2f s\n", an.name.c_str(), r.bones, r.entries, an.frames, an.duration);
            for (auto& n2 : r.notes) print("  note: %s\n", n2.c_str());
            ++copies;
        }
    };
    if (l.caff) {
        apply(*l.caff);
    } else if (l.sb) {
        for (auto& e : l.sb->entries) {
            if (!caff::isCaff(e.data)) continue;
            caff::Caff c = caff::Caff::parse(e.data);
            int before = copies;
            apply(c);
            if (copies != before) e.data = c.serialize();
        }
    } else {
        fail(p.string() + " is not a bundle or stream bundle (" + kindName(l.kind) + ")");
    }
    if (!copies) fail("no animation '" + a.at(1, "animation name") + "' in " + p.string());
    fs::path out = prepareOutput(p, a.opt("-o"));
    if (l.caff) saveCaff(out, *l.caff, a.flag("--xctd"));
    else writeFile(out, l.sb->serialize());
    print("wrote %s (%d cop%s of the animation)\n", out.string().c_str(), copies, copies == 1 ? "y" : "ies");
    return 0;
}

namespace {

// The title update folder for actor-list and actor-swap: --update, else <assets>\..\update when it exists.
fs::path actorUpdateDir(const Args& a, const fs::path& assets) {
    if (!a.opt("--update").empty()) return a.opt("--update");
    const fs::path u = assets.parent_path() / "update";
    std::error_code ec;
    return fs::exists(u, ec) ? u : fs::path();
}

actorswap::Index actorIndex(const Args& a, const fs::path& assets) {
    return actorswap::scan(assets, actorUpdateDir(a, assets), [](const std::string& s) { print("%s\n", s.c_str()); });
}

} // namespace

int actorList(const Args& a) {
    const fs::path assets = a.at(0, "assets dir");
    const actorswap::Index ix = actorIndex(a, assets);
    const std::string only = a.opt("--actor");
    for (const actorswap::Actor& act : ix.actors) {
        if (!only.empty() && act.name != only && act.name != "aid_objparams_banjox_actor_" + only) continue;
        print("%s: model %s, table %s (%zu entries)\n", act.name.c_str(), ix.nameOf(act.model).c_str(), ix.nameOf(act.table).c_str(),
              act.entries.size());
        if (only.empty()) continue;
        for (const actorswap::Entry& e : act.entries)
            print("  %-32s %s%s%s\n", e.name.c_str(), ix.nameOf(e.anim).c_str(), e.events ? "  events " : "", ix.nameOf(e.events).c_str());
        print("animations it can lend:\n");
        for (const std::string& n : actorswap::sourceAnims(act, ix)) print("  %s\n", n.c_str());
    }
    return 0;
}

int actorSwap(const Args& a) {
    const fs::path assets = a.at(0, "assets dir");
    actorswap::Plan plan;
    plan.target = a.at(1, "target actor");
    const std::string source = a.at(2, "source actor or .glb");
    if (endsWith(lower(source), ".glb")) {
        plan.glb = source;
        if (!a.opt("--scale").empty()) plan.scale = std::stof(a.opt("--scale"));
    } else {
        plan.source = source;
    }
    for (std::string* n : {&plan.target, &plan.source})
        if (!n->empty() && !startsWith(*n, "aid_")) *n = "aid_objparams_banjox_actor_" + *n;
    if (!a.opt("--plan").empty()) plan.anims = actorswap::planFromJson(readText(a.opt("--plan"))).anims;
    const std::string events = a.opt("--events", "keep");
    if (events == "source") plan.events = actorswap::Events::Source;
    else if (events == "none") plan.events = actorswap::Events::None;
    else if (events != "keep") fail("--events is keep, source or none");
    const actorswap::Index ix = actorIndex(a, assets);
    const actorswap::Actor* target = ix.actor(plan.target);
    const actorswap::Actor* from = plan.glb.empty() ? ix.actor(plan.source) : nullptr;
    std::map<std::string, std::string> matched;
    if (target && from) matched = actorswap::autoMatch(*target, *from, ix);
    if (target && !plan.glb.empty()) matched = actorswap::autoMatchGlb(*target, actorswap::glbAnimations(plan.glb));
    for (const auto& [entry, anim] : matched)
        if (plan.anims.emplace(entry, anim).second) print("  %-32s -> %s\n", entry.c_str(), anim.c_str());
    const auto written = actorswap::apply(ix, plan, [](const std::string& s) { print("%s\n", s.c_str()); });
    for (const fs::path& f : written) print("wrote %s\n", f.string().c_str());
    return 0;
}

int modelImport(const Args& a) {
    fs::path p = a.at(0, "file");
    std::string name = a.at(1, "model name");
    Bytes mesh = readFile(a.at(2, "OBJ or FBX file"));
    bool rig = a.flag("--rig");
    editModel(a, p, name, [&](caff::Caff& c, size_t s) {
        model::ImportReport r = model::importMeshFile(c, s, mesh, rig);
        print("%s: %d mesh(es) edited in place, %d rebuilt, %d hidden\n", c.symbolName(s).c_str(), r.edited, r.rebuilt, r.hidden);
        for (auto& n : r.notes) print("  %s\n", n.c_str());
        return std::vector<streambundle::Entry>{};
    });
    return 0;
}

int modelSwap(const Args& a) {
    fs::path p = a.at(0, "file");
    std::string name = a.at(1, "model name");
    fs::path sp = a.at(2, "source file");
    std::string sname = a.at(3, "source model name");
    // The source: the first copy of the model in its file.
    Loaded sl = loadAsset(sp);
    std::optional<caff::Caff> entry;
    const caff::Caff* src = nullptr;
    size_t ss = SIZE_MAX;
    if (sl.caff) {
        src = &*sl.caff;
        ss = findModel(*src, sname);
    } else if (sl.sb) {
        for (auto& e : sl.sb->entries) {
            if (!caff::isCaff(e.data)) continue;
            entry = caff::Caff::parse(e.data);
            ss = findModel(*entry, sname);
            if (ss != SIZE_MAX) { src = &*entry; break; }
        }
    }
    if (!src || ss == SIZE_MAX) fail("no model '" + sname + "' in " + sp.string());
    bool sameFile = fs::equivalent(longPath(p), longPath(sp));
    // Textures the destination lacks come from the source's bundle and its stream bundle.
    std::shared_ptr<const caff::Caff> srcRef(src, [](const caff::Caff*) {});
    model::FindTexture find = model::texturesIn(srcRef, sl.sb ? sp : model::pairedStreamBundle(sp));
    editModel(a, p, name, [&](caff::Caff& c, size_t s) {
        // Within one bundle, copy from the destination's own CAFF so pools are shared.
        size_t own = sameFile ? findModel(c, sname) : SIZE_MAX;
        model::CopyResult r = own != SIZE_MAX ? model::copyModel(c, s, c, own, find) : model::copyModel(c, s, *src, ss, find);
        print("%s now holds %s\n", c.symbolName(s).c_str(), src->symbolName(ss).c_str());
        for (auto& n : r.notes) print("  %s\n", n.c_str());
        return r.streamEntries;
    });
    return 0;
}

// The one texture whose name is, or contains, `name`.
ops::TexRef textureNamed(caff::Caff& c, const std::string& wanted) {
    std::string name = lower(wanted);
    std::vector<ops::TexRef> matches;
    for (auto& t : ops::findTextures(c)) {
        std::string n = lower(t.name);
        if (n == name || lower(ops::fullName(t.name)) == name) return t;
        if (n.find(name) != std::string::npos) matches.push_back(t);
    }
    if (matches.size() != 1)
        fail(matches.empty() ? "no texture matches '" + name + "'" : "'" + name + "' matches " + std::to_string(matches.size()) + " textures; be more specific");
    return matches[0];
}

int texImport(const Args& a) {
    fs::path p = a.at(0, "file");
    std::string name = a.at(1, "texture name");
    fs::path img = a.at(2, "image");
    Loaded l = loadAsset(p);
    auto& c = requireCaff(l, p);
    std::vector<ops::TexRef> matches = {textureNamed(c, name)};
    std::optional<Loaded> stream;
    if (!a.opt("--stream").empty()) {
        stream = loadAsset(a.opt("--stream"));
        if (!stream->sb) fail("--stream must be a stream bundle");
    }
    bool streamChanged = false;
    std::string summary = ops::importTexture(c, matches[0], img, stream ? &*stream->sb : nullptr, &streamChanged);
    fs::path out = prepareOutput(p, a.opt("-o"));
    saveCaff(out, c, a.flag("--xctd"));
    print("%s: %s; wrote %s\n", matches[0].name.c_str(), summary.c_str(), out.string().c_str());
    if (streamChanged) {
        fs::path sout = prepareOutput(a.opt("--stream"), a.opt("--stream-out"));
        writeFile(sout, stream->sb->serialize());
        print("wrote %s\n", sout.string().c_str());
    }
    return 0;
}

int texRemoveMips(const Args& a) {
    fs::path p = a.at(0, "file");
    Loaded l = loadAsset(p);
    auto& c = requireCaff(l, p);
    ops::TexRef t = textureNamed(c, a.at(1, "texture name"));
    bool changed = false;
    std::string summary = ops::removeMipmaps(c, t, &changed);
    if (!changed) {
        print("%s: %s; nothing written\n", t.name.c_str(), summary.c_str());
        return 0;
    }
    fs::path out = prepareOutput(p, a.opt("-o"));
    saveCaff(out, c, a.flag("--xctd"));
    print("%s: %s; wrote %s\n", t.name.c_str(), summary.c_str(), out.string().c_str());
    return 0;
}

int textExport(const Args& a) {
    fs::path p = a.at(0, "loctext file"), out = a.at(1, "output .json");
    Loaded l = loadAsset(p);
    auto& c = requireCaff(l, p);
    auto t = loctext::read(c);
    Json j = loctext::toJson(t);
    if (!c.symbols.empty()) j["asset"] = assetdb::baseAssetName(c.symbolName(0));
    writeText(out, j.dump());
    print("exported %zu strings to %s\n", t.entries.size(), out.string().c_str());
    return 0;
}

int textImport(const Args& a) {
    fs::path p = a.at(0, "loctext file");
    Json j = Json::parse(readText(a.at(1, "input .json")));
    Loaded l = loadAsset(p);
    auto& c = requireCaff(l, p);
    auto t = loctext::read(c);
    size_t before = t.entries.size();
    loctext::applyJson(t, j, a.flag("--prune"));
    loctext::write(c, t);
    fs::path out = prepareOutput(p, a.opt("-o"));
    saveCaff(out, c);
    print("imported %zu strings (%zu before) into %s\n", t.entries.size(), before, out.string().c_str());
    return 0;
}

namespace {

// Names for asset ids: the bundle's own symbols plus db_index (from --assets, or the bundle's assets folder).
assetdb::Names scriptNames(const caff::Caff& c, const fs::path& bundle, const std::string& assets) {
    assetdb::Names names;
    for (size_t i = 0; i < c.symbols.size(); ++i) names.add(assetdb::baseAssetName(c.symbolName(i)));
    fs::path root = !assets.empty() ? fs::path(assets) : fs::absolute(bundle).parent_path().parent_path().parent_path();
    fs::path idx = root / "Debug" / "db_index.txt";
    if (fs::exists(idx))
        for (auto& e : assetdb::readDbIndex(idx)) names.add(e.name);
    return names;
}

bool isScriptSymbol(const caff::Caff& c, size_t i) {
    return gamescript::kindOf(assetdb::baseAssetName(c.symbolName(i))) && validSection(c, c.symbols[i]) &&
           c.sections[c.symbols[i].section - 1].name == ".data";
}

} // namespace

int gamescriptExport(const Args& a) {
    fs::path p = a.at(0, "bundle"), dir = a.at(1, "output dir");
    std::string filter = lower(a.opt("--name"));
    Loaded l = loadAsset(p);
    auto& c = requireCaff(l, p);
    assetdb::Names names = scriptNames(c, p, a.opt("--assets"));
    auto lookup = [&names](uint32_t id) { return names.lookup(id); };
    fs::create_directories(longPath(dir));
    std::set<std::string> done;
    for (size_t i = 0; i < c.symbols.size(); ++i) {
        std::string name = assetdb::baseAssetName(c.symbolName(i));
        if (!isScriptSymbol(c, i) || (!filter.empty() && lower(name).find(filter) == std::string::npos) || !done.insert(name).second) continue;
        writeText(dir / (safeName(name) + ".txt"), "# " + name + "\n" + gamescript::toText(*gamescript::kindOf(name), c.symbolData(i), lookup));
    }
    print("exported %zu script%s to %s\n", done.size(), done.size() == 1 ? "" : "s", dir.string().c_str());
    return 0;
}

int gamescriptImport(const Args& a) {
    fs::path p = a.at(0, "bundle"), in = a.at(1, "script .txt or folder");
    std::vector<fs::path> files;
    if (fs::is_directory(in)) {
        for (auto& e : fs::directory_iterator(in))
            if (e.is_regular_file() && lower(e.path().extension().string()) == ".txt") files.push_back(e.path());
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(in);
    }
    Loaded l = loadAsset(p);
    auto& c = requireCaff(l, p);
    size_t changed = 0;
    for (auto& f : files) {
        std::string name = f.stem().string();
        auto kind = gamescript::kindOf(name);
        if (!kind) fail(f.filename().string() + ": the file must be named after its asset (aid_script_..., aid_actorgoals_..., aid_scripttable_..., aid_statetable_...)");
        Bytes data;
        try {
            data = gamescript::fromText(*kind, readText(f));
        } catch (std::exception& e) {
            fail(f.filename().string() + ": " + e.what());
        }
        bool found = false;
        for (size_t i = 0; i < c.symbols.size(); ++i) {
            if (!isScriptSymbol(c, i) || assetdb::baseAssetName(c.symbolName(i)) != name) continue;
            found = true;
            if (c.symbolData(i) != data) {
                c.replaceSymbolData(i, data);
                ++changed;
            }
        }
        if (!found) fail(name + " is not in " + p.filename().string());
    }
    if (!changed) {
        print("nothing changed: the %zu file(s) match %s\n", files.size(), p.string().c_str());
        return 0;
    }
    fs::path out = prepareOutput(p, a.opt("-o"));
    saveCaff(out, c, a.flag("--xctd"));
    print("applied %zu file(s), %zu symbol(s) changed; wrote %s\n", files.size(), changed, out.string().c_str());
    return 0;
}

int objparamsExport(const Args& a) {
    fs::path p = a.at(0, "bundle"), dir = a.at(1, "output dir");
    std::string filter = lower(a.opt("--name"));
    bool partsOnly = a.flag("--parts");
    Loaded l = loadAsset(p);
    auto& c = requireCaff(l, p);
    assetdb::Names names = scriptNames(c, p, a.opt("--assets"));
    auto lookup = [&names](uint32_t id) { return names.lookup(id); };
    fs::create_directories(longPath(dir));
    std::set<std::string> done;
    for (size_t i = 0; i < c.symbols.size(); ++i) {
        std::string name = assetdb::baseAssetName(c.symbolName(i));
        if (!startsWith(name, "aid_objparams_") || !validSection(c, c.symbols[i]) || c.sections[c.symbols[i].section - 1].name != ".data") continue;
        if ((!filter.empty() && lower(name).find(filter) == std::string::npos) || done.count(name)) continue;
        Bytes d = c.symbolData(i);
        if (!objparams::isObjParams(d) || (partsOnly && !objparams::isVehiclePart(d))) continue;
        done.insert(name);
        writeText(dir / (safeName(name) + ".txt"), "# " + name + "\n" + objparams::toText(d, lookup));
    }
    print("exported %zu objparams to %s\n", done.size(), dir.string().c_str());
    return 0;
}

int renutPart(const Args& a) {
    fs::path p = a.at(0, "bundle"), mod = a.at(2, "mod folder");
    renutpart::Part part;
    part.base = renutpart::baseName(a.at(1, "part"));
    part.id = lower(a.opt("--id"));
    if (part.id.empty()) fail("renut-part needs --id (the new part's id: mymod_bigwheel)");
    part.name = a.opt("--name");
    if (!a.opt("--count").empty()) part.count = std::atoi(a.opt("--count").c_str());
    part.behaviour = a.flag("--behaviour");
    Loaded l = loadAsset(p);
    auto& c = requireCaff(l, p);
    const std::string asset = "aid_objparams_banjox_vehicleblock_" + part.base;
    for (size_t i = 0; i < c.symbols.size() && part.original.empty(); ++i)
        if (assetdb::baseAssetName(c.symbolName(i)) == asset && validSection(c, c.symbols[i]) &&
            c.sections[c.symbols[i].section - 1].name == ".data")
            part.original = c.symbolData(i);
    if (!objparams::isVehiclePart(part.original)) fail(part.base + " is not a vehicle part in " + p.filename().string());
    // The edits: objparams text (as objparams-export writes it), applied to a copy of the part.
    part.edited = a.opt("--fields").empty() ? part.original : objparams::fromText(part.original, readText(a.opt("--fields")));
    // Its own model (the base's, or --template's, with the mesh file's geometry) and footprint (the
    // base's, with --attachable's faces), in the mod's stream bundle.
    if (!a.opt("--model").empty() || !a.opt("--attachable").empty()) {
        renutmake::PartAssets assets;
        assets.model = a.opt("--model");
        assets.templateName = a.opt("--template");
        assets.glb.node = a.opt("--mesh");
        for (auto& s : split(a.opt("--into"), ','))
            if (!trim(s).empty()) assets.glb.into.push_back(std::atoi(trim(s).c_str()));
        for (auto& s : split(a.opt("--flat"), ','))
            if (!trim(s).empty()) assets.glb.flat.push_back(float(std::atof(trim(s).c_str())));
        assets.collision = a.opt("--collision");
        assets.attachable = a.opt("--attachable");
        assets.bundle = a.opt("--bundle");
        auto say = [](const std::string& s) { print("%s\n", s.c_str()); };
        if (!renutmake::addPartAssets(mod, model::pairedStreamBundle(p), part, assets, a.flag("--force"), say)) return 0;
    }
    assetdb::Names names = scriptNames(c, p, a.opt("--assets"));
    fs::path file = renutpart::write(mod, part, a.opt("--mod-name"), a.opt("--author"), a.flag("--force"),
                                     [&names](uint32_t id) { return names.lookup(id); });
    print("wrote %s\n", file.string().c_str());
    return 0;
}

// --shapes "name=weight,name=weight": shape keys' starting weights (0 to 1).
std::map<std::string, float> shapeWeightsOpt(const std::string& text) {
    std::map<std::string, float> out;
    for (const std::string& item : split(text, ',')) {
        if (trim(item).empty()) continue;
        const size_t eq = item.rfind('=');
        const std::string name = eq == std::string::npos ? std::string() : trim(item.substr(0, eq));
        const std::string value = eq == std::string::npos ? std::string() : trim(item.substr(eq + 1));
        char* end = nullptr;
        const float weight = value.empty() ? 0 : std::strtof(value.c_str(), &end);
        if (name.empty() || value.empty() || (end && *end)) fail("--shapes is name=weight,name=weight (got '" + item + "')");
        out[name] = weight;
    }
    return out;
}

// A block of its own for reNut: its model, its footprint and a script with no base part.
int renutBlock(const Args& a) {
    fs::path mod = a.at(0, "mod folder");
    const std::string id = lower(a.opt("--id"));
    if (!renutpart::validId(id)) fail("renut-block needs --id, 1-40 characters of a-z, 0-9 and _ (got '" + id + "')");
    if (a.opt("--model").empty() || a.opt("--mesh").empty()) fail("renut-block needs --model <file.glb> and --mesh <node>");
    renutmake::Block b;
    b.id = id;
    b.name = a.opt("--name");
    b.model = a.opt("--model");
    b.mesh = a.opt("--mesh");
    b.shape = a.opt("--shape");
    b.attachable = a.opt("--attachable");
    b.category = a.opt("--category");
    b.type = a.opt("--type");
    b.mass = a.opt("--mass");
    b.health = a.opt("--health");
    b.count = a.opt("--count");
    b.bundle = a.opt("--bundle");
    b.shapeWeights = shapeWeightsOpt(a.opt("--shapes"));
    renutmake::block(mod, b, a.flag("--force"), [](const std::string& s) { print("%s\n", s.c_str()); });
    return 0;
}

// "x,y,z" as three numbers.
std::array<float, 3> vec3Opt(const std::string& option, const std::string& text) {
    const auto parts = split(text, ',');
    if (parts.size() != 3) fail(option + " is x,y,z (got '" + text + "')");
    std::array<float, 3> v{};
    for (size_t k = 0; k < 3; ++k) v[k] = float(std::atof(trim(parts[k]).c_str()));
    return v;
}

// A world of its own for reNut: its model, collision, marker and scene, and a script with its door.
int renutWorld(const Args& a) {
    fs::path assets = a.at(0, "assets dir"), mod = a.at(1, "mod folder");
    renutworld::World w;
    w.id = lower(a.opt("--id"));
    if (!renutpart::validId(w.id)) fail("renut-world needs --id, 1-40 characters of a-z, 0-9 and _ (got '" + w.id + "')");
    if (a.opt("--model").empty()) fail("renut-world needs --model <file.glb|file.gltf|file.obj|file.dae>");
    w.name = a.opt("--name");
    // --model a.dae;b.dae: more models are more of the world in the same space (a level's other rooms).
    for (const std::string& part : split(a.opt("--model"), ';')) {
        const std::string file = trim(part);
        if (file.empty()) continue;
        if (w.model.empty()) w.model = file;
        else w.moreModels.push_back(file);
    }
    if (!a.opt("--spawn").empty()) w.spawn = vec3Opt("--spawn", a.opt("--spawn"));
    if (!a.opt("--exit").empty()) w.exit = vec3Opt("--exit", a.opt("--exit"));
    if (!a.opt("--door").empty()) w.door = vec3Opt("--door", a.opt("--door"));
    w.townDoor = !a.flag("--no-door");
    w.exitDoor = !a.flag("--no-exit");
    if (!a.opt("--spawn-yaw").empty()) w.spawnYaw = float(std::atof(a.opt("--spawn-yaw").c_str()));
    if (!a.opt("--exit-yaw").empty()) w.exitYaw = float(std::atof(a.opt("--exit-yaw").c_str()));
    if (!a.opt("--door-yaw").empty()) w.doorYaw = float(std::atof(a.opt("--door-yaw").c_str()));
    if (!a.opt("--door-style").empty()) w.doorStyle = lower(trim(a.opt("--door-style")));
    for (const std::string& n : split(a.opt("--water"), ','))
        if (!trim(n).empty()) w.waterNames.push_back(trim(n));
    w.shapeWeights = shapeWeightsOpt(a.opt("--shapes"));
    // --stats jiggies=1,trophies=1,jinjos=0,notes=300: the door panel's totals.
    if (const std::string s = trim(a.opt("--stats")); !s.empty()) {
        static const char* const keys[] = {"jiggies", "trophies", "jinjos", "notes"};
        size_t at = 0;
        while (at <= s.size()) {
            const size_t end = std::min(s.find(',', at), s.size());
            const std::string item = trim(s.substr(at, end - at));
            const size_t eq = item.find('=');
            const std::string key = eq == std::string::npos ? item : lower(trim(item.substr(0, eq)));
            const std::string value = eq == std::string::npos ? "" : trim(item.substr(eq + 1));
            const auto k = std::find(std::begin(keys), std::end(keys), key);
            if (k == std::end(keys) || value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                fail("--stats is jiggies=N,trophies=N,jinjos=N,notes=N (got '" + item + "')");
            w.statTotals[size_t(k - std::begin(keys))] = std::atoi(value.c_str());
            at = end + 1;
        }
    }
    if (!a.opt("--smooth").empty()) w.smoothDegrees = float(std::atof(a.opt("--smooth").c_str()));
    if (!a.opt("--scale").empty()) w.scale = float(std::atof(a.opt("--scale").c_str()));
    if (!a.opt("--ground").empty()) w.ground = float(std::atof(a.opt("--ground").c_str()));
    if (const std::string c = trim(a.opt("--ground-colour")); !c.empty()) {
        if (c.size() != 6 || c.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            fail("--ground-colour is rrggbb (got '" + c + "')");
        const uint32_t v = parseHex(c);
        w.groundColour = {float((v >> 16) & 0xFF) / 255, float((v >> 8) & 0xFF) / 255, float(v & 0xFF) / 255};
    }
    w.bundle = a.opt("--bundle");
    w.hologram = a.opt("--hologram");
    w.sky = trim(a.opt("--sky"));
    if (!a.opt("--colour-scale").empty()) w.colourScale = float(std::atof(a.opt("--colour-scale").c_str()));
    if (!a.opt("--max-texture").empty()) w.maxTexture = uint32_t(std::atol(a.opt("--max-texture").c_str()));
    w.music = a.opt("--music");
    if (!a.opt("--music-volume").empty()) w.musicVolume = std::clamp(float(std::atof(a.opt("--music-volume").c_str())), 0.0f, 1.0f);
    w.npcs = a.opt("--npcs");
    w.materialsFile = a.opt("--materials");
    w.playerModel = lower(trim(a.opt("--player-model")));
    if (!w.playerModel.empty() && !renutpart::validId(w.playerModel))
        fail("--player-model is a player model's id: 1-40 characters of a-z, 0-9 and _");
    renutworld::make(assets, mod, w, a.flag("--force"), [](const std::string& s) { print("%s\n", s.c_str()); });
    return 0;
}

// A script graph's Lua, as the GUI's editor writes it: beside the graphs folder in scripts\ unless given.
int graphLua(const Args& a) {
    fs::path in = a.at(0, "graph.json");
    fs::path out = a.pos.size() > 1 ? fs::path(a.pos[1]) : in.parent_path().parent_path() / "scripts" / (in.stem().string() + ".lua");
    const scriptgraph::Graph g = scriptgraph::load(in);
    std::vector<std::string> problems;
    const std::string lua = scriptgraph::toLua(g, in.stem().string(), problems);
    for (const std::string& p : problems) print("warning: %s\n", p.c_str());
    if (!out.parent_path().empty()) fs::create_directories(longPath(out.parent_path()));
    writeText(out, lua);
    print("wrote %s (%zu nodes)\n", out.string().c_str(), g.nodes.size());
    return 0;
}

// A player model for reNut: Banjo's model with a file's meshes, in the mod's bundle.
int renutPlayerModel(const Args& a) {
    fs::path assets = a.at(0, "assets dir"), mod = a.at(1, "mod folder"), file = a.at(2, "model.fbx");
    renutmake::playerModel(assets, mod, file, lower(a.opt("--id")), a.opt("--template"), a.opt("--bundle"),
                           [](const std::string& s) { print("%s\n", s.c_str()); });
    return 0;
}

// Character packs for reNut: what each game character needs beyond its own bundles, to spawn it in any world.
int renutCharacters(const Args& a) {
    fs::path assets = a.at(0, "assets dir"), mods = a.at(1, "mods folder");
    std::vector<std::string> only;
    for (const std::string& n : split(a.opt("--only"), ','))
        if (!trim(n).empty()) only.push_back(lower(trim(n)));
    const int n = renutchars::build(assets, mods, only, [](const std::string& s) { print("%s\n", s.c_str()); });
    print("wrote %d character pack(s) to %s\n", n, (mods / ".characters").string().c_str());
    return 0;
}

// A model's materials as a materials file, every setting as the model gives it, to edit for --materials.
int renutMaterials(const Args& a) {
    fs::path model = a.at(0, "model"), out = a.at(1, "out.json");
    const auto list = renutworld::modelMaterials(model);
    if (list.empty()) fail(model.string() + " has no materials");
    std::map<std::string, renutworld::MaterialSettings> materials(list.begin(), list.end());
    if (!out.parent_path().empty()) fs::create_directories(longPath(out.parent_path()));
    writeText(out, renutworld::materialsJson(materials, out));
    print("wrote %s (%zu materials)\n", out.string().c_str(), list.size());
    return 0;
}

int renutVehicle(const Args& a) {
    fs::path p = a.at(0, "bundle"), in = a.at(1, "vehicle .json"), mod = a.at(2, "mod folder");
    Loaded l = loadAsset(p);
    auto& c = requireCaff(l, p);
    Bytes bp = renutvehicle::blueprint(c, readText(in));
    fs::path out = mod / "vehicles" / (in.stem().string() + ".blueprint");
    fs::create_directories(longPath(out.parent_path()));
    writeFile(out, bp);
    print("wrote %s (%u parts)\n", out.string().c_str(), unsigned(be16(&bp[8])));
    return 0;
}

// The starting weights in a mod's renut.shapes.define calls (its scripts folder), by shape table and name.
std::map<uint32_t, std::map<std::string, float>> modShapeWeights(const fs::path& scripts) {
    std::map<uint32_t, std::map<std::string, float>> out;
    std::error_code ec;
    if (!fs::is_directory(longPath(scripts), ec)) return out;
    // The text of a "key = {...}" list or a "key = \"...\"" string after `from`.
    auto field = [](const std::string& text, size_t from, const std::string& key, char open, char close) {
        const size_t k = text.find(key + " = " + open, from);
        if (k == std::string::npos) return std::string();
        const size_t start = k + key.size() + 4, end = text.find(close, start);
        return end == std::string::npos ? std::string() : text.substr(start, end - start);
    };
    for (const auto& entry : fs::directory_iterator(longPath(scripts), ec)) {
        if (entry.path().extension() != ".lua") continue;
        const std::string text = readText(entry.path());
        for (size_t at = text.find("renut.shapes.define{"); at != std::string::npos; at = text.find("renut.shapes.define{", at + 1)) {
            std::string model = field(text, at, "model", '"', '"');
            if (startsWith(model, "aid_model_")) model = model.substr(10);
            const auto table = assetdb::idFromName("aid_model_" + model);
            if (model.empty() || !table) continue;
            const auto names = split(field(text, at, "shapes", '{', '}'), ',');
            const auto weights = split(field(text, at, "weights", '{', '}'), ',');
            for (size_t i = 0; i < names.size() && i < weights.size(); ++i) {
                std::string name = trim(names[i]);
                if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
                out[*table][name] = float(std::atof(trim(weights[i]).c_str()));
            }
        }
    }
    return out;
}

// The shape keys of NutTool's models in a file (a model, or a mod's stream bundle) as a .glb: each draw
// with shape keys a mesh whose morph targets they are, named, at the mod's starting weights.
int renutShapesExport(const Args& a) {
    fs::path p = a.at(0, "file"), out = a.at(1, "out.glb");
    const std::string filter = lower(trim(a.opt("--model")));
    Loaded l = loadAsset(p);
    std::vector<std::pair<std::string, caff::Caff>> models;
    if (l.sb) {
        for (auto& e : l.sb->entries) {
            if (!caff::isCaff(e.data) || uint8_t(e.id >> 24) != 0x04) continue;
            const std::string name = ops::entryName(e);
            if (filter.empty() || lower(name).find(filter) != std::string::npos) models.emplace_back(name, caff::Caff::parse(e.data));
        }
    } else {
        models.emplace_back(p.stem().string(), requireCaff(l, p));
    }
    std::map<uint32_t, std::map<std::string, float>> weights = modShapeWeights(p.parent_path().parent_path() / "scripts");
    if (weights.empty()) weights = modShapeWeights(p.parent_path() / "scripts");

    // The binary buffer (little-endian, as glTF's is) and its views and accessors.
    Bytes bin;
    Json views = Json::array(), accessors = Json::array(), meshes = Json::array(), nodes = Json::array(), sceneNodes = Json::array();
    auto le32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) bin.push_back(uint8_t(v >> (8 * k))); };
    auto lef = [&](float f) { uint32_t v; std::memcpy(&v, &f, 4); le32(v); };
    auto view = [&](size_t start, uint32_t target) {
        Json v = Json::object();
        v["buffer"] = 0;
        v["byteOffset"] = uint64_t(start);
        v["byteLength"] = uint64_t(bin.size() - start);
        v["target"] = target;
        views.push(v);
        while (bin.size() % 4) bin.push_back(0);
        return int(views.size() - 1);
    };
    // A float accessor of `n`-component items (VEC2/VEC3), with its bounds when `bounds`.
    auto floats = [&](const std::vector<std::array<float, 3>>* v3, const std::vector<std::array<float, 2>>* v2, bool bounds) {
        const size_t start = bin.size(), n = v3 ? 3 : 2, count = v3 ? v3->size() : v2->size();
        std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (size_t i = 0; i < count; ++i)
            for (size_t k = 0; k < n; ++k) {
                const float f = v3 ? (*v3)[i][k] : (*v2)[i][k];
                lef(f);
                lo[k] = std::min(lo[k], f), hi[k] = std::max(hi[k], f);
            }
        Json acc = Json::object();
        acc["bufferView"] = view(start, 34962);
        acc["componentType"] = 5126;
        acc["count"] = uint64_t(count);
        acc["type"] = n == 3 ? "VEC3" : "VEC2";
        if (bounds) {
            Json mn = Json::array(), mx = Json::array();
            for (size_t k = 0; k < n; ++k) mn.push(double(lo[k])), mx.push(double(hi[k]));
            acc["min"] = mn;
            acc["max"] = mx;
        }
        accessors.push(acc);
        return int(accessors.size() - 1);
    };

    size_t draws = 0;
    for (auto& [name, c] : models) {
        size_t k = 0;
        for (size_t i = 0; i < c.symbols.size(); ++i) {
            for (const blockmodel::ShapeDraw& d : blockmodel::readShapes(c.symbolData(i))) {
                if (d.positions.empty() || d.indices.empty()) continue;
                Json attributes = Json::object();
                attributes["POSITION"] = floats(&d.positions, nullptr, true);
                attributes["NORMAL"] = floats(&d.normals, nullptr, false);
                if (!d.uvs.empty()) attributes["TEXCOORD_0"] = floats(nullptr, &d.uvs, false);
                const size_t indexStart = bin.size();
                for (uint16_t idx : d.indices) bin.push_back(uint8_t(idx)), bin.push_back(uint8_t(idx >> 8));
                Json indexAcc = Json::object();
                indexAcc["bufferView"] = view(indexStart, 34963);
                indexAcc["componentType"] = 5123;
                indexAcc["count"] = uint64_t(d.indices.size());
                indexAcc["type"] = "SCALAR";
                accessors.push(indexAcc);
                const int indexAccessor = int(accessors.size() - 1);
                Json targets = Json::array(), targetNames = Json::array(), targetWeights = Json::array();
                const auto set = weights.find(d.table);
                for (size_t s = 0; s < d.deltas.size(); ++s) {
                    std::vector<std::array<float, 3>> pos, nrm;
                    for (const blockmodel::ShapeDelta& x : d.deltas[s]) pos.push_back(x.position), nrm.push_back(x.normal);
                    Json t = Json::object();
                    t["POSITION"] = floats(&pos, nullptr, true);
                    t["NORMAL"] = floats(&nrm, nullptr, false);
                    targets.push(t);
                    targetNames.push(d.names[s]);
                    float w = 0;
                    if (set != weights.end())
                        if (auto it = set->second.find(d.names[s]); it != set->second.end()) w = it->second;
                    targetWeights.push(double(w));
                }
                Json prim = Json::object();
                prim["attributes"] = attributes;
                prim["indices"] = indexAccessor;
                prim["targets"] = targets;
                Json prims = Json::array();
                prims.push(prim);
                Json mesh = Json::object(), extras = Json::object();
                const std::string meshName = assetdb::baseAssetName(name) + "_draw" + std::to_string(k++);
                mesh["name"] = meshName;
                mesh["primitives"] = prims;
                mesh["weights"] = targetWeights;
                extras["targetNames"] = targetNames;
                mesh["extras"] = extras;
                meshes.push(mesh);
                Json node = Json::object();
                node["name"] = meshName;
                node["mesh"] = uint64_t(meshes.size() - 1);
                nodes.push(node);
                sceneNodes.push(uint64_t(nodes.size() - 1));
                ++draws;
            }
        }
    }
    if (!draws) fail(p.string() + (filter.empty() ? "" : " (models named with " + filter + ")") + " has no draws with shape keys");

    Json gltf = Json::object(), asset = Json::object(), buffer = Json::object(), buffers = Json::array(), scene = Json::object(),
         scenes = Json::array();
    asset["version"] = "2.0";
    asset["generator"] = "nuttool renut-shapes-export";
    buffer["byteLength"] = uint64_t(bin.size());
    buffers.push(buffer);
    scene["nodes"] = sceneNodes;
    scenes.push(scene);
    gltf["asset"] = asset;
    gltf["scene"] = 0;
    gltf["scenes"] = scenes;
    gltf["nodes"] = nodes;
    gltf["meshes"] = meshes;
    gltf["accessors"] = accessors;
    gltf["bufferViews"] = views;
    gltf["buffers"] = buffers;

    // The .glb: its header, the JSON chunk (padded with spaces) and the binary chunk.
    std::string text = gltf.dump(0);
    while (text.size() % 4) text += ' ';
    Bytes glb;
    auto put = [&glb](uint32_t v) { for (int k = 0; k < 4; ++k) glb.push_back(uint8_t(v >> (8 * k))); };
    put(0x46546C67);  // "glTF"
    put(2);
    put(uint32_t(12 + 8 + text.size() + 8 + bin.size()));
    put(uint32_t(text.size()));
    put(0x4E4F534A);  // "JSON"
    glb.insert(glb.end(), text.begin(), text.end());
    put(uint32_t(bin.size()));
    put(0x004E4942);  // "BIN"
    glb.insert(glb.end(), bin.begin(), bin.end());
    if (!out.parent_path().empty()) fs::create_directories(longPath(out.parent_path()));
    writeFile(out, glb);
    print("wrote %s (%zu draws with shape keys)\n", out.string().c_str(), draws);
    return 0;
}

int objparamsImport(const Args& a) {
    fs::path p = a.at(0, "bundle"), in = a.at(1, "objparams .txt or folder");
    std::vector<fs::path> files;
    if (fs::is_directory(in)) {
        for (auto& e : fs::directory_iterator(in))
            if (e.is_regular_file() && lower(e.path().extension().string()) == ".txt") files.push_back(e.path());
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(in);
    }
    Loaded l = loadAsset(p);
    auto& c = requireCaff(l, p);
    size_t changed = 0;
    for (auto& f : files) {
        std::string name = f.stem().string();
        if (!startsWith(name, "aid_objparams_")) fail(f.filename().string() + ": the file must be named after its asset (aid_objparams_...)");
        std::string text = readText(f);
        bool found = false;
        for (size_t i = 0; i < c.symbols.size(); ++i) {
            if (assetdb::baseAssetName(c.symbolName(i)) != name || !validSection(c, c.symbols[i]) || c.sections[c.symbols[i].section - 1].name != ".data") continue;
            found = true;
            Bytes old = c.symbolData(i), data;
            try {
                data = objparams::fromText(old, text);
            } catch (std::exception& e) {
                fail(f.filename().string() + ": " + e.what());
            }
            if (data != old) {
                c.replaceSymbolData(i, data);
                ++changed;
            }
        }
        if (!found) fail(name + " is not in " + p.filename().string());
    }
    if (!changed) {
        print("nothing changed: the %zu file(s) match %s\n", files.size(), p.string().c_str());
        return 0;
    }
    fs::path out = prepareOutput(p, a.opt("-o"));
    saveCaff(out, c, a.flag("--xctd"));
    print("applied %zu file(s), %zu symbol(s) changed; wrote %s\n", files.size(), changed, out.string().c_str());
    return 0;
}

int fontExport(const Args& a) {
    fs::path p = a.at(0, "font .sbp"), dir = a.at(1, "output dir");
    auto f = font::Font::parse(readFile(p));
    font::exportTo(f, dir);
    print("exported %zu glyphs to %s\n", f.glyphs.size(), dir.string().c_str());
    return 0;
}

int fontImport(const Args& a) {
    fs::path dir = a.at(0, "exported dir"), out = a.at(1, "output .sbp");
    std::optional<font::Font> base;
    if (!a.opt("--base").empty()) base = font::Font::parse(readFile(a.opt("--base")));
    auto f = font::importFrom(dir, base ? &*base : nullptr);
    writeFile(out, f.serialize());
    print("wrote %zu glyphs to %s\n", f.glyphs.size(), out.string().c_str());
    return 0;
}

int xmvInfo(const Args& a) {
    fs::path p = a.at(0, "file");
    Bytes d = readFile(p);
    print("%s", media::describeAsf(d).c_str());
    if (a.pos.size() > 1) {
        writeFile(a.pos[1], d);
        print("copied to %s (plain WMV/ASF)\n", a.pos[1].c_str());
    }
    return 0;
}

int waveExport(const Args& a) {
    fs::path p = a.at(0, "file"), dir = a.at(1, "output dir");
    bool wav = a.flag("--wav");
    if (wav && !audio::haveDecoder()) print("warning: this build cannot decode XMA (no FFmpeg); those waves stay .xma\n");
    std::string only = lower(a.opt("--name"));
    Loaded l = loadAsset(p);
    int banks = 0, decoded = 0;
    if (l.kind == FileKind::WaveBank) {
        decoded += ops::exportWaveBank(l.raw, dir, p.stem().string(), wav);
        ++banks;
    } else if (l.sb) {
        for (auto& e : l.sb->entries) {
            if (!media::isWaveBank(e.data)) continue;
            std::string name = ops::entryName(e);
            if (!only.empty() && lower(name).find(only) == std::string::npos) continue;
            decoded += ops::exportWaveBank(e.data, dir, hex32(e.id) + "_" + name, wav);
            ++banks;
        }
    } else {
        fail("no wave banks in " + p.string());
    }
    print("exported %d wave bank(s) to %s", banks, dir.string().c_str());
    if (wav) print(", %d wave(s) decoded to WAV", decoded);
    print("\n");
    return 0;
}

int iniExport(const Args& a) {
    Bytes d = readFile(a.at(0, "ini"));
    Bytes out = d.size() >= 2 && d[0] == 0xFE && d[1] == 0xFF ? ops::precacheToUtf8(d) : d;
    writeFile(a.at(1, "output"), out);
    print("wrote %s\n", a.pos[1].c_str());
    return 0;
}

int iniImport(const Args& a) {
    std::string text = readText(a.at(0, "text file"));
    fs::path out = a.at(1, "output .ini");
    if (a.flag("--utf16")) writeFile(out, ops::utf8ToPrecache(text));
    else writeText(out, text);
    print("wrote %s\n", out.string().c_str());
    return 0;
}

int extractAll(const Args& a) {
    fs::path root = a.at(0, "assets dir"), out = a.at(1, "output dir");
    ops::ExtractOptions opt;
    opt.png = a.flag("--png");
    if (!a.opt("--only").empty()) for (auto& s : split(a.opt("--only"), ',')) opt.only.insert(trim(s));
    opt.texturesOnly = a.flag("--textures-only");
    opt.texturesDds = a.flag("--dds");
    opt.filter = a.opt("--filter");
    int failures = ops::extractAll(root, out, opt, consoleProgress());
    print("extract-all finished with %d error(s); output in %s\n", failures, out.string().c_str());
    return failures ? 1 : 0;
}

int verify(const Args& a) {
    ops::Progress pr;
    pr.log = [](const std::string& s) { print("%s\n", s.c_str()); };
    auto next = std::make_shared<int>(0);
    pr.step = [next](double f, const std::string& label) {
        int pct = int(f * 100);
        if (pct >= *next) { print("  ... %d%% (%s)\n", pct, label.c_str()); std::fflush(stdout); *next = pct + 10; }
    };
    ops::VerifyResult r = ops::verifyAll(a.at(0, "assets dir"), pr);
    print("verify: %d passed, %d failed\n", r.passed, r.failed);
    return r.failed ? 1 : 0;
}

int makeNbpatch(const Args& a) {
    fs::path root = a.at(0, "game dir"), out = a.at(1, "out.nbpatch");
    nbpatch::Info info;
    info.name = a.opt("--name");
    info.id = a.opt("--id");
    info.version = a.opt("--version", "1");
    info.author = a.opt("--author");
    info.category = a.opt("--category");
    info.description = a.opt("--description");
    if (info.name.empty()) fail("make-nbpatch needs --name");
    ops::Progress pr;
    pr.log = [](const std::string& s) { print("%s\n", s.c_str()); };
    auto changes = nbpatch::findChanges(root, a.opt("--original"), pr);
    // The title update's edits go in as update/<file>, which reNut serves as UPDATE: files.
    fs::path update = ops::updateFolder(root, a.opt("--update"));
    fs::path originalUpdate = a.opt("--original").empty() ? fs::path() : ops::updateFolder(a.opt("--original"));
    size_t before = changes.size();
    nbpatch::addUpdateChanges(changes, root, update, originalUpdate, pr);
    if (changes.size() > before) print("title update (%s): %zu changed file(s)\n", update.string().c_str(), changes.size() - before);
    if (changes.empty()) {
        print("no changed files found (%s)\n", a.opt("--original").empty() ? "no <file>.bak backups differ from their files" : "the folders match");
        return 1;
    }
    print("%zu changed file(s):\n", changes.size());
    if (!nbpatch::write(out, info, changes, pr)) return 1;
    print("wrote %s\n", out.string().c_str());
    return 0;
}

int texRemoveMipsChanged(const Args& a) {
    fs::path root = a.at(0, "game dir");
    ops::Progress pr;
    pr.log = [](const std::string& s) { print("%s\n", s.c_str()); };
    auto changes = nbpatch::findChanges(root, a.opt("--original"), pr);
    // The title update's files have their own backups beside them.
    if (a.opt("--original").empty())
        for (auto& c : nbpatch::findChanges(ops::updateFolder(root, a.opt("--update")), {}, pr)) changes.push_back(c);
    int textures = 0, files = 0;
    for (auto& c : changes) {
        int n = ops::removeChangedMipmaps(c.modified, c.original, pr);
        textures += n;
        files += n ? 1 : 0;
    }
    print("removed the mipmaps of %d changed texture(s) in %d file(s) (%zu changed file(s) looked at)\n", textures, files, changes.size());
    return 0;
}

int menuFix(const Args& a) {
    fs::path root = a.at(0, "game dir");
    ops::Progress pr;
    pr.log = [](const std::string& s) { print("%s\n", s.c_str()); };
    fs::path update = ops::updateFolder(root, a.opt("--update"));
    if (!a.opt("--update").empty() && update.empty()) fail("no folder " + a.opt("--update"));
    if (!update.empty()) print("title update: %s\n", update.string().c_str());
    auto resized = ops::resizedTextures(root, pr, update);
    print("%zu texture(s) differ in size from their backups\n", resized.size());
    std::vector<fs::path> bundles;
    for (size_t i = 1; i < a.pos.size(); ++i) bundles.push_back(a.pos[i]);
    if (bundles.empty()) bundles = ops::menuBundles(root, pr, update);
    int elements = 0;
    for (auto& b : bundles) elements += ops::adaptMenus(b, resized, pr);
    print("adjusted %d menu element(s) in %zu bundle(s) with menus\n", elements, bundles.size());
    return 0;
}

// ---- command table -----------------------------------------------------------

const std::vector<Command>& commands() {
    static const std::vector<Command> cmds = {
        {"info", "<file>", "Identify any asset file and summarise its contents", info},
        {"list", "<file>", "List symbols of a CAFF/bundle, entries of a stream bundle or waves of a wave bank", list},
        {"hash", "<asset name>...", "Compute asset ids (and on-disk paths) for aid_ names", hash},
        {"index", "<assets dir> [--json]", "List every asset in Debug\\db_index.txt with its id and file", index},
        {"verify", "<assets dir>", "Round-trip every container to prove lossless parsing", verify},
        {"extract-all", "<assets dir> <out dir> [--png] [--only bundles,streams,text,video,fonts,ini,misc] [--textures-only [--dds]] [--filter text]",
         "Unpack everything into editable files. --textures-only writes just the textures, as PNG in one folder; --filter keeps only "
         "what is named with the text (ui_, say)", extractAll},

        {"decompress", "<in> <out>", "Remove XCTD compression from a bundle file", decompress},
        {"compress", "<in> <out>", "Wrap a file in an (uncompressed) XCTD container", compress},
        {"caff-unpack", "<file> <dir>", "Unpack a CAFF/bundle losslessly (caff.json + one file per symbol)", caffUnpack},
        {"caff-pack", "<dir> <file> [--xctd]", "Rebuild a CAFF from caff-unpack output (edited/resized symbols are applied)", caffPack},
        {"sb-unpack", "<file> <dir>", "Unpack a stream bundle into its entries", sbUnpack},
        {"sb-pack", "<dir> <file>", "Rebuild a stream bundle from sb-unpack output", sbPack},
        {"extract", "<file> <#index|name> <out> [--section .gpu]", "Extract one symbol (or stream bundle entry)", extract},
        {"replace", "<file> <#index|name> <data> [-o out] [--section .gpu] [--xctd]", "Replace one symbol or stream bundle entry", replace},

        {"tex-export", "<file> <dir> [--stream <stream bundle>] [--png|--png-only]", "Export textures as DDS (and PNG)", texExport},
        {"tex-import", "<file> <texture name> <image.png|jpg|bmp|tga|dds> [-o out] [--stream <sb> [--stream-out out]]",
         "Replace a texture; an image of another size resizes it. --stream also replaces the streamed top level", texImport},
        {"tex-remove-mips", "<file> <texture name> [-o out]",
         "Remove a texture's mipmaps (a split texture keeps level 1 beside its streamed level 0)", texRemoveMips},
        {"tex-remove-mips-changed", "<game dir> [--original <clean game dir>] [--update <title update dir>]",
         "Remove the mipmaps of every changed texture: those that differ from the file's .bak backup (the title update's too), or from --original",
         texRemoveMipsChanged},
        {"menu-fix", "<game dir> [bundle...] [--update <title update dir>]",
         "Keep menu images the same size on screen when their textures were resized (compared with the .bak backups); "
         "without bundles, every bundle with menus is found and fixed, the title update's too (<game dir>\\..\\update, or --update)",
         menuFix},
        {"model-export", "<file> <dir> [--all-groups] [--png]",
         "Export models as OBJ + MTL with a bone list; --all-groups includes every LOD and alternative part, --png writes the file's textures",
         modelExport},
        {"model-import", "<file> <model name> <in.obj|in.fbx|in.glb> [--rig] [-o out] [--xctd]",
         "Apply an OBJ, binary FBX or skinned .glb to a model: objects named like the export (group0_mesh3) edit that mesh in place when vertex and "
         "triangle counts match, otherwise replace its geometry (skinning from the FBX's weights on bones named like the model's, else "
         "copied from the nearest original vertex); an object without faces removes the mesh. --rig also moves the model's joints to the "
         "FBX's bones of the same names (orientations, hierarchy and bone count stay); the file may then hold only the skeleton",
         modelImport},
        {"model-swap", "<file> <model name> <source file> <source model> [-o out] [--stream-out out] [--xctd]",
         "Replace a model with a copy of another one (from the same or another bundle); the name stays, so the game loads it in its place. "
         "Textures it needs that the bundle lacks are copied in from the source bundle, their streamed levels into the paired stream bundle",
         modelSwap},
        {"model-fbx",
         "<file> <model name> <out.fbx> [--all-groups] [--no-textures] [--no-anims] [--anims <bundle>] [--all-anims] [--no-fur] "
         "[--fur-length pct] [--fur-tiling n] [--fur-size px] [--no-tint] [--reorient-bones] [--bone-axis X|Y|Z|-X|-Y|-Z] [--bone-ends] [--bone-end-size pct]",
         "Export a model as FBX: meshes skinned to its skeleton, materials with embedded PNG textures (tinted by their material's "
         "colour gain and offset as the game draws them; --no-tint keeps them as stored), bones in the game's frames (--reorient-bones "
         "turns each so --bone-axis, default Y as Blender uses, runs along it to its child; --bone-ends gives each childless bone a "
         "short <bone>_end child, percent of the skeleton's size, default 1.5, so Blender draws it as a stub), its animations as takes "
         "(those named after it, from the same bundle or --anims; --all-anims for every animation there), and its fur as skinned "
         "shell meshes with embedded baked alpha textures (--no-fur leaves it out; length in percent of the model's radius, default "
         "1.5; tiling default 6; size default 512)",
         modelFbx},
        {"model-obj",
         "<file> <model name> <out.obj> [--all-groups] [--no-textures] [--no-fur] [--fur-length pct] [--fur-tiling n] [--fur-size px] "
         "[--no-tint]",
         "Export a model as OBJ with the same meshes, materials, tinted textures (PNG files) and fur shells as model-fbx; for the "
         "skeleton, skin and animations use model-fbx",
         modelFbx},
        {"anim-import", "<file> <animation name> <keys.json> [-o out] [--xctd]",
         "Give an animation new motion from a JSON timeline of poses (bone rotations in Euler degrees, offsets in metres); bones "
         "without keys keep its own motion, and its length and frame count stay (see reNut's docs/nuttool/animations.md)",
         animImport},
        {"actor-list", "<assets dir> [--actor name] [--update <title update dir>]",
         "List the game's actors (objparams with a model) with their model and animation table; with --actor, that actor's "
         "table entries and the animations it can lend another actor",
         actorList},
        {"actor-swap",
         "<assets dir> <target actor> <source actor | rigged.glb> [--scale s] [--plan plan.json] [--events keep|source|none] [--update dir]",
         "Replace an actor with another (banjotux npc_klungo, say), or with one of your own from a rigged .glb (its skinned meshes "
         "on its own skeleton, its textures and its animations, --scale times its size): the target, and every actor sharing its "
         "animation table, draws the new model (with what it needs copied in), and the table plays the new animations, which "
         "are copied into its bundles. --plan's \"anims\" {entry: animation} picks one per entry (an aid_anim_ name, or the "
         ".glb's animation name); the others take the one of the same name, else a stand or idle. --events: the entries keep "
         "the target's animation events (footsteps, hits), take the source's, or have none. Bundles are written in place "
         "after a one-time .bak",
         actorSwap},
        {"make-nbpatch",
         "<game dir> <out.nbpatch> --name <name> [--original <clean game dir>] [--update <title update dir>] [--version v] [--author a] "
         "[--category c] [--description d] [--id id]",
         "Package edited files as a mod for reNut / NB Studio: files with a .bak backup, or every file that differs from --original. "
         "The title update's edited files (<game dir>\\..\\update, or --update) go in as update/<file>, which reNut serves as UPDATE: files",
         makeNbpatch},
        {"text-export", "<loctext file> <out.json>", "Export a string table to UTF-8 JSON", textExport},
        {"text-import", "<loctext file> <in.json> [-o out] [--prune]", "Apply edited/added strings from JSON", textImport},
        {"gamescript-export", "<bundle> <dir> [--name text] [--assets dir]",
         "Write the bundle's game scripts (scene scripts, actor goals, script and state tables) as editable text, one <asset>.txt each",
         gamescriptExport},
        {"gamescript-import", "<bundle> <file.txt|dir> [-o out] [--xctd]",
         "Apply edited game script text (files named <asset>.txt, as gamescript-export writes them)", gamescriptImport},
        {"objparams-export", "<bundle> <dir> [--parts] [--name text] [--assets dir]",
         "Write the bundle's object parameters as editable text, one <asset>.txt each; --parts keeps the vehicle parts (named fields: mass, health, ratings...)",
         objparamsExport},
        {"objparams-import", "<bundle> <file.txt|dir> [-o out] [--xctd]",
         "Apply edited objparams text (files named <asset>.txt); fields not listed keep their values", objparamsImport},
        {"renut-part",
         "<bundle> <part> <mod folder> --id <id> [--name n] [--count n] [--fields edited.txt] [--behaviour] "
         "[--model mesh.obj|fbx|glb [--mesh node --into 8,0,17,25 [--flat u,v[,u2,v2]]] [--template model] [--collision model]] "
         "[--attachable all|none|top|bottom|sides|top+sides|-x,-y,+z...] [--bundle name] "
         "[--mod-name n] [--author a] [--force] [--assets dir]",
         "Make a new vehicle part for reNut from a shipped one (wheels_monster), leaving the shipped part as it is: a RexFabric Lua mod "
         "(<mod folder>/scripts/<id>.lua and mod.toml) calling renut.parts.add. --fields takes objparams text, as objparams-export "
         "writes it, and what differs becomes the part's fields; --behaviour adds every callback, to make a new kind of part; "
         "--model gives it a model of its own: the base's (or --template's) model with the file's meshes (named as model-export "
         "writes them), in the mod's stream bundle (<mod folder>/bundles, named after the folder or --bundle), keeping the "
         "template's collision (or --collision's, another part model: body_light_wedge); from a .glb, --mesh names the node "
         "taken and --into the template's meshes that get it (the rest are removed), --flat one spot of their textures "
         "for every vertex (a flat colour); --attachable gives it a footprint of "
         "its own, the base's with only those faces taking other parts (top: it hangs under a block; or a list of "
         "directions); with either, an existing script is kept unless --force",
         renutPart},
        {"renut-block",
         "<mod folder> --id <id> --model file.glb --mesh <node> [--name n] [--shape cube|wedge|corner] "
         "[--attachable all|none|top|bottom|sides|top+sides|-x,-y,+z...] [--category body] [--type t] [--mass m] [--health h] "
         "[--count n] [--bundle name] [--shapes name=weight,...] [--force]",
         "Make a block of its own for reNut, from scratch (nothing taken from the game): a model of the .glb node's mesh (with "
         "its shape keys, the mesh's morph targets: --shapes sets their starting weights, Lua part:set_shape them), drawn "
         "in one flat paint colour by reNut's own part shaders; a footprint whose --attachable faces take other parts and which "
         "collides as --shape; and "
         "<mod folder>/scripts/<id>.lua calling renut.parts.add without a base, listing it in --category (and the garage type "
         "--type, shared by every block naming it). An existing script is kept unless --force",
         renutBlock},
        {"renut-world",
         "<assets dir> <mod folder> --id <id> --model file.glb|file.gltf|file.obj|file.dae[;more.dae...] [--name n] [--spawn x,y,z] [--spawn-yaw deg] "
         "[--exit x,y,z] [--exit-yaw deg] [--door x,y,z] [--door-yaw deg] [--door-style wall|arch|palace|hidden] [--no-door] [--no-exit] [--smooth deg] [--scale s] [--colour-scale s] [--max-texture side] "
         "[--music file.ogg|mp3|flac|wav] [--music-volume 0-1] [--stats jiggies=N,trophies=N,jinjos=N,notes=N] [--water name,...] [--shapes name=weight,...] "
         "[--materials materials.json] "
         "[--ground size] [--ground-colour rrggbb] [--sky panorama.png|jpg|rrggbb,rrggbb] [--hologram image.png|jpg] "
         "[--npcs npcs.json] [--player-model id] [--bundle name] [--force]",
         "Make a world of its own for reNut from every mesh of the .glb, .gltf, .obj or .dae (with its materials' colours and textures, "
         "times its vertex colours, those times --colour-scale: 2 for 3DS rips; skinned meshes at their rest pose): its "
         "model (a big world in several chunks, each a model and collision of at most 65535 places), textures at most "
         "--max-texture pixels a side (default 1024), its "
         "model, drawn by reNut's world shaders, one draw per texture, and its textures (a .glb's base colour textures, an OBJ's "
         "map_Kd, a .dae's diffuse textures, as DXT1/DXT5 with every mip level; texels under half alpha are cut out); --sky "
         "gives it a sky around the camera, behind everything and unlit: a 360-degree panorama (equirectangular) or a "
         "gradient from the first colour overhead to the second at the horizon; its collision (reNut builds its MOPP as it loads); a marker with the "
         "player's start (--spawn, else 2 m above the flat ground nearest the middle) and a door home (--exit, else on flat "
         "ground 10-15 m from the start, facing it); its scene, in the mod's bundle; the world itself in "
         "<mod folder>/worlds; and <mod folder>/scripts/renutworld_<id>.lua declaring it and its door in Showdown Town "
         "(--door). --door-style picks the door: wall (default; the world door, one-sided, so it stands against a wall), "
         "arch (the stone arch home, two-sided, so it stands anywhere; a copy goes in the mod's bundle), palace (L.O.G.'s "
         "palace door) or hidden (no model: a trigger in a doorway already there). --stats sets the totals its door's "
         "stats panel shows (the rest, and what the player has, reNut counts from the world's challenges). Objects named "
         "with _water, and those whose object, material or texture name holds a --water name, become water, not model "
         "or collision: flat water at one height is a body (up to 8, the largest kept), the surface at its top, over "
         "its area seen from above (its shape a mask), with the car park's calm water; sloped water (falls) stays "
         "drawn but not solid. A .glb/.gltf's shape keys (morph targets) come along: reNut blends them by the "
         "weights Lua sets (renut.worlds.set_shape), --shapes giving their starting ones (scripts/renutworld_<id>_shapes.lua). "
         "Each material keeps its look in reNut: alpha (opaque, cut out, blended or added), emissive colour, strength and "
         "texture, glow, double-sided and unlit, from the file (glTF alphaMode, emissive, doubleSided; .dae emission and "
         "transparency; .mtl Ke and d) with --materials' settings over them by name (renut-materials writes a model's to "
         "edit); Lua changes emissive, glow and opacity while played (renut.worlds.set_material). "
         "--hologram gives a wall or arch door "
         "a look of its own: a copy of the door showing the image (cropped to fit) as its hologram, in the mod's bundle. --npcs adds characters of its own from a JSON file (see "
         "README): each one's skinned model on an automatic skeleton with a looping idle, its actor, dialogs and a \"reach "
         "the gate\" challenge it gives (notes, a jiggy, a T.T. trophy), placed by the world's marker and scene, and "
         "<mod folder>/scripts/renutnpc_<world>_<npc>.lua giving reNut their text and voice (written anew each time; their "
         "sound files are copied to <mod folder>/sounds/<npc>). --player-model gives the script player_model, so reNut "
         "makes Banjo with that renut-player-model there. An existing "
         "world script is kept unless --force",
         renutWorld},
        {"graph-lua", "<mod>/graphs/<name>.json [out.lua]",
         "Make a script graph (the GUI's reNut > Script graphs editor) into its Lua: <mod>/scripts/<name>.lua unless given; "
         "what keeps it from working as drawn is listed as warnings",
         graphLua},
        {"renut-materials", "<model.glb|gltf|obj|dae> <out.json>",
         "Write a model's materials as a materials file (renut-world --materials): each one's alpha (opaque, cutout, blend, "
         "add), cutoff, opacity, emissive colour and strength, glow, double-sided and unlit as the model gives them, to edit",
         renutMaterials},
        {"renut-player-model", "<assets dir> <mod folder> <model.fbx|glb|obj> --id <id> [--template actor_banjo|actor_banjotuxedo] [--bundle name]",
         "Make a player model for reNut: a copy of Banjo's model (--template; by default the one a model-fbx export of "
         "it names) with the file's meshes applied as model-import does (objects named like the export's meshes; two "
         "naming one mesh, as Blender's .001 copies, are joined; meshes the model draws by default that the file left out "
         "are removed), as aid_model_renut_player_<id> in the mod's bundle. "
         "Lua makes Banjo with it: renut.player.set_model(\"<id>\"), or player_model = \"<id>\" in renut.worlds.add",
         renutPlayerModel},
        {"renut-characters", "<assets dir> <reNut mods folder> [--only character,...]",
         "Make reNut's character packs: for each character of the game (objparams with a model in an actor bundle), or "
         "those --only names (npc_cpu_klungo, say), what it needs that its own bundles lack (its model, textures and the "
         "rest only world bundles hold), copied into <mods folder>/.characters/Bundle/50/<id of renutchar_<character>>. "
         "reNut loads a character's pack to spawn it in any world, in place of the whole world bundles",
         renutCharacters},
        {"renut-shapes-export", "<model or stream bundle> <out.glb> [--model text]",
         "Write the shape keys of NutTool's models (worlds, characters, blocks) as a .glb: each draw that has them a mesh with "
         "them as named morph targets (Blender's shape keys), at the starting weights the mod's Lua gives them; from a stream "
         "bundle, every model in it, or those named with --model's text. Skinned models come out in their rest pose, unskinned",
         renutShapesExport},
        {"renut-vehicle", "<bundle> <vehicle.json> <mod folder>",
         "Make a vehicle for a reNut mod: <mod folder>/vehicles/<name>.blueprint from a vehicle file (its name and parts: "
         "\"part\", \"cell\": [x, y, z], \"rotation\": [x, y, z] in degrees, \"colour\": \"rrggbb\", \"button\"; a reNut part "
         "also names its \"base\"), which reNut adds to the player's saved vehicles once",
         renutVehicle},
        {"font-export", "<font.sbp> <dir>", "Export a glyph cache to PNG pages + font.json, and as a font: <dir name>.ttf and font.fnt (BMFont)", fontExport},
        {"font-import", "<dir> <out.sbp> [--base original.sbp]", "Rebuild a glyph cache from exported pages", fontImport},
        {"xmv-info", "<file> [copy.wmv]", "Show movie streams (XMV files are plain WMV/ASF)", xmvInfo},
        {"wave-export", "<stream bundle|xwb> <dir> [--wav] [--name text]",
         "Export XACT wave banks and their waves (.xma as stored, or decoded to .wav with --wav); --name keeps banks whose name contains text",
         waveExport},
        {"ini-export", "<file.ini> <out.txt>", "Convert font metadata (UTF-16BE precache lists become UTF-8)", iniExport},
        {"ini-import", "<in.txt> <out.ini> [--utf16]", "Write font metadata back (--utf16 for precache lists)", iniImport},
        {"script", "<file.lua> [--assets dir] [script arguments...]",
         "Run a Lua script (see reNut's docs/nuttool/cli.md); arguments after the file go to the script's `arg` table", nullptr},
    };
    return cmds;
}

namespace {

Args parseArgs(const std::vector<std::string>& argv) {
    static const std::set<std::string> valued = {"-o", "--out", "--stream", "--stream-out", "--base", "--section", "--only",
                                                "--name", "--id", "--version", "--author", "--category", "--description", "--original", "--anims", "--filter",
                                                "--assets", "--update", "--fur-length", "--fur-tiling", "--fur-size", "--bone-axis", "--bone-end-size",
                                                "--count", "--fields", "--mod-name", "--model", "--template", "--bundle",
                                                "--attachable", "--mesh", "--into", "--collision", "--flat", "--shape", "--category",
                                                "--type", "--mass", "--health", "--spawn", "--spawn-yaw", "--exit", "--exit-yaw",
                                                "--door", "--door-yaw", "--door-style", "--smooth", "--scale", "--ground", "--ground-colour",
                                                "--hologram", "--sky", "--colour-scale", "--npcs", "--max-texture", "--music", "--music-volume", "--stats", "--water", "--shapes",
                                                "--materials", "--player-model", "--plan", "--events", "--actor"};
    Args a;
    for (size_t i = 1; i < argv.size(); ++i) {
        std::string s = argv[i];
        if (valued.count(s)) {
            if (i + 1 >= argv.size()) fail("option " + s + " needs a value");
            a.opts[s == "--out" ? "-o" : s] = argv[++i];
        } else if (s.size() > 1 && s[0] == '-' && !(s.size() > 1 && std::isdigit(uint8_t(s[1])))) {
            a.flags.insert(s);
        } else {
            a.pos.push_back(s);
        }
    }
    return a;
}


} // namespace

void usage() {
    print("nuttool - Banjo-Kazooie: Nuts & Bolts asset tool\n\nusage: nuttool <command> [arguments]\n\n");
    for (auto& cmd : commands()) print("  %-12s %s\n  %12s   %s\n", cmd.name, cmd.args, "", cmd.help);
    print("\nFiles that are modified in place get a one-time <file>.bak backup unless -o is given.\n");
}

// nuttool script <file.lua> [--assets dir] [args...]: the script's arguments are passed through as given.
int runScript(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        print("usage: nuttool script <file.lua> [--assets dir] [script arguments...]\n");
        return 1;
    }
    script::Options opt;
    size_t i = 2;
    if (i + 1 < args.size() && args[i] == "--assets") {
        opt.assets = fs::path(std::u8string(reinterpret_cast<const char8_t*>(args[i + 1].c_str())));
        i += 2;
    }
    opt.args.assign(args.begin() + std::ptrdiff_t(i), args.end());
    opt.scriptDir = fs::absolute(fs::path(std::u8string(reinterpret_cast<const char8_t*>(args[1].c_str())))).parent_path();
    opt.output = [](const std::string& s) { print("%s", s.c_str()); };
    std::string code;
    try {
        code = readText(args[1]);
    } catch (std::exception& e) {
        print("error: %s\n", e.what());
        return 1;
    }
    return script::run(code, args[1], opt) ? 0 : 1;
}

int run(const std::vector<std::string>& args) {
    if (args.empty()) {
        usage();
        return 1;
    }
    if (args[0] == "script") return runScript(args);
    if (args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        usage();
        return 0;
    }
    for (auto& cmd : commands()) {
        if (args[0] != cmd.name) continue;
        try {
            return cmd.fn(parseArgs(args));
        } catch (std::exception& e) {
            print("error: %s\n", e.what());
            return 1;
        }
    }
    print("unknown command '%s' (run 'nuttool help')\n", args[0].c_str());
    return 1;
}

} // namespace nut::cli
