#include "model.hpp"

#include "assetdb.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace nut::model {
namespace {

float half(uint16_t h) {
    uint32_t s = uint32_t(h >> 15) << 31, e = (h >> 10) & 0x1F, m = h & 0x3FF, f;
    if (e == 0) {
        if (m == 0) f = s;
        else { // subnormal
            int k = 0;
            while (!(m & 0x400)) { m <<= 1; ++k; }
            m &= 0x3FF;
            f = s | uint32_t(127 - 15 - k + 1) << 23 | m << 13;
        }
    } else if (e == 31) {
        f = s | 0x7F800000u | m << 13;
    } else {
        f = s | (e + 112) << 23 | m << 13;
    }
    float r;
    std::memcpy(&r, &f, 4);
    return r;
}

// A symbol's bytes plus where each relocated pointer inside it points.
class View {
public:
    View(const caff::Caff& c, size_t sym) : c_(c), sym_(sym), data_(c.symbolData(sym)) {
        for (int set = 0; set < 2; ++set)
            for (auto& r : c.relocs[set])
                if (r.from == sym + 1)
                    for (uint32_t o : r.offsets) target_[o] = r.to - 1;
    }
    const Bytes& data() const { return data_; }
    size_t size() const { return data_.size(); }
    uint32_t u32(size_t o) const {
        if (o + 4 > data_.size()) fail("model: read past end of symbol");
        return be32(&data_[o]);
    }
    uint16_t u16(size_t o) const {
        if (o + 2 > data_.size()) fail("model: read past end of symbol");
        return be16(&data_[o]);
    }
    float f32(size_t o) const {
        uint32_t v = u32(o);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    // Target symbol (0-based) of the pointer stored at `o`, if relocated.
    std::optional<size_t> targetOf(size_t o) const {
        auto it = target_.find(uint32_t(o));
        if (it == target_.end()) return std::nullopt;
        return it->second;
    }
    // Follow a pointer that must point back into this symbol.
    uint32_t local(size_t o) const {
        auto t = targetOf(o);
        if (!t || *t != sym_) fail("model: unexpected pointer target");
        return u32(o);
    }
    bool isLocal(size_t o) const { auto t = targetOf(o); return t && *t == sym_; }
    std::string cstr(size_t ptrAt) const {
        auto t = targetOf(ptrAt);
        if (!t) return {};
        uint32_t off = u32(ptrAt);
        const Bytes& d = bytes(*t);
        if (off >= d.size()) return {};
        size_t e = off;
        while (e < d.size() && d[e]) ++e;
        return std::string(reinterpret_cast<const char*>(&d[off]), e - off);
    }
    // Bytes of any symbol of the CAFF (cached).
    const Bytes& bytes(size_t sym) const {
        if (sym == sym_) return data_;
        auto it = cache_.find(sym);
        if (it == cache_.end()) it = cache_.emplace(sym, c_.symbolData(sym)).first;
        return it->second;
    }
    size_t sym() const { return sym_; }

private:
    const caff::Caff& c_;
    size_t sym_;
    Bytes data_;
    std::map<uint32_t, size_t> target_;
    mutable std::map<size_t, Bytes> cache_;
};

// Detail levels named by an authoring node: "LODBIT_|...|name_0_1_" (levels 0 and 1) or a
// last path segment ending in "LOD_2" / "LOD2". Returns a bit mask, 0 for other nodes.
uint32_t lodBits(const std::string& path) {
    std::string seg = path.substr(path.rfind('|') == std::string::npos ? 0 : path.rfind('|') + 1);
    uint32_t bits = 0;
    if (startsWith(path, "LODBIT_")) {
        // Trailing "_<n>_<m>_" numbers.
        std::vector<std::string> tok;
        size_t a = 0;
        while (a <= seg.size()) {
            size_t b = seg.find('_', a);
            if (b == std::string::npos) b = seg.size();
            tok.push_back(seg.substr(a, b - a));
            a = b + 1;
        }
        while (!tok.empty() && tok.back().empty()) tok.pop_back();
        for (size_t i = tok.size(); i-- > 0;) {
            const std::string& t = tok[i];
            if (t.empty() || t.size() > 2 || !std::all_of(t.begin(), t.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) break;
            bits |= 1u << std::stoi(t);
        }
        return bits;
    }
    // A "lod<n>" / "lod_<n>" token anywhere in the segment ("Kazooie_LOD2", "CASE_lod_0_SWITCH_1_0").
    std::string low = lower(seg);
    for (size_t p = low.find("lod"); p != std::string::npos; p = low.find("lod", p + 1)) {
        if (p > 0 && low[p - 1] != '_') continue;
        size_t d = p + 3;
        if (d < low.size() && low[d] == '_') ++d;
        size_t e = d;
        while (e < low.size() && low[e] >= '0' && low[e] <= '9') ++e;
        if (e == d || e - d > 2 || (e < low.size() && low[e] != '_')) continue;
        return 1u << std::stoi(low.substr(d, e - d));
    }
    return 0;
}

// "<name>_SWITCH<id>_<state>" (or "_SWITCH_<id>_<state>") in the last path segment: drawn while
// switch <id> is in <state>.
bool switchOf(const std::string& path, int& id, int& state) {
    std::string seg = path.substr(path.rfind('|') == std::string::npos ? 0 : path.rfind('|') + 1);
    size_t p = lower(seg).rfind("_switch");
    if (p == std::string::npos) return false;
    p += 7;
    if (p < seg.size() && seg[p] == '_') ++p;
    return std::sscanf(seg.c_str() + p, "%d_%d", &id, &state) == 2;
}

// A "pose" block (version 19.12.06.0036): u32 boneCount at +0x18, 52-byte records from +0x30
// {vec3 local, vec3 world, quat rotation, u16 parent, ...}, then a name table
// {count, ->entries {->name, ?, u32 index}}.
std::vector<Bone> readPose(const View& v, uint32_t pose) {
    std::vector<Bone> bones;
    if (pose + 0x30 > v.size() || std::memcmp(&v.data()[pose], "pose", 4) != 0) return bones;
    uint32_t n = v.u32(pose + 0x18);
    uint32_t rec = pose + 0x30;
    if (n >= 1024 || rec + n * 52 > v.size()) return bones;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t r = rec + i * 52;
        Bone b;
        for (int k = 0; k < 3; ++k) b.local[k] = v.f32(r + 4 * k);
        for (int k = 0; k < 3; ++k) b.world[k] = v.f32(r + 12 + 4 * k);
        for (int k = 0; k < 4; ++k) b.rotation[k] = v.f32(r + 24 + 4 * k);
        uint16_t parent = v.u16(r + 40);
        b.parent = parent == 0xFFFF || parent >= n ? -1 : int(parent);
        b.name = "bone" + std::to_string(i);
        bones.push_back(b);
    }
    uint32_t nt = rec + n * 52;
    if (nt + 8 <= v.size() && v.u32(nt) == n && v.isLocal(nt + 4)) {
        uint32_t ents = v.local(nt + 4);
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t e = ents + 12 * i;
            if (e + 12 > v.size()) break;
            uint32_t idx = v.u32(e + 8);
            std::string nm = v.cstr(e);
            if (idx < n && !nm.empty()) bones[idx].name = nm;
        }
    }
    return bones;
}

struct Material {
    std::string name;
    std::vector<uint32_t> programs;  // .stream offsets of its programs (one per pass)
    std::vector<std::string> params; // constant names in register order
};

std::optional<uint32_t> findRendergraph(const View& v) {
    // Chunk directory first; fall back to searching for the header string.
    try {
        uint32_t dir = v.u32(0), n = v.u32(4);
        if (dir + 8 * uint64_t(n) <= v.size()) {
            for (uint32_t i = 0; i < n && i < 64; ++i) {
                uint32_t type = v.u32(dir + 8 * i);
                if (type == 0x1E && v.isLocal(dir + 4 + 8 * i)) {
                    uint32_t at = v.local(dir + 4 + 8 * i);
                    if (v.isLocal(at)) {
                        uint32_t rg = v.local(at);
                        if (rg + 12 <= v.size() && std::memcmp(&v.data()[rg], "rendergraph", 11) == 0) return rg;
                    }
                }
            }
        }
    } catch (...) {
    }
    static const char sig[] = "rendergraph";
    auto it = std::search(v.data().begin(), v.data().end(), sig, sig + 11);
    if (it == v.data().end()) return std::nullopt;
    return uint32_t(it - v.data().begin());
}

// Chunk directory: model+0 points at {type, ptr} pairs, model+4 is their count.
std::optional<uint32_t> chunk(const View& v, uint32_t type) {
    uint32_t dir = v.u32(0), n = v.u32(4);
    for (uint32_t i = 0; i < n && i < 64; ++i)
        if (dir + 8 * i + 8 <= v.size() && v.u32(dir + 8 * i) == type && v.isLocal(dir + 4 + 8 * i)) return v.local(dir + 4 + 8 * i);
    return std::nullopt;
}

std::string mergedTextureName(const std::string& n) {
    if (n.size() > 14 && (endsWith(n, "mip") || endsWith(n, "top"))) {
        size_t p = n.rfind("_0x");
        if (p != std::string::npos && n.size() - p == 14) return n.substr(0, n.size() - 3);
    }
    return n;
}

// 10:10:10 signed normalised vector packed into a big-endian u32 (x in the low bits).
void dec3n(uint32_t v, float out[3]) {
    for (int k = 0; k < 3; ++k) {
        int c = int((v >> (10 * k)) & 0x3FF);
        if (c >= 512) c -= 1024;
        out[k] = std::max(-1.0f, float(c) / 511.0f);
    }
}

// The bytes of a vertex buffer that are vertices: a NutTool draw's material record ("RMTH" .. "RMTF")
// and shape keys ("RMPH" .. "RMPF") follow them, and taken as vertices would spoil the layout's detection.
uint32_t vertexBytes(const uint8_t* base, uint32_t size, uint32_t stride) {
    constexpr uint32_t kRecordEnd = 44;  // the material record's word: where what it follows ends
    if (size >= 8 && be32(base + size - 4) == 0x524D5446) {  // "RMTF"
        const uint32_t header = be32(base + size - 8);
        if (header + kRecordEnd + 4 <= size && be32(base + header) == 0x524D5448)  // "RMTH"
            size = std::min(size, be32(base + header + kRecordEnd));
    }
    if (size >= 8 && be32(base + size - 4) == 0x524D5046) {  // "RMPF"
        const uint32_t header = be32(base + size - 8);
        if (header + 8 <= size && be32(base + header) == 0x524D5048)  // "RMPH"
            size = uint32_t(std::min<uint64_t>(size, uint64_t(be32(base + header + 4)) * stride));
    }
    return size;
}

// Detects which fields a vertex buffer has by looking at every vertex.
Layout detectLayout(const uint8_t* data, uint32_t stride, size_t n) {
    Layout l;
    if (n == 0 || stride < 12) return l;
    auto fraction = [&](auto&& pred) {
        size_t ok = 0;
        for (size_t i = 0; i < n; ++i) ok += pred(data + i * stride) ? 1 : 0;
        return double(ok) / double(n);
    };
    // Position: half4 with w = 1.0, or float3.
    double half4 = fraction([](const uint8_t* p) { return p[6] == 0x3C && p[7] == 0x00; });
    if (half4 < 0.98) {
        double float3 = fraction([](const uint8_t* p) {
            for (int k = 0; k < 3; ++k) {
                uint32_t b = be32(p + 4 * k);
                float f;
                std::memcpy(&f, &b, 4);
                if (!std::isfinite(f) || std::fabs(f) > 1e5f) return false;
            }
            return true;
        });
        l.floatPosition = float3 > 0.98;
    }
    int base = l.floatPosition ? 12 : 8;
    l.attributes = base;
    if (int(stride) >= base + 16) {
        // Skinned: u16x4 bone indices (palette-sized, so the high bytes are zero) and u8x4 weights
        // that sum to 255.
        double skin = fraction([base](const uint8_t* p) {
            const uint8_t* s = p + base;
            int sum = s[8] + s[9] + s[10] + s[11];
            return s[0] == 0 && s[2] == 0 && s[4] == 0 && s[6] == 0 && sum >= 250 && sum <= 260;
        });
        l.skinned = skin > 0.98;
    }
    int at = l.skinned ? base + 12 : base;
    auto isHalf2 = [&](int off) {
        return fraction([&](const uint8_t* p) {
            float a = half(be16(p + off)), b = half(be16(p + off + 2));
            return std::isfinite(a) && std::isfinite(b) && std::fabs(a) < 4096 && std::fabs(b) < 4096;
        }) > 0.98;
    };
    if (at + 4 <= int(stride)) {
        l.normal = at;
        at += 4;
    }
    // Tangent (and possibly a binormal): consistently unit-length DEC3N vectors. That is a
    // stronger signal than "plausible as two half floats", which tangent bits often are too.
    // A tangent frame vector is unit length and perpendicular to the normal (and to the tangent);
    // texture coordinates read as DEC3N are not.
    auto isFrameVector = [&](int off, int other1, int other2) {
        return fraction([&](const uint8_t* p) {
            float v[3], a[3], b[3];
            dec3n(be32(p + off), v);
            float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len < 0.8f || len > 1.2f) return false;
            dec3n(be32(p + other1), a);
            if (std::fabs(a[0] * v[0] + a[1] * v[1] + a[2] * v[2]) > 0.3f) return false;
            if (other2 >= 0) {
                dec3n(be32(p + other2), b);
                if (std::fabs(b[0] * v[0] + b[1] * v[1] + b[2] * v[2]) > 0.3f) return false;
            }
            return true;
        }) > 0.85;
    };
    if (l.normal >= 0 && at + 4 <= int(stride) && isFrameVector(at, l.normal, -1)) {
        l.tangent = at;
        at += 4;
        if (at + 4 <= int(stride) && isFrameVector(at, l.normal, l.tangent)) at += 4; // binormal
    }
    for (; at + 4 <= int(stride); at += 4)
        if (isHalf2(at)) l.uvSets.push_back(at);
    return l;
}

