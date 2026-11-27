#include "renutworld.hpp"

#include "assetdb.hpp"
#include "blockmodel.hpp"
#include "caff.hpp"
#include "container.hpp"
#include "gamescript.hpp"
#include "gltfobj.hpp"
#include "image.hpp"
#include "json.hpp"
#include "ops.hpp"
#include "renutmodel.hpp"
#include "renutnpc.hpp"
#include "renutpart.hpp"
#include "streambundle.hpp"
#include "texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace nut::renutworld {
namespace {

constexpr float kPi = 3.14159265358979f;

uint32_t assetId(const std::string& name) {
    auto id = assetdb::idFromName(name);
    if (!id) fail("no asset id for " + name);
    return *id;
}

void putF32(uint8_t* p, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    putBe32(p, v);
}

// A CAFF holding one asset: a .data symbol named `name`, of `kind`, with its pointers to itself
// (offsets into it) relocated as it loads.
Bytes oneAsset(const std::string& name, uint8_t kind, const Bytes& data, const std::vector<uint32_t>& selfPointers = {}) {
    caff::Caff c;
    c.symbolNames.push_back(name);
    caff::Section s;
    s.name = ".data";
    s.alignLog2 = 4;
    s.data = data;
    while (s.data.size() % 16) s.data.push_back(0);
    c.sections.push_back(s);
    c.symbols.push_back({1, 0, uint32_t(data.size()), 1, kind});
    if (!selfPointers.empty()) c.relocs[0].push_back({1, 1, selfPointers});
    return c.serialize();
}

// ---- Collision -------------------------------------------------------------------------------
//
// A havok asset of a mesh without its MOPP (entry type 0, which no game file uses; reNut builds the
// MOPP as it loads). Its Havok 5.5 packfile copies the car park's class tables and holds one
// hkpExtendedMeshShape with one triangles subpart, whose vertices and indices the loader points at
// the asset's own (sub_822D7248).

// The car park's mesh collision: its packfile's header, class tables (the __classnames__ and
// __types__ sections and their headers, byte for byte), and its mesh and subpart as templates.
struct HavokTemplate {
    Bytes header;        // 0x40
    Bytes sectionHeads;  // the first two section headers (0x60)
    Bytes dataHead;      // the __data__ section's header (0x30)
    Bytes tables;        // the two sections' data, from 0xD0 up to the __data__ section
    uint32_t dataStart = 0;
    uint32_t meshClassName = 0;  // "hkpExtendedMeshShape" in __classnames__
    Bytes mesh;          // hkpExtendedMeshShape, 192 bytes
    Bytes subpart;       // its triangles subpart, 64 bytes
};

constexpr uint32_t kPackfileHeader = 0x40, kSectionHead = 0x30, kMeshSize = 192, kSubpartSize = 64;
constexpr uint32_t kMeshSubparts = 0x50;       // trianglesSubparts {ptr, count}
constexpr uint32_t kMeshWelding = 0x60;        // weldingInfo {ptr, count, capacity and flags}
constexpr uint32_t kMeshWeldingType = 0x6C;    // u8: 6, none
constexpr uint32_t kMeshEmbedded = 0x70;       // an embedded subpart, kept like the array's
constexpr uint32_t kMeshHalfExtents = 0x30, kMeshCentre = 0x40;
constexpr uint32_t kSubpartTriangles = 0x10, kSubpartVertices = 0x1C;

// The car park's bundle (aid_bundle_banjox_showdowntown_carpark), which the world's templates come from.
fs::path carParkBundle(const fs::path& assets) { return assets / "Bundle" / "4f" / "20abf9"; }

HavokTemplate carParkHavok(const caff::Caff& c, const fs::path& bundle) {
    size_t sym = SIZE_MAX;
    for (size_t i = 0; i < c.symbols.size() && sym == SIZE_MAX; ++i)
        if (assetdb::baseAssetName(c.symbolName(i)) == "aid_havok_banjox_background_carpark_default") sym = i;
    if (sym == SIZE_MAX) fail("the car park's collision is not in " + bundle.string());
    const Bytes d = c.symbolData(sym);
    auto word = [&](size_t at) {
        if (at + 4 > d.size()) fail("the car park's collision is shorter than expected");
        return be32(&d[at]);
    };
    // Its entries {type, ->entry}: the type-1 one {->packfile, size, ...}.
    uint32_t entry = 0;
    for (uint32_t i = 0, table = word(0), count = word(4); i < count && !entry; ++i)
        if (word(table + 8 * i) == 1) entry = word(table + 8 * i + 4);
    if (!entry) fail("the car park's collision has no mesh entry");
    const uint32_t pk = word(entry), size = word(entry + 4);
    if (pk + size > d.size() || size < kPackfileHeader + 3 * kSectionHead) fail("the car park's collision packfile is cut short");
    const uint8_t* p = &d[pk];
    if (be32(p) != 0x57E0E057 || be32(p + 4) != 0x10C0C010 || be32(p + 0x14) != 3) fail("the car park's collision is not a Havok packfile of three sections");

    HavokTemplate t;
    t.header.assign(p, p + kPackfileHeader);
    t.sectionHeads.assign(p + kPackfileHeader, p + kPackfileHeader + 2 * kSectionHead);
    const uint8_t* dh = p + kPackfileHeader + 2 * kSectionHead;
    t.dataHead.assign(dh, dh + kSectionHead);
    t.dataStart = be32(dh + 0x14);
    const uint32_t tablesStart = kPackfileHeader + 3 * kSectionHead;
    if (t.dataStart < tablesStart || t.dataStart > size) fail("the car park's collision sections are out of order");
    t.tables.assign(p + tablesStart, p + t.dataStart);
    const uint32_t namesStart = be32(p + kPackfileHeader + 0x14);

    // The data section's virtual fixups {object, section, class name} name the mesh.
    const uint8_t* data = p + t.dataStart;
    const uint32_t dataEnd = be32(dh + 0x2C), local = be32(dh + 0x18), virt = be32(dh + 0x20), exports = be32(dh + 0x24);
    if (t.dataStart + dataEnd > size) fail("the car park's collision data is cut short");
    uint32_t mesh = UINT32_MAX;
    for (uint32_t at = virt; at + 12 <= exports; at += 12) {
        const uint32_t object = be32(data + at), name = be32(data + at + 8);
        if (object == 0xFFFFFFFF) break;
        const char* n = reinterpret_cast<const char*>(p + namesStart + name);
        if (namesStart + name < t.dataStart && std::string(n) == "hkpExtendedMeshShape") {
            mesh = object;
            t.meshClassName = name;
        }
    }
    if (mesh == UINT32_MAX || mesh + kMeshSize > dataEnd) fail("the car park's collision has no hkpExtendedMeshShape");
    t.mesh.assign(data + mesh, data + mesh + kMeshSize);
    // The local fixup that points its subpart array at the subpart.
    uint32_t subpart = UINT32_MAX;
    for (uint32_t at = local; at + 8 <= be32(dh + 0x1C); at += 8)
        if (be32(data + at) == mesh + kMeshSubparts) subpart = be32(data + at + 4);
    if (subpart == UINT32_MAX || subpart + kSubpartSize > dataEnd) fail("the car park's collision mesh has no subpart");
    t.subpart.assign(data + subpart, data + subpart + kSubpartSize);
    return t;
}

// The packfile: the template's tables, then the mesh, its subpart and their fixups.
Bytes packfile(const HavokTemplate& t, uint32_t triangles, uint32_t vertices, const std::array<float, 3>& lo,
               const std::array<float, 3>& hi) {
    Bytes mesh = t.mesh, subpart = t.subpart;
    for (int k = 0; k < 3; ++k) {
        putF32(&mesh[kMeshHalfExtents + 4 * size_t(k)], (hi[size_t(k)] - lo[size_t(k)]) / 2);
        putF32(&mesh[kMeshCentre + 4 * size_t(k)], (hi[size_t(k)] + lo[size_t(k)]) / 2);
    }
    putF32(&mesh[kMeshCentre + 12], 1.0f);
    std::fill(mesh.begin(), mesh.begin() + 16, 0);     // vtable, references, user data: set as it loads
    putBe32(&mesh[kMeshSubparts], 0);                   // -> the subpart (a local fixup)
    putBe32(&mesh[kMeshSubparts + 4], 1);
    putBe32(&mesh[kMeshSubparts + 8], 0);               // no shapes subparts
    putBe32(&mesh[kMeshSubparts + 12], 0);
    putBe32(&mesh[kMeshWelding], 0);                    // no welding information
    putBe32(&mesh[kMeshWelding + 4], 0);
    putBe32(&mesh[kMeshWelding + 8], 0x80000000u);
    mesh[kMeshWeldingType] = 6;
    putBe32(&subpart[kSubpartTriangles], triangles);
    putBe32(&subpart[kSubpartVertices], vertices);
    putBe32(&mesh[kMeshEmbedded + kSubpartTriangles], triangles);
    putBe32(&mesh[kMeshEmbedded + kSubpartVertices], vertices);

    // __data__: the mesh at 0 (the contents), the subpart after it, then the fixup tables padded
    // with 0xFF: local {from, to}, global none, virtual {object, section 0, class name}.
    Writer data;
    data.bytes(mesh);
    const uint32_t subpartAt = uint32_t(data.pos());
    data.bytes(subpart);
    const uint32_t localAt = uint32_t(data.pos());
    data.u32(kMeshSubparts);
    data.u32(subpartAt);
    while (data.pos() % 16) data.u8(0xFF);
    const uint32_t virtualAt = uint32_t(data.pos());
    data.u32(0);
    data.u32(0);
    data.u32(t.meshClassName);
    while (data.pos() % 16) data.u8(0xFF);
    const uint32_t end = uint32_t(data.pos());

    Writer w;
    Bytes header = t.header;
    putBe32(&header[0x18], 2);                // contents: section 2, offset 0
    putBe32(&header[0x1C], 0);
    putBe32(&header[0x20], 0);                // its class name: section 0, at the mesh's name
    putBe32(&header[0x24], t.meshClassName);
    w.bytes(header);
    w.bytes(t.sectionHeads);
    Bytes dh = t.dataHead;
    putBe32(&dh[0x14], t.dataStart);
    putBe32(&dh[0x18], localAt);
    putBe32(&dh[0x1C], virtualAt);            // global fixups: none
    putBe32(&dh[0x20], virtualAt);
    putBe32(&dh[0x24], end);
    putBe32(&dh[0x28], end);
    putBe32(&dh[0x2C], end);
    w.bytes(dh);
    w.bytes(t.tables);
    w.bytes(data.buf());
    return w.buf();
}

// The havok asset: {->entries, 1, runtime words}, the entry {->packfile, size, ->indices, index
// count, ->vertices, vertex count, ->triangle materials, ->materials, count}, a mass block, then the
// data and the entries table.
Bytes havokAsset(const HavokTemplate& t, const std::vector<std::array<float, 3>>& pos, const std::vector<uint32_t>& indices) {
    std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (auto& p : pos)
        for (size_t k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
    const Bytes pk = packfile(t, uint32_t(indices.size() / 3), uint32_t(pos.size()), lo, hi);

    Writer w;
    w.zeros(0x50);
    // The mass block (the game reads it for a mesh's body): mass, 1, centre, inertia rows.
    const float mass = 100000.0f;
    std::array<float, 3> e{};
    for (size_t k = 0; k < 3; ++k) e[k] = std::max(hi[k] - lo[k], 1.0f);
    w.f32(mass);
    w.f32(1.0f);
    w.zeros(8);
    for (size_t k = 0; k < 3; ++k) w.f32((lo[k] + hi[k]) / 2);
    w.f32(0);
    const float inertia[3] = {mass * (e[1] * e[1] + e[2] * e[2]) / 12, mass * (e[0] * e[0] + e[2] * e[2]) / 12,
                              mass * (e[0] * e[0] + e[1] * e[1]) / 12};
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) w.f32(r == k ? inertia[r] : 0.0f);
    w.align(16);
    const uint32_t indexAt = uint32_t(w.pos());
    for (uint32_t i : indices) w.u16(uint16_t(i));
    w.align(16);
    const uint32_t vertexAt = uint32_t(w.pos());
    for (auto& p : pos)
        for (float c : p) w.f32(c);
    // Each triangle's surface material (a u8 index) and the one material {0, surface id, pad, flags}:
    // the car park's commonest ground. The game reads a touched triangle's (sub_821F3DB8).
    w.align(16);
    const uint32_t materialIndexAt = uint32_t(w.pos());
    for (size_t t = 0; t < indices.size() / 3; ++t) w.u8(0);
    w.align(16);
    const uint32_t materialAt = uint32_t(w.pos());
    w.u32(0);
    w.u8(0);
    w.zeros(3);
    w.u32(4);
    w.align(16);
    const uint32_t packfileAt = uint32_t(w.pos());
    w.bytes(pk);
    w.align(16);
    const uint32_t entriesAt = uint32_t(w.pos());
    w.u32(0);     // type 0: a mesh without its MOPP
    w.u32(0x20);
    Bytes& b = w.buf();
    putBe32(&b[0x00], entriesAt);
    putBe32(&b[0x04], 1);
    putBe32(&b[0x20], packfileAt);
    putBe32(&b[0x24], uint32_t(pk.size()));
    putBe32(&b[0x28], indexAt);
    putBe32(&b[0x2C], uint32_t(indices.size()));
    putBe32(&b[0x30], vertexAt);
    putBe32(&b[0x34], uint32_t(pos.size()));
    putBe32(&b[0x38], materialIndexAt);
    putBe32(&b[0x3C], materialAt);
    putBe32(&b[0x40], 1);
    return b;
}

// ---- Marker ----------------------------------------------------------------------------------

constexpr uint32_t kRecordPosition = 0x14, kRecordRotation = 0x20, kRecordScale = 0x2C;

Bytes record(uint32_t size, uint16_t kind, uint16_t id, const std::array<float, 3>& pos, float yawDegrees) {
    Bytes r(size, 0);
    putBe32(&r[0], size);
    putBe16(&r[4], kind);
    putBe16(&r[6], id);
    for (size_t k = 0; k < 3; ++k) putF32(&r[kRecordPosition + 4 * k], pos[k]);
    putF32(&r[kRecordRotation + 4], yawDegrees * kPi / 180);
    putF32(&r[kRecordScale], 1.0f);
    return r;
}

// The player's start (kind 4), the door home (kind 28: the car park's stone arch to Showdown Town),
// `extra` records (the NPCs' and their goal gates', numbered from 3) and the end record. No door home without `home`.
Bytes marker(const std::array<float, 3>& spawn, float spawnYaw, bool home, const std::array<float, 3>& exit, float exitYaw,
             const Bytes& extra) {
    Bytes out = record(124, 4, 1, spawn, spawnYaw);
    if (home) {
        Bytes door = record(208, 28, 2, exit, exitYaw);
        putF32(&door[0x34], 5.0f);
        putF32(&door[0x38], 5.0f);
        putBe32(&door[0x3C], assetId("aid_objparams_banjox_portal_hubtemp_default"));
        putBe32(&door[0x40], assetId("aid_script_banjox_showdowntown_main"));
        const std::string indicator = "\"sceneIndicatorType_Null\"";
        std::memcpy(&door[0x90], indicator.data(), indicator.size());
        out.insert(out.end(), door.begin(), door.end());
    }
    out.insert(out.end(), extra.begin(), extra.end());
    Bytes end(52, 0);
    putBe32(&end[0], 52);
    putBe16(&end[6], 1);
    putF32(&end[kRecordScale], 1.0f);
    out.insert(out.end(), end.begin(), end.end());
    return out;
}

// ---- Hologram door ---------------------------------------------------------------------------

constexpr const char* kDoorModel = "aid_model_banjox_objects_showdowntown_gameworlddoornew";
constexpr const char* kDoorHavok = "aid_havok_banjox_objects_showdowntown_gameworlddoornew";
constexpr const char* kDoorParams = "aid_objparams_banjox_portal_worlddoor";
constexpr uint32_t kParamsModel = 0x0C0;

struct Atlas {
    const char* name;  // without its "mip"/"top" suffix
    const char* tail;  // the copy's name after aid_texture_renutworld_<id>_
    bool mask;         // the alpha mask (grey levels), else the colour
    uint32_t cells;    // pictures in a row: the world door's hold one per world (2-7), its U offset (world - 2) / 6
};
constexpr Atlas kAtlases[] = {
    {"aid_texture_banjox_shared_showdowntown_worlddoor_allhologram_0x0f328c35", "hologram_0x0f328c35", false, 6},
    {"aid_texture_banjox_shared_showdowntown_worlddoor_allhologramalpha_0x07cc3635", "hologramalpha_0x07cc3635", true, 6},
};

// The stone arch home (the car park's exit, two-sided), from the car park's bundle. Showdown Town
// loads all of its textures but its hologram's, which the mod gets copies of.
constexpr const char* kArchModel = "aid_model_banjox_objects_stonearchdoor";
constexpr const char* kArchHavok = "aid_havok_banjox_objects_stonearchdoor";
constexpr const char* kArchParams = "aid_objparams_banjox_portal_hubtemp_default";
constexpr Atlas kArchAtlases[] = {
    {"aid_texture_banjox_shared_showdowntown_worlddoor_showdowntown_hologram_0x02606125", "archhologram_0x02606125", false, 1},
    {"aid_texture_banjox_shared_showdowntown_worlddoor_showdowntown_hologramalpha_0x05fb68a5", "archhologramalpha_0x05fb68a5", true, 1},
};

size_t symbolOf(const caff::Caff& c, const std::string& name, const std::string& section) {
    for (size_t i = 0; i < c.symbols.size(); ++i) {
        const caff::Symbol& s = c.symbols[i];
        if (s.section && s.section <= c.sections.size() && c.sections[s.section - 1].name == section &&
            assetdb::baseAssetName(c.symbolName(i)) == name)
            return i;
    }
    fail(name + " (" + section + ") is not in the game bundle it is copied from");
}

// A CAFF of its own holding `syms` of `src` (as its symbols 0, 1...) and the pool entries they point
// into, with their names, kinds, relocations and byte swaps.
caff::Caff standalone(const caff::Caff& src, const std::vector<size_t>& syms) {
    std::map<size_t, size_t> at;  // source symbol -> its copy
    for (size_t k = 0; k < syms.size(); ++k) at[syms[k]] = k;
    std::vector<Bytes> data;
    for (size_t s : syms) data.push_back(src.symbolData(s));

    // Each pool entry the symbols point into, copied once at the alignment it had.
    struct PoolCopy {
        Bytes data;
        std::vector<caff::PoolString> strings;
        std::map<uint32_t, uint32_t> moved;  // source entry offset -> the copy's
    };
    std::map<size_t, PoolCopy> pools;
    for (int set = 0; set < 2; ++set)
        for (const caff::Relocation& r : src.relocs[set]) {
            if (!at.count(r.from - 1) || at.count(r.to - 1)) continue;
            const caff::Pool* sp = nullptr;
            for (const caff::Pool& p : src.pools)
                if (p.symbol == r.to) sp = &p;
            if (!sp) fail(src.symbolName(r.from - 1) + " points at " + src.symbolName(r.to - 1) + ", which is not copied with it");
            const Bytes poolBytes = src.symbolData(r.to - 1);
            const uint32_t base = src.symbols[r.to - 1].offset;
            PoolCopy& pc = pools[r.to - 1];
            Bytes& d = data[at[r.from - 1]];
            for (uint32_t off : r.offsets) {
                if (off + 4 > d.size()) fail("a relocation of " + src.symbolName(r.from - 1) + " is outside it");
                const uint32_t v = be32(&d[off]);
                if (v >= poolBytes.size()) continue;  // not a pointer: the game relocates only values inside the target
                const caff::PoolString* e = nullptr;
                for (const caff::PoolString& x : sp->strings)
                    if (v >= x.offset && v < x.offset + std::max<uint32_t>(x.length, 1)) e = &x;
                if (!e || e->offset + e->length > poolBytes.size()) fail(src.symbolName(r.from - 1) + " points outside its pool's entries");
                auto [it, fresh] = pc.moved.emplace(e->offset, 0);
                if (fresh) {
                    uint32_t align = 4096;
                    while (align > 1 && (base + e->offset) % align) align >>= 1;
                    while (pc.data.size() % align) pc.data.push_back(0);
                    it->second = uint32_t(pc.data.size());
                    pc.data.insert(pc.data.end(), poolBytes.begin() + e->offset, poolBytes.begin() + e->offset + e->length);
                    caff::PoolString ne = *e;
                    ne.offset = it->second;
                    pc.strings.push_back(ne);
                }
                putBe32(&d[off], it->second + (v - e->offset));
            }
        }

    // The sections used, in the source's order; a section with a pool names it as its first symbol.
    std::vector<uint8_t> used;
    for (size_t s : syms) used.push_back(src.symbols[s].section);
    for (auto& p : pools) used.push_back(src.symbols[p.first].section);
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());
    caff::Caff c;
    std::map<uint8_t, uint8_t> section;  // source section -> the copy's (1-based)
    for (uint8_t s : used) {
        caff::Section ns;
        ns.name = src.sections.at(s - 1).name;
        ns.alignLog2 = src.sections[s - 1].alignLog2;
        c.sections.push_back(ns);
        section[s] = uint8_t(c.sections.size());
    }
    auto nameIndex = [&](size_t s) {
        const std::string n = src.symbolName(s);
        return n.empty() ? 0u : c.symbolNameIndex(n);
    };
    for (size_t k = 0; k < syms.size(); ++k)
        c.appendSymbol(nameIndex(syms[k]), section[src.symbols[syms[k]].section], src.symbols[syms[k]].kind, data[k]);
    std::map<size_t, size_t> poolAt;  // source pool symbol -> its copy
    for (auto& [p, pc] : pools) {
        const uint8_t s = section[src.symbols[p].section];
        const size_t i = c.appendSymbol(nameIndex(p), s, src.symbols[p].kind, pc.data);
        poolAt[p] = i;
        c.sections[s - 1].firstSymbol = uint32_t(i + 1);
        caff::Pool np;
        np.symbol = uint32_t(i + 1);
        np.strings = pc.strings;
        c.pools.push_back(np);
    }
    auto copyOf = [&](uint32_t one) {  // 1-based, both ways
        auto a = at.find(one - 1);
        return uint32_t((a != at.end() ? a->second : poolAt.at(one - 1)) + 1);
    };
    for (int set = 0; set < 2; ++set) {
        for (const caff::Relocation& r : src.relocs[set])
            if (at.count(r.from - 1)) c.relocs[set].push_back({copyOf(r.from), copyOf(r.to), r.offsets});
        for (const caff::SwapEntry& w : src.swaps[set])
            if (at.count(w.symbol - 1)) {
                caff::SwapEntry e = w;
                e.symbol = copyOf(w.symbol);
                c.swaps[set].push_back(e);
            }
    }
    return c;
}

