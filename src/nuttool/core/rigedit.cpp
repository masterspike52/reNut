#include "rigedit.hpp"

#include "blockmodel.hpp"
#include "modelimport.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <set>
#include <unordered_map>

namespace nut::rigedit {
namespace {

using V3 = std::array<float, 3>;
using Q4 = std::array<double, 4>;

// Pose block layout ("pose" 19.12.06.0036): count at +0x18, name table pointer at +0x20, records pointer at +0x2C.
constexpr uint32_t kPoseHeader = 0x30, kPoseCount = 0x18, kPoseNames = 0x20, kPoseRecords = 0x2C, kRecord = 52;

// A symbol's bytes and the target (0-based) of each relocated pointer in it.
struct Sym {
    Bytes data;
    std::map<uint32_t, uint32_t> target;
    Sym(const caff::Caff& c, size_t s) : data(c.symbolData(s)) {
        for (int set = 0; set < 2; ++set)
            for (auto& r : c.relocs[set])
                if (r.from == s + 1)
                    for (uint32_t o : r.offsets) target[o] = r.to - 1;
    }
    std::optional<uint32_t> targetOf(uint32_t at) const {
        auto it = target.find(at);
        if (it == target.end()) return std::nullopt;
        return it->second;
    }
    uint32_t u32(uint32_t at) const {
        if (uint64_t(at) + 4 > data.size()) fail("rig: read past the end of a symbol");
        return be32(&data[at]);
    }
    uint16_t u16(uint32_t at) const {
        if (uint64_t(at) + 2 > data.size()) fail("rig: read past the end of a symbol");
        return be16(&data[at]);
    }
};

std::optional<uint32_t> rendergraph(const Bytes& d) {
    if (d.size() < 8) return std::nullopt;
    const uint32_t dir = be32(&d[0]), n = be32(&d[4]);
    for (uint32_t i = 0; i < n && i < 64 && uint64_t(dir) + 8 * i + 8 <= d.size(); ++i)
        if (be32(&d[dir + 8 * i]) == 0x1E) {
            const uint32_t at = be32(&d[dir + 8 * i + 4]);
            if (uint64_t(at) + 4 > d.size()) break;
            const uint32_t rg = be32(&d[at]);
            if (uint64_t(rg) + 0x108 <= d.size() && std::memcmp(&d[rg], "rendergraph", 11) == 0) return rg;
        }
    return std::nullopt;
}

uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// A relocated pointer at `at` in symbol `from` to symbol `to` (0-based); one already there is moved to `to`.
void addReloc(caff::Caff& c, size_t from, size_t to, uint32_t at) {
    for (int set = 0; set < 2; ++set)
        for (auto& r : c.relocs[set]) {
            if (r.from != from + 1) continue;
            auto it = std::find(r.offsets.begin(), r.offsets.end(), at);
            if (it == r.offsets.end()) continue;
            if (r.to == to + 1) return;
            r.offsets.erase(it);
        }
    for (auto& r : c.relocs[0])
        if (r.from == from + 1 && r.to == to + 1) {
            r.offsets.push_back(at);
            return;
        }
    c.relocs[0].push_back({uint32_t(from + 1), uint32_t(to + 1), {at}});
}

void putF32(Bytes& d, uint32_t at, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    putBe32(&d[at], bits);
}

// ---- render program palettes ------------------------------------------------------------------

struct Palette {
    uint32_t op = 0;        // the op 0x12 record in its .stream symbol
    int listSymbol = -1;    // where its entries are
    uint32_t list = 0;
    std::vector<uint16_t> bones;
};

struct Program {
    std::vector<Palette> palettes;
    bool ok = false;  // walked to its end op
};

// The render program in .stream symbol `s`: ops {u32 size << 16 | op << 8, ...} from the pointer at +0 to op 0x1D.
Program walkProgram(const caff::Caff& c, size_t s) {
    Program p;
    Sym v(c, s);
    if (v.data.size() < 4 || v.targetOf(0) != s) return p;
    uint32_t at = v.u32(0);
    for (int guard = 0; guard < 1000000 && uint64_t(at) + 4 <= v.data.size(); ++guard) {
        const uint32_t h = v.u32(at), size = h >> 16, op = (h >> 8) & 0xFF;
        if (size < 4 || size % 4) return p;
        if (op == 0x12 && size >= 12) {
            Palette pal;
            pal.op = at;
            if (auto t = v.targetOf(at + 4)) {
                pal.listSymbol = int(*t);
                pal.list = v.u32(at + 4);
                const uint16_t n = v.u16(at + 8);
                const Bytes list = *t == s ? v.data : c.symbolData(*t);
                for (uint32_t k = 0; k < n && uint64_t(pal.list) + 8 * k + 2 <= list.size(); ++k) pal.bones.push_back(be16(&list[pal.list + 8 * k]));
            }
            p.palettes.push_back(std::move(pal));
        }
        if (op == 0x1D) {
            p.ok = true;
            return p;
        }
        at += size;
    }
    return p;
}

// The palette each skinned mesh draws with: the last op 0x12 before its draw's program; {-1, -1} when unknown.
std::vector<std::pair<int, int>> meshPalettes(const caff::Caff& c, const model::Model& m, std::map<int, Program>& programs) {
    std::vector<std::pair<int, int>> out(m.meshes.size(), {-1, -1});
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        const model::Mesh& me = m.meshes[i];
        const int s = me.source.programSymbol;
        if (!me.layout.skinned || s < 0) continue;
        auto it = programs.find(s);
        if (it == programs.end()) it = programs.emplace(s, walkProgram(c, size_t(s))).first;
        if (!it->second.ok) continue;
        for (size_t k = 0; k < it->second.palettes.size(); ++k)
            if (it->second.palettes[k].op < me.source.programAt) out[i] = {s, int(k)};
    }
    return out;
}

uint64_t vertexKey(const model::Mesh& me, uint32_t v) {
    const uint32_t b = v < me.source.vertex.size() ? me.source.vertex[v] : v;
    return uint64_t(uint16_t(me.source.vbSymbol + 1)) << 48 | uint64_t(me.source.vbOffset & 0xFFFFFF) << 24 | (b & 0xFFFFFF);
}

// ---- maths ----------------------------------------------------------------------------------------

Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[size_t(k * 4 + row)] * b[size_t(col * 4 + k)];
            r[size_t(col * 4 + row)] = s;
        }
    return r;
}