Vertex decodeVertex(const uint8_t* p, const Layout& l) {
    Vertex v;
    for (int k = 0; k < 3; ++k) {
        if (l.floatPosition) {
            uint32_t b = be32(p + 4 * k);
            std::memcpy(&v.pos[k], &b, 4);
        } else {
            v.pos[k] = half(be16(p + 2 * k));
        }
    }
    if (l.normal >= 0) dec3n(be32(p + l.normal), v.normal);
    if (!l.uvSets.empty()) {
        v.uv[0] = half(be16(p + l.uvSets[0]));
        v.uv[1] = half(be16(p + l.uvSets[0] + 2));
    }
    if (l.uvSets.size() > 1) {
        v.uv2[0] = half(be16(p + l.uvSets[1]));
        v.uv2[1] = half(be16(p + l.uvSets[1] + 2));
    }
    if (l.uvSets.size() > 2) {
        v.uv3[0] = half(be16(p + l.uvSets[2]));
        v.uv3[1] = half(be16(p + l.uvSets[2] + 2));
    }
    if (l.skinned) {
        // Indices index the draw's bone palette; weights are stored in the opposite byte order.
        float sum = 0;
        for (int k = 0; k < 4; ++k) {
            v.bones[k] = uint8_t(be16(p + l.attributes + 2 * k));
            sum += (v.weights[k] = p[l.attributes + 8 + 3 - k] / 255.0f);
        }
        if (sum <= 0) { v.weights[0] = 1; v.weights[1] = v.weights[2] = v.weights[3] = 0; }
    }
    return v;
}

} // namespace