// Renames asset `from` to `to` in `c`: its symbol names (plain, "name,timestamp,version" or a
// texture's "path\name\default.rtx") and the debug name that ends a model's .stream part.
void renameAsset(caff::Caff& c, const std::string& from, const std::string& to) {
    for (std::string& n : c.symbolNames) {
        const size_t at = n.find(from);
        if (at == std::string::npos) continue;
        const size_t end = at + from.size();
        if ((at == 0 || n[at - 1] == '\\') && (end == n.size() || n[end] == ',' || n[end] == '\\')) n.replace(at, from.size(), to);
    }
    const std::string tail = from + '\0';
    for (size_t i = 0; i < c.symbols.size(); ++i) {
        if (c.sectionOf(c.symbols[i]).name != ".stream" || assetdb::baseAssetName(c.symbolName(i)) != to) continue;
        Bytes d = c.symbolData(i);
        if (d.size() < tail.size()) continue;
        // The name and its terminator, then at most a few bytes of padding.
        for (size_t at = d.size() - tail.size() + 1; at-- > 0 && at + tail.size() + 4 >= d.size();) {
            if (std::memcmp(&d[at], tail.data(), tail.size()) != 0) continue;
            d.resize(at);
            d.insert(d.end(), to.begin(), to.end());
            d.push_back(0);
            c.replaceSymbolData(i, d);
            break;
        }
    }
}

// Points the model's texture names (strings in its .data part `sym`, which its property dictionary's
// texturefileN entries point at) that are keys of `names` at new strings added to its end.
int retexture(caff::Caff& c, size_t sym, const std::map<std::string, std::string>& names) {
    Bytes d = c.symbolData(sym);
    const size_t size = d.size();
    std::vector<uint32_t> self;
    for (int set = 0; set < 2; ++set)
        for (const caff::Relocation& r : c.relocs[set])
            if (r.from == sym + 1 && r.to == sym + 1) self.insert(self.end(), r.offsets.begin(), r.offsets.end());
    std::map<std::string, uint32_t> added;
    int pointed = 0;
    for (uint32_t at : self) {
        if (at + 4 > size) continue;
        const uint32_t o = be32(&d[at]);
        if (o >= size) continue;
        size_t e = o;
        while (e < size && d[e]) ++e;
        if (e >= size) continue;
        auto it = names.find(std::string(reinterpret_cast<const char*>(&d[o]), e - o));
        if (it == names.end()) continue;
        auto [a, fresh] = added.emplace(it->second, 0);
        if (fresh) {
            while (d.size() % 4) d.push_back(0);
            a->second = uint32_t(d.size());
            d.insert(d.end(), it->second.begin(), it->second.end());
            d.push_back(0);
        }
        putBe32(&d[at], a->second);
        ++pointed;
    }
    if (!pointed) fail("the door's model names none of the hologram's textures");
    c.replaceSymbolData(sym, d);
    return pointed;
}

// `img` cropped about its middle to the shape of a `w` x `h` cell, and scaled to it.
image::Rgba fitCell(const image::Rgba& img, uint32_t w, uint32_t h) {
    uint32_t cw = img.width, ch = img.height;
    if (uint64_t(cw) * h > uint64_t(ch) * w) cw = std::max<uint32_t>(1, uint32_t(uint64_t(ch) * w / h));
    else ch = std::max<uint32_t>(1, uint32_t(uint64_t(cw) * h / w));
    image::Rgba crop;
    crop.width = cw;
    crop.height = ch;
    crop.pixels.resize(size_t(cw) * ch * 4);
    const uint32_t x0 = (img.width - cw) / 2, y0 = (img.height - ch) / 2;
    for (uint32_t y = 0; y < ch; ++y)
        std::memcpy(&crop.pixels[size_t(y) * cw * 4], &img.pixels[(size_t(y0 + y) * img.width + x0) * 4], size_t(cw) * 4);
    return texture::downscale(crop, w, h);
}

// A `w` x `h` atlas with the image in every cell, so any cell the door picks shows it. The mask fades
// out at each cell's edges and is brighter where the image is, as the game's is.
image::Rgba atlasOf(const image::Rgba& img, uint32_t w, uint32_t h, bool mask, uint32_t cells) {
    const uint32_t cw = w / cells;
    if (!cw || !h || cw * cells != w) fail("the door's hologram is not " + std::to_string(cells) + " pictures in a row");
    const image::Rgba cell = fitCell(img, cw, h);
    auto fade = [](uint32_t i, uint32_t n) {  // 0 at the border, 1 from 12% in
        const float t = std::clamp(std::min(float(i) + 0.5f, float(n) - float(i) - 0.5f) / (0.12f * float(n)), 0.0f, 1.0f);
        return t * t * (3 - 2 * t);
    };
    image::Rgba out;
    out.width = w;
    out.height = h;
    out.pixels.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < cw; ++x) {
            const uint8_t* p = &cell.pixels[(size_t(y) * cw + x) * 4];
            uint8_t px[4] = {p[0], p[1], p[2], 255};
            if (mask) {
                const float lum = (0.299f * float(p[0]) + 0.587f * float(p[1]) + 0.114f * float(p[2])) / 255.0f;
                const float v = (90.0f + 66.0f * lum) * (float(p[3]) / 255.0f) * fade(x, cw) * fade(y, h);
                px[0] = px[1] = px[2] = uint8_t(std::lround(v));
            }
            for (uint32_t k = 0; k < cells; ++k) std::memcpy(&out.pixels[(size_t(y) * w + k * cw + x) * 4], px, 4);
        }
    return out;
}