Mat4 trs(const V3& t, const std::array<float, 4>& q, const V3& s) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    const float n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n > 0) x /= n, y /= n, z /= n, w /= n;
    Mat4 m = identity();
    m[0] = (1 - 2 * (y * y + z * z)) * s[0];
    m[1] = (2 * (x * y + z * w)) * s[0];
    m[2] = (2 * (x * z - y * w)) * s[0];
    m[4] = (2 * (x * y - z * w)) * s[1];
    m[5] = (1 - 2 * (x * x + z * z)) * s[1];
    m[6] = (2 * (y * z + x * w)) * s[1];
    m[8] = (2 * (x * z + y * w)) * s[2];
    m[9] = (2 * (y * z - x * w)) * s[2];
    m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    m[12] = t[0], m[13] = t[1], m[14] = t[2];
    return m;
}

Q4 qmul(const Q4& a, const Q4& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

std::array<double, 3> qrot(const Q4& q, const std::array<double, 3>& v) {
    const Q4 r = qmul(qmul(q, {v[0], v[1], v[2], 0}), {-q[0], -q[1], -q[2], q[3]});
    return {r[0], r[1], r[2]};
}

float dist2(const V3& a, const V3& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

float segmentDistance(const V3& p, const V3& a, const V3& b) {
    const V3 ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ap{p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    const float len = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = len > 0 ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len : 0;
    t = std::clamp(t, 0.0f, 1.0f);
    const V3 q{a[0] + ab[0] * t, a[1] + ab[1] * t, a[2] + ab[2] * t};
    return std::sqrt(dist2(p, q));
}

// The closest point of triangle abc to p, as barycentric weights (Ericson, Real-Time Collision Detection 5.1.5).
std::array<float, 3> closestBarycentric(const V3& p, const V3& a, const V3& b, const V3& c) {
    auto sub = [](const V3& x, const V3& y) { return V3{x[0] - y[0], x[1] - y[1], x[2] - y[2]}; };
    auto dot = [](const V3& x, const V3& y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    const V3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return {1, 0, 0};
    const V3 bp = sub(p, b);
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return {0, 1, 0};
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        const float v = d1 / (d1 - d3);
        return {1 - v, v, 0};
    }
    const V3 cp = sub(p, c);
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return {0, 0, 1};
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        const float w = d2 / (d2 - d6);
        return {1 - w, 0, w};
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return {0, 1 - w, w};
    }
    const float denom = 1 / (va + vb + vc);
    const float v = vb * denom, w = vc * denom;
    return {1 - v - w, v, w};
}

V3 posOf(const model::Mesh& me, size_t v) { return {me.vertices[v].pos[0], me.vertices[v].pos[1], me.vertices[v].pos[2]}; }

// A bone name without case, underscores, dots, dashes and spaces.
std::string looseName(const std::string& s) {
    std::string out;
    for (char ch : s)
        if (ch != '_' && ch != '.' && ch != '-' && ch != ' ') out.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
    return out;
}

// Every skinned vertex the mask takes, with what to do once its weights changed.
template <typename F>
void forMasked(Rig& rig, const Mask& mask, F f) {
    for (size_t mi = 0; mi < rig.skins.size(); ++mi)
        for (size_t v = 0; v < rig.skins[mi].size(); ++v)
            if (mask.has(mi, v)) {
                f(mi, v, rig.skins[mi][v]);
                syncAliases(rig, mi, uint32_t(v));
            }
}

// A uniform grid of points or triangles' boxes, for the nearest ones to a point.
struct Grid {
    V3 lo{}, cell{};
    int n[3] = {1, 1, 1};
    std::vector<std::vector<uint32_t>> cells;
    void init(V3 mn, V3 mx, size_t items) {
        lo = mn;
        const float side = std::max({mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2], 1e-4f});
        const int per = std::clamp(int(std::cbrt(double(std::max<size_t>(items, 1)) / 2.0)), 1, 64);
        for (int k = 0; k < 3; ++k) {
            const float extent = std::max(mx[size_t(k)] - mn[size_t(k)], side * 1e-3f);
            n[k] = std::max(1, int(float(per) * extent / side));
            cell[size_t(k)] = extent / float(n[k]) * 1.0001f;
        }
        cells.assign(size_t(n[0]) * size_t(n[1]) * size_t(n[2]), {});
    }
    int at(float x, int k) const { return std::clamp(int((x - lo[size_t(k)]) / cell[size_t(k)]), 0, n[k] - 1); }
    void insert(uint32_t item, const V3& mn, const V3& mx) {
        for (int z = at(mn[2], 2); z <= at(mx[2], 2); ++z)
            for (int y = at(mn[1], 1); y <= at(mx[1], 1); ++y)
                for (int x = at(mn[0], 0); x <= at(mx[0], 0); ++x) cells[(size_t(z) * size_t(n[1]) + size_t(y)) * size_t(n[0]) + size_t(x)].push_back(item);
    }
    // Visits the items in the cells `ring` steps from p's cell; false once every cell has been visited.
    template <typename F>
    bool ring(const V3& p, int r, F f) const {
        const int cx = at(p[0], 0), cy = at(p[1], 1), cz = at(p[2], 2);
        bool any = false;
        for (int z = cz - r; z <= cz + r; ++z)
            for (int y = cy - r; y <= cy + r; ++y)
                for (int x = cx - r; x <= cx + r; ++x) {
                    if (std::max({std::abs(x - cx), std::abs(y - cy), std::abs(z - cz)}) != r) continue;
                    if (x < 0 || y < 0 || z < 0 || x >= n[0] || y >= n[1] || z >= n[2]) continue;
                    any = true;
                    for (uint32_t item : cells[(size_t(z) * size_t(n[1]) + size_t(y)) * size_t(n[0]) + size_t(x)]) f(item);
                }
        return any || r <= std::max({n[0], n[1], n[2]});
    }
    float minCell() const { return std::min({cell[0], cell[1], cell[2]}); }
};

// ---- writing ----------------------------------------------------------------------------------

void writePose(caff::Caff& c, size_t symbol, const model::Model& m, const Rig& rig, std::vector<std::string>& notes) {
    const Sym mv(c, symbol);
    const auto rg = rendergraph(mv.data);
    if (!rg) fail("rig: cannot find the model's rendergraph");
    const auto poseTarget = mv.targetOf(*rg + 0x104);
    if (!poseTarget) fail("rig: cannot find the model's skeleton");
    const size_t poseSym = *poseTarget;
    Sym pv(c, poseSym);
    Bytes& d = pv.data;
    const uint32_t pose = mv.u32(*rg + 0x104);
    const size_t n0 = m.bones.size(), n = rig.bones.size();
    if (uint64_t(pose) + kPoseHeader > d.size() || std::memcmp(&d[pose], "pose", 4) != 0 || be32(&d[pose + kPoseCount]) != n0)
        fail("rig: unexpected skeleton layout");
    const uint32_t records = pv.u32(pose + kPoseRecords);
    if (uint64_t(records) + uint64_t(kRecord) * n0 > d.size()) fail("rig: the skeleton runs past its symbol");

    // Each joint's offset from its parent in the parent's bind frame (identity for the game's own skeletons).
    std::vector<Q4> rot(n, Q4{0, 0, 0, 1});
    std::vector<std::array<double, 3>> local(n);
    std::vector<uint16_t> child(n, 0xFFFF), sibling(n, 0xFFFF);
    for (size_t i = 0; i < n; ++i) {
        const Bone& b = rig.bones[i];
        Q4 q{0, 0, 0, 1};
        if (i < n0) {
            q = {m.bones[i].rotation[0], m.bones[i].rotation[1], m.bones[i].rotation[2], m.bones[i].rotation[3]};
            const double len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (len > 0) for (double& x : q) x /= len;
        }
        if (b.parent >= 0) {
            const size_t p = size_t(b.parent);
            rot[i] = qmul(rot[p], q);
            const Q4& r = rot[p];
            local[i] = qrot({-r[0], -r[1], -r[2], r[3]}, {double(b.pos[0]) - rig.bones[p].pos[0], double(b.pos[1]) - rig.bones[p].pos[1],
                                                         double(b.pos[2]) - rig.bones[p].pos[2]});
        } else {
            rot[i] = q;
            local[i] = {b.pos[0], b.pos[1], b.pos[2]};
        }
    }
    // The first-child and next-sibling links are only made again when the hierarchy changed.
    bool relinked = n != n0;
    for (size_t i = 0; i < n0; ++i) relinked |= rig.bones[i].parent != m.bones[i].parent;
    for (size_t i = n; i-- > 0;)
        if (rig.bones[i].parent >= 0 && size_t(rig.bones[i].parent) < n) child[size_t(rig.bones[i].parent)] = uint16_t(i);
    for (size_t i = 0; i < n; ++i)
        for (size_t k = i + 1; k < n; ++k)
            if (rig.bones[k].parent == rig.bones[i].parent) {
                sibling[i] = uint16_t(k);
                break;
            }
    auto parentField = [&](size_t i) { return rig.bones[i].parent < 0 ? uint16_t(0xFFFF) : uint16_t(rig.bones[i].parent); };

    auto fillRecord = [&](uint32_t r, size_t i) {
        for (int k = 0; k < 3; ++k) {
            putF32(d, r + 4 * uint32_t(k), float(local[i][size_t(k)]));
            putF32(d, r + 12 + 4 * uint32_t(k), rig.bones[i].pos[size_t(k)]);
        }
        putBe16(&d[r + 40], parentField(i));
        if (relinked) {
            putBe16(&d[r + 42], child[i]);
            putBe16(&d[r + 44], sibling[i]);
        }
    };

    if (n == n0) {
        int moved = 0;
        for (size_t i = 0; i < n; ++i) {
            const uint32_t r = records + kRecord * uint32_t(i);
            bool same = true;
            for (int k = 0; k < 3; ++k) {
                same &= std::fabs(bef32(&d[r + 4 * uint32_t(k)]) - float(local[i][size_t(k)])) < 1e-5f;
                same &= std::fabs(bef32(&d[r + 12 + 4 * uint32_t(k)]) - rig.bones[i].pos[size_t(k)]) < 1e-6f;
            }
            same &= be16(&d[r + 40]) == parentField(i);
            if (relinked) same &= be16(&d[r + 42]) == child[i] && be16(&d[r + 44]) == sibling[i];
            if (same) continue;
            fillRecord(r, i);
            ++moved;
        }
        if (moved) {
            c.replaceSymbolData(poseSym, d);
            notes.push_back("rig: " + std::to_string(moved) + " joint(s) changed");
        }
        return;
    }

    // A new pose block at the symbol's end: the header, every record, then a name table with the new names.
    const uint32_t oldTable = pv.u32(pose + kPoseNames);
    if (pv.targetOf(pose + kPoseNames) != poseSym || uint64_t(oldTable) + 16 > d.size() || pv.u32(oldTable) != n0 ||
        pv.targetOf(oldTable + 4) != poseSym)
        fail("rig: the skeleton's name table isn't laid out as NutTool knows it, so no bones can be added");
    const uint32_t oldEntries = pv.u32(oldTable + 4);
    const bool hashed = pv.targetOf(oldTable + 12).has_value();
    std::vector<std::pair<uint32_t, size_t>> pointers;  // new pointers: offset, target symbol

    const uint32_t at = alignUp(uint32_t(d.size()), 16);
    const Bytes header(d.begin() + pose, d.begin() + pose + kPoseHeader);
    d.resize(at, 0);
    d.insert(d.end(), header.begin(), header.end());
    for (uint32_t o = 0; o < kPoseHeader; o += 4)
        if (auto t = pv.targetOf(pose + o); t && o != kPoseNames && o != kPoseRecords) pointers.push_back({at + o, *t});
    putBe32(&d[at + kPoseCount], uint32_t(n));
    const uint32_t recs = uint32_t(d.size());
    putBe32(&d[at + kPoseRecords], recs);
    pointers.push_back({at + kPoseRecords, poseSym});
    for (size_t i = 0; i < n; ++i) {
        const uint32_t r = uint32_t(d.size());
        if (i < n0) {
            const Bytes old(d.begin() + records + kRecord * uint32_t(i), d.begin() + records + kRecord * uint32_t(i + 1));
            d.insert(d.end(), old.begin(), old.end());
        } else {
            d.resize(d.size() + kRecord, 0);
            putF32(d, r + 24 + 12, 1.0f);  // identity rotation
            putBe16(&d[r + 46], uint16_t(i));
            const int twin = mirrorBone(rig, i);
            putBe16(&d[r + 48], twin < 0 ? uint16_t(0xFFFF) : uint16_t(twin));
        }
        fillRecord(r, i);
    }
    // The table {count, ->entries, 0, ->hashes}: entries {->name, 0, index}, hashes {hash, 0, ->entry}, then new names.
    const uint32_t table = alignUp(uint32_t(d.size()), 4);
    d.resize(table + 16, 0);
    putBe32(&d[at + kPoseNames], table);
    pointers.push_back({at + kPoseNames, poseSym});
    putBe32(&d[table], uint32_t(n));
    putBe32(&d[table + 4], table + 16);
    pointers.push_back({table + 4, poseSym});
    const uint32_t entries = table + 16;
    d.resize(entries + 12 * uint32_t(n), 0);
    std::vector<uint32_t> namePtr;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t e = entries + 12 * uint32_t(i);
        if (i < n0) {
            const uint32_t oe = oldEntries + 12 * uint32_t(i);
            std::memcpy(&d[e], &d[oe], 12);
            if (auto t = pv.targetOf(oe)) pointers.push_back({e, *t});
        } else {
            namePtr.push_back(e);
            putBe32(&d[e + 8], uint32_t(i));
        }
    }
    if (hashed) {
        const uint32_t hashes = uint32_t(d.size());
        putBe32(&d[table + 12], hashes);
        pointers.push_back({table + 12, poseSym});
        std::vector<size_t> order(n);
        for (size_t i = 0; i < n; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return blockmodel::detail::elfHash(rig.bones[a].name) < blockmodel::detail::elfHash(rig.bones[b].name);
        });
        for (size_t i : order) {
            const uint32_t h = uint32_t(d.size());
            d.resize(h + 12, 0);
            putBe32(&d[h], blockmodel::detail::elfHash(rig.bones[i].name));
            putBe32(&d[h + 8], entries + 12 * uint32_t(i));
            pointers.push_back({h + 8, poseSym});
        }
    }
    for (size_t k = 0; k < namePtr.size(); ++k) {
        const uint32_t s = uint32_t(d.size());
        const std::string& name = rig.bones[n0 + k].name;
        d.insert(d.end(), name.begin(), name.end());
        d.push_back(0);
        putBe32(&d[namePtr[k]], s);
        pointers.push_back({namePtr[k], poseSym});
    }
    d.resize(alignUp(uint32_t(d.size()), 16), 0);

    if (poseSym == symbol) {
        putBe32(&d[*rg + 0x104], at);
        c.replaceSymbolData(poseSym, d);
    } else {
        c.replaceSymbolData(poseSym, d);
        Bytes md = mv.data;
        putBe32(&md[*rg + 0x104], at);
        c.replaceSymbolData(symbol, md);
    }
    for (auto& [o, t] : pointers) addReloc(c, poseSym, t, o);
    notes.push_back("rig: " + std::to_string(n - n0) + " bone(s) added after the model's " + std::to_string(n0) +
                    "; the game's animations leave them following their parent");
}