bool isModelSymbol(const caff::Caff& c, size_t symbol) {
    const auto& s = c.symbols.at(symbol);
    if (s.section == 0 || s.section > c.sections.size() || c.sections[s.section - 1].name != ".data" || s.size < 0x100) return false;
    std::string n = assetdb::baseAssetName(c.symbolName(symbol));
    return startsWith(n, "aid_model_");
}

// The colour map among a draw's bound textures, by name: the shader programs that give each
// sampler its meaning are not decoded. -1 when only detail maps (normal, specular...) are bound.
static int colourMap(const std::vector<std::string>& names, const std::vector<int>& bound) {
    for (const char* key : {"_colour", "_color", "diffuse", "_col_", "_albedo"})
        for (int t : bound)
            if (names[size_t(t)].find(key) != std::string::npos) return t;
    for (int t : bound) {
        const std::string& n = names[size_t(t)];
        bool detail = false;
        for (const char* key : {"normal", "specular", "ambientocclusion", "reflection", "velvet", "thinning", "_spec", "_nrm", "mask", "_ao_", "envmap",
                                "cubemap", "lightmap", "_fur_"})
            detail |= n.find(key) != std::string::npos;
        if (!detail) return t;
    }
    return -1;
}

static void computeBounds(Model& m);
static void applyBufferHeuristic(Model& m);