// The atlas `a` copied as `stem`'s, showing `img` (null: as it is): its "mip" half (levels 1 on) from the
// resident bundle `res` and its level 0 from the stream bundle `tops`, an entry each. Adds their new names to `names`.
std::vector<streambundle::Entry> hologramTexture(const caff::Caff& res, const fs::path& tops, const Atlas& a, const std::string& stem,
                                                 const image::Rgba* img, std::map<std::string, std::string>& names, const Say& say) {
    const std::string from = a.name, to = "aid_texture_" + stem + "_" + a.tail;
    caff::Caff mip = standalone(res, {symbolOf(res, from + "mip", ".data"), symbolOf(res, from + "mip", ".texturegpu")});
    renameAsset(mip, from + "mip", to + "mip");
    std::vector<streambundle::IndexEntry> index;
    if (!streambundle::readIndex(longPath(tops), index)) fail(tops.string() + " is not a stream bundle");
    const uint32_t topId = assetId(from + "top");
    std::optional<caff::Caff> top;
    for (const auto& e : index)
        if (e.id == topId && e.size && !top) {
            const Bytes d = streambundle::readEntry(longPath(tops), e);
            if (caff::isCaff(d)) top = caff::Caff::parse(d);
        }
    if (!top) fail(from + "top is not in " + tops.string());
    renameAsset(*top, from + "top", to + "top");
    streambundle::StreamBundle sb;
    sb.entries.push_back({assetId(to + "top"), top->serialize()});
    names[from + "mip"] = to + "mip";
    names[from + "top"] = to + "top";
    if (!img) {
        say(to + ": a copy of the game's");
        return {{assetId(to + "mip"), mip.serialize()}, sb.entries[0]};
    }

    // The image goes in at the atlas's own size, so its format, size and levels stay.
    std::vector<ops::TexRef> refs = ops::findTextures(mip);
    if (refs.size() != 1) fail(from + "mip: its copy holds no texture");
    const texture::Header h = texture::parseHeader(mip.symbolData(refs[0].header));
    ops::TextureImage ti;
    ti.rgba = atlasOf(*img, h.width, h.height, a.mask, a.cells);
    ti.width = h.width;
    ti.height = h.height;
    bool topChanged = false;
    const std::string summary = ops::importTexture(mip, refs[0], ti, &sb, &topChanged);
    if (!topChanged) fail(to + "top: its level 0 was not written");
    say(to + ": " + std::to_string(h.width) + "x" + std::to_string(h.height) + (a.mask ? " mask, " : ", ") + summary);
    return {{assetId(to + "mip"), mip.serialize()}, sb.entries[0]};
}

image::Rgba hologramImage(const fs::path& picture) {
    image::Rgba img = image::decodeImage(readFile(picture));
    if (!img.width || !img.height || img.pixels.size() < size_t(img.width) * img.height * 4) fail(picture.string() + " has no pixels");
    return img;
}

// The stone arch as `stem`_arch: the car park's arch model, collision and objparams, with copies of its
// hologram's textures showing `picture` (empty: Showdown Town, as the game's do).
std::vector<streambundle::Entry> archDoor(const fs::path& assets, const std::string& stem, const fs::path& picture, const Say& say) {
    std::optional<image::Rgba> img;
    if (!picture.empty()) img = hologramImage(picture);
    const fs::path bundle = carParkBundle(assets), tops = assets / "Bundle" / "50" / "20abf9";
    say("reading the stone arch from " + bundle.string() + " (the car park's bundle)");
    Loaded l = loadAsset(bundle);
    if (!l.caff) fail(bundle.string() + " is not the car park's bundle (is the assets folder the game's?)");
    const caff::Caff& res = *l.caff;
    std::vector<streambundle::Entry> out;
    std::map<std::string, std::string> names;
    for (const Atlas& a : kArchAtlases)
        for (auto& e : hologramTexture(res, tops, a, stem, img ? &*img : nullptr, names, say)) out.push_back(std::move(e));

    const std::string model = "aid_model_" + stem + "_arch", havok = "aid_havok_" + stem + "_arch", params = "aid_objparams_" + stem + "_arch";
    caff::Caff mc = standalone(res, {symbolOf(res, kArchModel, ".data"), symbolOf(res, kArchModel, ".gpu"), symbolOf(res, kArchModel, ".stream")});
    const int pointed = retexture(mc, 0, names);
    renameAsset(mc, kArchModel, model);
    out.push_back({assetId(model), mc.serialize()});
    say(model + ": the stone arch's model, its hologram's " + std::to_string(pointed) + " texture names pointed at the copies");

    caff::Caff hc = standalone(res, {symbolOf(res, kArchHavok, ".data")});
    renameAsset(hc, kArchHavok, havok);
    out.push_back({assetId(havok), hc.serialize()});
    say(havok + ": the stone arch's collision");

    caff::Caff pc = standalone(res, {symbolOf(res, kArchParams, ".data")});
    Bytes d = pc.symbolData(0);
    if (d.size() < kParamsModel + 4 || be32(&d[kParamsModel]) != assetId(kArchModel))
        fail(std::string(kArchParams) + " does not name the stone arch's model at 0xC0");
    putBe32(&d[kParamsModel], assetId(model));
    pc.replaceSymbolData(0, d);
    renameAsset(pc, kArchParams, params);
    out.push_back({assetId(params), pc.serialize()});
    say(params + ": the stone arch's objparams, with that model");
    return out;
}

// The door with the hologram: the world door's model and collision as `stem`_door, the hologram's
// atlases as textures of the mod's own, and the door's objparams naming the model.
std::vector<streambundle::Entry> hologramDoor(const fs::path& assets, const std::string& stem, const fs::path& picture, const Say& say) {
    const image::Rgba img = hologramImage(picture);
    const fs::path bundle = assets / "Bundle" / "4f" / "234cec", tops = assets / "Bundle" / "50" / "234cec";
    say("reading the world door from " + bundle.string() + " (Showdown Town's common bundle)");
    Loaded l = loadAsset(bundle);
    if (!l.caff) fail(bundle.string() + " is not Showdown Town's common bundle (is the assets folder the game's?)");
    const caff::Caff& res = *l.caff;
    std::vector<streambundle::Entry> out;
    std::map<std::string, std::string> names;
    for (const Atlas& a : kAtlases)
        for (auto& e : hologramTexture(res, tops, a, stem, &img, names, say)) out.push_back(std::move(e));

    const std::string model = "aid_model_" + stem + "_door", havok = "aid_havok_" + stem + "_door", params = "aid_objparams_" + stem + "_door";
    caff::Caff mc = standalone(res, {symbolOf(res, kDoorModel, ".data"), symbolOf(res, kDoorModel, ".gpu"), symbolOf(res, kDoorModel, ".stream")});
    const int pointed = retexture(mc, 0, names);
    renameAsset(mc, kDoorModel, model);
    out.push_back({assetId(model), mc.serialize()});
    say(model + ": the world door's model, its hologram's " + std::to_string(pointed) + " texture names pointed at the copies");

    // The game finds a model's collision by its name, so the copy has its own.
    caff::Caff hc = standalone(res, {symbolOf(res, kDoorHavok, ".data")});
    renameAsset(hc, kDoorHavok, havok);
    out.push_back({assetId(havok), hc.serialize()});
    say(havok + ": the world door's collision");

    caff::Caff pc = standalone(res, {symbolOf(res, kDoorParams, ".data")});
    Bytes d = pc.symbolData(0);
    if (d.size() < kParamsModel + 4 || be32(&d[kParamsModel]) != assetId(kDoorModel))
        fail(std::string(kDoorParams) + " does not name the world door's model at 0xC0");
    putBe32(&d[kParamsModel], assetId(model));
    pc.replaceSymbolData(0, d);
    renameAsset(pc, kDoorParams, params);
    out.push_back({assetId(params), pc.serialize()});
    say(params + ": the world door's objparams, with that model (its animations are the game's)");
    return out;
}

// ---- Water -----------------------------------------------------------------------------------
//
// The water is one region the game draws from the water's own triangles (a model's chunk 0x26), with a mask
// (DXT1, blue above 0: no water) over its outline. Its look is op_7D's 71 arguments, the car park's calm
// water's with the mask (argument 66) its own.

constexpr const char* kCalmWater = "aid_script_banjox_live_testtrack_watersetup_calmwater";
constexpr const char* kWaterDisplacement = "aid_texture_banjox_shared_water_testtrack_ocean_animatedwave_displacementfoamcolour";
constexpr size_t kWaterArgs = 71, kWaterDisplacementArg = 61, kWaterMaskArg = 66;
// A mask's side: a texel about kWaterTexel metres (256-1024).
constexpr uint32_t kWaterMaskMin = 256, kWaterMaskMax = 1024;
constexpr float kWaterTexel = 0.5f;

// The calm water's op_7D arguments, found by its displacement texture (argument 61).
std::vector<uint32_t> calmWaterSetup(const caff::Caff& carPark) {
    for (size_t i = 0; i < carPark.symbols.size(); ++i) {
        if (assetdb::baseAssetName(carPark.symbolName(i)) != kCalmWater) continue;
        const Bytes d = carPark.symbolData(i);
        const uint32_t id = assetId(kWaterDisplacement);
        for (size_t at = kWaterDisplacementArg * 4; at + 4 <= d.size(); at += 4) {
            if (be32(&d[at]) != id) continue;
            const size_t first = at - kWaterDisplacementArg * 4;
            if (first + kWaterArgs * 4 > d.size()) break;
            std::vector<uint32_t> args(kWaterArgs);
            for (size_t k = 0; k < kWaterArgs; ++k) args[k] = be32(&d[first + 4 * k]);
            return args;
        }
    }
    fail(std::string(kCalmWater) + " (the car park's water) is not in its bundle as expected");
}

// What water makes of each triangle: ground, part of a level (water to swim in), or falling water (a steep
// triangle, as a waterfall's). The game draws both kinds of water; neither is solid.
enum class WaterUse : uint8_t { Ground, Body, Falls };
constexpr float kSteepWater = 0.5f;  // a triangle whose normal points up less than this (over 60 degrees) falls
constexpr float kSameWater = 0.4f;   // triangles this close in height to a level's lowest are one level

struct WaterPlan {
    std::vector<WaterUse> use;  // per triangle
    struct Body {
        std::string name;
        std::vector<size_t> tris;
        float height = -1e30f;
        float area = 0;  // of its rectangle, seen from above
    };
    std::vector<Body> bodies;
    size_t falls = 0;  // steep triangles
};

// Each water triangle by its own slope: steep ones fall; the rest are water to swim in, banded into levels by
// their height (a level takes those within kSameWater of its lowest, its height their area-weighted mean), so a
// gently sloping river is a run of levels and a waterfall joining two stretches doesn't take them with it.
WaterPlan planWater(const gltfobj::SceneMesh& src) {
    WaterPlan plan;
    const size_t n = src.indices.size() / 3;
    plan.use.assign(n, WaterUse::Ground);
    auto waterOf = [&](size_t t) { return t < src.triangleWater.size() ? src.triangleWater[t] : -1; };
    struct Tri {
        size_t t;
        float height;
        double area;  // seen from above
    };
    std::vector<Tri> level;
    for (size_t t = 0; t < n; ++t) {
        if (waterOf(t) < 0) continue;
        const auto& a = src.positions[src.indices[3 * t]];
        const auto& b = src.positions[src.indices[3 * t + 1]];
        const auto& c = src.positions[src.indices[3 * t + 2]];
        const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len < 1e-9) continue;  // no area: neither water nor ground
        if (std::fabs(ny) / len < kSteepWater) {
            plan.use[t] = WaterUse::Falls;
            ++plan.falls;
            continue;
        }
        level.push_back({t, float((a[1] + b[1] + c[1]) / 3), std::fabs(ny) / 2});
    }
    std::sort(level.begin(), level.end(), [](const Tri& a, const Tri& b) { return a.height < b.height; });
    std::vector<std::set<std::string>> names;
    std::vector<double> weighted, areas;
    float lowest = 0;
    for (const Tri& tri : level) {
        if (plan.bodies.empty() || tri.height - lowest > kSameWater) {
            plan.bodies.emplace_back();
            names.emplace_back();
            weighted.push_back(0);
            areas.push_back(0);
            lowest = tri.height;
        }
        WaterPlan::Body& b = plan.bodies.back();
        b.tris.push_back(tri.t);
        weighted.back() += tri.area * tri.height;
        areas.back() += tri.area;
        b.height = float(areas.back() > 0 ? weighted.back() / areas.back() : tri.height);
        names.back().insert(fs::path(src.water[size_t(waterOf(tri.t))].name).filename().string());  // a texture's file
    }
    for (size_t i = 0; i < plan.bodies.size(); ++i) {
        WaterPlan::Body& b = plan.bodies[i];
        for (const std::string& s : names[i]) b.name += (b.name.empty() ? "" : ", ") + s;
        float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
        for (size_t t : b.tris)
            for (size_t c = 0; c < 3; ++c) {
                const auto& p = src.positions[src.indices[3 * t + c]];
                x0 = std::min(x0, p[0]), x1 = std::max(x1, p[0]), z0 = std::min(z0, p[2]), z1 = std::max(z1, p[2]);
            }
        b.area = (x1 - x0) * (z1 - z0);
    }
    for (const WaterPlan::Body& b : plan.bodies)
        for (size_t t : b.tris) plan.use[t] = WaterUse::Body;
    return plan;
}

// Each spot of an n x n grid over lo-hi (corners on the edges) that triangle `t` covers seen from above:
// visit(xi, zi, the triangle's height there).
template <typename Visit>
void rasterize(const std::array<std::array<float, 3>, 3>& t, const std::array<float, 2>& lo, float dx, float dz, uint32_t n, Visit&& visit) {
    const float ax = t[0][0], az = t[0][2], bx = t[1][0], bz = t[1][2], cx = t[2][0], cz = t[2][2];
    const float area = (bx - ax) * (cz - az) - (cx - ax) * (bz - az);
    if (std::fabs(area) < 1e-8f) return;
    const int x0 = std::max(0, int(std::floor((std::min({ax, bx, cx}) - lo[0]) / dx))),
              x1 = std::min(int(n) - 1, int(std::ceil((std::max({ax, bx, cx}) - lo[0]) / dx)));
    const int z0 = std::max(0, int(std::floor((std::min({az, bz, cz}) - lo[1]) / dz))),
              z1 = std::min(int(n) - 1, int(std::ceil((std::max({az, bz, cz}) - lo[1]) / dz)));
    for (int zi = z0; zi <= z1; ++zi)
        for (int xi = x0; xi <= x1; ++xi) {
            const float px = lo[0] + float(xi) * dx, pz = lo[1] + float(zi) * dz;
            const float w0 = ((bx - px) * (cz - pz) - (cx - px) * (bz - pz)) / area;
            const float w1 = ((cx - px) * (az - pz) - (ax - px) * (cz - pz)) / area;
            const float w2 = 1 - w0 - w1;
            if (w0 >= -1e-4f && w1 >= -1e-4f && w2 >= -1e-4f) visit(xi, zi, w0 * t[0][1] + w1 * t[1][1] + w2 * t[2][1]);
        }
}