void writeSkins(caff::Caff& c, size_t symbol, const model::Model& m, const Rig& rig, std::vector<std::string>& notes) {
    (void)symbol;
    std::map<int, Program> programs;
    const auto palettes = meshPalettes(c, m, programs);

    // Each palette grown by the bones its draws' vertices need.
    std::map<std::pair<int, int>, std::vector<uint16_t>> grown;
    std::map<std::pair<int, int>, size_t> originalSize;
    int unknown = 0;
    for (size_t mi = 0; mi < m.meshes.size(); ++mi) {
        if (rig.skins[mi].empty()) continue;
        const auto key = palettes[mi];
        unknown += key.first < 0;
        std::set<uint16_t> need;
        for (const VertexSkin& s : rig.skins[mi])
            for (const Influence& in : s)
                if (in.weight > 0.5f / 255) need.insert(in.bone);
        if (key.first < 0) continue;
        auto it = grown.find(key);
        if (it == grown.end()) {
            it = grown.emplace(key, programs[key.first].palettes[size_t(key.second)].bones).first;
            originalSize[key] = it->second.size();
        }
        for (uint16_t b : need)
            if (std::find(it->second.begin(), it->second.end(), b) == it->second.end()) it->second.push_back(b);
    }
    int added = 0;
    for (auto& [key, bones] : grown) {
        const size_t before = originalSize[key];
        if (bones.size() == before) continue;
        const Palette& pal = programs[key.first].palettes[size_t(key.second)];
        if (pal.listSymbol != key.first) fail("a draw's bone palette is kept outside its render program; it can't grow");
        Bytes sd = c.symbolData(size_t(key.first));
        uint8_t entry[8] = {0, 0, 0xFF, 0xFF, 0x7F, 0, 0, 0};
        if (before && uint64_t(pal.list) + 8 <= sd.size()) std::memcpy(entry, &sd[pal.list], 8);
        const uint32_t list = alignUp(uint32_t(sd.size()), 16);
        sd.resize(list + 8 * uint32_t(bones.size()), 0);
        for (size_t k = 0; k < bones.size(); ++k) {
            std::memcpy(&sd[list + 8 * k], entry, 8);
            putBe16(&sd[list + 8 * k], bones[k]);
        }
        putBe32(&sd[pal.op + 4], list);
        putBe16(&sd[pal.op + 8], uint16_t(bones.size()));
        c.replaceSymbolData(size_t(key.first), sd);
        added += int(bones.size() - before);
    }
    if (added) notes.push_back("rig: " + std::to_string(added) + " bone(s) added to the draws' lists of the bones they use");
    if (unknown && rig.bones.size() > rig.gameBones)
        notes.push_back("rig: " + std::to_string(unknown) + " mesh(es) have no list of the bones they use that NutTool found; new bones "
                        "weighted there may not move in the game");

    std::map<int, Bytes> gpu;
    int written = 0;
    for (size_t mi = 0; mi < m.meshes.size(); ++mi) {
        const model::Mesh& me = m.meshes[mi];
        if (rig.skins[mi].empty()) continue;
        if (me.source.vbSymbol < 0 || me.source.vertex.size() != me.vertices.size()) fail("mesh " + std::to_string(mi) + ": buffer location unknown");
        auto it = gpu.find(me.source.vbSymbol);
        if (it == gpu.end()) it = gpu.emplace(me.source.vbSymbol, c.symbolData(size_t(me.source.vbSymbol))).first;
        Bytes& g = it->second;
        for (size_t v = 0; v < me.vertices.size(); ++v) {
            const VertexSkin& s = rig.skins[mi][v];
            float total = 0;
            for (const Influence& in : s) total += std::max(0.0f, in.weight);
            if (total <= 0) continue;
            int q[4];
            int sum = 0, heaviest = 0;
            for (int k = 0; k < 4; ++k) {
                q[k] = int(std::lround(std::max(0.0f, s[size_t(k)].weight) / total * 255));
                sum += q[k];
                if (q[k] > q[heaviest]) heaviest = k;
            }
            q[heaviest] += 255 - sum;
            const uint32_t at = me.source.vbOffset + me.source.vertex[v] * me.stride + uint32_t(me.layout.attributes);
            if (uint64_t(at) + 12 > g.size()) fail("mesh " + std::to_string(mi) + ": a vertex is outside its buffer");
            for (int k = 0; k < 4; ++k) {
                putBe16(&g[at + 2 * uint32_t(k)], q[k] > 0 ? s[size_t(k)].bone : uint16_t(0));
                g[at + 8 + 3 - uint32_t(k)] = uint8_t(std::clamp(q[k], 0, 255));
            }
            ++written;
        }
    }
    for (auto& [s, data] : gpu) c.replaceSymbolData(size_t(s), data);
    if (written) notes.push_back("rig: weights written for " + std::to_string(written) + " vertices");
}

} // namespace