Model parse(const caff::Caff& c, size_t symbol) {
    Model m;
    m.name = assetdb::baseAssetName(c.symbolName(symbol));
    View v(c, symbol);
    auto rgOpt = findRendergraph(v);
    if (!rgOpt) fail("no rendergraph in " + m.name);
    uint32_t rg = *rgOpt;
    if (std::memcmp(&v.data()[rg + 12], "26.09.07.0039", 13) != 0) m.warnings.push_back("unexpected rendergraph version");

    // ---- property dictionary: textures, materials, bone indices
    std::map<std::string, uint32_t> intProps;
    std::map<int, int> textureOfFile; // texturefileN -> index into m.textures
    std::map<uint32_t, Material> materials; // by material block offset
    if (v.isLocal(rg + 0x20)) {
        uint32_t dict = v.local(rg + 0x20);
        uint32_t count = v.u32(dict);
        if (v.isLocal(dict + 8)) {
            uint32_t arr = v.local(dict + 8);
            std::map<int, std::string> texByIndex; // texturefileN -> merged name
            for (uint32_t k = 0; k < count && k < 100000; ++k) {
                uint32_t e = arr + 12 * k;
                if (!v.isLocal(e + 8)) continue;
                uint32_t ent = v.local(e + 8);
                std::string key = v.cstr(ent);
                if (key.empty()) continue;
                if (v.targetOf(ent + 8)) {
                    std::string val = v.cstr(ent + 8);
                    if (startsWith(key, "texturefile") && !val.empty()) {
                        texByIndex[std::atoi(key.c_str() + 11)] = mergedTextureName(val);
                    } else if (startsWith(key, "shader_")) {
                        if (std::find(m.materials.begin(), m.materials.end(), key.substr(7)) == m.materials.end()) m.materials.push_back(key.substr(7));
                        // Material block: {->programs {u32 offset, ->..}[], u32 count, ..., +0x20 u32 paramCount,
                        // ->params {name, 0, u32 register, u32 type}[]}.
                        if (v.isLocal(ent + 8) && !materials.count(v.u32(ent + 8))) {
                            uint32_t blk = v.local(ent + 8);
                            Material mat;
                            mat.name = key.substr(7);
                            // {->programs {u32 offset, ->..}[count], count, ...}
                            if (v.isLocal(blk))
                                for (uint32_t q = 0, pc = std::min<uint32_t>(v.u32(blk + 4), 16); q < pc; ++q) mat.programs.push_back(v.u32(v.local(blk) + 8 * q));
                            uint32_t np = std::min<uint32_t>(v.u32(blk + 0x20), 64);
                            if (np && v.isLocal(blk + 0x24)) {
                                uint32_t pa = v.local(blk + 0x24);
                                mat.params.resize(np);
                                for (uint32_t j = 0; j < np; ++j) {
                                    uint32_t reg = v.u32(pa + 16 * j + 8);
                                    if (reg < np) mat.params[reg] = v.cstr(pa + 16 * j);
                                }
                            }
                            materials.emplace(blk, std::move(mat));
                        }
                    }
                } else {
                    intProps.emplace(key, v.u32(ent + 8));
                }
            }
            // Split textures take two slots (N = mip half, N+1 = streamed top); list each once.
            for (auto& [i, n] : texByIndex) {
                auto at = std::find(m.textures.begin(), m.textures.end(), n);
                textureOfFile[i] = int(at - m.textures.begin());
                if (at == m.textures.end()) {
                    m.textures.push_back(n);
                    m.textureFiles.push_back(i);
                }
            }
        }
    }

    // ---- skeleton (pose)
    if (v.isLocal(rg + 0x104)) m.bones = readPose(v, v.local(rg + 0x104));
    else if (auto t = v.targetOf(rg + 0x104)) m.bones = readPose(View(c, *t), v.u32(rg + 0x104)); // a skeleton kept elsewhere (copyModel)

    // ---- authoring nodes: {count, ->records of 0x50 bytes {->name, ->short name, u32 index, ...}}
    if (v.isLocal(rg + 0x28)) {
        uint32_t t = v.local(rg + 0x28);
        uint32_t n = std::min<uint32_t>(v.u32(t), 65536);
        if (n && v.isLocal(t + 4)) {
            uint32_t arr = v.local(t + 4);
            for (uint32_t i = 0; i < n && arr + 0x50 * (i + 1) <= v.size(); ++i) {
                Node nd;
                nd.name = v.cstr(arr + 0x50 * i);
                nd.lods = lodBits(nd.name);
                switchOf(nd.name, nd.switchId, nd.switchState);
                for (int b = 0; b < 32; ++b)
                    if (nd.lods >> b & 1) m.lodCount = std::max(m.lodCount, b + 1);
                if (nd.switchId >= 0) {
                    auto& states = m.switches[nd.switchId];
                    if (std::find(states.begin(), states.end(), nd.switchState) == states.end()) states.push_back(nd.switchState);
                    std::sort(states.begin(), states.end());
                }
                m.nodes.push_back(std::move(nd));
            }
        }
    }

    // ---- GPU resources: vertex and index buffer records
    struct Vb { uint32_t gpu, size, stride; size_t gpuSym; uint32_t record, header; };
    struct Ib { uint32_t gpu, size, format; size_t gpuSym; uint32_t record; };
    std::map<uint32_t, Vb> vbs; // keyed by buffer object offset
    std::map<uint32_t, Ib> ibs;
    if (v.isLocal(rg + 0xFC)) {
        uint32_t res = v.local(rg + 0xFC);
        uint32_t nvb = v.u32(res + 44), nib = v.u32(res + 52);
        if (nvb && v.isLocal(res + 36)) {
            uint32_t arr = v.local(res + 36);
            for (uint32_t i = 0; i < nvb && i < 4096; ++i) {
                uint32_t d = arr + 12 * i;
                if (!v.isLocal(d) || !v.targetOf(d + 4)) continue;
                uint32_t hdr = v.local(d);          // points at the record's object field
                uint32_t obj = v.u32(hdr);
                Vb b{v.u32(d + 4), v.u32(d + 8), v.u32(hdr - 4), *v.targetOf(d + 4), d, hdr};
                vbs[obj] = b;
                vbs[hdr] = b;
            }
        }
        if (nib && v.isLocal(res + 48)) {
            uint32_t arr = v.local(res + 48);
            for (uint32_t i = 0; i < nib && i < 4096; ++i) {
                uint32_t d = arr + 16 * i;
                if (!v.isLocal(d) || !v.targetOf(d + 4)) continue;
                ibs[v.local(d)] = Ib{v.u32(d + 4), v.u32(d + 8), v.u32(d + 12), *v.targetOf(d + 4), d};
            }
        }
    }

    // ---- render list: groups of draws
    auto rl = chunk(v, 0x0A);
    std::map<size_t, Bytes> gpuCache;
    auto gpuData = [&](size_t sym) -> const Bytes& {
        auto it = gpuCache.find(sym);
        if (it == gpuCache.end()) it = gpuCache.emplace(sym, c.symbolData(sym)).first;
        return it->second;
    };
    // Each vertex buffer is decoded once; meshes of different groups copy from here.
    std::map<uint32_t, Mesh> decoded;
    if (rl && v.isLocal(*rl)) {
        uint32_t groups = v.local(*rl);
        uint32_t ngroups = std::min<uint32_t>(v.u32(*rl + 4), 4096);
        m.groups.resize(ngroups);
        for (uint32_t gi = 0; gi < ngroups; ++gi) {
            uint32_t g = groups + 16 * gi;
            uint32_t ndraws = v.u32(g + 8);
            uint32_t draws = v.isLocal(g + 12) ? v.local(g + 12) : 0;
            // The group's node path: every listed node must be active for it to draw.
            uint32_t npath = std::min<uint32_t>(v.u32(g), 64);
            if (npath && v.isLocal(g + 4)) {
                uint32_t pl = v.local(g + 4);
                for (uint32_t k = 0; k < npath; ++k) {
                    uint32_t node = v.u32(pl + 4 * k);
                    if (node < m.nodes.size()) m.groups[gi].path.push_back(int(node));
                }
            }
            for (uint32_t i = 0; draws && i < ndraws && i < 100000; ++i) {
                uint32_t d = draws + 32 * i;
                if (!v.isLocal(d) || !v.isLocal(d + 28)) continue;
                uint32_t binding = v.local(d);
                uint32_t vbObj = v.isLocal(binding) ? v.local(binding) : v.u32(binding);
                uint32_t stride = v.u32(binding + 4);
                uint32_t pair = v.local(d + 28);
                uint32_t ibObj = v.isLocal(pair) ? v.local(pair) : v.u32(pair);
                uint32_t nidx = v.u32(pair + 4);
                auto vb = vbs.find(vbObj);
                auto ib = ibs.find(ibObj);
                if (vb == vbs.end() || ib == ibs.end()) {
                    m.warnings.push_back("group " + std::to_string(gi) + " draw " + std::to_string(i) + ": buffer not found");
                    continue;
                }
                if (stride == 0) stride = vb->second.stride;
                const Bytes& ig = gpuData(ib->second.gpuSym);
                auto dec = decoded.find(vbObj);
                if (dec == decoded.end()) {
                    const Bytes& vg = gpuData(vb->second.gpuSym);
                    Mesh mesh;
                    mesh.stride = stride;
                    mesh.buffer = vbObj;
                    uint32_t nverts = stride ? vb->second.size / stride : 0;
                    if (uint64_t(vb->second.gpu) + uint64_t(nverts) * stride > vg.size()) {
                        m.warnings.push_back("vertex buffer exceeds GPU data");
                        nverts = 0;
                    }
                    if (nverts) {
                        const uint8_t* base = &vg[vb->second.gpu];
                        if (uint64_t(vb->second.gpu) + vb->second.size <= vg.size())
                            nverts = vertexBytes(base, vb->second.size, stride) / stride;
                        mesh.layout = detectLayout(base, stride, nverts);
                        for (uint32_t k = 0; k < nverts; ++k) mesh.vertices.push_back(decodeVertex(base + size_t(k) * stride, mesh.layout));
                    }
                    dec = decoded.emplace(vbObj, std::move(mesh)).first;
                }
                bool wide = ib->second.format != 1 && ib->second.format != 0;
                uint32_t isz = wide ? 4 : 2;
                if (uint64_t(ib->second.gpu) + uint64_t(nidx) * isz > ig.size()) {
                    m.warnings.push_back("index buffer exceeds GPU data");
                    continue;
                }
                // One mesh per draw, holding only the vertices it references.
                const Mesh& src = dec->second;
                Mesh mesh;
                mesh.stride = src.stride;
                mesh.layout = src.layout;
                mesh.buffer = vbObj;
                mesh.group = int(gi);
                auto& so = mesh.source;
                so.vbSymbol = int(vb->second.gpuSym);
                so.vbOffset = vb->second.gpu;
                so.vbCount = uint32_t(src.vertices.size());
                so.vbRecord = vb->second.record;
                so.vbHeader = vb->second.header;
                so.ibSymbol = int(ib->second.gpuSym);
                so.ibOffset = ib->second.gpu;
                so.ibRecord = ib->second.record;
                so.wideIndices = wide;
                so.indexCountAt = pair + 4;
                so.groupRecord = g;
                so.drawRecord = d;
                std::vector<int32_t> remap(src.vertices.size(), -1);
                for (uint32_t k = 0; k + 2 < nidx; k += 3) {
                    uint32_t tri[3];
                    for (int t = 0; t < 3; ++t) {
                        size_t at = ib->second.gpu + size_t(k + uint32_t(t)) * isz;
                        tri[t] = wide ? be32(&ig[at]) : be16(&ig[at]);
                    }
                    if (tri[0] >= src.vertices.size() || tri[1] >= src.vertices.size() || tri[2] >= src.vertices.size()) continue;
                    for (uint32_t x : tri) {
                        if (remap[x] < 0) {
                            remap[x] = int32_t(mesh.vertices.size());
                            mesh.vertices.push_back(src.vertices[x]);
                            so.vertex.push_back(x);
                        }
                        mesh.indices.push_back(uint32_t(remap[x]));
                    }
                }
                // Sampler bindings: {u32 count, ->program in .stream, then {u32 slot, u16 N+1, u16 N, u32, u32}}
                // where N is the texturefileN of the bound texture.
                std::vector<int> slots; // sampler slot of each entry in mesh.textures
                if (v.isLocal(d + 24)) {
                    uint32_t b = v.local(d + 24);
                    if (auto t = v.targetOf(b + 4); t && *t != v.sym()) {
                        so.programSymbol = int(*t);
                        so.programAt = v.u32(b + 4);
                    }
                    uint32_t nb = std::min<uint32_t>(v.u32(b), 16);
                    for (uint32_t k = 0; k < nb && b + 8 + 16 * k + 8 <= v.size(); ++k) {
                        auto t = textureOfFile.find(int(v.u32(b + 8 + 16 * k + 4) & 0xFFFF));
                        if (t == textureOfFile.end()) continue;
                        mesh.textures.push_back(t->second);
                        slots.push_back(int(v.u32(b + 8 + 16 * k)));
                        so.samplers.push_back(b + 8 + 16 * k);
                    }
                }
                mesh.texture = colourMap(m.textures, mesh.textures);
                mesh.textureSlots = slots;
                // Material constants: field 5 lists register blocks {->.stream values, u32 count, u32}
                // at +0xd4; the material's parameters take the registers in order.
                std::vector<std::array<float, 4>> regs;
                std::vector<std::pair<int, uint32_t>> regAt; // symbol and offset of each register
                if (v.isLocal(d + 20)) {
                    uint32_t f5 = v.local(d + 20);
                    for (uint32_t k = 0; k < 8 && f5 + 0xd4 + 12 * k + 8 <= v.size(); ++k) {
                        uint32_t e = f5 + 0xd4 + 12 * k;
                        auto t = v.targetOf(e);
                        if (!t || *t == v.sym()) break;
                        const Bytes& sd = v.bytes(*t);
                        uint32_t at = v.u32(e), cnt = std::min<uint32_t>(v.u32(e + 4), 64);
                        if (uint64_t(at) + uint64_t(cnt) * 16 > sd.size()) break;
                        for (uint32_t r = 0; r < cnt; ++r) {
                            std::array<float, 4> x;
                            for (int q = 0; q < 4; ++q) {
                                uint32_t bits = be32(&sd[at + 16 * r + 4 * uint32_t(q)]);
                                std::memcpy(&x[size_t(q)], &bits, 4);
                            }
                            regs.push_back(x);
                            regAt.push_back({int(*t), at + 16 * r});
                        }
                    }
                }
                // The material: textured draws name its program (first word of their program block);
                // otherwise take the materials with as many parameters and use them if they agree.
                const Material* mat = nullptr;
                bool ambiguous = false;
                if (v.isLocal(d + 24)) {
                    uint32_t b = v.local(d + 24);
                    if (v.targetOf(b + 4) && *v.targetOf(b + 4) != v.sym()) { // untextured draws (count 0) name their program too
                        const Bytes& sd = v.bytes(*v.targetOf(b + 4));
                        uint32_t at = v.u32(b + 4);
                        if (at + 4 <= sd.size())
                            for (auto& [blk, mt] : materials)
                                if (std::find(mt.programs.begin(), mt.programs.end(), be32(&sd[at])) != mt.programs.end()) mat = &mt;
                    }
                }
                if (!mat && !regs.empty())
                    for (auto& [blk, mt] : materials) {
                        if (mt.params.size() != regs.size()) continue;
                        if (mat && mat->params != mt.params) ambiguous = true;
                        mat = &mt;
                    }
                if (mat && mat->params.size() == regs.size()) {
                    if (!ambiguous) mesh.material = mat->name;
                    for (size_t k = 0; k < regs.size(); ++k) mesh.constants.push_back({mat->params[k], regs[k], regAt[k].first, regAt[k].second});
                    // The colour map's sampler slot k scales by c:textureGain<k> and adds c:textureOffset<k>.
                    int slot = -1;
                    for (size_t k = 0; k < mesh.textures.size(); ++k)
                        if (mesh.textures[k] == mesh.texture) slot = slots[k];
                    std::string gain = "c:textureGain" + std::to_string(slot), offset = "c:textureOffset" + std::to_string(slot);
                    for (auto& cst : mesh.constants) {
                        if (cst.name == "c:basecolour") {
                            std::copy(cst.value.begin(), cst.value.end(), mesh.colour);
                            mesh.hasColour = true;
                        }
                        if (cst.name == "c:specularColour") std::copy(cst.value.begin(), cst.value.begin() + 3, mesh.specular);
                        if (cst.name == "c:specularPower") mesh.specularPower = cst.value[0];
                        if (slot >= 0 && cst.name == gain) std::copy(cst.value.begin(), cst.value.end(), mesh.textureGain);
                        if (slot >= 0 && cst.name == offset) std::copy(cst.value.begin(), cst.value.end(), mesh.textureOffset);
                    }
                } else {
                    // Unknown material: keep the registers unnamed.
                    for (size_t k = 0; k < regs.size(); ++k) mesh.constants.push_back({"c" + std::to_string(k), regs[k], regAt[k].first, regAt[k].second});
                }
                if (mesh.indices.empty()) continue;
                m.groups[gi].triangles += mesh.indices.size() / 3;
                m.groups[gi].meshes.push_back(m.meshes.size());
                m.meshes.push_back(std::move(mesh));
            }
        }
    }
    if (m.meshes.empty())
        m.warnings.push_back(m.groups.empty() ? "no draw groups: an invisible helper (collision, rotator...) or geometry the game builds at runtime"
                                              : "no geometry found");

    // Default visibility: the most detailed level with every switch in its first state.
    // (A few models leave level 0 empty; then the first level with geometry is used.)
    if (!m.nodes.empty()) {
        std::map<int, int> state;
        for (auto& [id, states] : m.switches) state[id] = states.front();
        for (int lod = 0; lod < m.lodCount; ++lod) {
            auto shown = visibleGroups(m, lod, state);
            bool any = false;
            for (size_t i = 0; i < m.groups.size(); ++i) any |= shown[i] && !m.groups[i].meshes.empty();
            if (!any && lod + 1 < m.lodCount) continue;
            for (size_t i = 0; i < m.groups.size(); ++i) m.groups[i].shown = shown[i];
            m.defaultLod = lod;
            break;
        }
    } else {
        applyBufferHeuristic(m);
    }

    // Bounds of the shown geometry.
    computeBounds(m);
    return m;
}