// The water as one region the game draws from its triangles (every flat level and fall), and for its physics
// each flat level's triangles in the 16 x 16 cells their outlines seen from above reach.
blockmodel::WaterRegion waterRegionOf(const gltfobj::SceneMesh& src, const WaterPlan& plan) {
    blockmodel::WaterRegion w;
    w.lo = {1e30f, 1e30f, 1e30f};
    w.hi = {-1e30f, -1e30f, -1e30f};
    auto corner = [&](size_t t, size_t c) -> const std::array<float, 3>& { return src.positions[src.indices[3 * t + c]]; };
    for (size_t t = 0; t < plan.use.size(); ++t) {
        if (plan.use[t] == WaterUse::Ground) continue;
        for (size_t c = 0; c < 3; ++c) {
            const auto& p = corner(t, c);
            w.vertices.push_back(p);
            for (size_t k = 0; k < 3; ++k) w.lo[k] = std::min(w.lo[k], p[k]), w.hi[k] = std::max(w.hi[k], p[k]);
        }
    }
    for (size_t k : {0, 2})
        if (w.hi[k] - w.lo[k] < 1) w.hi[k] = w.lo[k] + 1;
    w.cells.assign(256, {});
    const float cw = (w.hi[0] - w.lo[0]) / 16, cd = (w.hi[2] - w.lo[2]) / 16;
    auto cell = [](float v, float lo, float size) { return std::clamp(int((v - lo) / size), 0, 15); };
    for (const WaterPlan::Body& body : plan.bodies)
        for (size_t t : body.tris) {
            const auto &a = corner(t, 0), &b = corner(t, 1), &c = corner(t, 2);
            const std::array<float, 6> tri{a[0], a[2], b[0], b[2], c[0], c[2]};
            const int x0 = cell(std::min({a[0], b[0], c[0]}), w.lo[0], cw), x1 = cell(std::max({a[0], b[0], c[0]}), w.lo[0], cw);
            const int z0 = cell(std::min({a[2], b[2], c[2]}), w.lo[2], cd), z1 = cell(std::max({a[2], b[2], c[2]}), w.lo[2], cd);
            for (int cx = x0; cx <= x1; ++cx)
                for (int cz = z0; cz <= z1; ++cz) {
                    auto& levels = w.cells[size_t(cz) * 16 + size_t(cx)];  // z-major, as sub_8221D050 looks them up
                    auto it = std::find_if(levels.begin(), levels.end(), [&](const auto& lv) { return lv.height == body.height; });
                    if (it == levels.end()) {
                        levels.push_back({body.height, {}});
                        it = levels.end() - 1;
                    }
                    it->triangles.push_back(tri);
                }
        }
    return w;
}

// The water's mask over the region's outline: black (water) where its triangles are seen from above and a texel past.
image::Rgba regionMask(const blockmodel::WaterRegion& w) {
    const std::array<float, 2> lo{w.lo[0], w.lo[2]}, hi{w.hi[0], w.hi[2]};
    uint32_t n = kWaterMaskMin;
    while (n < kWaterMaskMax && std::max(hi[0] - lo[0], hi[1] - lo[1]) / float(n) > kWaterTexel) n *= 2;
    const float dx = (hi[0] - lo[0]) / float(n - 1), dz = (hi[1] - lo[1]) / float(n - 1);
    std::vector<uint8_t> wet(size_t(n) * n, 0);
    for (size_t v = 0; v + 2 < w.vertices.size(); v += 3)
        rasterize({w.vertices[v], w.vertices[v + 1], w.vertices[v + 2]}, lo, dx, dz, n,
                  [&](int xi, int zi, float) { wet[size_t(zi) * n + size_t(xi)] = 1; });
    image::Rgba mask;
    mask.width = mask.height = n;
    mask.pixels.assign(size_t(n) * n * 4, 255);
    for (uint32_t zi = 0; zi < n; ++zi)
        for (uint32_t xi = 0; xi < n; ++xi) {
            bool any = false;
            for (int oz = -1; oz <= 1 && !any; ++oz)
                for (int ox = -1; ox <= 1 && !any; ++ox) {
                    const int qx = int(xi) + ox, qz = int(zi) + oz;
                    any = qx >= 0 && qz >= 0 && qx < int(n) && qz < int(n) && wet[size_t(qz) * n + size_t(qx)];
                }
            if (any) std::fill_n(&mask.pixels[(size_t(zi) * n + xi) * 4], 3, uint8_t(0));
        }
    return mask;
}

// ---- World textures --------------------------------------------------------------------------
//
// Each image the model shows becomes a whole texture asset of the world's own (not split for
// streaming), a copy of a car park texture with every mip level in one piece.

constexpr const char* kTextureTemplate = "aid_texture_banjox_shared_water_testtrack_ocean_foam";  // 2D DXT1, all levels
constexpr uint32_t kTextureMax = 1024;

// The template texture: its header and GPU data as a CAFF of their own.
caff::Caff textureTemplate(const caff::Caff& carPark, const fs::path& bundle) {
    size_t header = SIZE_MAX, gpu = SIZE_MAX;
    for (size_t i = 0; i < carPark.symbols.size(); ++i) {
        const caff::Symbol& s = carPark.symbols[i];
        if (!s.section || s.section > carPark.sections.size() || assetdb::baseAssetName(carPark.symbolName(i)) != kTextureTemplate) continue;
        const std::string& section = carPark.sections[s.section - 1].name;
        if (section == ".data") header = i;
        else if (section == ".texturegpu") gpu = i;
    }
    if (header == SIZE_MAX || gpu == SIZE_MAX) fail(std::string(kTextureTemplate) + " (the world textures' template) is not in " + bundle.string());
    return standalone(carPark, {header, gpu});
}

// A side as a power of two from 4 up to `maxSide`, at least `v` when it fits.
uint32_t textureSide(uint32_t v, uint32_t maxSide) {
    uint32_t p = 4;
    while (p < v && p < maxSide) p <<= 1;
    return p;
}

// Half the size, each texel the darkest of the four it covers: a water mask's smaller levels keep thin water
// (black) that averaging would grey out, which the game reads as dry from further away.
image::Rgba darkestHalf(const image::Rgba& img) {
    image::Rgba out;
    out.width = std::max(1u, img.width / 2);
    out.height = std::max(1u, img.height / 2);
    out.pixels.assign(size_t(out.width) * out.height * 4, 255);
    for (uint32_t y = 0; y < out.height; ++y)
        for (uint32_t x = 0; x < out.width; ++x)
            for (uint32_t k = 0; k < 4; ++k) {
                uint8_t v = 255;
                for (uint32_t dy = 0; dy < 2; ++dy)
                    for (uint32_t dx = 0; dx < 2; ++dx) {
                        const uint32_t sx = std::min(img.width - 1, x * 2 + dx), sy = std::min(img.height - 1, y * 2 + dy);
                        v = std::min(v, img.pixels[(size_t(sy) * img.width + sx) * 4 + k]);
                    }
                out.pixels[(size_t(y) * out.width + x) * 4 + k] = v;
            }
    return out;
}

// The texture asset `name` showing `img`: power-of-two sides, DXT1 (DXT5 when the image has alpha),
// every mip level down to 1x1, in the template's layout, sides at most `maxSide`. `summary` says what it became.
// A water mask's levels keep the darkest texels (darkestHalf) instead of averaging.
Bytes worldTexture(const caff::Caff& tmpl, const std::string& name, const image::Rgba& img, std::string& summary,
                   uint32_t maxSide = kTextureMax, bool waterMask = false) {
    caff::Caff c = tmpl;
    renameAsset(c, kTextureTemplate, name);
    std::vector<ops::TexRef> refs = ops::findTextures(c);
    if (refs.size() != 1) fail(std::string(kTextureTemplate) + ": its copy holds no texture");
    Bytes hd = c.symbolData(refs[0].header);
    if (hd.size() < 0x38) fail(std::string(kTextureTemplate) + ": its header is cut short");
    bool alpha = false;
    for (size_t i = 3; i < img.pixels.size() && !alpha; i += 4) alpha = img.pixels[i] < 250;

    texture::Header h;
    h.d3dFormat = (be32(&hd[0x18]) & ~0x3Fu) | (alpha ? 0x14u : 0x12u);  // DXT5 or DXT1, the template's tiling and byte order
    h.kind = texture::Kind::Tex2D;
    h.width = textureSide(img.width, maxSide);
    h.height = textureSide(img.height, maxSide);
    h.baseOffset = 0;
    h.mipOffset = 0xFFFFFFFFu;  // the mip levels follow level 0
    h.mipLevels = 1;
    while ((std::max(h.width, h.height) >> (h.mipLevels - 1)) > 1) ++h.mipLevels;
    const texture::Contents cont = texture::describeContents(h, 0);
    const texture::Format f = texture::describeFormat(h.d3dFormat);
    const image::Rgba top = img.width == h.width && img.height == h.height ? img : texture::downscale(img, h.width, h.height);
    std::vector<texture::Level> levels;
    image::Rgba darkest = top;  // a water mask's current level
    for (auto [lw, lh] : texture::levelDims(h, cont)) {
        texture::Level lv;
        lv.width = lw;
        lv.height = lh;
        if (waterMask) {
            while (darkest.width > lw || darkest.height > lh) darkest = darkestHalf(darkest);
            lv.slices.push_back(texture::fromRgba(f, darkest));
        } else {
            lv.slices.push_back(texture::fromRgba(f, lw == h.width && lh == h.height ? top : texture::downscale(top, lw, lh)));
        }
        levels.push_back(std::move(lv));
    }
    putBe32(&hd[0x18], h.d3dFormat);
    putBe32(&hd[0x1C], uint32_t(texture::Kind::Tex2D));
    putBe32(&hd[0x28], 0);
    putBe32(&hd[0x2C], h.mipOffset);
    texture::writeHeaderDims(hd, h.width, h.height, h.mipLevels);
    c.replaceSymbolData(refs[0].header, hd);
    c.replaceSymbolData(refs[0].gpu, texture::encode(h, cont, levels));
    summary = std::to_string(img.width) + "x" + std::to_string(img.height) + " as " + std::to_string(h.width) + "x" +
              std::to_string(h.height) + " " + f.name + ", " + std::to_string(h.mipLevels) + " levels";
    return c.serialize();
}

// ---- Sky -------------------------------------------------------------------------------------
//
// A sphere of directions around the camera (reNut's sky shaders draw each vertex at infinity), its
// texture a panorama wrapped once around (u) from straight up to straight down (v).

constexpr uint32_t kSkySegments = 48, kSkyRings = 24;
constexpr float kSkyRadius = 1000;  // only the model's bounds see it: the shader keeps each vertex's direction