Rig read(const caff::Caff& c, size_t symbol, const model::Model& m) {
    (void)symbol;
    Rig r;
    for (const auto& b : m.bones) r.bones.push_back({b.name, b.parent, {b.world[0], b.world[1], b.world[2]}, false});
    r.gameBones = r.bones.size();
    // A vertex's indices are bone numbers: the game's shaders read the instance's matrices with them (fetch constant 85).
    std::map<int, Bytes> gpu;
    r.skins.resize(m.meshes.size());
    r.keys.resize(m.meshes.size());
    for (size_t mi = 0; mi < m.meshes.size(); ++mi) {
        const model::Mesh& me = m.meshes[mi];
        for (size_t v = 0; v < me.vertices.size(); ++v) r.keys[mi].push_back(vertexKey(me, uint32_t(v)));
        if (!me.layout.skinned || r.bones.empty()) continue;
        for (size_t v = 0; v < me.vertices.size(); ++v) r.shared[r.keys[mi][v]].push_back({uint32_t(mi), uint32_t(v)});
        const Bytes* g = nullptr;
        if (me.source.vbSymbol >= 0 && me.source.vertex.size() == me.vertices.size()) {
            auto it = gpu.find(me.source.vbSymbol);
            if (it == gpu.end()) it = gpu.emplace(me.source.vbSymbol, c.symbolData(size_t(me.source.vbSymbol))).first;
            g = &it->second;
        }
        r.skins[mi].resize(me.vertices.size());
        for (size_t v = 0; v < me.vertices.size(); ++v) {
            VertexSkin& s = r.skins[mi][v];
            const uint64_t at = g ? uint64_t(me.source.vbOffset) + uint64_t(me.source.vertex[v]) * me.stride + uint64_t(me.layout.attributes) : 0;
            for (int k = 0; k < 4; ++k) {
                uint16_t bone = g && at + 8 <= g->size() ? be16(&(*g)[at + 2 * uint64_t(k)]) : me.vertices[v].bones[k];
                if (bone >= r.bones.size()) bone = 0;
                s[size_t(k)] = {bone, me.vertices[v].weights[k]};
            }
        }
    }
    for (auto it = r.shared.begin(); it != r.shared.end();) it = it->second.size() > 1 ? std::next(it) : r.shared.erase(it);
    return r;
}