std::vector<bool> visibleGroups(const Model& m, int lod, const std::map<int, int>& switchState) {
    std::vector<bool> out(m.groups.size(), true);
    for (size_t i = 0; i < m.groups.size(); ++i)
        for (int n : m.groups[i].path) {
            const Node& nd = m.nodes[size_t(n)];
            if (nd.lods && !(nd.lods >> lod & 1)) out[i] = false;
            if (nd.switchId >= 0) {
                auto it = switchState.find(nd.switchId);
                if (it != switchState.end() && it->second != nd.switchState) out[i] = false;
            }
        }
    return out;
}

void computeBounds(Model& m) {
    bool first = true;
    for (auto& mesh : m.meshes) {
        if (!m.groups[size_t(mesh.group)].shown) continue;
        for (auto& vx : mesh.vertices)
            for (int k = 0; k < 3; ++k) {
                if (first || vx.pos[k] < m.boundsMin[k]) m.boundsMin[k] = vx.pos[k];
                if (first || vx.pos[k] > m.boundsMax[k]) m.boundsMax[k] = vx.pos[k];
                if (k == 2) first = false;
            }
    }
}

// Without node names: groups sharing a vertex buffer are probably alternatives of one part,
// so show the one with the most triangles from each connected set.
void applyBufferHeuristic(Model& m) {
    std::vector<size_t> setOf(m.groups.size());
    for (size_t i = 0; i < setOf.size(); ++i) setOf[i] = i;
    std::function<size_t(size_t)> find = [&](size_t x) { return setOf[x] == x ? x : setOf[x] = find(setOf[x]); };
    std::map<uint32_t, size_t> groupOfBuffer;
    for (auto& mesh : m.meshes) {
        auto [it, fresh] = groupOfBuffer.emplace(mesh.buffer, size_t(mesh.group));
        if (!fresh) setOf[find(size_t(mesh.group))] = find(it->second);
    }
    std::map<size_t, size_t> best; // set -> group
    for (size_t i = 0; i < m.groups.size(); ++i) {
        if (m.groups[i].meshes.empty()) continue;
        auto [it, fresh] = best.emplace(find(i), i);
        if (!fresh && m.groups[i].triangles > m.groups[it->second].triangles) it->second = i;
    }
    for (auto& [set, g] : best) m.groups[g].shown = true;
}