// "rrggbb" as sRGB, if it is that.
std::optional<std::array<float, 3>> hexColour(const std::string& s) {
    if (s.size() != 6 || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return std::nullopt;
    const unsigned long v = std::strtoul(s.c_str(), nullptr, 16);
    return std::array<float, 3>{float((v >> 16) & 0xFF) / 255, float((v >> 8) & 0xFF) / 255, float(v & 0xFF) / 255};
}

// The sky's texture: the panorama file `sky`, or for "rrggbb,rrggbb" a gradient from the first colour
// overhead to the second at the horizon (a little darker below it). `what` says which.
image::Rgba skyImage(const std::string& sky, std::string& what) {
    const std::vector<std::string> parts = split(sky, ',');
    if (parts.size() == 2 && !fs::exists(longPath(sky))) {
        const auto top = hexColour(trim(parts[0])), horizon = hexColour(trim(parts[1]));
        if (!top || !horizon) fail("the sky is an image file or two colours rrggbb,rrggbb (got '" + sky + "')");
        image::Rgba g;
        g.width = 4;
        g.height = 128;
        g.pixels.resize(size_t(g.width) * g.height * 4);
        for (uint32_t y = 0; y < g.height; ++y) {
            const float t = (float(y) + 0.5f) / float(g.height);  // 0 overhead, 0.5 the horizon, 1 below
            std::array<float, 3> c;
            if (t < 0.5f) {
                const float f = std::pow(t / 0.5f, 1.6f);  // most of the change near the horizon
                for (size_t k = 0; k < 3; ++k) c[k] = (*top)[k] + ((*horizon)[k] - (*top)[k]) * f;
            } else {
                for (size_t k = 0; k < 3; ++k) c[k] = (*horizon)[k] * (1 - 0.25f * (t - 0.5f) / 0.5f);
            }
            for (uint32_t x = 0; x < g.width; ++x)
                for (size_t k = 0; k < 4; ++k)
                    g.pixels[(size_t(y) * g.width + x) * 4 + k] = k < 3 ? uint8_t(std::lround(std::clamp(c[k], 0.0f, 1.0f) * 255)) : 255;
        }
        what = "a gradient from #" + trim(parts[0]) + " overhead to #" + trim(parts[1]) + " at the horizon";
        return g;
    }
    if (!fs::exists(longPath(sky))) fail("no sky image " + sky + " (or give two colours rrggbb,rrggbb)");
    image::Rgba img = image::decodeImage(readFile(sky));
    if (!img.width || !img.height || img.pixels.size() < size_t(img.width) * img.height * 4) fail(sky + " has no pixels");
    for (size_t i = 3; i < img.pixels.size(); i += 4) img.pixels[i] = 255;  // a sky is opaque
    what = fs::path(sky).filename().string() + " as a panorama";
    return img;
}

// The dome: rings from straight up to straight down, each face both ways round so either side shows.
blockmodel::WorldDraw skyDome() {
    blockmodel::WorldDraw dr;
    for (uint32_t r = 0; r <= kSkyRings; ++r) {
        const float theta = float(r) / kSkyRings * kPi;
        // v kept half a texel of a 128-high image inside, so its top and bottom rows don't wrap.
        const float v = std::clamp(float(r) / kSkyRings, 0.004f, 0.996f);
        for (uint32_t s = 0; s <= kSkySegments; ++s) {
            const float phi = float(s) / kSkySegments * 2 * kPi;
            blockmodel::WorldVertex vx;
            vx.pos = {kSkyRadius * std::sin(theta) * std::sin(phi), kSkyRadius * std::cos(theta), kSkyRadius * std::sin(theta) * std::cos(phi)};
            vx.uv = {float(s) / kSkySegments, v};
            dr.vertices.push_back(vx);
        }
    }
    const uint32_t row = kSkySegments + 1;
    for (uint32_t r = 0; r < kSkyRings; ++r)
        for (uint32_t s = 0; s < kSkySegments; ++s) {
            const uint32_t a = r * row + s, b = a + 1, c = a + row, d = c + 1;
            for (uint32_t i : {a, c, b, b, c, d, a, b, c, b, d, c}) dr.indices.push_back(i);
        }
    dr.texture = 0;
    return dr;
}

// ---- Ground ----------------------------------------------------------------------------------
//
// Where the player starts and the door home stands when not given: on solid, roughly flat ground,
// found by casting straight down against the world's triangles.

class Ground {
public:
    Ground(const std::vector<std::array<float, 3>>& pos, const std::vector<uint32_t>& tris) : pos_(pos), tris_(tris) {
        lo_ = {1e30f, 1e30f};
        hi_ = {-1e30f, -1e30f};
        for (auto& p : pos)
            for (size_t k = 0; k < 2; ++k) lo_[k] = std::min(lo_[k], p[k * 2]), hi_[k] = std::max(hi_[k], p[k * 2]);
        const size_t count = tris.size() / 3;
        cells_ = std::clamp<uint32_t>(uint32_t(std::sqrt(double(count) / 4)), 1, 256);
        grid_.resize(size_t(cells_) * cells_);
        for (size_t t = 0; t < count; ++t) {
            std::array<float, 2> a{1e30f, 1e30f}, b{-1e30f, -1e30f};
            for (size_t c = 0; c < 3; ++c)
                for (size_t k = 0; k < 2; ++k) a[k] = std::min(a[k], corner(t, c)[k * 2]), b[k] = std::max(b[k], corner(t, c)[k * 2]);
            const uint32_t x0 = cell(a[0], 0), x1 = cell(b[0], 0), z0 = cell(a[1], 1), z1 = cell(b[1], 1);
            for (uint32_t z = z0; z <= z1; ++z)
                for (uint32_t x = x0; x <= x1; ++x) grid_[size_t(z) * cells_ + x].push_back(uint32_t(t));
        }
    }

    // The middle of the world seen from above, and its larger side.
    std::array<float, 2> middle() const { return {(lo_[0] + hi_[0]) / 2, (lo_[1] + hi_[1]) / 2}; }
    float size() const { return std::max(hi_[0] - lo_[0], hi_[1] - lo_[1]); }

    // The topmost surface under (x, z): its height and how upright it is (1 level, 0 a wall).
    std::optional<std::pair<float, float>> down(float x, float z) const {
        if (x < lo_[0] || x > hi_[0] || z < lo_[1] || z > hi_[1]) return std::nullopt;
        std::optional<std::pair<float, float>> best;
        for (uint32_t t : grid_[size_t(cell(z, 1)) * cells_ + cell(x, 0)]) {
            const auto &a = corner(t, 0), &b = corner(t, 1), &c = corner(t, 2);
            const float det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
            if (std::fabs(det) < 1e-9f) continue;  // edge-on from above
            const float l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / det;
            const float l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / det;
            const float l3 = 1 - l1 - l2;
            if (l1 < -1e-5f || l2 < -1e-5f || l3 < -1e-5f) continue;
            const float y = l1 * a[1] + l2 * b[1] + l3 * c[1];
            if (best && best->first >= y) continue;
            const std::array<float, 3> u{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const std::array<float, 3> n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            best = std::make_pair(y, len > 0 ? std::fabs(n[1]) / len : 0.0f);
        }
        return best;
    }

    // The ground's height at (x, z) when it and a 1.5 m circle around it are solid and roughly flat.
    std::optional<float> flat(float x, float z) const {
        const auto at = down(x, z);
        if (!at || at->second < 0.8f) return std::nullopt;
        for (int i = 0; i < 8; ++i) {
            const float a = float(i) * kPi / 4;
            const auto around = down(x + 1.5f * std::cos(a), z + 1.5f * std::sin(a));
            if (!around || around->second < 0.7f || std::fabs(around->first - at->first) > 0.75f) return std::nullopt;
        }
        return at->first;
    }

private:
    const std::array<float, 3>& corner(size_t t, size_t c) const { return pos_[tris_[3 * t + c]]; }
    uint32_t cell(float v, size_t axis) const {
        const float span = hi_[axis] - lo_[axis];
        if (!(span > 0)) return 0;
        return std::min<uint32_t>(cells_ - 1, uint32_t(std::max(0.0f, (v - lo_[axis]) / span * float(cells_))));
    }

    const std::vector<std::array<float, 3>>& pos_;
    const std::vector<uint32_t>& tris_;
    std::array<float, 2> lo_{}, hi_{};
    uint32_t cells_ = 1;
    std::vector<std::vector<uint32_t>> grid_;
};

// The flat ground nearest the world's middle, on a grid of about 160 steps across: x, ground y, z.
std::optional<std::array<float, 3>> groundNearMiddle(const Ground& g) {
    const std::array<float, 2> m = g.middle();
    const float step = std::max(0.5f, g.size() / 160);
    const int reach = int(std::ceil(g.size() / 2 / step)) + 1;
    std::vector<std::pair<int, std::array<int, 2>>> spots;
    for (int z = -reach; z <= reach; ++z)
        for (int x = -reach; x <= reach; ++x) spots.push_back({x * x + z * z, {x, z}});
    std::sort(spots.begin(), spots.end());
    for (const auto& spot : spots) {
        const float x = m[0] + float(spot.second[0]) * step, z = m[1] + float(spot.second[1]) * step;
        if (auto y = g.flat(x, z)) return std::array<float, 3>{x, *y, z};
    }
    return std::nullopt;
}

// Flat ground 10-15 m from `from` (x, ground y, z), first at about its height, then at any.
std::optional<std::array<float, 3>> groundBeside(const Ground& g, const std::array<float, 3>& from) {
    for (int pass = 0; pass < 2; ++pass)
        for (float dist : {12.0f, 10.0f, 14.0f, 11.0f, 13.0f, 15.0f})
            for (int i = 0; i < 24; ++i) {
                const float a = float(i) * kPi / 12, x = from[0] + dist * std::sin(a), z = from[2] + dist * std::cos(a);
                const auto y = g.flat(x, z);
                if (y && (pass == 1 || std::fabs(*y - from[1]) <= 2)) return std::array<float, 3>{x, *y, z};
            }
    return std::nullopt;
}

// A resident bundle (Bundle/4f) of whole assets, each given as a standalone CAFF: their symbols in its
// sections, then its manifest {magic, timestamp, ->entries, count, ->dependencies, 0, 0, 0, entries}.
Bytes residentBundle(const std::vector<Bytes>& assets) {
    caff::Caff out;
    auto sectionIndex = [&](const caff::Section& s) {
        for (size_t i = 0; i < out.sections.size(); ++i)
            if (out.sections[i].name == s.name) return uint8_t(i + 1);
        caff::Section n;
        n.name = s.name;
        n.alignLog2 = s.alignLog2;
        out.sections.push_back(n);
        return uint8_t(out.sections.size());
    };
    // Asset id, its symbol name index as the symbols store it (1-based, like the game's bundles).
    std::vector<std::pair<uint32_t, uint32_t>> entries;
    for (const Bytes& file : assets) {
        const caff::Caff c = caff::Caff::parse(file);
        if (!c.pools.empty()) fail("a world texture has string pools, which its bundle can't take");
        const uint32_t base = uint32_t(out.symbols.size());
        std::vector<uint32_t> names(c.symbolNames.size() + 1, 0);
        for (size_t s = 0; s < c.symbols.size(); ++s) {
            const caff::Symbol& sym = c.symbols[s];
            const caff::Section& from = c.sectionOf(sym);
            const uint8_t sec = sectionIndex(from);
            caff::Section& to = out.sections[sec - 1u];
            to.alignLog2 = std::max(to.alignLog2, from.alignLog2);
            while (to.data.size() % (size_t(1) << from.alignLog2)) to.data.push_back(0);
            const Bytes d = c.symbolData(s);
            uint32_t name = 0;
            if (sym.nameIndex) {
                if (!names[sym.nameIndex]) {
                    out.symbolNames.push_back(c.symbolNames[sym.nameIndex - 1]);
                    names[sym.nameIndex] = uint32_t(out.symbolNames.size());
                }
                name = names[sym.nameIndex];
            }
            out.symbols.push_back({name, uint32_t(to.data.size()), uint32_t(d.size()), sec, sym.kind});
            to.data.insert(to.data.end(), d.begin(), d.end());
        }
        for (int set = 0; set < 2; ++set) {
            for (caff::Relocation r : c.relocs[set]) {
                r.from += base;
                r.to += base;
                out.relocs[set].push_back(r);
            }
            for (caff::SwapEntry sw : c.swaps[set]) {
                sw.symbol += base;
                out.swaps[set].push_back(sw);
            }
        }
        if (c.symbols.empty() || !c.symbols[0].nameIndex) fail("a world texture has no name");
        entries.push_back({assetId(assetdb::baseAssetName(c.symbolNames[c.symbols[0].nameIndex - 1])),
                           names[c.symbols[0].nameIndex]});
    }
    // The manifest, last, in .data, its two pointers to itself.
    Writer m;
    m.u32(0x438CB47C);
    m.u32(uint32_t(std::time(nullptr)));
    m.u32(0x20);
    m.u32(uint32_t(entries.size()));
    m.u32(0x20 + 8 * uint32_t(entries.size()));  // no dependencies
    m.u32(0);
    m.u32(0);  // no streamed assets
    m.u32(0);
    for (auto& [id, name] : entries) {
        m.u32(id);
        m.u32(name);
    }
    caff::Section data;
    data.name = ".data";
    data.alignLog2 = 5;
    const uint8_t sec = sectionIndex(data);
    caff::Section& to = out.sections[sec - 1u];
    while (to.data.size() % 32) to.data.push_back(0);
    out.symbolNames.push_back("manifest");
    out.symbols.push_back({uint32_t(out.symbolNames.size()), uint32_t(to.data.size()), uint32_t(m.buf().size()), sec, 4});
    to.data.insert(to.data.end(), m.buf().begin(), m.buf().end());
    const uint32_t manifest = uint32_t(out.symbols.size());
    out.relocs[0].push_back({manifest, manifest, {8, 16}});
    for (caff::Section& s : out.sections)
        while (s.data.size() % (size_t(1) << s.alignLog2)) s.data.push_back(0);
    return out.serialize();
}

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) out += ch == '"' || ch == '\\' ? std::string("\\") + ch : std::string(1, ch);
    return out + "\"";
}

std::string number(float f) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", double(f));
    std::string s = b;
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s;
}