std::vector<std::string> write(caff::Caff& c, size_t symbol, const Rig& rig) {
    const model::Model m = model::parse(c, symbol);
    if (rig.gameBones != m.bones.size() || rig.bones.size() < m.bones.size()) fail("rig: the model's skeleton changed since the rig was read");
    for (size_t i = 0; i < m.bones.size(); ++i)
        if (rig.bones[i].name != m.bones[i].name) fail("rig: the model's bone " + m.bones[i].name + " isn't where the rig has it");
    for (size_t i = 0; i < rig.bones.size(); ++i) {
        if (i >= m.bones.size() && rig.bones[i].parent >= int(i)) fail("rig: bone " + rig.bones[i].name + " comes before its parent");
        if (i < m.bones.size() && rig.bones[i].parent != m.bones[i].parent && rig.bones[i].parent >= int(i))
            fail("rig: bone " + rig.bones[i].name + " can only have a parent listed before it");
        if (rig.bones[i].name.empty()) fail("rig: a bone has no name");
        for (size_t k = 0; k < i; ++k)
            if (rig.bones[k].name == rig.bones[i].name) fail("rig: two bones are named " + rig.bones[i].name);
    }
    if (rig.bones.size() > 0xFFFE) fail("rig: too many bones");
    if (rig.skins.size() != m.meshes.size()) fail("rig: the model's meshes changed since the rig was read");
    for (size_t mi = 0; mi < m.meshes.size(); ++mi)
        if (!rig.skins[mi].empty() && rig.skins[mi].size() != m.meshes[mi].vertices.size())
            fail("rig: mesh " + std::to_string(mi) + " changed since the rig was read");
    std::vector<std::string> notes;
    writePose(c, symbol, m, rig, notes);
    writeSkins(c, symbol, model::parse(c, symbol), rig, notes);
    // A rest shape bent with the bones goes in as NutTool's OBJ round trip does: same counts, edited in place.
    model::Model now = model::parse(c, symbol);
    std::vector<bool> include(now.meshes.size(), false);
    bool reshaped = false;
    for (size_t mi = 0; mi < now.meshes.size() && mi < rig.shape.size(); ++mi) {
        if (rig.shape[mi].empty()) continue;
        if (rig.shape[mi].size() != now.meshes[mi].vertices.size()) fail("rig: mesh " + std::to_string(mi) + " changed since it was bent");
        for (size_t v = 0; v < rig.shape[mi].size(); ++v)
            for (int k = 0; k < 3; ++k) {
                now.meshes[mi].vertices[v].pos[k] = rig.shape[mi][v][size_t(k)];
                now.meshes[mi].vertices[v].normal[k] = rig.shape[mi][v][size_t(3 + k)];
            }
        include[mi] = true;
        reshaped = true;
    }
    if (reshaped) {
        const model::ImportReport r = model::importObj(c, symbol, model::toObj(now, "rig.mtl", {}, include));
        notes.push_back("rig: " + std::to_string(r.edited + r.rebuilt) + " mesh(es) take the shape bent with the bones");
    }
    return notes;
}

void syncAliases(Rig& rig, size_t mesh, uint32_t v) {
    if (mesh >= rig.keys.size() || v >= rig.keys[mesh].size() || mesh >= rig.skins.size() || v >= rig.skins[mesh].size()) return;
    const auto it = rig.shared.find(rig.keys[mesh][v]);
    if (it == rig.shared.end()) return;
    const VertexSkin s = rig.skins[mesh][v];
    for (auto [mi, k] : it->second)
        if (mi < rig.skins.size() && k < rig.skins[mi].size()) rig.skins[mi][k] = s;
}

void evaluate(const std::vector<Bone>& bones, const std::vector<PoseKey>& pose, std::vector<Mat4>& world, std::vector<Mat4>& skin) {
    const size_t n = bones.size();
    world.assign(n, identity());
    skin.assign(n, identity());
    for (size_t i = 0; i < n; ++i) {
        const Bone& b = bones[i];
        const PoseKey key = i < pose.size() ? pose[i] : PoseKey{};
        V3 t = b.pos;
        if (b.parent >= 0 && size_t(b.parent) < i)
            for (int k = 0; k < 3; ++k) t[size_t(k)] -= bones[size_t(b.parent)].pos[size_t(k)];
        for (int k = 0; k < 3; ++k) t[size_t(k)] += key.offset[size_t(k)];
        const Mat4 local = trs(t, key.rotation, key.scale);
        world[i] = b.parent >= 0 && size_t(b.parent) < i ? mul(world[size_t(b.parent)], local) : local;
        Mat4 unbind = identity();
        unbind[12] = -b.pos[0], unbind[13] = -b.pos[1], unbind[14] = -b.pos[2];
        skin[i] = mul(world[i], unbind);
    }
}