std::string toObj(const Model& m, const std::string& mtlFile, const std::vector<int>& meshTexture, const std::vector<bool>& include) {
    std::string o = "# " + m.name + " (exported by nuttool)\nmtllib " + mtlFile + "\n";
    size_t base = 1;
    char t[160];
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        const Mesh& mesh = m.meshes[i];
        bool use = i < include.size() ? include[i] : size_t(mesh.group) < m.groups.size() && m.groups[size_t(mesh.group)].shown;
        if (!use) continue;
        o += "o group" + std::to_string(mesh.group) + "_mesh" + std::to_string(i) + "\n";
        for (auto& v : mesh.vertices) { std::snprintf(t, sizeof t, "v %g %g %g\n", v.pos[0], v.pos[1], v.pos[2]); o += t; }
        for (auto& v : mesh.vertices) { std::snprintf(t, sizeof t, "vt %.9g %.9g\n", v.uv[0], 1.0f - v.uv[1]); o += t; }
        for (auto& v : mesh.vertices) { std::snprintf(t, sizeof t, "vn %g %g %g\n", v.normal[0], v.normal[1], v.normal[2]); o += t; }
        int tex = i < meshTexture.size() ? meshTexture[i] : mesh.texture;
        if (tex >= 0) o += "usemtl tex" + std::to_string(tex) + "\n";
        for (size_t k = 0; k + 2 < mesh.indices.size(); k += 3) {
            size_t a = base + mesh.indices[k], b = base + mesh.indices[k + 1], c = base + mesh.indices[k + 2];
            std::snprintf(t, sizeof t, "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n", a, a, a, b, b, b, c, c, c);
            o += t;
        }
        base += mesh.vertices.size();
    }
    return o;
}

// ---- animations ---------------------------------------------------------------------

namespace {

// Parameters of a BITSTREAM32 / QUAT_BITSTREAM32 track block (decoder at 0x8220A6A8 in the game):
//   +4 ->f32 scale[channelsPerRecord]   +8 ->f32 default[channelsPerRecord]   +12 ->u8 masks[]
//   +16 ->data: per present channel a base value (s32, or s16 when +38 is 0), then the frames
//   +20 ->bit widths (bytes, or low-first nibbles), stored minus one
//   +24 u16, +26 u16: bytes before the first frame; +28 u16 bytes per frame
//   +30 u16 channels per record, +32 u16 floats per record, +34 u16 records, +38 u8 wide
// Per record, groups of 9 channels: a mask byte covers 3 channels (bits 0x80/0x40/0x20 present,
// 0x10/0x08/0x04 constant) and its low bits say which of the next two triples follow (1: the
// third, 2: the second, 3: both, each with its own mask byte). Absent channels take the default,
// constant ones base * scale, animated ones (base + bits) * scale with the bits read LSB-first
// from big-endian words. Quaternion tracks lay a record out as {x y z w tx ty tz - sx sy sz -}
// with w = sqrt(1 - x^2 - y^2 - z^2).
struct Track {
    bool ok = false, quat = false, wide = true;
    uint32_t channels = 0, floats = 0, records = 0;
    uint32_t start = 0, frameStride = 0;
    uint32_t masks = 0, data = 0, widths = 0;
    bool hasData = false, hasWidths = false;
    std::vector<float> scale, def;
};

Track readTrack(const View& v, uint32_t block, bool quat) {
    Track t;
    t.quat = quat;
    if (!v.isLocal(block + 24)) return t;
    uint32_t p = v.local(block + 24);
    t.channels = v.u16(p + 30);
    t.floats = v.u16(p + 32);
    t.records = v.u16(p + 34);
    t.wide = v.data()[p + 38] != 0;
    t.start = uint32_t(v.u16(p + 24)) + v.u16(p + 26);
    t.frameStride = v.u16(p + 28);
    if (!t.channels || t.channels > 64 || !t.floats || t.floats > 64 || t.records > 4096) return t;
    if (!v.isLocal(p + 4) || !v.isLocal(p + 8) || !v.isLocal(p + 12)) return t;
    uint32_t sc = v.local(p + 4), df = v.local(p + 8);
    for (uint32_t k = 0; k < t.channels; ++k) {
        t.scale.push_back(v.f32(sc + 4 * k));
        t.def.push_back(v.f32(df + 4 * k));
    }
    t.masks = v.local(p + 12);
    t.hasData = v.isLocal(p + 16);
    if (t.hasData) t.data = v.local(p + 16);
    t.hasWidths = v.isLocal(p + 20);
    if (t.hasWidths) t.widths = v.local(p + 20);
    t.ok = true;
    return t;
}

// Decodes one frame of a track into out[records * floats].
void decodeTrackFrame(const View& v, const Track& t, uint32_t frame, float* out) {
    const Bytes& d = v.data();
    uint32_t mask = t.masks, base = t.data, width = t.widths;
    bool highNibble = false;
    // Bit reader over big-endian words, LSB first.
    uint32_t addr = t.data + t.start + t.frameStride * frame;
    uint32_t word = addr & ~3u, shift = (addr & 3) * 8;
    auto be32At = [&](uint32_t o) -> uint32_t { return o + 4 <= d.size() ? be32(&d[o]) : 0; };
    auto readBits = [&](uint32_t n) -> uint32_t {
        uint64_t both = (uint64_t(be32At(word + 4)) << 32) | be32At(word);
        uint32_t value = uint32_t(both >> shift) & (n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1));
        shift += n;
        word += (shift >> 5) * 4;
        shift &= 31;
        return value;
    };
    auto nextBase = [&]() -> int32_t {
        if (!t.hasData) return 0;
        int32_t b = t.wide ? int32_t(be32At(base)) : int16_t(base + 2 <= d.size() ? be16(&d[base]) : 0);
        base += t.wide ? 4 : 2;
        return b;
    };
    auto nextWidth = [&]() -> uint32_t {
        if (!t.hasWidths || width >= d.size()) return 0;
        uint32_t w;
        if (t.wide) {
            w = d[width++];
        } else {
            w = (d[width] >> (highNibble ? 4 : 0)) & 0xF;
            if (highNibble) ++width;
            highNibble = !highNibble;
        }
        return w + 1;
    };
    for (uint32_t r = 0; r < t.records; ++r) {
        float* rec = out + size_t(r) * t.floats;
        auto put = [&](uint32_t slot, float x) {
            if (slot < t.floats) rec[slot] = x;
        };
        // Output slot of channel ch (quaternion records insert w after the first triple and pad).
        auto slotOf = [&](uint32_t ch) -> uint32_t {
            if (!t.quat) return ch;
            if (ch < 3) return ch;
            if (ch < 6) return ch + 1;
            if (ch < 9) return ch + 2;
            return ch + 2;
        };
        auto triple = [&](uint32_t first, uint8_t m) {
            for (uint32_t k = 0; k < 3; ++k) {
                uint32_t ch = first + k;
                if (ch >= t.channels) break;
                uint8_t present = uint8_t(0x80 >> k), constant = uint8_t(0x10 >> k);
                float x;
                if (!(m & present)) {
                    x = t.def[ch];
                } else if (m & constant) {
                    x = float(nextBase()) * t.scale[ch];
                } else {
                    int32_t b = nextBase();
                    uint32_t w = nextWidth();
                    x = float(int64_t(readBits(w)) + b) * t.scale[ch];
                }
                put(slotOf(ch), x);
            }
        };
        auto absent = [&](uint32_t first) {
            for (uint32_t k = 0; k < 3 && first + k < t.channels; ++k) put(slotOf(first + k), t.def[first + k]);
        };
        for (uint32_t g = 0; g < t.channels; g += 9) {
            uint8_t m0 = mask < d.size() ? d[mask] : 0;
            triple(g, m0);
            switch (m0 & 3) {
                case 0:
                    absent(g + 3);
                    absent(g + 6);
                    break;
                case 1:
                    absent(g + 3);
                    triple(g + 6, d[++mask]);
                    break;
                case 2:
                    triple(g + 3, d[++mask]);
                    absent(g + 6);
                    break;
                default:
                    triple(g + 3, d[++mask]);
                    triple(g + 6, d[++mask]);
                    break;
            }
            ++mask;
        }
        if (t.quat && t.floats >= 4) {
            float s = 1 - rec[0] * rec[0] - rec[1] * rec[1] - rec[2] * rec[2];
            rec[3] = s > 0 ? std::sqrt(s) : 0;
        }
    }
}

} // namespace