std::string utf8(const fs::path& p) {
    const auto u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

// A materials file's alpha mode by name.
uint32_t alphaOf(const std::string& name) {
    const std::string n = lower(trim(name));
    if (n == "opaque") return blockmodel::WorldMaterial::Opaque;
    if (n == "cutout" || n == "mask") return blockmodel::WorldMaterial::Cutout;
    if (n == "blend") return blockmodel::WorldMaterial::Blend;
    if (n == "add" || n == "additive") return blockmodel::WorldMaterial::Add;
    fail("a material's alpha is opaque, cutout, blend or add (got " + name + ")");
}

const char* alphaName(uint32_t alpha) {
    static const char* const kNames[] = {"opaque", "cutout", "blend", "add"};
    return kNames[alpha & 3];
}

// "rrggbb" or [r, g, b] (0 to 1) as a colour.
std::array<float, 3> colourOf(const Json& j, const std::string& what) {
    if (j.type() == Json::String) {
        const std::string s = trim(j.asString());
        if (s.size() != 6 || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            fail(what + " is rrggbb or [r, g, b] (got " + s + ")");
        const uint32_t v = parseHex(s);
        return {float((v >> 16) & 0xFF) / 255, float((v >> 8) & 0xFF) / 255, float(v & 0xFF) / 255};
    }
    if (j.type() == Json::Array && j.size() == 3)
        return {float(j[0].asNumber()), float(j[1].asNumber()), float(j[2].asNumber())};
    fail(what + " is rrggbb or [r, g, b]");
}

// The model's materials with `settings` over them (by name, any case), their emissive textures added to
// the scene's images.
void applyMaterials(gltfobj::SceneMesh& src, const std::map<std::string, MaterialSettings>& settings, const Say& say) {
    std::map<std::string, int> loaded;  // emissive image files, by path
    for (const auto& [name, s] : settings) {
        bool found = false;
        for (gltfobj::SceneMesh::Material& m : src.materials) {
            if (lower(m.name) != lower(name)) continue;
            found = true;
            blockmodel::WorldMaterial& look = m.look;
            if (s.alpha) look.alpha = alphaOf(*s.alpha);
            if (s.cutoff) look.cutoff = std::clamp(*s.cutoff, 0.0f, 1.0f);
            if (s.opacity) look.opacity = std::clamp(*s.opacity, 0.0f, 1.0f);
            if (s.emissive) look.emissive = *s.emissive;
            if (s.strength) look.strength = std::max(0.0f, *s.strength);
            if (s.glow) look.glow = *s.glow;
            if (s.doubleSided) look.doubleSided = *s.doubleSided;
            if (s.unlit) look.unlit = *s.unlit;
            if (s.solid) m.solid = *s.solid;
            if (s.emissiveTexture && s.emissiveTexture->empty()) {
                look.emissiveTexture = -1;
            } else if (s.emissiveTexture) {
                const std::string key = utf8(*s.emissiveTexture);
                auto [it, fresh] = loaded.emplace(key, -1);
                if (fresh) {
                    image::Rgba img = image::decodeImage(readFile(*s.emissiveTexture));
                    if (!img.width || !img.height) fail("the emissive texture " + key + " has no pixels");
                    it->second = int(src.images.size());
                    src.images.push_back(std::move(img));
                    src.imageNames.push_back(key);
                }
                look.emissiveTexture = it->second;
                // A texture alone gives off its own colour.
                if (!s.emissive && look.emissive == std::array<float, 3>{0, 0, 0}) look.emissive = {1, 1, 1};
            }
        }
        if (!found) {
            std::string names;
            for (const auto& m : src.materials) names += (names.empty() ? "" : ", ") + m.name;
            say("warning: no material \"" + name + "\" in the model to set (it has: " + (names.empty() ? "none" : names) + ")");
        }
    }
}

// What a material does differently from the plain look, for the log ("" for nothing).
std::string lookSummary(const blockmodel::WorldMaterial& m) {
    std::string out;
    auto add = [&](const std::string& s) { out += (out.empty() ? "" : ", ") + s; };
    if (m.alpha != blockmodel::WorldMaterial::Cutout || m.cutoff != 0.5f) add(std::string(alphaName(m.alpha)) + (m.alpha == 1 ? " at " + number(m.cutoff) : ""));
    if (m.opacity < 1) add("opacity " + number(m.opacity));
    if (m.emissive != std::array<float, 3>{0, 0, 0}) add("emissive x" + number(m.strength) + (m.emissiveTexture >= 0 ? " (textured)" : ""));
    if (m.glow >= 0) add("glow " + number(m.glow));
    if (m.doubleSided) add("double-sided");
    if (m.unlit) add("unlit");
    return out;
}

// `more` added to `into`, as if one file: its images, materials, water and shape keys joined to `into`'s (by name).
void appendScene(gltfobj::SceneMesh& into, const gltfobj::SceneMesh& more) {
    const uint32_t vertexBase = uint32_t(into.positions.size());
    const size_t intoTriangles = into.indices.size() / 3, moreTriangles = more.indices.size() / 3;
    const int imageBase = int(into.images.size());
    // Per-triangle lists one side may have left empty: -1 (none) for its triangles.
    const auto join = [&](std::vector<int>& a, const std::vector<int>& b, const std::function<int(int)>& map) {
        if (a.empty() && b.empty()) return;
        a.resize(intoTriangles, -1);
        for (size_t t = 0; t < moreTriangles; ++t) a.push_back(t < b.size() && b[t] >= 0 ? map(b[t]) : -1);
    };
    join(into.triangleImage, more.triangleImage, [&](int i) { return i + imageBase; });
    std::vector<int> materialOf;
    for (const gltfobj::SceneMesh::Material& m : more.materials) {
        blockmodel::WorldMaterial look = m.look;
        if (look.emissiveTexture >= 0) look.emissiveTexture += imageBase;
        materialOf.push_back(gltfobj::addMaterial(into.materials, m.name, look));
    }
    join(into.triangleMaterial, more.triangleMaterial, [&](int i) { return materialOf[size_t(i)]; });
    std::vector<int> waterOf;
    for (const gltfobj::SceneMesh::Water& wt : more.water) {
        int at = -1;
        for (const auto& tri : wt.triangles) at = gltfobj::addWater(into.water, wt.name, tri);
        waterOf.push_back(at);  // -1 (none) for a body without triangles
    }
    join(into.triangleWater, more.triangleWater, [&](int i) { return waterOf[size_t(i)]; });
    std::vector<uint32_t> shapeOf;
    for (size_t s = 0; s < more.shapeNames.size(); ++s) {
        const auto it = std::find(into.shapeNames.begin(), into.shapeNames.end(), more.shapeNames[s]);
        shapeOf.push_back(uint32_t(it - into.shapeNames.begin()));
        if (it == into.shapeNames.end()) {
            into.shapeNames.push_back(more.shapeNames[s]);
            into.shapeWeights.push_back(s < more.shapeWeights.size() ? more.shapeWeights[s] : 0);
        }
    }
    if (!into.vertexShapes.empty() || !more.vertexShapes.empty()) {
        into.vertexShapes.resize(vertexBase);
        for (size_t v = 0; v < more.positions.size(); ++v) {
            std::vector<blockmodel::ShapeDelta> deltas = v < more.vertexShapes.size() ? more.vertexShapes[v] : std::vector<blockmodel::ShapeDelta>{};
            for (blockmodel::ShapeDelta& d : deltas) d.shape = shapeOf[d.shape];
            into.vertexShapes.push_back(std::move(deltas));
        }
    }
    into.positions.insert(into.positions.end(), more.positions.begin(), more.positions.end());
    into.normals.insert(into.normals.end(), more.normals.begin(), more.normals.end());
    into.colours.insert(into.colours.end(), more.colours.begin(), more.colours.end());
    into.uvs.insert(into.uvs.end(), more.uvs.begin(), more.uvs.end());
    for (uint32_t i : more.indices) into.indices.push_back(vertexBase + i);
    into.images.insert(into.images.end(), more.images.begin(), more.images.end());
    into.imageNames.insert(into.imageNames.end(), more.imageNames.begin(), more.imageNames.end());
    into.warnings.insert(into.warnings.end(), more.warnings.begin(), more.warnings.end());
}

} // namespace

caff::Caff copyAssets(const caff::Caff& src, const std::vector<size_t>& syms) { return standalone(src, syms); }

std::function<Bytes(const std::string&, const image::Rgba&, std::string&)> textureMaker(const fs::path& assets, uint32_t maxSide) {
    const fs::path bundle = carParkBundle(assets);
    Loaded l = loadAsset(bundle);
    if (!l.caff) fail(bundle.string() + " (the car park, which holds the textures' template) is not a bundle");
    auto tmpl = std::make_shared<const caff::Caff>(textureTemplate(*l.caff, bundle));
    return [tmpl, maxSide](const std::string& name, const image::Rgba& img, std::string& summary) {
        return worldTexture(*tmpl, name, img, summary, maxSide);
    };
}
void renameAssetIn(caff::Caff& c, const std::string& from, const std::string& to) { renameAsset(c, from, to); }
int retextureIn(caff::Caff& c, size_t sym, const std::map<std::string, std::string>& names) { return retexture(c, sym, names); }

std::vector<streambundle::Entry> textureCopy(const caff::Caff& res, const fs::path& tops, const std::string& from, const std::string& to,
                                             const image::Rgba& img, uint32_t maxSide, std::map<std::string, std::string>& names,
                                             std::string& summary) {
    bool split = false;
    for (size_t i = 0; i < res.symbols.size() && !split; ++i) {
        const caff::Symbol& s = res.symbols[i];
        split = s.section && s.section <= res.sections.size() && res.sections[s.section - 1].name == ".data" &&
                assetdb::baseAssetName(res.symbolName(i)) == from + "mip";
    }
    const std::string half = split ? "mip" : "";
    const caff::Caff original = standalone(res, {symbolOf(res, from + half, ".data"), symbolOf(res, from + half, ".texturegpu")});
    // A split texture's streamed top level, renamed with it.
    streambundle::StreamBundle sb;
    if (split) {
        std::vector<streambundle::IndexEntry> index;
        if (!streambundle::readIndex(longPath(tops), index)) fail(tops.string() + " is not a stream bundle");
        const uint32_t topId = assetId(from + "top");
        std::optional<caff::Caff> top;
        for (const auto& e : index)
            if (e.id == topId && e.size && !top) {
                const Bytes d = streambundle::readEntry(longPath(tops), e);
                if (caff::isCaff(d)) top = caff::Caff::parse(d);
            }
        if (!top) fail(from + "top is not in " + tops.string());
        renameAsset(*top, from + "top", to + "top");
        sb.entries.push_back({assetId(to + "top"), top->serialize()});
        names[from + "top"] = to + "top";
    }
    names[from + half] = to + half;
    // The image at its own size (a power of two, at most maxSide), else at the texture's.
    auto attempt = [&](uint32_t w, uint32_t h) {
        caff::Caff c = original;
        renameAsset(c, from + half, to + half);
        std::vector<ops::TexRef> refs = ops::findTextures(c);
        if (refs.size() != 1) fail(from + half + ": its copy holds no texture");
        if (!w) {
            const texture::Header th = texture::parseHeader(c.symbolData(refs[0].header));
            w = th.width, h = th.height;
        }
        ops::TextureImage ti;
        ti.width = w;
        ti.height = h;
        ti.rgba = img.width == w && img.height == h ? img : texture::downscale(img, w, h);
        streambundle::StreamBundle s = sb;
        bool topChanged = false;
        summary = ops::importTexture(c, refs[0], ti, split ? &s : nullptr, split ? &topChanged : nullptr);
        if (split && !topChanged) fail(to + "top: its level 0 was not written");
        std::vector<streambundle::Entry> out{{assetId(to + half), c.serialize()}};
        if (split) out.push_back(s.entries[0]);
        return out;
    };
    try {
        return attempt(textureSide(img.width, maxSide), textureSide(img.height, maxSide));
    } catch (const std::exception&) {
        return attempt(0, 0);
    }
}

std::map<std::string, MaterialSettings> loadMaterials(const fs::path& json) {
    const Json root = Json::parse(readText(json));
    if (!root.has("materials") || root["materials"].type() != Json::Object) fail(json.string() + ": no \"materials\" object");
    std::map<std::string, MaterialSettings> out;
    for (const auto& [name, j] : root["materials"].members()) {
        const std::string what = json.filename().string() + ": material \"" + name + "\"";
        if (j.type() != Json::Object) fail(what + " is an object of settings");
        MaterialSettings s;
        for (const auto& [key, v] : j.members()) {
            if (key == "alpha") s.alpha = alphaName(alphaOf(v.asString()));
            else if (key == "cutoff") s.cutoff = float(v.asNumber());
            else if (key == "opacity") s.opacity = float(v.asNumber());
            else if (key == "emissive") s.emissive = colourOf(v, what + "'s emissive");
            else if (key == "emissiveStrength") s.strength = float(v.asNumber());
            else if (key == "emissiveTexture") {
                const std::string file = v.asString();
                const fs::path f = std::u8string(file.begin(), file.end());
                s.emissiveTexture = file.empty() ? fs::path() : f.is_relative() ? json.parent_path() / f : f;
            } else if (key == "glow") s.glow = float(v.asNumber());
            else if (key == "doubleSided") s.doubleSided = v.asBool();
            else if (key == "unlit") s.unlit = v.asBool();
            else if (key == "solid") s.solid = v.asBool();
            else fail(what + ": unknown setting \"" + key + "\"");
        }
        out[name] = s;
    }
    return out;
}

std::string materialsJson(const std::map<std::string, MaterialSettings>& materials, const fs::path& json) {
    Json all = Json::object();
    for (const auto& [name, s] : materials) {
        Json j = Json::object();
        if (s.alpha) j["alpha"] = *s.alpha;
        if (s.cutoff) j["cutoff"] = double(*s.cutoff);
        if (s.opacity) j["opacity"] = double(*s.opacity);
        if (s.emissive) {
            char hex[8];
            std::snprintf(hex, sizeof hex, "%02x%02x%02x", unsigned(std::lround(std::clamp((*s.emissive)[0], 0.0f, 1.0f) * 255)),
                          unsigned(std::lround(std::clamp((*s.emissive)[1], 0.0f, 1.0f) * 255)),
                          unsigned(std::lround(std::clamp((*s.emissive)[2], 0.0f, 1.0f) * 255)));
            j["emissive"] = std::string(hex);
        }
        if (s.strength) j["emissiveStrength"] = double(*s.strength);
        if (s.emissiveTexture) {
            // Under the file's folder: written relative to it.
            std::error_code ec;
            fs::path abs = fs::absolute(*s.emissiveTexture, ec).lexically_normal();
            if (ec) abs = *s.emissiveTexture;
            const fs::path rel = abs.lexically_relative(fs::absolute(json.parent_path(), ec).lexically_normal());
            const bool under = !rel.empty() && *rel.begin() != "..";
            j["emissiveTexture"] = s.emissiveTexture->empty() ? std::string() : utf8(under ? rel : abs);
        }
        if (s.glow) j["glow"] = double(*s.glow);
        if (s.doubleSided) j["doubleSided"] = *s.doubleSided;
        if (s.unlit) j["unlit"] = *s.unlit;
        if (s.solid) j["solid"] = *s.solid;
        all[name] = j;
    }
    Json root = Json::object();
    root["materials"] = all;
    return root.dump() + "\n";
}

std::vector<std::pair<std::string, MaterialSettings>> modelMaterials(const fs::path& model) {
    const std::string ext = lower(model.extension().string());
    const gltfobj::SceneMesh src = ext == ".obj"   ? gltfobj::objScene(model, 40, {})
                                   : ext == ".dae" ? gltfobj::daeScene(model, 40, {})
                                                   : gltfobj::scene(readFile(model), 40, model.parent_path(), {});
    std::vector<std::pair<std::string, MaterialSettings>> out;
    for (const gltfobj::SceneMesh::Material& m : src.materials) {
        MaterialSettings s;
        s.alpha = alphaName(m.look.alpha);
        s.cutoff = m.look.cutoff;
        s.opacity = m.look.opacity;
        s.emissive = m.look.emissive;
        s.strength = m.look.strength;
        s.glow = m.look.glow;
        s.doubleSided = m.look.doubleSided;
        s.unlit = m.look.unlit;
        out.push_back({m.name, s});
    }
    return out;
}

void make(const fs::path& assets, const fs::path& mod, const World& w, bool force, const Say& say) {
    if (!renutpart::validId(w.id)) fail("a world id is 1-40 characters of a-z, 0-9 and _ (got '" + w.id + "')");
    if (w.model.empty()) fail("a world needs its model (.glb, .dae, or .obj with its .mtl)");
    if (!w.hologram.empty() && !fs::exists(longPath(w.hologram))) fail("no hologram image " + w.hologram.string());
    const bool arch = w.doorStyle == "arch";
    if (w.doorStyle != "wall" && !arch && w.doorStyle != "palace" && w.doorStyle != "hidden")
        fail("the door's style is wall, arch, palace or hidden (got " + w.doorStyle + ")");
    if (!w.hologram.empty() && w.doorStyle != "wall" && !arch) fail("only wall and arch doors have a hologram");
    const std::string stem = "renutworld_" + w.id;
    const std::string modelName = "aid_model_" + stem, havokName = "aid_havok_" + stem, markerName = "aid_marker_" + stem,
                      sceneName = "aid_script_" + stem, streamName = "aid_streambundle_" + stem;
    const std::string skyModelName = modelName + "_sky", skyTextureName = "aid_texture_" + stem + "_sky";

    // The model: every mesh node of the file (and of the more models), with their materials' colours and textures,
    // scaled, on its ground.
    const auto load = [&](const fs::path& file) {
        const std::string ext = lower(file.extension().string());
        return ext == ".obj"   ? gltfobj::objScene(file, w.smoothDegrees, w.waterNames)
               : ext == ".dae" ? gltfobj::daeScene(file, w.smoothDegrees, w.waterNames)
                               : gltfobj::scene(readFile(file), w.smoothDegrees, file.parent_path(), w.waterNames);
    };
    gltfobj::SceneMesh src = load(w.model);
    for (const fs::path& more : w.moreModels) appendScene(src, load(more));
    for (const std::string& s : src.warnings) say("warning: " + s);
    // Its materials' settings: the file's, then the materials file's and the world's own over them.
    {
        std::map<std::string, MaterialSettings> settings;
        if (!w.materialsFile.empty()) settings = loadMaterials(w.materialsFile);
        for (const auto& [name, s] : w.materials) settings[name] = s;
        applyMaterials(src, settings, say);
        for (const gltfobj::SceneMesh::Material& m : src.materials)
            if (const std::string what = lookSummary(m.look); !what.empty()) say("material " + m.name + ": " + what);
    }
    if (!(w.scale > 0)) fail("the scale is more than 0");
    if (!(w.colourScale > 0)) fail("the colour scale is more than 0");
    for (auto& p : src.positions)
        for (float& c : p) c *= w.scale;
    for (auto& vertex : src.vertexShapes)
        for (blockmodel::ShapeDelta& d : vertex)
            for (float& c : d.position) c *= w.scale;
    for (auto& c : src.colours)
        for (size_t k = 0; k < 3; ++k) c[k] = std::min(1.0f, c[k] * w.colourScale);
    if (w.ground > 0) {
        std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (auto& p : src.positions)
            for (size_t k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
        const float cx = (lo[0] + hi[0]) / 2, cz = (lo[2] + hi[2]) / 2, h = w.ground / 2;
        const uint32_t first = uint32_t(src.positions.size());
        for (int corner = 0; corner < 4; ++corner) {
            src.positions.push_back({cx + (corner & 1 ? h : -h), lo[1], cz + (corner & 2 ? h : -h)});
            src.normals.push_back({0, 1, 0});
            src.colours.push_back({w.groundColour[0], w.groundColour[1], w.groundColour[2], 1});
            src.uvs.push_back({0, 0});
        }
        // Two triangles facing up (counter-clockwise seen from above, as the model's faces are), untextured.
        for (uint32_t i : {0u, 2u, 1u, 1u, 2u, 3u}) src.indices.push_back(first + i);
        src.triangleImage.push_back(-1);
        src.triangleImage.push_back(-1);
    }

    // The textures, first in the stream bundle so they are loaded as the model looks them up: each
    // image a triangle shows, and a small white one for the triangles without (their colour shows).
    const fs::path carParkPath = carParkBundle(assets);
    Loaded carPark = loadAsset(carParkPath);
    if (!carPark.caff) fail(carParkPath.string() + " is not the car park's bundle (is the assets folder the game's?)");
    const size_t triangles = src.indices.size() / 3;
    std::vector<int> textureOf(src.images.size() + 1, -1);  // image (the last: none) -> texture
    std::vector<std::string> textureNames;
    std::vector<uint32_t> triangleTexture(triangles);
    for (size_t t = 0; t < triangles; ++t) {
        const int image = t < src.triangleImage.size() ? src.triangleImage[t] : -1;
        const size_t key = image >= 0 && size_t(image) < src.images.size() ? size_t(image) : src.images.size();
        if (textureOf[key] < 0) {
            textureOf[key] = int(textureNames.size());
            textureNames.push_back("aid_texture_" + stem + "_" + (key < src.images.size() ? std::to_string(key) : std::string("white")));
        }
        triangleTexture[t] = uint32_t(textureOf[key]);
    }
    // Each material's emissive texture (-1: none), for the draws that show it (fetch constant 1).
    auto materialOf = [&](size_t t) { return t < src.triangleMaterial.size() ? src.triangleMaterial[t] : -1; };
    std::vector<int> materialEmissive(src.materials.size(), -1);
    for (size_t t = 0; t < triangles; ++t) {
        const int mat = materialOf(t);
        if (mat < 0 || size_t(mat) >= src.materials.size() || materialEmissive[size_t(mat)] >= 0) continue;
        const int image = src.materials[size_t(mat)].look.emissiveTexture;
        if (image < 0 || size_t(image) >= src.images.size()) continue;
        if (textureOf[size_t(image)] < 0) {
            textureOf[size_t(image)] = int(textureNames.size());
            textureNames.push_back("aid_texture_" + stem + "_" + std::to_string(image));
        }
        materialEmissive[size_t(mat)] = textureOf[size_t(image)];
    }
    // The textures go in a resident bundle of the world's: a stream bundle's textures are only the
    // top levels of resident ones, which is how the game reads them.
    std::vector<streambundle::Entry> world, textures;
    const caff::Caff tmpl = textureTemplate(*carPark.caff, carParkPath);
    if (w.maxTexture < 4 || (w.maxTexture & (w.maxTexture - 1)) || w.maxTexture > 2048)
        fail("the largest texture side is a power of two from 4 to 2048 (got " + std::to_string(w.maxTexture) + ")");
    const uint32_t maxTexture = w.maxTexture;
    {
        for (size_t key = 0; key < textureOf.size(); ++key) {
            if (textureOf[key] < 0) continue;
            const std::string& name = textureNames[size_t(textureOf[key])];
            image::Rgba white;
            white.width = white.height = 4;
            white.pixels.assign(4 * 4 * 4, 255);
            const bool none = key == src.images.size();
            std::string summary;
            textures.push_back({assetId(name), worldTexture(tmpl, name, none ? white : src.images[key], summary, maxTexture)});
            say(name + ": " + (none ? std::string("white, for what shows no texture") : src.imageNames[key]) + ", " + summary);
        }
        if (!w.sky.empty()) {
            std::string what;
            const image::Rgba img = skyImage(w.sky, what);
            std::string summary;
            textures.push_back({assetId(skyTextureName), worldTexture(tmpl, skyTextureName, img, summary, maxTexture)});
            say(skyTextureName + ": " + what + ", " + summary);
        }
    }
    if (!w.sky.empty()) {
        world.push_back({assetId(skyModelName), blockmodel::writeSky(skyModelName, {skyDome()}, {skyTextureName})});
        say(skyModelName + ": the sky, a dome around the camera drawn behind everything");
    }

    // Water: one region the game draws from the water's own triangles, as its levels' water (chunk 0x26 of the
    // model), every flat level and fall in it; the car park's calm water setup with a mask of its shape.
    std::string waterSetups, waterLua;
    const WaterPlan waterPlan = planWater(src);
    std::optional<blockmodel::WaterRegion> waterRegion;
    if (waterPlan.falls)
        say(std::to_string(waterPlan.falls) + " steep water triangle(s) (falls) are drawn as water you don't swim in");
    if (!waterPlan.bodies.empty() || waterPlan.falls) {
        waterRegion = waterRegionOf(src, waterPlan);
        const blockmodel::WaterRegion& wr = *waterRegion;
        const std::string mask = "aid_texture_" + stem + "_watermask";
        std::string summary;
        textures.push_back({assetId(mask), worldTexture(tmpl, mask, regionMask(wr), summary, kWaterMaskMax, true)});  // not --max-texture's
        std::vector<uint32_t> args = calmWaterSetup(*carPark.caff);
        args[kWaterMaskArg] = assetId(mask);
        waterSetups += "op_7D";
        for (uint32_t a : args) waterSetups += " 0x" + hex32(a);
        waterSetups += "\n";
        for (const WaterPlan::Body& b : waterPlan.bodies) {
            char where[200];
            std::snprintf(where, sizeof where, "%s: water at height %.2f (%zu triangles)", b.name.c_str(), double(b.height), b.tris.size());
            say(where);
        }
        char where[200];
        std::snprintf(where, sizeof where, "the water: %zu triangles over x %.1f to %.1f, z %.1f to %.1f (its mask %s)",
                      wr.vertices.size() / 3, double(wr.lo[0]), double(wr.hi[0]), double(wr.lo[2]), double(wr.hi[2]), summary.c_str());
        say(where);
    }

    // The collision's places: the triangles' corners, one vertex per place (the start and door home
    // are found on them).
    std::vector<std::array<float, 3>> places;
    std::map<std::array<long, 3>, uint32_t> weld;
    std::vector<uint32_t> at(src.positions.size());
    for (size_t i = 0; i < src.positions.size(); ++i) {
        const auto& p = src.positions[i];
        const std::array<long, 3> key{std::lround(p[0] * 1e4f), std::lround(p[1] * 1e4f), std::lround(p[2] * 1e4f)};
        auto [it, fresh] = weld.emplace(key, uint32_t(places.size()));
        if (fresh) places.push_back(p);
        at[i] = it->second;
    }
    std::vector<uint32_t> tris, solidTris;  // every triangle's places; the solid ones' (not water, nor light)
    for (uint32_t i : src.indices) tris.push_back(at[i]);
    auto solidMaterial = [&](size_t t) {
        const int m = t < src.triangleMaterial.size() ? src.triangleMaterial[t] : -1;
        if (m < 0 || size_t(m) >= src.materials.size()) return true;
        const gltfobj::SceneMesh::Material& mat = src.materials[size_t(m)];
        return mat.solid.value_or(mat.look.alpha != blockmodel::WorldMaterial::Add);
    };
    for (size_t t = 0; t < triangles; ++t)
        if (waterPlan.use[t] == WaterUse::Ground && solidMaterial(t))
            for (size_t c = 0; c < 3; ++c) solidTris.push_back(tris[3 * t + c]);

    // The chunks: a collision holds at most 65535 places (16-bit indices), so a big world is cut into
    // pieces of neighbouring triangles (in Morton order of their middles), each a model and collision.
    std::vector<std::vector<uint32_t>> chunks;
    {
        std::array<float, 3> clo{1e30f, 1e30f, 1e30f}, chi{-1e30f, -1e30f, -1e30f};
        for (auto& p : places)
            for (size_t k = 0; k < 3; ++k) clo[k] = std::min(clo[k], p[k]), chi[k] = std::max(chi[k], p[k]);
        auto spread = [](uint32_t v) {  // 16 bits apart to every other bit
            v &= 0xFFFF;
            v = (v | (v << 8)) & 0x00FF00FF;
            v = (v | (v << 4)) & 0x0F0F0F0F;
            v = (v | (v << 2)) & 0x33333333;
            return (v | (v << 1)) & 0x55555555;
        };
        std::vector<std::pair<uint32_t, uint32_t>> order(triangles);  // Morton code, triangle
        for (size_t t = 0; t < triangles; ++t) {
            float mx = 0, mz = 0;
            for (size_t c = 0; c < 3; ++c) mx += places[tris[3 * t + c]][0] / 3, mz += places[tris[3 * t + c]][2] / 3;
            const auto cell = [](float v, float a, float b) {
                return b > a ? uint32_t(std::clamp((v - a) / (b - a), 0.0f, 1.0f) * 65535.0f) : 0u;
            };
            order[t] = {spread(cell(mx, clo[0], chi[0])) | spread(cell(mz, clo[2], chi[2])) << 1, uint32_t(t)};
        }
        std::sort(order.begin(), order.end());
        std::vector<uint32_t> seen(places.size(), UINT32_MAX);
        size_t used = 0;
        for (auto& [code, t] : order) {
            size_t fresh = 0;
            for (size_t c = 0; c < 3; ++c) fresh += seen[tris[3 * t + c]] != uint32_t(chunks.size()) - 1 || chunks.empty() ? 1 : 0;
            if (chunks.empty() || used + fresh > 65000) {
                chunks.emplace_back();
                used = 0;
            }
            for (size_t c = 0; c < 3; ++c) {
                uint32_t& s = seen[tris[3 * t + c]];
                if (s != uint32_t(chunks.size()) - 1) s = uint32_t(chunks.size()) - 1, ++used;
            }
            chunks.back().push_back(t);
        }
    }
    // Its shape keys: one table for every chunk, named by the world's first model.
    blockmodel::ShapeTable shapeTable;
    if (!src.vertexShapes.empty()) {
        shapeTable.table = assetId(modelName);
        shapeTable.names = src.shapeNames;
        for (const auto& [name, weight] : w.shapeWeights)
            if (std::find(src.shapeNames.begin(), src.shapeNames.end(), name) == src.shapeNames.end())
                say("warning: no shape key \"" + name + "\" to set (the model's: " + std::to_string(src.shapeNames.size()) + ")");
        say(std::to_string(src.shapeNames.size()) + " shape key(s), set from Lua (renut.worlds.set_shape)");
    }
    const HavokTemplate havokTemplate = carParkHavok(*carPark.caff, carParkPath);
    std::vector<std::string> chunkModels;
    std::vector<uint32_t> local(src.positions.size(), UINT32_MAX);
    for (size_t ci = 0; ci < chunks.size(); ++ci) {
        // The first chunk keeps the world's own names; the others are _<n> after them.
        const std::string suffix = ci == 0 ? std::string() : "_" + std::to_string(ci);
        const std::string chunkModel = modelName + suffix, chunkHavok = havokName + suffix;
        const std::vector<uint32_t>& ts = chunks[ci];

        // The draws: each texture and material's triangles, in draws of at most 65535 vertices; the
        // model names only the textures it shows (a material's emissive one too).
        std::vector<uint32_t> texLocal(textureNames.size(), UINT32_MAX);
        std::vector<std::string> chunkTextures;
        auto useTexture = [&](uint32_t tex) {
            if (texLocal[tex] == UINT32_MAX) {
                texLocal[tex] = uint32_t(chunkTextures.size());
                chunkTextures.push_back(textureNames[tex]);
            }
        };
        std::set<std::pair<uint32_t, int>> kinds;  // texture, material
        for (uint32_t t : ts)
            if (waterPlan.use[t] == WaterUse::Ground) kinds.insert({triangleTexture[t], materialOf(t)});
        for (const auto& [tex, mat] : kinds) {
            useTexture(tex);
            if (mat >= 0 && materialEmissive[size_t(mat)] >= 0) useTexture(uint32_t(materialEmissive[size_t(mat)]));
        }
        // A material's record: its look, its place in the world's table (Lua's names), its emissive texture here.
        auto materialRecord = [&](int mat) -> std::optional<blockmodel::WorldMaterial> {
            if (mat < 0 || size_t(mat) >= src.materials.size()) return std::nullopt;
            blockmodel::WorldMaterial look = src.materials[size_t(mat)].look;
            look.table = assetId(modelName);
            look.index = uint32_t(mat);
            look.emissiveTexture = materialEmissive[size_t(mat)] >= 0 ? int(texLocal[size_t(materialEmissive[size_t(mat)])]) : -1;
            return look;
        };
        std::vector<blockmodel::WorldDraw> draws;
        for (const std::pair<uint32_t, int>& kind : kinds) {
            const uint32_t tex = kind.first;
            const int mat = kind.second;
            blockmodel::WorldDraw dr;
            dr.texture = texLocal[tex];
            dr.material = materialRecord(mat);
            std::vector<uint32_t> touched;
            auto finish = [&] {
                if (!dr.indices.empty()) draws.push_back(std::move(dr));
                dr = blockmodel::WorldDraw{};
                dr.texture = texLocal[tex];
                dr.material = materialRecord(mat);
                for (uint32_t i : touched) local[i] = UINT32_MAX;
                touched.clear();
            };
            for (uint32_t t : ts) {
                // The game draws water (its region), falls too.
                if (triangleTexture[t] != tex || materialOf(t) != mat || waterPlan.use[t] != WaterUse::Ground) continue;
                size_t fresh = 0;
                for (size_t c = 0; c < 3; ++c) fresh += local[src.indices[3 * t + c]] == UINT32_MAX ? 1 : 0;
                if (dr.vertices.size() + fresh > 65535) finish();
                for (size_t c = 0; c < 3; ++c) {
                    const uint32_t i = src.indices[3 * t + c];
                    if (local[i] == UINT32_MAX) {
                        local[i] = uint32_t(dr.vertices.size());
                        touched.push_back(i);
                        dr.vertices.push_back({src.positions[i], src.normals[i], src.colours[i], src.uvs[i]});
                        if (!src.vertexShapes.empty())
                            dr.shapes.push_back(i < src.vertexShapes.size() ? src.vertexShapes[i] : std::vector<blockmodel::ShapeDelta>{});
                    }
                    dr.indices.push_back(local[i]);
                }
            }
            finish();
        }
        // Every chunk carries the water region: the scene's water reads it from whichever is its background.
        world.push_back({assetId(chunkModel), blockmodel::writeWorld(chunkModel, draws, chunkTextures,
                                                                     shapeTable.names.empty() ? nullptr : &shapeTable,
                                                                     waterRegion ? &*waterRegion : nullptr)});
        size_t vertices = 0;
        for (const blockmodel::WorldDraw& dr : draws) vertices += dr.vertices.size();
        say(chunkModel + ": " + std::to_string(vertices) + " vertices, " + std::to_string(ts.size()) + " triangles, " +
            std::to_string(draws.size()) + " draws over " + std::to_string(chunkTextures.size()) + " textures");

        // Its collision: the same triangles over its own places. Pointers: the entries, the packfile,
        // indices, vertices and materials, and the entry (the last word).
        std::vector<std::array<float, 3>> chunkPlaces;
        std::map<uint32_t, uint32_t> placeOf;
        std::vector<uint32_t> chunkTris;
        for (uint32_t t : ts)
            for (size_t c = 0; c < 3 && waterPlan.use[t] == WaterUse::Ground && solidMaterial(t); ++c) {
                auto [it, fresh] = placeOf.emplace(tris[3 * t + c], uint32_t(chunkPlaces.size()));
                if (fresh) chunkPlaces.push_back(places[tris[3 * t + c]]);
                chunkTris.push_back(it->second);
            }
        const Bytes havok = havokAsset(havokTemplate, chunkPlaces, chunkTris);
        world.push_back({assetId(chunkHavok),
                         oneAsset(chunkHavok, 5, havok, {0x00, 0x20, 0x28, 0x30, 0x38, 0x3C, uint32_t(havok.size() - 4)})});
        say(chunkHavok + ": " + std::to_string(chunkPlaces.size()) + " places, " + std::to_string(chunkTris.size() / 3) +
            " triangles (reNut builds its MOPP as the world loads)");
        chunkModels.push_back(chunkModel);
    }

    // The marker, unless given: the start 2 m above the flat ground nearest the model's middle (else
    // above its middle), and the door home on flat ground 10-15 m from it, facing it.
    std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (auto& p : places)
        for (size_t k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
    const Ground ground(places, solidTris);
    std::array<float, 3> spawn{(lo[0] + hi[0]) / 2, hi[1] + 2, (lo[2] + hi[2]) / 2};
    std::array<float, 3> spawnGround{spawn[0], lo[1], spawn[2]};  // where the door home is sought from
    if (w.spawn) {
        spawn = *w.spawn;
        const auto under = ground.down(spawn[0], spawn[2]);
        spawnGround = {spawn[0], under && under->first <= spawn[1] ? under->first : spawn[1] - 2, spawn[2]};
    } else if (const auto found = groundNearMiddle(ground)) {
        spawnGround = *found;
        spawn = {spawnGround[0], spawnGround[1] + 2, spawnGround[2]};
        say("the start: on ground found nearest the model's middle");
    } else {
        say("warning: no flat ground found for the start; it is above the model's middle (give --spawn)");
    }
    std::array<float, 3> exit{spawn[0] + 8, spawnGround[1], spawn[2]};
    float exitYaw = w.exitYaw.value_or(0);
    if (w.exit) {
        exit = *w.exit;
    } else if (const auto found = groundBeside(ground, spawnGround)) {
        exit = *found;
        // Facing the start: yaw turns the door's +z towards it.
        if (!w.exitYaw) exitYaw = std::atan2(spawn[0] - exit[0], spawn[2] - exit[2]) * 180 / kPi;
        say("the door home: on ground found beside the start");
    } else {
        say("warning: no flat ground found for the door home; it is 8 m beside the start (give --exit)");
    }
    // The world's characters: their assets in its bundles, their records in its marker (after the start
    // and the door home), their lines in its scene.
    renutnpc::Built npcs;
    if (!w.npcs.empty()) {
        const std::string worldName = w.name.empty() ? w.id : w.name;
        npcs = renutnpc::build(assets, w.id, worldName, renutnpc::load(w.npcs), 3,
                               [&](const std::string& name, const image::Rgba& img, std::string& summary) {
                                   return worldTexture(tmpl, name, img, summary, maxTexture);
                               },
                               say);
        for (auto& e : npcs.stream) world.push_back(std::move(e));
        for (auto& e : npcs.textures) textures.push_back(std::move(e));
    }
    world.push_back({assetId(markerName),
                     oneAsset(markerName, 4, marker(spawn, w.spawnYaw, w.exitDoor, exit, exitYaw, npcs.markerRecords))});
    char where[200];
    if (w.exitDoor)
        std::snprintf(where, sizeof where, "%s: the start at %.1f, %.1f, %.1f and the door home at %.1f, %.1f, %.1f (facing %.0f degrees)",
                      markerName.c_str(), spawn[0], spawn[1], spawn[2], exit[0], exit[1], exit[2], exitYaw);
    else
        std::snprintf(where, sizeof where, "%s: the start at %.1f, %.1f, %.1f, no door home", markerName.c_str(), spawn[0],
                      spawn[1], spawn[2]);
    say(where);

    // The world's stream bundle: only served to the game, which loads it with the scene.
    char hex[8];
    std::snprintf(hex, sizeof hex, "%06x", unsigned(assetId(streamName) & 0xFFFFFF));
    const fs::path worldPath = mod / "worlds" / "Bundle" / "50" / hex;
    streambundle::StreamBundle sb;
    sb.timestamp = uint32_t(std::time(nullptr));
    sb.entries = world;
    fs::create_directories(longPath(worldPath.parent_path()));
    writeFile(worldPath, sb.serialize());
    say("wrote " + worldPath.string());
    const std::string bundleName = "aid_bundle_" + stem;
    std::snprintf(hex, sizeof hex, "%06x", unsigned(assetId(bundleName) & 0xFFFFFF));
    const fs::path texturesPath = mod / "worlds" / "Bundle" / "4f" / hex;
    std::vector<Bytes> textureFiles;
    for (const streambundle::Entry& e : textures) textureFiles.push_back(e.data);
    fs::create_directories(longPath(texturesPath.parent_path()));
    writeFile(texturesPath, residentBundle(textureFiles));
    say("wrote " + texturesPath.string() + " (" + bundleName + ": its textures)");

    // The scene: the world's textures, then the car park's bundle (its door home, lights and the
    // vehicles' parts) and the world's stream bundle, its marker and model, and the car park's lights.
    // The sky, as Spiral Mountain's scene sets its own: op_2C <model> 1, no spin.
    std::string placed;  // each chunk's model (and so its collision)
    for (const std::string& m : chunkModels)
        placed += "op_01 @" + m + " 0 0x00000000 0x00000000 0x00000000 0 0.0 20.0 4000.0 20.0 1.0 1.0 1.0\n";
    const std::string sceneText = "op_03 0 @" + bundleName + " 0\n"
                                  "op_03 0 @aid_bundle_banjox_showdowntown_carpark @" + streamName + "\n"
                                  "op_4E @" + markerName + " 0\n" + waterSetups + placed +
                                  "op_52 @aid_script_banjox_lightsetup_showdowntown_carpark\n" +
                                  (w.sky.empty() ? std::string() : "op_2C @" + skyModelName + " 1 0 0 0 0\n") +
                                  npcs.sceneText + "op_00\n";
    const Bytes scene = gamescript::fromText(gamescript::Kind::Script, sceneText);
    std::string bundle = w.bundle.empty() ? lower(fs::absolute(mod).filename().string()) : lower(w.bundle);
    for (char& ch : bundle)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) ch = '_';
    if (!renutpart::validId(bundle)) fail("the mod's bundle name is 1-40 characters of a-z, 0-9 and _");
    say(sceneName + ": its scene, in the mod's bundle (loaded with the game)");
    std::vector<streambundle::Entry> preloaded{{assetId(sceneName), oneAsset(sceneName, 4, scene)}};
    // The door's own look, loaded with the game too: Showdown Town draws it.
    if (arch)
        for (auto& e : archDoor(assets, stem, w.hologram, say)) preloaded.push_back(std::move(e));
    else if (!w.hologram.empty())
        for (auto& e : hologramDoor(assets, stem, w.hologram, say)) preloaded.push_back(std::move(e));
    say("wrote " + renutmodel::addToMod(mod, bundle, preloaded, {}).string());

    // Its shape keys' table, made anew each time: their names and starting weights.
    if (!shapeTable.names.empty()) {
        std::string names, weights;
        for (size_t i = 0; i < shapeTable.names.size(); ++i) {
            const auto set = w.shapeWeights.find(shapeTable.names[i]);
            const float weight = set != w.shapeWeights.end() ? set->second : i < src.shapeWeights.size() ? src.shapeWeights[i] : 0.0f;
            const std::string& name = shapeTable.names[i];  // const: std::quoted's overload doesn't win
            names += (i ? ", " : "") + quoted(name);
            weights += (i ? ", " : "") + number(weight);
        }
        const fs::path shapesScript = mod / "scripts" / (stem + "_shapes.lua");
        fs::create_directories(longPath(shapesScript.parent_path()));
        writeText(shapesScript, "-- " + (w.name.empty() ? w.id : w.name) + "'s shape keys, from " + w.model.filename().string() +
                                    ": written anew by renut-world each time.\n-- Set them from Lua with renut.worlds.set_shape(" +
                                    quoted(w.id) + ", name, weight).\n\nrenut.shapes.define{\n  model = " + quoted(stem) +
                                    ",\n  shapes = {" + names + "},\n  weights = {" + weights + "},\n}\n");
        say("wrote " + shapesScript.string());
    }
    // Its materials' names, made anew each time, which Lua changes them by (renut.worlds.set_material).
    if (!src.materials.empty()) {
        std::string names;
        for (size_t i = 0; i < src.materials.size(); ++i) {
            const std::string& name = src.materials[i].name;  // const: std::quoted's overload doesn't win
            names += (i ? ",\n    " : "\n    ") + quoted(name);
        }
        const fs::path materialsScript = mod / "scripts" / (stem + "_materials.lua");
        fs::create_directories(longPath(materialsScript.parent_path()));
        writeText(materialsScript, "-- " + (w.name.empty() ? w.id : w.name) + "'s materials, from " + w.model.filename().string() +
                                       ": written anew by renut-world each time.\n-- Change them from Lua with renut.worlds.set_material(" +
                                       quoted(w.id) + ", name, {emissive = {r, g, b}, emissive_strength = n, glow = n, opacity = n}).\n\n"
                                       "renut.materials.define{\n  model = " + quoted(stem) + ",\n  names = {" + names + ",\n  },\n}\n");
        say("wrote " + materialsScript.string());
    }
    // The NPCs' Lua, made anew from their JSON each time; an enemy's behaviour (*_ai.lua) is the modder's
    // to edit, so it is kept unless forced.
    for (const auto& [file, text] : npcs.scripts) {
        const fs::path path = mod / file;
        if (endsWith(file.filename().string(), "_ai.lua") && fs::exists(longPath(path)) && !force) {
            say("kept " + path.string() + " (--force writes it anew)");
            continue;
        }
        fs::create_directories(longPath(path.parent_path()));
        writeText(path, text);
        say("wrote " + path.string());
    }
    for (const auto& [from, file] : npcs.files) {
        const fs::path to = mod / file;
        fs::create_directories(longPath(to.parent_path()));
        if (fs::absolute(longPath(from)) != fs::absolute(longPath(to)))
            fs::copy_file(longPath(from), longPath(to), fs::copy_options::overwrite_existing);
        say("copied " + from.filename().string() + " to " + to.string());
    }

    // Its music: copied into the mod's music folder, which the Lua names.
    std::string musicFile;
    if (!w.music.empty()) {
        if (!fs::exists(longPath(w.music))) fail("no music file " + w.music.string());
        const std::string ext = lower(w.music.extension().string());
        if (ext != ".ogg" && ext != ".mp3" && ext != ".flac" && ext != ".wav")
            fail("the music is an .ogg, .mp3, .flac or .wav file (got " + w.music.filename().string() + ")");
        const fs::path to = mod / "music" / w.music.filename();
        fs::create_directories(longPath(to.parent_path()));
        if (fs::absolute(longPath(w.music)) != fs::absolute(longPath(to)))
            fs::copy_file(longPath(w.music), longPath(to), fs::copy_options::overwrite_existing);
        musicFile = "music/" + w.music.filename().string();
        say("copied the music to " + to.string());
    }

    // The Lua that declares it and its door.
    const fs::path script = mod / "scripts" / (stem + ".lua");
    if (fs::exists(longPath(script)) && !force) {
        say("kept " + script.string() + " (--force writes it anew)");
        return;
    }
    const std::string name = w.name.empty() ? w.id : w.name;
    std::string text = "-- " + name + ": a world of its own, made with NutTool from " + w.model.filename().string() +
                       ".\n-- reNut's docs/tutorials/worlds-and-doors.md describes renut.worlds.add and renut.doors.add.\n\n";
    text += "renut.worlds.add{\n  id = " + quoted(w.id) + ",\n  name = " + quoted(name) + ",\n";
    if (!musicFile.empty()) {
        // Const, so std::quoted's non-const overload doesn't win over ours.
        const std::string& file = musicFile;
        text += "  music = " + quoted(file) + ",   -- loops while you're in the world\n  music_volume = " +
                number(w.musicVolume) + ",\n";
    }
    if (!w.playerModel.empty()) {
        const std::string& model = w.playerModel;
        text += "  player_model = " + quoted(model) + ",   -- Banjo is made with it here (renut-player-model)\n";
    }
    static const char* const kStatKeys[] = {"jiggies", "trophies", "jinjos", "notes"};
    std::string stats;
    for (size_t i = 0; i < w.statTotals.size(); ++i)
        if (w.statTotals[i] >= 0)
            stats += std::string(stats.empty() ? "" : ", ") + kStatKeys[i] + " = " + std::to_string(w.statTotals[i]);
    if (!stats.empty()) text += "  stats = {" + stats + "},   -- its door's totals (the rest are counted)\n";
    if (!waterLua.empty())
        text += "  -- Its water bodies (the scene's water setups, in order): surface height over a rectangle\n  water = {\n" +
                waterLua + "  },\n";
    text += "}\n";
    if (!w.townDoor) {
        text += "-- No door in Showdown Town: it's reached from Lua (renut.worlds.go(\"" + w.id + "\")).\n";
    } else {
        text += "\nrenut.doors.add{\n  world = " + quoted(w.id) + ",\n  position = {" + number(w.door[0]) + ", " +
                number(w.door[1]) + ", " + number(w.door[2]) + "},   -- where it stands in Showdown Town\n  yaw = " +
                number(w.doorYaw) + ",\n";
        if (arch) {
            text += "  style = \"arch\",   -- the stone arch, two-sided\n  look = " + quoted(stem + "_arch") + ",   -- its copy in the mod";
            text += w.hologram.empty() ? std::string("\n") : ", showing " + w.hologram.filename().string() + "\n";
        } else if (w.doorStyle != "wall") {
            text += "  style = " + quoted(w.doorStyle) + ",\n";
        } else if (!w.hologram.empty()) {
            text += "  look = " + quoted(stem + "_door") + ",   -- the door showing " + w.hologram.filename().string() + "\n";
        }
        text += "}\n";
    }
    fs::create_directories(longPath(script.parent_path()));
    writeText(script, text);
    say("wrote " + script.string());
    const fs::path toml = mod / "mod.toml";
    if (!fs::exists(longPath(toml)))
        writeText(toml, "name = " + quoted(mod.filename().string()) + "\ndescription = \"A world made with NutTool.\"\nversion = \"1\"\n");
}

} // namespace nut::renutworld