std::vector<float> skinMesh(const model::Mesh& mesh, const std::vector<VertexSkin>& weights, const std::vector<Mat4>& skin) {
    std::vector<float> out(mesh.vertices.size() * 3);
    for (size_t v = 0; v < mesh.vertices.size(); ++v) {
        const float* p = mesh.vertices[v].pos;
        float r[3] = {p[0], p[1], p[2]};
        if (v < weights.size()) {
            float acc[3] = {}, total = 0;
            for (const Influence& in : weights[v]) {
                if (in.weight <= 0 || in.bone >= skin.size()) continue;
                const Mat4& M = skin[in.bone];
                for (int k = 0; k < 3; ++k) acc[k] += in.weight * (M[size_t(k)] * p[0] + M[size_t(4 + k)] * p[1] + M[size_t(8 + k)] * p[2] + M[size_t(12 + k)]);
                total += in.weight;
            }
            if (total > 0)
                for (int k = 0; k < 3; ++k) r[k] = acc[k] / total;
        }
        out[v * 3] = r[0], out[v * 3 + 1] = r[1], out[v * 3 + 2] = r[2];
    }
    return out;
}

float weightOf(const VertexSkin& s, uint16_t bone) {
    float w = 0;
    for (const Influence& in : s)
        if (in.bone == bone) w += in.weight;
    return w;
}

void normalize(VertexSkin& s) {
    float total = 0;
    for (const Influence& in : s) total += std::max(0.0f, in.weight);
    if (total <= 0) return;
    for (Influence& in : s) in.weight = std::max(0.0f, in.weight) / total;
}

void setWeight(VertexSkin& s, uint16_t bone, float w, bool normalizeOthers) {
    w = std::clamp(w, 0.0f, 1.0f);
    int slot = -1;
    for (int k = 0; k < 4; ++k)
        if (s[size_t(k)].bone == bone && s[size_t(k)].weight > 0) slot = k;
    if (slot < 0) {
        if (w <= 0) return;
        slot = 0;
        for (int k = 1; k < 4; ++k)
            if (s[size_t(k)].weight < s[size_t(slot)].weight) slot = k;
        s[size_t(slot)] = {bone, 0};
    }
    float others = 0;
    for (int k = 0; k < 4; ++k)
        if (k != slot) others += std::max(0.0f, s[size_t(k)].weight);
    s[size_t(slot)].weight = w;
    if (normalizeOthers && others > 1e-6f) {
        const float scale = (1 - w) / others;
        for (int k = 0; k < 4; ++k)
            if (k != slot) s[size_t(k)].weight = std::max(0.0f, s[size_t(k)].weight) * scale;
    }
}

VertexSkin fromWeights(const std::map<uint16_t, float>& weights, int maxInfluences) {
    std::vector<std::pair<float, uint16_t>> sorted;
    for (auto& [b, w] : weights)
        if (w > 0) sorted.push_back({w, b});
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first > b.first; });
    VertexSkin s{};
    const size_t n = std::min<size_t>(sorted.size(), size_t(std::clamp(maxInfluences, 1, 4)));
    for (size_t k = 0; k < n; ++k) s[k] = {sorted[k].second, sorted[k].first};
    normalize(s);
    return s;
}

void autoWeights(const model::Model& m, Rig& rig, const Mask& mask, const AutoOptions& opt) {
    const size_t n = rig.bones.size();
    if (!n) fail("the model has no bones");
    // Each bone's segment: its joint to its child with the most bones under it (a point when it has none).
    std::vector<int> descendants(n, 0), mainChild(n, -1);
    for (size_t i = n; i-- > 0;)
        if (rig.bones[i].parent >= 0) descendants[size_t(rig.bones[i].parent)] += descendants[i] + 1;
    for (size_t i = 0; i < n; ++i) {
        const int p = rig.bones[i].parent;
        if (p >= 0 && (mainChild[size_t(p)] < 0 || descendants[i] > descendants[size_t(mainChild[size_t(p)])])) mainChild[size_t(p)] = int(i);
    }
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const Bone& b : rig.bones)
        for (int k = 0; k < 3; ++k) lo[size_t(k)] = std::min(lo[size_t(k)], b.pos[size_t(k)]), hi[size_t(k)] = std::max(hi[size_t(k)], b.pos[size_t(k)]);
    const float size = std::max(std::sqrt(dist2(lo, hi)), 1e-3f);
    std::vector<size_t> allowed;
    for (size_t i = 0; i < n; ++i)
        if (opt.bones.empty() || (i < opt.bones.size() && opt.bones[i])) allowed.push_back(i);
    if (allowed.empty()) fail("no bones were picked to take weight");
    const float radius = std::max(opt.envelope, 1e-4f) * size;
    forMasked(rig, mask, [&](size_t mi, size_t v, VertexSkin& s) {
        const V3 p = posOf(m.meshes[mi], v);
        std::vector<std::pair<float, uint16_t>> d;
        for (size_t i : allowed) {
            const V3& a = rig.bones[i].pos;
            const V3& b = mainChild[i] >= 0 ? rig.bones[size_t(mainChild[i])].pos : a;
            d.push_back({segmentDistance(p, a, b), uint16_t(i)});
        }
        std::sort(d.begin(), d.end());
        std::map<uint16_t, float> w;
        switch (opt.method) {
        case AutoMethod::Nearest: w[d[0].second] = 1; break;
        case AutoMethod::Envelope:
            for (auto& [dist, b] : d)
                if (dist < radius) w[b] = (1 - dist / radius) * (1 - dist / radius);
            if (w.empty()) w[d[0].second] = 1;
            break;
        case AutoMethod::Blend:
            for (size_t k = 0; k < d.size() && k < 8; ++k)
                w[d[k].second] = 1.0f / (std::pow(d[k].first / size, std::max(opt.power, 0.5f)) + 1e-9f);
            break;
        }
        s = fromWeights(w, opt.maxInfluences);
    });
}