bool isAnimationSymbol(const caff::Caff& c, size_t symbol) {
    const auto& s = c.symbols.at(symbol);
    if (s.section == 0 || s.section > c.sections.size() || c.sections[s.section - 1].name != ".data" || s.size < 0x60) return false;
    if (!startsWith(assetdb::baseAssetName(c.symbolName(symbol)), "aid_anim_")) return false;
    const auto& sec = c.sections[s.section - 1].data;
    return s.offset + 10 <= sec.size() && std::memcmp(&sec[s.offset], "animation", 9) == 0;
}

Animation parseAnimation(const caff::Caff& c, size_t symbol) {
    Animation a;
    a.name = assetdb::baseAssetName(c.symbolName(symbol));
    View v(c, symbol);
    if (v.size() < 0x60 || std::memcmp(v.data().data(), "animation", 9) != 0) fail("not an animation: " + a.name);
    if (std::memcmp(&v.data()[10], "19.12.06.0036", 13) != 0) a.warnings.push_back("unexpected animation version");
    a.duration = v.f32(0x1c);
    uint32_t bones = v.u16(0x20);
    // +0x22 counts authored frames; +0x5C is the fraction of them stored (0.5 on most
    // animations). The game (0x8220FD00) keeps ceil((N - 1) * s + 1) frames, one every 1/s
    // authored frames, and the last stored frame is always the final authored frame.
    uint32_t authored = v.u16(0x22);
    float keep = v.f32(0x5c);
    if (!(keep > 0 && keep <= 1)) keep = 1;
    a.authoredFrames = int(authored);
    a.frames = authored > 1 ? int(std::ceil(float(authored - 1) * keep + 1.0f - 1e-4f)) : int(authored);
    uint32_t step = uint32_t(std::lround(1.0 / keep));
    for (int f = 0; f < a.frames; ++f) {
        uint32_t at = f + 1 == a.frames ? authored - 1 : std::min(authored - 1, uint32_t(f) * step);
        a.frameTimes.push_back(authored > 1 ? a.duration * float(at) / float(authored - 1) : 0.0f);
    }
    if (v.isLocal(0x48)) {
        a.pose = readPose(v, v.local(0x48));
        for (auto& b : a.pose) {
            a.boneNames.push_back(b.name);
            a.parents.push_back(b.parent);
        }
    }
    if (a.boneNames.size() != bones) a.warnings.push_back("pose has " + std::to_string(a.boneNames.size()) + " bones, header says " + std::to_string(bones));
    a.bones = uint32_t(a.boneNames.size());
    // Rotation/translation/scale track.
    Track t;
    if (v.isLocal(0x24)) {
        uint32_t block = v.local(0x24);
        std::string codec(reinterpret_cast<const char*>(&v.data()[block]), strnlen(reinterpret_cast<const char*>(&v.data()[block]), 24));
        a.codec = codec;
        t = readTrack(v, block, startsWith(codec, "QUAT"));
        if (!startsWith(codec, "QUAT_BITSTREAM")) a.warnings.push_back("unsupported rotation codec " + codec);
    }
    if (!t.ok || !t.quat || t.floats < 11) fail("animation " + a.name + ": no decodable rotation track");
    if (t.records != a.bones) a.warnings.push_back("track has " + std::to_string(t.records) + " records for " + std::to_string(a.bones) + " bones");
    uint32_t n = std::min(t.records, a.bones);
    std::vector<float> buf(size_t(t.records) * t.floats);
    a.keys.resize(size_t(a.frames) * a.bones);
    for (uint32_t f = 0; f < uint32_t(a.frames); ++f) {
        decodeTrackFrame(v, t, f, buf.data());
        for (uint32_t b = 0; b < n; ++b) {
            const float* r = &buf[size_t(b) * t.floats];
            Key& k = a.keys[size_t(f) * a.bones + b];
            std::copy(r, r + 4, k.rotation);
            std::copy(r + 4, r + 7, k.translation);
            std::copy(r + 8, r + 11, k.scale);
        }
    }
    return a;
}

