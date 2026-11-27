#include "renutmodel.hpp"

#include "assetdb.hpp"
#include "caff.hpp"
#include "gltfobj.hpp"
#include "modelimport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <cstdio>
#include <ctime>

namespace nut::renutmodel {
namespace {

uint32_t assetId(const std::string& name) {
    auto id = assetdb::idFromName(name);
    if (!id) fail("no asset id for " + name);
    return *id;
}

std::vector<streambundle::IndexEntry> streamIndex(const fs::path& stream) {
    std::vector<streambundle::IndexEntry> out;
    if (!streambundle::readIndex(stream, out)) fail(stream.string() + " is not a stream bundle");
    return out;
}

std::optional<caff::Caff> entryCaff(const std::vector<streambundle::IndexEntry>& idx, const fs::path& stream, uint32_t id) {
    for (auto& e : idx) {
        if (e.id != id || !e.size) continue;
        Bytes d = streambundle::readEntry(stream, e);
        if (caff::isCaff(d)) return caff::Caff::parse(d);
    }
    return std::nullopt;
}

caff::Caff requireEntry(const std::vector<streambundle::IndexEntry>& idx, const fs::path& stream, const std::string& asset) {
    auto c = entryCaff(idx, stream, assetId(asset));
    if (!c) fail(asset + " is not in " + stream.string() + " (is the template a part model of the game?)");
    return *c;
}

// Renames asset `from` to `to`: its symbol name (with any ",timestamp,version" kept) and the debug
// name that ends its .stream part.
void rename(caff::Caff& c, const std::string& from, const std::string& to) {
    for (auto& n : c.symbolNames) {
        if (n == from) n = to;
        else if (startsWith(n, from + ",")) n = to + n.substr(from.size());
    }
    const std::string oldTail = from + '\0';
    for (size_t i = 0; i < c.symbols.size(); ++i) {
        if (assetdb::baseAssetName(c.symbolName(i)) != to || c.sectionOf(c.symbols[i]).name != ".stream") continue;
        Bytes d = c.symbolData(i);
        if (d.size() < oldTail.size()) continue;
        // The name and its terminator, then at most a few bytes of padding.
        for (size_t at = d.size() - oldTail.size() + 1; at-- > 0 && at + oldTail.size() + 4 >= d.size();) {
            if (std::memcmp(&d[at], oldTail.data(), oldTail.size()) != 0) continue;
            d.resize(at);
            d.insert(d.end(), to.begin(), to.end());
            d.push_back(0);
            c.replaceSymbolData(i, d);
            break;
        }
    }
}

// A matte material: the draws' sheen colours (specular, velvet, reflection; not their powers) set
// to black. Returns the constants turned off.
std::vector<std::string> matte(caff::Caff& c, size_t sym) {
    model::Model m = model::parse(c, sym);
    std::map<int, Bytes> edits;
    std::vector<std::string> names;
    for (auto& me : m.meshes)
        for (auto& k : me.constants) {
            const std::string n = lower(k.name);
            const bool sheen = n.find("specular") != std::string::npos || n.find("velvet") != std::string::npos ||
                               n.find("reflect") != std::string::npos;
            if (k.symbol < 0 || !sheen || n.find("power") != std::string::npos) continue;
            Bytes& d = edits.try_emplace(k.symbol, c.symbolData(size_t(k.symbol))).first->second;
            if (k.offset + 12 > d.size()) continue;
            for (uint32_t q = 0; q < 3; ++q) putBe32(&d[k.offset + 4 * q], 0);
            if (std::find(names.begin(), names.end(), k.name) == names.end()) names.push_back(k.name);
        }
    for (auto& [s, d] : edits) c.replaceSymbolData(size_t(s), d);
    return names;
}

} // namespace

std::array<bool, 6> faceRule(const std::string& attachable) {
    const std::string rule = lower(trim(attachable));
    std::array<bool, 6> on{};
    // In the face records' order: +y, +z, -y, -z, -x, +x.
    static const char* const kDirections[] = {"+y", "+z", "-y", "-z", "-x", "+x"};
    for (int k = 0; k < 6; ++k) {
        const bool up = k == 0, down = k == 2;
        on[size_t(k)] = rule == "all" || (rule == "top" && up) || (rule == "bottom" && down) || (rule == "sides" && !up && !down) ||
                        (rule == "top+sides" && !down);
    }
    if (rule == "all" || rule == "none" || rule == "top" || rule == "bottom" || rule == "sides" || rule == "top+sides") return on;
    for (auto& part : split(rule, ',')) {
        const std::string dir = trim(part);
        size_t k = 0;
        while (k < 6 && dir != kDirections[k]) ++k;
        if (k == 6) fail("--attachable is all, none, top, bottom, sides, top+sides, or directions such as -x,-y,+z");
        on[k] = true;
    }
    return on;
}

streambundle::Entry newFootprint(const std::string& id, const std::string& shape, const std::string& attachable, std::string* note) {
    // The collision cell of each shape, as the game's light body blocks have it (51 words):
    // cell, orientation, scale, shape, two values, a count, then six face entries of 6 words.
    static const std::map<std::string, std::pair<uint32_t, std::vector<uint32_t>>> kShapes = {
        {"cube", {0x3f800000, {0, 0, 0, 0, 0x3f800000, 0x3f800000, 0x3f800000, 0, 0, 0x3f5cac08, 0x3f800000, 6, 0, 0, 0,
                               0x3f1d70a4, 0x3f800000, 0x3f400000, 0, 0, 0, 0x3f1d70a4, 0x3f800000, 0x3f400000, 0, 0, 0,
                               0x3f1d70a4, 0x3f800000, 0x3f400000, 0, 0, 0, 0x3f1d70a4, 0x3f800000, 0x3f400000, 0, 0, 0,
                               0x3f1d70a4, 0x3f800000, 0x3f400000, 0, 0, 0, 0x3f1d70a4, 0x3f800000, 0x3f400000, 0, 0, 0}}},
        {"wedge", {0x3f000000, {0, 0, 0, 0, 0x3f800000, 0x3f800000, 0x3f800000, 0, 1, 0, 0x3f800000, 2, 0, 0, 0,
                                0, 0x3f800000, 0x3f2b851f, 0, 0, 0, 0, 0x3f800000, 0x3f400000, 0, 0, 0,
                                0, 0x3f800000, 0x3f400000, 0, 0, 0, 0, 0x3f800000, 0x3f400000, 0, 0, 0,
                                0, 0x3f800000, 0x3f400000, 0, 0, 0, 0, 0, 0, 0, 0, 0}}},
        {"corner", {0x3e2ab368, {0, 0, 0, 0, 0x3f800000, 0x3f800000, 0x3f800000, 0, 2, 0, 0x3f800000, 2, 0, 0, 0,
                                 0, 0x3f800000, 0x3f400000, 0, 0, 0, 0, 0x3f800000, 0x3f400000, 0, 0, 0,
                                 0, 0x3f800000, 0x3f400000, 0, 0, 0, 0, 0x3f800000, 0x3f400000, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}},
    };
    auto sh = kShapes.find(lower(trim(shape)));
    if (sh == kShapes.end()) fail("--shape is cube, wedge or corner (the collision of the game's body blocks of that shape)");
    const std::array<bool, 6> on = faceRule(attachable);
    static const float kCentres[6][3] = {{0, 0.5f, 0}, {0, 0, 0.5f}, {0, -0.5f, 0}, {0, 0, -0.5f}, {-0.5f, 0, 0}, {0.5f, 0, 0}};
    auto f32 = [](float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; };

    // Sections 1 (cells), 2 (faces) and 3 (collision cells), one cell at the origin.
    Writer w;
    w.u32(0xE);
    w.u32(1);
    w.zeros(36);            // bounds (min, max) and three words
    w.zeros(12);            // the cell
    w.u32(6);
    int open = 0;
    for (bool b : on) open += b;
    w.u32(uint32_t(open));
    for (uint32_t k = 0; k < 6; ++k) {
        w.zeros(12);
        w.u32(k);
        for (float c : kCentres[k]) w.u32(f32(c));
        w.u32(on[k] ? 1 : 0);
        w.u32(0xFFFFFFFF);  // no switch group
        w.u32(0x40000000);  // 2.0
    }
    w.u32(1);
    w.u32(sh->second.first); // the shape's share of a cell
    w.u32(0);
    for (uint32_t x : sh->second.second) w.u32(x);

    caff::Caff c;
    const std::string name = std::string(kAttachPrefix) + id;
    c.symbolNames.push_back(name);
    caff::Section s;
    s.name = ".data";
    s.alignLog2 = 4;
    s.data = w.buf();
    c.sections.push_back(s);
    c.symbols.push_back({1, 0, uint32_t(w.buf().size()), 1, 4});
    if (note) *note = name + ": a " + lower(trim(shape)) + "'s collision, " + std::to_string(open) + " of 6 faces attachable";
    return {assetId(name), c.serialize()};
}

std::optional<std::string> recordModel(const fs::path& stream, const Bytes& record) {
    if (record.size() < kRecordModel + 4) return std::nullopt;
    auto c = entryCaff(streamIndex(stream), stream, be32(&record[kRecordModel]));
    if (!c) return std::nullopt;
    for (size_t i = 0; i < c->symbols.size(); ++i) {
        std::string n = assetdb::baseAssetName(c->symbolName(i));
        if (startsWith(n, kModelPrefix)) return n.substr(kModelPrefix.size());
    }
    return std::nullopt;
}

Built build(const fs::path& stream, const std::string& templateName, const std::string& id, const Bytes& meshFile,
            const GlbMesh& glb, const std::string& collision) {
    auto idx = streamIndex(stream);
    Built b;

    const std::string model = std::string(kModelPrefix) + templateName, newModel = std::string(kModelPrefix) + id;
    caff::Caff mc = requireEntry(idx, stream, model);
    size_t sym = SIZE_MAX;
    for (size_t i = 0; i < mc.symbols.size() && sym == SIZE_MAX; ++i)
        if (model::isModelSymbol(mc, i) && assetdb::baseAssetName(mc.symbolName(i)) == model) sym = i;
    if (sym == SIZE_MAX) fail(model + ": its stream entry holds no model");
    Bytes meshes = meshFile;
    if (gltfobj::isGlb(meshFile)) {
        // The node's mesh in the named meshes, bound to the root bone; every other mesh removed.
        if (glb.node.empty()) fail("a .glb needs --mesh, the node to take (one of its blocks)");
        if (glb.into.empty()) fail("a .glb needs --into, the template's meshes that get it (10,1,18,25)");
        model::Model m = model::parse(mc, sym);
        gltfobj::Options o;
        o.node = glb.node;
        o.into = glb.into;
        for (int i : glb.into)
            if (i < 0 || size_t(i) >= m.meshes.size()) fail("--into: " + templateName + " has " + std::to_string(m.meshes.size()) + " meshes");
        for (size_t i = 0; i < m.meshes.size(); ++i)
            if (std::find(glb.into.begin(), glb.into.end(), int(i)) == glb.into.end()) o.hide.push_back(int(i));
        if (!m.bones.empty()) o.skinBone = m.bones[0].name;
        if (!glb.flat.empty()) {
            if (glb.flat.size() != 2 && glb.flat.size() != 4) fail("--flat is u,v or u,v,u2,v2");
            o.flat = true;
            o.flatUv[0] = glb.flat[0], o.flatUv[1] = glb.flat[1];
            if (glb.flat.size() == 4) {
                o.flatUv2 = true;
                o.flatUv2At[0] = glb.flat[2], o.flatUv2At[1] = glb.flat[3];
            }
        }
        const std::string text = gltfobj::toObj(meshFile, o);
        meshes.assign(text.begin(), text.end());
    }
    model::ImportReport r = model::importMeshFile(mc, sym, meshes);
    char line[160];
    std::snprintf(line, sizeof line, "%s: from %s, %d mesh(es) edited in place, %d rebuilt, %d removed", newModel.c_str(),
                  templateName.c_str(), r.edited, r.rebuilt, r.hidden);
    b.notes.push_back(line);
    for (auto& n : r.notes) b.notes.push_back("  " + n);
    if (glb.matte) {
        const std::vector<std::string> off = matte(mc, sym);
        std::string list;
        for (auto& n : off) list += (list.empty() ? "" : ", ") + n;
        b.notes.push_back("  matte: " + (list.empty() ? std::string("no sheen constants found") : list + " off"));
    }
    rename(mc, model, newModel);
    b.entries.push_back({assetId(newModel), mc.serialize()});
    if (lower(collision) == "none") return b;

    // reNut only uses a model whose collision is loaded too (but for parts of their own).
    std::string from = lower(collision);
    if (startsWith(from, kModelPrefix)) from.erase(0, kModelPrefix.size());
    if (from.empty()) from = templateName;
    const std::string havok = std::string(kHavokPrefix) + from, newHavok = std::string(kHavokPrefix) + id;
    caff::Caff hc = requireEntry(idx, stream, havok);
    rename(hc, havok, newHavok);
    b.entries.push_back({assetId(newHavok), hc.serialize()});
    b.notes.push_back(newHavok + ": " + from + "'s collision");
    return b;
}

streambundle::Entry buildFootprint(const fs::path& stream, const Bytes& record, const std::string& id,
                                   const std::string& attachable, std::string* note) {
    const std::array<bool, 6> listed = faceRule(attachable);
    if (record.size() < kRecordAttach + 4) fail("not a part record");
    auto c = entryCaff(streamIndex(stream), stream, be32(&record[kRecordAttach]));
    if (!c) fail("the part's footprint is not in " + stream.string());
    size_t sym = SIZE_MAX;
    for (size_t i = 0; i < c->symbols.size() && sym == SIZE_MAX; ++i)
        if (startsWith(assetdb::baseAssetName(c->symbolName(i)), kAttachPrefix) && c->sectionOf(c->symbols[i]).name == ".data") sym = i;
    if (sym == SIZE_MAX) fail("the part's footprint entry holds no footprint");
    const std::string from = assetdb::baseAssetName(c->symbolName(sym)), to = std::string(kAttachPrefix) + id;

    // A face record (0x28 bytes): s32 cell x, y, z, u32 direction (0 +Y, 1 +Z, 2 -Y, 3 -Z, 4 -X, 5 +X),
    // f32 face centre x, y, z, u32 attachable, s32 neighbour, f32 2.0. They follow the 0x40 header.
    Bytes d = c->symbolData(sym);
    int faces = 0, open = 0;
    for (size_t o = 0x50; o + 0x18 <= d.size(); o += 4) {
        if (be32(&d[o + 0x14]) != 0x40000000 || be32(&d[o + 0xC]) > 1) continue;
        uint32_t dir = be32(&d[o - 4]);
        if (dir > 5) continue;
        // A face centre: on the half-cell grid, off the cell grid on exactly one axis.
        int offGrid = 0;
        bool onHalves = true;
        for (int k = 0; k < 3; ++k) {
            float v = bef32(&d[o + 4 * size_t(k)]);
            onHalves &= std::fabs(v * 2 - std::round(v * 2)) < 1e-4f && std::fabs(v) < 64;
            offGrid += std::fabs(std::fabs(v - std::floor(v)) - 0.5f) < 1e-4f;
        }
        if (!onHalves || offGrid != 1) continue;
        bool on = listed[dir];
        putBe32(&d[o + 0xC], on ? 1 : 0);
        ++faces;
        open += on;
        o += 0x24;
    }
    if (!faces) fail("no attach points found in " + from);
    c->replaceSymbolData(sym, d);
    rename(*c, from, to);
    if (note) *note = to + ": " + from + "'s footprint, " + std::to_string(open) + " of " + std::to_string(faces) + " faces attachable";
    return {assetId(to), c->serialize()};
}

fs::path modBundlePath(const fs::path& mod, const std::string& bundle) {
    char hex[8];
    std::snprintf(hex, sizeof hex, "%06x", unsigned(assetId("aid_streambundle_banjox_" + bundle) & 0xFFFFFF));
    return mod / "bundles" / "Bundle" / "50" / hex;
}

fs::path addToMod(const fs::path& mod, const std::string& bundle, const std::vector<streambundle::Entry>& entries,
                  const fs::path& gameStream) {
    fs::path p = modBundlePath(mod, bundle);
    if (!gameStream.empty() && fs::exists(longPath(gameStream.parent_path() / p.filename())))
        fail("the bundle name '" + bundle + "' gives the id of a bundle of the game; choose another");
    streambundle::StreamBundle sb;
    if (fs::exists(longPath(p))) sb = streambundle::StreamBundle::parse(readFile(p));
    else sb.timestamp = uint32_t(std::time(nullptr));
    for (auto& e : entries) {
        bool replaced = false;
        for (auto& o : sb.entries)
            if (o.id == e.id) {
                o.data = e.data;
                replaced = true;
            }
        if (!replaced) sb.entries.push_back(e);
    }
    fs::create_directories(longPath(p.parent_path()));
    writeFile(p, sb.serialize());
    return p;
}

} // namespace nut::renutmodel