size_t transfer(const model::Model& src, const Rig& srcRig, const model::Model& dst, Rig& rig, const Mask& mask, const TransferOptions& opt) {
    // Source bone -> this rig's bone: by name, then loosely, then through its nearest matched ancestor.
    std::map<std::string, uint16_t> exact, loose;
    for (size_t i = 0; i < rig.bones.size(); ++i) {
        exact.emplace(rig.bones[i].name, uint16_t(i));
        loose.emplace(looseName(rig.bones[i].name), uint16_t(i));
    }
    std::vector<int> map(srcRig.bones.size(), -1);
    size_t matched = 0;
    for (size_t i = 0; i < srcRig.bones.size(); ++i) {
        auto e = exact.find(srcRig.bones[i].name);
        auto l = loose.find(looseName(srcRig.bones[i].name));
        if (e != exact.end()) map[i] = e->second;
        else if (l != loose.end()) map[i] = l->second;
        if (map[i] >= 0) ++matched;
    }
    if (!matched) fail("none of the source's bones are named like this model's");
    for (size_t i = 0; i < map.size(); ++i)
        if (map[i] < 0) {
            int p = srcRig.bones[i].parent;
            while (p >= 0 && map[size_t(p)] < 0) p = srcRig.bones[size_t(p)].parent;
            map[i] = p >= 0 ? map[size_t(p)] : 0;
        }

    struct Tri {
        uint32_t mesh, v[3];
    };
    std::vector<Tri> tris;
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (size_t mi = 0; mi < src.meshes.size(); ++mi) {
        if (mi >= srcRig.skins.size() || srcRig.skins[mi].empty()) continue;
        const model::Mesh& me = src.meshes[mi];
        for (size_t i = 0; i + 2 < me.indices.size(); i += 3) {
            tris.push_back({uint32_t(mi), {me.indices[i], me.indices[i + 1], me.indices[i + 2]}});
            for (uint32_t v : tris.back().v)
                for (int k = 0; k < 3; ++k)
                    lo[size_t(k)] = std::min(lo[size_t(k)], me.vertices[v].pos[k]), hi[size_t(k)] = std::max(hi[size_t(k)], me.vertices[v].pos[k]);
        }
    }
    if (tris.empty()) fail("the source has no skinned meshes");
    Grid grid;
    grid.init(lo, hi, tris.size());
    for (uint32_t t = 0; t < tris.size(); ++t) {
        V3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
        for (uint32_t v : tris[t].v)
            for (int k = 0; k < 3; ++k) {
                const float x = src.meshes[tris[t].mesh].vertices[v].pos[k];
                mn[size_t(k)] = std::min(mn[size_t(k)], x), mx[size_t(k)] = std::max(mx[size_t(k)], x);
            }
        grid.insert(t, mn, mx);
    }
    forMasked(rig, mask, [&](size_t mi, size_t v, VertexSkin& s) {
        const V3 p = posOf(dst.meshes[mi], v);
        float best = 1e30f;
        std::array<float, 3> bary{1, 0, 0};
        int bestTri = -1;
        for (int r = 0;; ++r) {
            const bool more = grid.ring(p, r, [&](uint32_t t) {
                const model::Mesh& me = src.meshes[tris[t].mesh];
                const V3 a = posOf(me, tris[t].v[0]), b = posOf(me, tris[t].v[1]), c = posOf(me, tris[t].v[2]);
                const auto w = closestBarycentric(p, a, b, c);
                const V3 q{a[0] * w[0] + b[0] * w[1] + c[0] * w[2], a[1] * w[0] + b[1] * w[1] + c[1] * w[2], a[2] * w[0] + b[2] * w[1] + c[2] * w[2]};
                const float d = dist2(p, q);
                if (d < best) best = d, bary = w, bestTri = int(t);
            });
            if (bestTri >= 0 && float(r) * grid.minCell() > std::sqrt(best)) break;
            if (!more) break;
        }
        if (bestTri < 0) return;
        const Tri& t = tris[size_t(bestTri)];
        if (!opt.nearestSurface) {
            int k = int(std::max_element(bary.begin(), bary.end()) - bary.begin());
            bary = {0, 0, 0};
            bary[size_t(k)] = 1;
        }
        std::map<uint16_t, float> w;
        for (int k = 0; k < 3; ++k) {
            if (bary[size_t(k)] <= 0) continue;
            for (const Influence& in : srcRig.skins[t.mesh][t.v[k]])
                if (in.weight > 0 && in.bone < map.size()) w[uint16_t(map[in.bone])] += in.weight * bary[size_t(k)];
        }
        if (!w.empty()) s = fromWeights(w, opt.maxInfluences);
    });
    return matched;
}

Adjacency adjacency(const model::Model& m) {
    Adjacency a;
    a.place.resize(m.meshes.size());
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const auto& me : m.meshes)
        for (const auto& v : me.vertices)
            for (int k = 0; k < 3; ++k) lo[size_t(k)] = std::min(lo[size_t(k)], v.pos[k]), hi[size_t(k)] = std::max(hi[size_t(k)], v.pos[k]);
    const float eps = std::max(std::sqrt(dist2(lo, hi)) * 1e-5f, 1e-7f);
    std::unordered_map<uint64_t, uint32_t> byPos;
    auto keyOf = [&](const float* p) {
        uint64_t h = 1469598103934665603ull;
        for (int k = 0; k < 3; ++k) {
            const int64_t q = int64_t(std::llround(p[k] / eps));
            h = (h ^ uint64_t(q)) * 1099511628211ull;
        }
        return h;
    };
    for (size_t mi = 0; mi < m.meshes.size(); ++mi) {
        const auto& me = m.meshes[mi];
        a.place[mi].assign(me.vertices.size(), 0);
        for (size_t v = 0; v < me.vertices.size(); ++v) {
            const uint64_t key = keyOf(me.vertices[v].pos);
            auto it = byPos.find(key);
            if (it == byPos.end()) {
                it = byPos.emplace(key, uint32_t(a.members.size())).first;
                a.members.emplace_back();
            }
            a.place[mi][v] = it->second;
            a.members[it->second].push_back({uint32_t(mi), uint32_t(v)});
        }
    }
    a.neighbours.resize(a.members.size());
    for (size_t mi = 0; mi < m.meshes.size(); ++mi) {
        const auto& me = m.meshes[mi];
        for (size_t i = 0; i + 2 < me.indices.size(); i += 3)
            for (int k = 0; k < 3; ++k) {
                const uint32_t pa = a.place[mi][me.indices[i + size_t(k)]], pb = a.place[mi][me.indices[i + size_t((k + 1) % 3)]];
                if (pa == pb) continue;
                a.neighbours[pa].push_back(pb);
                a.neighbours[pb].push_back(pa);
            }
    }
    for (auto& n : a.neighbours) {
        std::sort(n.begin(), n.end());
        n.erase(std::unique(n.begin(), n.end()), n.end());
    }
    return a;
}