void writeAnimationKeys(caff::Caff& c, size_t symbol, const std::vector<Key>& keys) {
    const Animation a = parseAnimation(c, symbol);
    View v(c, symbol);
    uint32_t block = v.local(0x24);
    Track t = readTrack(v, block, true);
    uint32_t p = v.local(block + 24);
    if (!t.ok || t.channels != 9 || t.floats < 11 || t.records != a.bones) fail("animation " + a.name + ": unexpected rotation track layout");
    if (t.wide) fail("animation " + a.name + ": wide rotation tracks are not supported yet");
    if (!t.hasData || !t.hasWidths) fail("animation " + a.name + ": its rotation track has no data to replace");
    if (v.u16(p + 36) != uint32_t(a.frames)) fail("animation " + a.name + ": track frame count differs from the header's");
    if (keys.size() != size_t(a.frames) * a.bones) fail("animation " + a.name + ": wrong number of keys");
    const EncodedTrack e = encodeTrack(a.name, keys, uint32_t(a.frames), t.records, t.scale, t.def);

    Bytes d = c.symbolData(symbol);
    auto append = [&](const Bytes& b) {
        uint32_t at = uint32_t((d.size() + 15) & ~size_t(15));
        d.resize(at, 0);
        d.insert(d.end(), b.begin(), b.end());
        return at;
    };
    uint32_t maskAt = append(e.masks);
    Bytes body = e.bases;
    body.insert(body.end(), e.widths.begin(), e.widths.end());
    body.insert(body.end(), e.frames.begin(), e.frames.end());
    uint32_t dataAt = append(body);
    putBe32(&d[p + 12], maskAt);
    putBe32(&d[p + 16], dataAt);
    putBe32(&d[p + 20], dataAt + uint32_t(e.bases.size()));
    putBe16(&d[p + 24], uint16_t(e.bases.size()));
    putBe16(&d[p + 26], uint16_t(e.widths.size()));
    putBe16(&d[p + 28], uint16_t(e.stride));
    c.replaceSymbolData(symbol, d);
}

EncodedTrack encodeTrack(const std::string& name, const std::vector<Key>& keys, uint32_t nf, uint32_t records,
                         const std::vector<float>& scale, const std::vector<float>& def) {
    if (keys.size() != size_t(nf) * records || scale.size() < 9 || def.size() < 9) fail("animation " + name + ": wrong number of keys");
    // Channel values per record, quantised by the track's scales: x y z (w >= 0, the decoder rebuilds it),
    // translation, scale. Bases are 16-bit and widths at most 16 bits.
    auto quantise = [&](uint32_t ch, float x) {
        double q = std::round(double(x) / double(scale[ch]));
        return int32_t(std::clamp(q, -32768.0, 32767.0));
    };
    struct Channel {
        int kind = 0; // 0 absent, 1 constant, 2 animated
        int32_t base = 0;
        uint32_t width = 0;
        std::vector<uint32_t> bits;
    };
    Bytes masks, bases, widths;
    std::vector<Channel> animated;
    bool clamped = false;
    for (uint32_t r = 0; r < records; ++r) {
        Channel chs[9];
        for (uint32_t ch = 0; ch < 9; ++ch) {
            std::vector<int32_t> q(nf);
            for (uint32_t f = 0; f < nf; ++f) {
                const Key& k = keys[size_t(f) * records + r];
                float sign = k.rotation[3] < 0 ? -1.0f : 1.0f;
                float x = ch < 3 ? k.rotation[ch] * sign : ch < 6 ? k.translation[ch - 3] : k.scale[ch - 6];
                // A value at the channel's default isn't stored (narrow scales can't hold 1.0 itself).
                if (x != def[ch] && std::fabs(double(x) / double(scale[ch])) > 32767.5) clamped = true;
                q[f] = quantise(ch, x);
            }
            auto [lo, hi] = std::minmax_element(q.begin(), q.end());
            Channel& out = chs[ch];
            if (*lo == *hi) {
                out.kind = *lo == quantise(ch, def[ch]) ? 0 : 1;
                out.base = *lo;
                continue;
            }
            out.kind = 2;
            out.base = *lo;
            uint32_t range = uint32_t(*hi - *lo);
            while (out.width < 16 && (range >> out.width)) ++out.width;
            for (int32_t x : q) out.bits.push_back(uint32_t(x - *lo));
        }
        uint8_t tm[3] = {};
        for (int g = 0; g < 3; ++g)
            for (int k = 0; k < 3; ++k) {
                const Channel& ch = chs[g * 3 + k];
                if (ch.kind) tm[g] |= uint8_t(0x80 >> k);
                if (ch.kind == 1) tm[g] |= uint8_t(0x10 >> k);
            }
        masks.push_back(uint8_t(tm[0] | (tm[1] ? 2 : 0) | (tm[2] ? 1 : 0)));
        if (tm[1]) masks.push_back(tm[1]);
        if (tm[2]) masks.push_back(tm[2]);
        // Bases and widths in the decoder's order: rotation, then translation, then scale.
        for (int g = 0; g < 3; ++g) {
            if (!tm[g]) continue;
            for (int k = 0; k < 3; ++k) {
                Channel& ch = chs[g * 3 + k];
                if (!ch.kind) continue;
                bases.push_back(uint8_t(uint16_t(ch.base) >> 8));
                bases.push_back(uint8_t(ch.base));
                if (ch.kind == 2) animated.push_back(std::move(ch));
            }
        }
    }
    if (clamped) fail("animation " + name + ": a translation is more than 1 m from its bone's rest position, which this track cannot store");
    // Widths: low nibble first, stored minus one.
    for (size_t k = 0; k < animated.size(); ++k) {
        uint8_t w = uint8_t((std::max(1u, animated[k].width) - 1) & 0xF);
        if (k % 2 == 0) widths.push_back(w);
        else widths.back() |= uint8_t(w << 4);
    }
    while (bases.size() % 4) bases.push_back(0);
    while (widths.size() % 4) widths.push_back(0);
    uint32_t bitsPerFrame = 0;
    for (auto& ch : animated) bitsPerFrame += std::max(1u, ch.width);
    uint32_t stride = (bitsPerFrame + 31) / 32 * 4;
    if (bases.size() + widths.size() > 0xFFFF || stride > 0xFFFF) fail("animation " + name + ": too much motion for one track");
    // Frames: each starts on a word; values LSB first within big-endian words.
    Bytes frames(size_t(stride) * nf + 8, 0);
    for (uint32_t f = 0; f < nf; ++f) {
        std::vector<uint32_t> words(stride / 4 + 1, 0);
        uint32_t pos = 0;
        for (auto& ch : animated) {
            uint32_t w = std::max(1u, ch.width), x = ch.bits[f];
            for (uint32_t b = 0; b < w; ++b, ++pos)
                if ((x >> b) & 1) words[pos / 32] |= 1u << (pos % 32);
        }
        for (uint32_t k = 0; k < stride / 4; ++k) putBe32(&frames[size_t(f) * stride + 4 * k], words[k]);
    }
    return {std::move(masks), std::move(bases), std::move(widths), std::move(frames), stride};
}

std::string toMtl(const Model& m) {
    std::string o;
    for (size_t i = 0; i < m.textures.size(); ++i)
        o += "newmtl tex" + std::to_string(i) + "\nKd 1 1 1\nmap_Kd " + m.textures[i] + ".png\n\n";
    return o;
}

} // namespace nut::model