void smooth(const model::Model& m, const Adjacency& adj, Rig& rig, const Mask& mask, float factor, int iterations, int maxInfluences) {
    (void)m;
    // The places the mask takes: a place is taken when one of its skinned vertices is.
    std::vector<char> taken(adj.members.size(), 0);
    auto skinOf = [&](uint32_t place) -> const VertexSkin* {
        for (auto& [mi, v] : adj.members[place])
            if (mi < rig.skins.size() && !rig.skins[mi].empty()) return &rig.skins[mi][v];
        return nullptr;
    };
    for (size_t p = 0; p < adj.members.size(); ++p)
        for (auto& [mi, v] : adj.members[p])
            if (mi < rig.skins.size() && !rig.skins[mi].empty() && mask.has(mi, v)) taken[p] = 1;
    factor = std::clamp(factor, 0.0f, 1.0f);
    for (int it = 0; it < std::max(iterations, 1); ++it) {
        std::vector<std::pair<uint32_t, VertexSkin>> next;
        for (uint32_t p = 0; p < adj.members.size(); ++p) {
            if (!taken[p]) continue;
            const VertexSkin* own = skinOf(p);
            if (!own) continue;
            std::map<uint16_t, float> avg;
            int count = 0;
            for (uint32_t q : adj.neighbours[p])
                if (const VertexSkin* s = skinOf(q)) {
                    for (const Influence& in : *s)
                        if (in.weight > 0) avg[in.bone] += in.weight;
                    ++count;
                }
            if (!count) continue;
            std::map<uint16_t, float> w;
            for (const Influence& in : *own)
                if (in.weight > 0) w[in.bone] += in.weight * (1 - factor);
            for (auto& [b, x] : avg) w[b] += x / float(count) * factor;
            next.push_back({p, fromWeights(w, maxInfluences)});
        }
        for (auto& [p, s] : next)
            for (auto& [mi, v] : adj.members[p])
                if (mi < rig.skins.size() && !rig.skins[mi].empty()) rig.skins[mi][v] = s;
    }
}

void normalizeAll(Rig& rig, const Mask& mask) {
    forMasked(rig, mask, [](size_t, size_t, VertexSkin& s) { normalize(s); });
}

void clean(Rig& rig, const Mask& mask, float threshold) {
    forMasked(rig, mask, [threshold](size_t, size_t, VertexSkin& s) {
        std::map<uint16_t, float> w;
        float heaviest = 0;
        uint16_t top = 0;
        for (const Influence& in : s) {
            if (in.weight > heaviest) heaviest = in.weight, top = in.bone;
            if (in.weight >= threshold) w[in.bone] += in.weight;
        }
        if (w.empty() && heaviest > 0) w[top] = 1;
        if (!w.empty()) s = fromWeights(w);
    });
}

void limit(Rig& rig, const Mask& mask, int maxInfluences) {
    forMasked(rig, mask, [maxInfluences](size_t, size_t, VertexSkin& s) {
        std::map<uint16_t, float> w;
        for (const Influence& in : s)
            if (in.weight > 0) w[in.bone] += in.weight;
        if (!w.empty()) s = fromWeights(w, maxInfluences);
    });
}

int mirrorBone(const Rig& rig, size_t bone) {
    if (bone >= rig.bones.size()) return -1;
    const std::string& n = rig.bones[bone].name;
    static const std::pair<const char*, const char*> kPrefix[] = {{"LF_", "RT_"}, {"L_", "R_"}, {"Left", "Right"}, {"left", "right"}};
    static const std::pair<const char*, const char*> kSuffix[] = {{"_L", "_R"}, {".L", ".R"}, {"_l", "_r"}, {".l", ".r"}, {"Left", "Right"}};
    auto find = [&](const std::string& name) {
        for (size_t i = 0; i < rig.bones.size(); ++i)
            if (rig.bones[i].name == name) return int(i);
        return -1;
    };
    for (auto [a, b] : kPrefix)
        for (int side = 0; side < 2; ++side) {
            const std::string from = side ? b : a, to = side ? a : b;
            if (startsWith(n, from)) return find(to + n.substr(from.size()));
        }
    for (auto [a, b] : kSuffix)
        for (int side = 0; side < 2; ++side) {
            const std::string from = side ? b : a, to = side ? a : b;
            if (n.size() > from.size() && n.compare(n.size() - from.size(), from.size(), from) == 0)
                return find(n.substr(0, n.size() - from.size()) + to);
        }
    return -1;
}

size_t mirrorWeights(const model::Model& m, Rig& rig, const Mask& mask, bool positiveToNegative) {
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    std::vector<std::pair<uint32_t, uint32_t>> all;
    for (size_t mi = 0; mi < m.meshes.size(); ++mi) {
        if (mi >= rig.skins.size() || rig.skins[mi].empty()) continue;
        for (size_t v = 0; v < m.meshes[mi].vertices.size(); ++v) {
            all.push_back({uint32_t(mi), uint32_t(v)});
            for (int k = 0; k < 3; ++k)
                lo[size_t(k)] = std::min(lo[size_t(k)], m.meshes[mi].vertices[v].pos[k]), hi[size_t(k)] = std::max(hi[size_t(k)], m.meshes[mi].vertices[v].pos[k]);
        }
    }
    if (all.empty()) return 0;
    const float tol = std::max(std::sqrt(dist2(lo, hi)) * 2e-3f, 1e-5f);
    Grid grid;
    grid.init(lo, hi, all.size());
    for (uint32_t i = 0; i < all.size(); ++i) {
        const V3 p = posOf(m.meshes[all[i].first], all[i].second);
        grid.insert(i, p, p);
    }
    std::vector<int> twin(rig.bones.size());
    for (size_t b = 0; b < rig.bones.size(); ++b) {
        const int t = mirrorBone(rig, b);
        twin[b] = t < 0 ? int(b) : t;
    }
    size_t count = 0;
    for (auto [mi, v] : all) {
        if (!mask.has(mi, v)) continue;
        const V3 p = posOf(m.meshes[mi], v);
        if (positiveToNegative ? p[0] >= -tol : p[0] <= tol) continue;  // only the side being written
        const V3 q{-p[0], p[1], p[2]};
        float best = tol * tol;
        int found = -1;
        for (int r = 0; r <= 1; ++r)
            grid.ring(q, r, [&](uint32_t i) {
                const float d = dist2(q, posOf(m.meshes[all[i].first], all[i].second));
                if (d <= best) best = d, found = int(i);
            });
        if (found < 0) continue;
        VertexSkin s = rig.skins[all[size_t(found)].first][all[size_t(found)].second];
        for (Influence& in : s)
            if (in.bone < twin.size()) in.bone = uint16_t(twin[in.bone]);
        rig.skins[mi][v] = s;
        syncAliases(rig, mi, v);
        ++count;
    }
    return count;
}

size_t mirrorJoints(Rig& rig, bool positiveToNegative) {
    size_t count = 0;
    for (size_t i = 0; i < rig.bones.size(); ++i) {
        const int t = mirrorBone(rig, i);
        if (t < 0) continue;
        const V3& p = rig.bones[i].pos;
        if (positiveToNegative ? p[0] <= 0 : p[0] >= 0) continue;
        rig.bones[size_t(t)].pos = {-p[0], p[1], p[2]};
        ++count;
    }
    return count;
}

std::string uniqueName(const Rig& rig, const std::string& base) {
    auto taken = [&](const std::string& n) {
        for (const Bone& b : rig.bones)
            if (b.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int i = 1;; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), ".%03d", i);
        if (!taken(base + buf)) return base + buf;
    }
}

} // namespace nut::rigedit
