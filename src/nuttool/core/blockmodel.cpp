#include "blockmodel.hpp"

#include "caff.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace nut::blockmodel {

uint32_t detail::elfHash(const std::string& s) {
    uint32_t h = 0;
    for (unsigned char c : s) {
        h = (h << 4) + c;
        if (uint32_t g = h & 0xF0000000u) h ^= g >> 24, h &= ~g;
    }
    return h;
}

namespace {

using detail::elfHash;
using detail::Sym;

// The model's symbols (1-based, as relocations name them): the rendergraph, the GPU data
// (vertices, indices, the shaders' physical parts) and the render program.
constexpr uint32_t kData = 1, kGpu = 2, kStream = 3;

uint16_t half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int exponent = int((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = x & 0x7FFFFFu;
    if (exponent <= 0) return uint16_t(sign);                 // too small: zero
    if (exponent >= 31) return uint16_t(sign | 0x7BFFu);      // too large: the largest
    mantissa += 0x1000u;                                       // round to nearest
    if (mantissa & 0x800000u) return uint16_t(sign | uint32_t(std::min(exponent + 1, 30)) << 10);
    return uint16_t(sign | uint32_t(exponent) << 10 | mantissa >> 13);
}

// 10:10:10 signed normalised, x in the low bits (as N&B's normals are stored).
uint32_t dec3n(const std::array<float, 3>& v) {
    uint32_t out = 0;
    for (int k = 0; k < 3; ++k) {
        int c = int(std::lround(std::clamp(v[size_t(k)], -1.0f, 1.0f) * 511.0f));
        out |= uint32_t(c & 0x3FF) << (10 * k);
    }
    return out;
}

// A table {count, ->entries, ->hashes x2}: entries {->key, ->key, value or, for `texts`, ->string}
// or {->name, 0, index} (`indices`, the shader styles), and its hash table {hash, 0, ->entry} by hash.
uint32_t dictionary(Sym& d, const std::vector<std::pair<std::string, uint32_t>>& items, bool indices,
                    const std::vector<std::pair<std::string, std::string>>& texts = {}) {
    const size_t count = items.size() + texts.size();
    auto key = [&](size_t i) -> const std::string& { return i < items.size() ? items[i].first : texts[i - items.size()].first; };
    std::vector<uint32_t> keys, values;
    for (size_t i = 0; i < count; ++i) {
        keys.push_back(d.pos());
        d.str(key(i));
    }
    for (auto& [k, v] : texts) {
        values.push_back(d.pos());
        d.str(v);
    }
    d.align(4);
    const uint32_t entries = d.pos();
    for (size_t i = 0; i < count; ++i) {
        d.ptr(kData, keys[i]);
        if (indices) d.u32(0);
        else d.ptr(kData, keys[i]);
        if (i < items.size()) d.u32(items[i].second);
        else d.ptr(kData, values[i - items.size()]);
    }
    constexpr uint32_t entrySize = 12;
    std::vector<size_t> order(count);
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return elfHash(key(a)) < elfHash(key(b)); });
    uint32_t tables[2];
    for (uint32_t& t : tables) {
        t = d.pos();
        for (size_t i : order) {
            d.u32(elfHash(key(i)));
            d.u32(0);
            d.ptr(kData, entries + uint32_t(i) * entrySize);
        }
    }
    const uint32_t head = d.pos();
    d.u32(uint32_t(count));
    d.ptr(kData, entries);
    d.ptr(kData, tables[0]);
    d.ptr(kData, tables[1]);
    return head;
}

// A shader object and its container's virtual part: the object's zeroed reservation, then the
// header, an empty definition table and shader info with no vertex fetches. Returns the object.
uint32_t shader(Sym& d, bool pixel, uint32_t physicalSize) {
    d.align(16);
    const uint32_t object = d.pos();
    d.zeros(pixel ? 0x28 : 0x368);
    const uint32_t virtualSize = 0x24 + 0x14 + 0x24;
    d.u32(pixel ? 0x102A1100u : 0x102A1141u);
    d.u32(virtualSize);
    d.u32(physicalSize);
    d.u32(0);
    d.u32(0);            // no constant table
    d.u32(0x24);         // definition table
    d.u32(0x38);         // shader info
    d.u32(0);
    d.u32(0);
    d.u32(0), d.u32(0), d.u32(0), d.u32(0), d.u32(4); // definition table: none
    d.u32(0);            // shader info: microcode offset, size, flags, ...
    d.u32(physicalSize);
    for (int i = 0; i < 7; ++i) d.u32(0);             // ... no vertex fetches
    return object;
}

} // namespace

uint32_t appendShapes(Sym& g, uint32_t start, uint32_t count, uint32_t stride, const std::vector<std::vector<ShapeDelta>>& shapes,
                      const ShapeTable& table) {
    constexpr uint32_t kMaxShapes = 32;
    if (shapes.empty()) return 0;
    // The draw's shapes, in the order they first appear.
    std::vector<uint32_t> used;
    for (const auto& vertex : shapes)
        for (const ShapeDelta& d : vertex)
            if (std::find(used.begin(), used.end(), d.shape) == used.end()) used.push_back(d.shape);
    if (used.empty()) return 0;
    if (used.size() > kMaxShapes) fail("a draw has " + std::to_string(used.size()) + " shape keys; reNut blends at most 32 in one");
    for (uint32_t s : used)
        if (s >= table.names.size()) fail("a shape key's delta names shape " + std::to_string(s) + ", which the model's table doesn't have");
    g.align(16);
    const uint32_t deltaOffset = g.pos() - start;
    for (uint32_t s : used)
        for (uint32_t v = 0; v < count; ++v) {
            const ShapeDelta* found = nullptr;
            if (v < shapes.size())
                for (const ShapeDelta& d : shapes[v])
                    if (d.shape == s) found = &d;
            for (float c : found ? found->position : std::array<float, 3>{}) g.f32(c);
            g.f32(0);
            for (float c : found ? found->normal : std::array<float, 3>{}) g.f32(c);
            g.f32(0);
        }
    g.align(16);
    const uint32_t header = g.pos() - start;
    g.u32(0x524D5048);  // "RMPH"
    g.u32(count);
    g.u32(deltaOffset);
    g.u32(uint32_t(used.size()));
    g.u32(table.table);
    g.u32(stride);
    const uint32_t indexField = g.pos();
    g.u32(0), g.u32(0);  // the index data's offset and count, set once they are written
    for (uint32_t s : used) g.u32(s);
    for (uint32_t s : used) g.str(table.names[s]);
    g.align(4);
    g.u32(header);
    g.u32(0x524D5046);  // "RMPF"
    return indexField;
}

std::vector<ShapeDraw> readShapes(const Bytes& gpu) {
    std::vector<ShapeDraw> out;
    const uint8_t* b = gpu.data();
    auto halfToFloat = [](uint16_t h) {
        const int exponent = (h >> 10) & 0x1F;
        const float mantissa = float(h & 0x3FF);
        const float v = exponent == 0 ? mantissa * std::ldexp(1.0f, -24) : (1.0f + mantissa / 1024.0f) * std::ldexp(1.0f, exponent - 15);
        return h & 0x8000 ? -v : v;
    };
    auto normal = [](uint32_t w) {
        std::array<float, 3> n{};
        for (int k = 0; k < 3; ++k) {
            int c = int((w >> (10 * k)) & 0x3FF);
            if (c & 0x200) c -= 0x400;
            n[size_t(k)] = std::max(-1.0f, float(c) / 511.0f);
        }
        return n;
    };
    for (size_t p = 0; p + 8 <= gpu.size(); p += 4) {
        if (be32(b + p + 4) != 0x524D5046) continue;  // "RMPF"
        const uint32_t headerOffset = be32(b + p);
        // The header is the nearest "RMPH" before the footer whose names end at it.
        for (size_t q = p; q >= 4 && p - q < 0x10000;) {
            q -= 4;
            if (be32(b + q) != 0x524D5048 || q < headerOffset || q + 32 > p) continue;
            const uint32_t vertexCount = be32(b + q + 4), deltaOffset = be32(b + q + 8), count = be32(b + q + 12);
            const uint32_t stride = be32(b + q + 20), indexAt = be32(b + q + 24), indexCount = be32(b + q + 28);
            const size_t start = q - headerOffset;
            if (!count || count > 32 || (stride != 20 && stride != 24 && stride != 32 && stride != 40)) continue;
            if (uint64_t(vertexCount) * stride > deltaOffset || start + deltaOffset + uint64_t(count) * vertexCount * 32 > q) continue;
            if (uint64_t(indexAt) + uint64_t(indexCount) * 2 > gpu.size() || q + 32 + 4 * size_t(count) > p) continue;
            ShapeDraw d;
            d.table = be32(b + q + 16);
            size_t at = q + 32 + 4 * size_t(count);
            for (uint32_t s = 0; s < count && at < p; ++s) {
                const size_t end = std::find(b + at, b + p, uint8_t(0)) - b;
                d.names.emplace_back(reinterpret_cast<const char*>(b + at), end - at);
                at = end + 1;
            }
            if (d.names.size() != count || ((at + 3) & ~size_t(3)) != p) continue;
            for (uint32_t v = 0; v < vertexCount; ++v) {
                const uint8_t* x = b + start + size_t(v) * stride;
                if (stride == 20) {
                    d.positions.push_back({halfToFloat(be16(x)), halfToFloat(be16(x + 2)), halfToFloat(be16(x + 4))});
                    d.normals.push_back(normal(be32(x + 8)));
                    continue;
                }
                d.positions.push_back({bef32(x), bef32(x + 4), bef32(x + 8)});
                d.normals.push_back(normal(be32(x + 12)));
                if (stride >= 32) d.uvs.push_back({bef32(x + 24), bef32(x + 28)});
            }
            for (uint32_t i = 0; i < indexCount; ++i) d.indices.push_back(be16(b + indexAt + size_t(i) * 2));
            for (uint32_t s = 0; s < count; ++s) {
                std::vector<ShapeDelta> deltas(vertexCount);
                for (uint32_t v = 0; v < vertexCount; ++v) {
                    const uint8_t* x = b + start + deltaOffset + (size_t(s) * vertexCount + v) * 32;
                    deltas[v].shape = be32(b + q + 32 + 4 * size_t(s));
                    deltas[v].position = {bef32(x), bef32(x + 4), bef32(x + 8)};
                    deltas[v].normal = {bef32(x + 16), bef32(x + 20), bef32(x + 24)};
                }
                d.deltas.push_back(std::move(deltas));
            }
            out.push_back(std::move(d));
            break;
        }
    }
    return out;
}

namespace {

// A draw's material record after its vertices (and any shape keys', which end at `before`, from `start`),
// as reNut's docs/nuttool/worlds.md lays it out, ending the vertex data.
void appendMaterial(Sym& g, uint32_t start, const WorldMaterial& m) {
    const uint32_t before = g.pos() - start;
    g.align(16);
    const uint32_t header = g.pos() - start;
    g.u32(0x524D5448);  // "RMTH"
    g.u32((m.alpha & 3) | (m.doubleSided ? 4u : 0u) | (m.unlit ? 8u : 0u) | (m.emissiveTexture >= 0 ? 16u : 0u));
    g.u32(m.table);
    g.u32(m.index);
    g.f32(m.cutoff);
    g.f32(m.opacity);
    for (float c : m.emissive) g.f32(c);
    g.f32(m.strength);
    g.f32(m.glow);
    g.u32(before);
    g.u32(header);
    g.u32(0x524D5446);  // "RMTF"
}

// A part's model (`world` false: aid_model_banjox_vehicleparts_<name>) or a world's (`name` is the
// whole asset name).
Bytes build(const std::string& id, const Mesh& mesh, bool world, const ShapeTable* shapes = nullptr) {
    if (mesh.vertices.empty() || mesh.indices.size() < 3) fail("the mesh has no triangles");
    if (mesh.vertices.size() > 65535)
        fail("the mesh has " + std::to_string(mesh.vertices.size()) + " vertices, more than its 16-bit indices reach (65535)");
    const std::string name = world ? id : "aid_model_banjox_vehicleparts_" + id;
    const uint32_t kStride = world ? 24 : 20;

    // ---- GPU data: vertices (a part's: half4 position, 10:10:10 normal and tangent, half2 texcoord;
    // a world's: float3 position, 10:10:10 normal, half4 colour), 16-bit indices, then each shader's
    // physical part: reNut's signature and its kind.
    Sym g;
    for (const Vertex& v : mesh.vertices) {
        if (world) {
            for (float c : v.pos) g.f32(c);
            g.u32(dec3n(v.normal));
            for (float c : v.colour) g.u16(half(c));
            continue;
        }
        g.u16(half(v.pos[0]));
        g.u16(half(v.pos[1]));
        g.u16(half(v.pos[2]));
        g.u16(half(1.0f));
        g.u32(dec3n(v.normal));
        const std::array<float, 3> t = std::fabs(v.normal[1]) < 0.9f ? std::array<float, 3>{-v.normal[2], 0, v.normal[0]}
                                                                      : std::array<float, 3>{1, 0, 0};
        g.u32(dec3n(t));
        g.u32(0);
    }
    const uint32_t indexField = shapes ? appendShapes(g, 0, uint32_t(mesh.vertices.size()), kStride, mesh.shapes, *shapes) : 0;
    const uint32_t vertexBytes = g.pos();
    g.align(32);
    const uint32_t indexAt = g.pos();
    for (uint32_t i : mesh.indices) g.u16(uint16_t(i));
    const uint32_t indexBytes = g.pos() - indexAt;
    if (indexField) g.set(indexField, indexAt), g.set(indexField + 4, uint32_t(mesh.indices.size()));
    uint32_t physical[2];
    for (uint32_t k = 0; k < 2; ++k) {
        g.align(32);
        physical[k] = g.pos();
        g.str("RNUTPART");
        g.b.pop_back();
        g.u32((k == 0 ? 1 : 2) + (world ? 2 : 0)); // 1 vertex, 2 pixel; a world's 3 and 4
        g.zeros(20);
    }
    constexpr uint32_t kPhysicalSize = 32;

    // ---- render program: the header, three style state tables, an empty program, the name.
    Sym s;
    const uint32_t sProgram = s.ptr(kStream);
    const uint32_t sStates = s.ptr(kStream);
    s.u32(0), s.u32(0), s.u32(0);
    const uint32_t sName = s.ptr(kStream);
    s.u32(0);
    s.u32(3);
    s.u32(0);
    static const uint32_t kStates[9][2] = {{0, 0}, {4, 0}, {8, 2}, {0x20, 0}, {0x14, 1}, {0x18, 1}, {0x10, 1}, {0x1C, 0}, {0x24, 1}};
    const uint32_t pairs = s.pos();
    for (auto& p : kStates) s.u32(p[0]), s.u32(p[1]);
    s.set(sStates, s.pos());
    for (int i = 0; i < 3; ++i) {
        s.ptr(kStream, pairs);
        s.u32(9);
    }
    s.set(sProgram, s.pos());
    s.u32(0x00041D00); // end
    s.set(sName, s.pos());
    s.str(name);
    s.align(4);

    // ---- rendergraph data
    Sym d;
    const uint32_t dChunks = d.ptr(kData);
    d.u32(4);
    d.zeros(0x18);

    // Chunk 0x05: bounds. Spheres around the box's centre and its bottom's, the largest extent,
    // the radius across x and z, then the box twice.
    std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const Vertex& v : mesh.vertices)
        for (int k = 0; k < 3; ++k) lo[size_t(k)] = std::min(lo[size_t(k)], v.pos[size_t(k)]), hi[size_t(k)] = std::max(hi[size_t(k)], v.pos[size_t(k)]);
    const std::array<float, 3> c{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2}, bottom{c[0], lo[1], c[2]};
    float r = 0, rBottom = 0, rxz = 0;
    for (const Vertex& v : mesh.vertices) {
        auto dist = [&](const std::array<float, 3>& o) {
            return std::sqrt((v.pos[0] - o[0]) * (v.pos[0] - o[0]) + (v.pos[1] - o[1]) * (v.pos[1] - o[1]) + (v.pos[2] - o[2]) * (v.pos[2] - o[2]));
        };
        r = std::max(r, dist(c));
        rBottom = std::max(rBottom, dist(bottom));
        rxz = std::max(rxz, std::sqrt((v.pos[0] - c[0]) * (v.pos[0] - c[0]) + (v.pos[2] - c[2]) * (v.pos[2] - c[2])));
    }
    d.align(16);
    const uint32_t bounds = d.pos();
    d.f32(r), d.f32(c[0]), d.f32(c[1]), d.f32(c[2]);
    d.f32(rBottom), d.f32(bottom[0]), d.f32(bottom[1]), d.f32(bottom[2]);
    d.f32(std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}));
    d.f32(rxz);
    for (int twice = 0; twice < 2; ++twice) {
        for (float x : lo) d.f32(x);
        for (float x : hi) d.f32(x);
    }

    // Chunk 0x17 (0) and chunk 0x1E (-> the rendergraph).
    const uint32_t chunk17 = d.pos();
    d.u32(0);
    const uint32_t chunk1E = d.pos();
    const uint32_t toRendergraph = d.ptr(kData);
    d.u32(0);

    // The rendergraph header.
    d.align(16);
    const uint32_t rg = d.pos();
    d.str("rendergraph");
    d.str("26.09.07.0039");
    d.u16(1);
    d.u32(0);                                 // +0x1C textures
    const uint32_t rgDictionary = d.ptr(kData);
    d.zeros(0x7C - 0x24);
    const uint32_t rgTextureSlots = d.ptr(kData);
    d.u32(0);
    d.u32(0x38000000);
    d.u32(0xFFFF), d.u32(0xFFFF);
    d.u32(0);                                 // +0x90 the world x view-projection register: c0
    for (int i = 0; i < 18; ++i) d.u32(0xFFFF);
    d.u32(0);
    d.u32(0);                                 // +0xE0 lights
    const uint32_t rgLighting = d.ptr(kData);
    d.u32(0);
    d.ptr(kStream, 0);                        // +0xEC the render program
    const uint32_t rgInstance = d.ptr(kData);
    d.u32(0), d.u32(0);
    const uint32_t rgResources = d.ptr(kData);
    constexpr uint32_t kInstanceResources = 0x50; // where an instance keeps its resources' copy
    d.u32(kInstanceResources);
    const uint32_t rgPose = d.ptr(kData);
    d.u32(0x4441);
    const uint32_t rgStyles = d.ptr(kData);
    d.u32(0), d.u32(0), d.u32(0x80000002), d.u32(0x7FFFFF00), d.u32(0xC0FFEFC0), d.u32(0xF);
    d.zeros(0x148 - 0x128);
    const uint32_t rgLightProbe = d.ptr(kData);
    d.u32(0);
    d.set(toRendergraph, rg);

    // The pose: no bones.
    d.set(rgPose, d.pos());
    d.str("pose");
    d.zeros(5);
    d.str("19.12.06.0036");
    d.zeros(0x30 - 0x18);
    d.set(rgTextureSlots, d.pos());
    d.u32(0);

    // The vertex stream {stride, ->buffer object, ->data, bytes}, the buffer objects (filled at load).
    d.align(16);
    const uint32_t stream = d.pos();
    d.u32(kStride);
    const uint32_t streamBuffer = d.ptr(kData);
    d.ptr(kGpu, 0);
    d.u32(vertexBytes);
    d.align(16);
    const uint32_t vertexBuffer = d.pos();
    d.zeros(0x20);
    d.set(streamBuffer, vertexBuffer);
    const uint32_t indexBuffer = d.pos();
    d.zeros(0x20);

    // The shaders.
    const uint32_t pixelShader = shader(d, true, kPhysicalSize);
    const uint32_t vertexShader = shader(d, false, kPhysicalSize);

    // Draw field pools: stream binding, extra streams (none), vertex shader, pixel shaders (one,
    // and none for the shadow draw), bucket-3 word, constants (none), samplers (none), indices.
    d.align(16);
    const uint32_t f0 = d.pos();
    d.ptr(kData, vertexBuffer);
    d.u32(kStride);
    const uint32_t f1 = d.pos();
    d.zeros(0x90);
    const uint32_t f2 = d.pos();
    d.ptr(kData, vertexShader);
    const uint32_t f3 = d.pos();
    d.ptr(kData, pixelShader);
    d.u32(0);
    const uint32_t f4 = d.pos();
    d.u32(0);
    const uint32_t f5 = d.pos();
    d.zeros(0x190);
    const uint32_t f6 = d.pos();
    d.zeros(0x108);
    const uint32_t f7 = d.pos();
    d.ptr(kData, indexBuffer);
    d.u32(uint32_t(mesh.indices.size()));

    // The draws (8 pointers each) and their groups {path count, ->path, draw count, ->draws}.
    const uint32_t path = d.pos();
    d.u32(0xFFFFFFFF); // always drawn
    uint32_t draws[2];
    for (int k = 0; k < 2; ++k) {
        draws[k] = d.pos();
        for (uint32_t f : {f0, f1, f2, k == 0 ? f3 : f3 + 4, f4, f5, f6, f7}) d.ptr(kData, f);
    }
    uint32_t groups[2];
    for (int k = 0; k < 2; ++k) {
        groups[k] = d.pos();
        d.u32(1);
        d.ptr(kData, path);
        d.u32(1);
        d.ptr(kData, draws[k]);
    }

    // The render list: the lit draw in bucket 0 (main pass), the shadow draw in bucket 5.
    d.align(16);
    const uint32_t renderList = d.pos();
    for (int bucket = 0; bucket < 8; ++bucket) {
        if (bucket == 0 || bucket == 5) {
            d.ptr(kData, groups[bucket == 0 ? 0 : 1]);
            d.u32(1);
        } else {
            d.u32(0), d.u32(0);
        }
    }
    for (uint32_t f : {f0, f1, f2, f3, f4, f5, f6, f7}) d.ptr(kData, f);
    for (uint32_t n : {1u, 1u, 1u, 2u, 1u, 1u, 1u, 1u}) d.u32(n);
    d.u32(7);
    d.u32(1); // one instance per batch

    // GPU resources: the shaders (by a word holding each object), their physical parts, the
    // vertex buffer {->stream +4, ->data, bytes} and the index buffer {->object, ->data, bytes, 16-bit}.
    const uint32_t vertexRecord = d.pos();
    d.ptr(kData, stream + 4);
    d.ptr(kGpu, 0);
    d.u32(vertexBytes);
    const uint32_t indexRecord = d.pos();
    d.ptr(kData, indexBuffer);
    d.ptr(kGpu, indexAt);
    d.u32(indexBytes);
    d.u32(1);
    const uint32_t pixelRefs = d.pos();
    d.ptr(kData, f3);
    const uint32_t pixelPhysical = d.pos();
    d.ptr(kGpu, physical[1]);
    const uint32_t vertexRefs = d.pos();
    d.ptr(kData, f2);
    const uint32_t vertexPhysical = d.pos();
    d.ptr(kGpu, physical[0]);
    d.set(rgResources, d.pos());
    d.ptr(kData, pixelRefs), d.ptr(kData, pixelPhysical), d.u32(1);
    d.ptr(kData, vertexRefs), d.ptr(kData, vertexPhysical), d.u32(1);
    d.u32(0), d.u32(0), d.u32(0);
    d.ptr(kData, vertexRecord), d.u32(0), d.u32(1);
    d.ptr(kData, indexRecord), d.u32(1);
    d.u32(0), d.u32(0), d.u32(0);

    // The property dictionary and the shader style names.
    d.set(rgDictionary, dictionary(d, {{"numTextures", 0}, {"VertexBufferCount", 0}, {"IndexBufferCount", 0}}, false));
    d.set(rgStyles, dictionary(d, {{"RENDERGRAPH_SHADERSTYLE_NORMAL", 0}, {"RENDERGRAPH_SHADERSTYLE_ZONLY", 1},
                                   {"RENDERGRAPH_SHADERSTYLE_REFLECTION", 2}}, true));

    // The instance data: its size, alignment, template and the pointers set in each instance
    // (texture slots, resources' copy at kInstanceResources).
    d.align(16);
    const uint32_t instanceTemplate = d.pos();
    d.zeros(kInstanceResources + 0x44);
    const uint32_t section = d.pos();
    d.u32(kInstanceResources + 0x44);
    d.u32(0x20);
    d.ptr(kData, instanceTemplate);
    d.u32(1);
    d.u32(2);
    d.u32(0);
    d.u32(0x00), d.u32(kInstanceResources);
    d.u32(0x48), d.u32(kInstanceResources);
    const uint32_t dataName = d.pos();
    d.str(".data");
    d.align(4);
    const uint32_t sections = d.pos();
    d.ptr(kData, section);
    d.ptr(kData, dataName);
    d.set(rgInstance, d.pos());
    d.ptr(kData, sections);
    d.u32(1), d.u32(0), d.u32(0);

    // A lighting block with no lights, and no light probe values.
    d.align(4);
    d.set(rgLighting, d.pos());
    d.str("lighting");
    d.str("28.10.05.0034");
    d.align(4);
    d.zeros(0x20);
    d.set(rgLightProbe, d.pos());
    d.zeros(50 * 4);

    // The chunk directory.
    d.align(16);
    d.set(dChunks, d.pos());
    d.u32(0x05), d.ptr(kData, bounds);
    d.u32(0x17), d.ptr(kData, chunk17);
    d.u32(0x1E), d.ptr(kData, chunk1E);
    d.u32(0x0A), d.ptr(kData, renderList);
    d.zeros(0x80);

    // ---- the container: .data, .gpu and .stream, each one symbol named after the model.
    caff::Caff out;
    out.symbolNames.push_back(name);
    const Sym* syms[3] = {&d, &g, &s};
    const char* const sectionNames[3] = {".data", ".gpu", ".stream"};
    const uint8_t alignLog2[3] = {5, 5, 2};
    for (uint32_t i = 0; i < 3; ++i) {
        caff::Section sec;
        sec.name = sectionNames[i];
        sec.alignLog2 = alignLog2[i];
        // The section's string pool symbol (1-based; the game's loader reads it so): none.
        sec.firstSymbol = 0;
        sec.data = syms[i]->b;
        while (sec.data.size() % (size_t(1) << alignLog2[i])) sec.data.push_back(0);
        out.sections.push_back(sec);
        out.symbols.push_back({1, 0, uint32_t(syms[i]->b.size()), uint8_t(i + 1), uint8_t(i == 2 ? 2 : 5)});
    }
    for (uint32_t from = 1; from <= 3; ++from) {
        std::map<uint32_t, std::vector<uint32_t>> byTarget;
        for (auto& [at, to] : syms[from - 1]->relocs) byTarget[to].push_back(at);
        for (auto& [to, offsets] : byTarget) out.relocs[1].push_back({from, to, offsets});
    }
    return out.serialize();
}

// Chunk 0x26, the water region, laid out as the game's levels have it (sub_82385678, sub_82386720): {1, ->record};
// the record {setup 0, 1 item, ->item, vertex count, ->triangles (.gpu), the quad at its lowest, min, max, cell
// size, 16 x 16, ->grid}; the item {0, triangles, first vertex, min, max}; the grid's cells {levels, ->headers};
// each cell's headers {height, 0, triangles, ->corners} and then their corners. Its offset in `d`.
uint32_t waterChunk(Sym& d, const WaterRegion& w, uint32_t trianglesAt) {
    const auto& lo = w.lo;
    const auto& hi = w.hi;
    d.align(16);
    const uint32_t chunk = d.pos();
    d.u32(1);
    const uint32_t toRecord = d.ptr(kData);
    d.zeros(8);
    d.align(16);
    d.set(toRecord, d.pos());
    d.u32(0);
    d.u32(1);
    const uint32_t toItem = d.ptr(kData);
    d.u32(uint32_t(w.vertices.size()));
    d.ptr(kGpu, trianglesAt);
    d.zeros(12);
    for (const auto& [x, z] : {std::pair{lo[0], lo[2]}, std::pair{hi[0], lo[2]}, std::pair{lo[0], hi[2]}, std::pair{hi[0], hi[2]}})
        d.f32(x), d.f32(lo[1]), d.f32(z), d.f32(0);
    d.f32(lo[0]), d.f32(lo[1]), d.f32(lo[2]), d.f32(0);
    d.f32(hi[0]), d.f32(hi[1]), d.f32(hi[2]), d.f32(0);
    d.f32((hi[0] - lo[0]) / 16), d.f32(0), d.f32((hi[2] - lo[2]) / 16), d.f32(0);
    d.u32(16), d.u32(16);
    const uint32_t toGrid = d.ptr(kData);
    d.u32(0);
    d.set(toItem, d.pos());
    d.u32(0);
    d.u32(uint32_t(w.vertices.size() / 3));
    d.u32(0);
    for (float c : lo) d.f32(c);
    for (float c : hi) d.f32(c);
    d.set(toGrid, d.pos());
    std::vector<uint32_t> toCell(256, 0);
    for (size_t c = 0; c < 256; ++c) {
        d.u32(uint32_t(w.cells[c].size()));
        if (w.cells[c].empty()) d.u32(0);
        else toCell[c] = d.ptr(kData);
    }
    for (size_t c = 0; c < 256; ++c) {
        const auto& levels = w.cells[c];
        if (levels.empty()) continue;
        d.set(toCell[c], d.pos());
        std::vector<uint32_t> toCorners;
        for (const auto& lv : levels) {
            d.f32(lv.height);
            d.u32(0);
            d.u32(uint32_t(lv.triangles.size()));
            toCorners.push_back(d.ptr(kData));
        }
        for (size_t i = 0; i < levels.size(); ++i) {
            d.set(toCorners[i], d.pos());
            for (const auto& t : levels[i].triangles)
                for (float v : t) d.f32(v);
        }
    }
    return chunk;
}

// A world's model: build()'s layout with a vertex and an index buffer per draw, each lit draw's
// sampler record binding its texture to fetch constant 0, and the textures named as texturefileN. A
// sky's draws use reNut's sky shaders and cast no shadow; a skinned model's (`skin`) reNut's skinned ones.
Bytes buildWorld(const std::string& name, const std::vector<WorldDraw>& draws, const std::vector<std::string>& textures, bool sky,
                 const Skin* skin = nullptr, const ShapeTable* shapes = nullptr, const WaterRegion* water = nullptr) {
    if (draws.empty()) fail("the world has no triangles");
    if (textures.empty() || textures.size() > 0xFFFF) fail("a world model names 1 to 65535 textures");
    for (const WorldDraw& dr : draws) {
        if (dr.vertices.empty() || dr.indices.size() < 3) fail("a draw of the world has no triangles");
        if (dr.vertices.size() > 65535)
            fail("a draw of the world has " + std::to_string(dr.vertices.size()) + " vertices, more than its 16-bit indices reach (65535)");
        if (dr.texture >= textures.size()) fail("a draw of the world names a texture the model doesn't have");
        if (dr.material && dr.material->emissiveTexture >= int(textures.size()))
            fail("a draw of the world names an emissive texture the model doesn't have");
    }
    if (skin) {
        if (skin->bones.empty() || skin->bones.size() > 255) fail(name + ": a skinned model has 1 to 255 bones");
        detail::boneDepths(skin->bones);
        if (skin->words.size() != draws.size()) fail(name + ": skinning words for " + std::to_string(skin->words.size()) + " of " + std::to_string(draws.size()) + " draws");
        for (size_t k = 0; k < draws.size(); ++k) {
            if (skin->words[k].size() != draws[k].vertices.size()) fail(name + ": a draw's vertices and skinning words differ in number");
            for (const auto& w : skin->words[k])
                for (int c = 0; c < 4; ++c)
                    if (((w[0] >> (8 * c)) & 0xFF) >= skin->bones.size() && ((w[1] >> (8 * c)) & 0xFF))
                        fail(name + ": a vertex is weighted to bone " + std::to_string((w[0] >> (8 * c)) & 0xFF) + ", which the model doesn't have");
        }
    }
    const uint32_t kStride = skin ? 40 : 32;
    constexpr uint32_t kSamplerRecord = 0x108;  // {count, ->program, 16 samplers of 16 bytes}
    const size_t n = draws.size();

    // ---- GPU data: each draw's vertices (float3 position, 10:10:10 normal, half4 colour, float2 texture
    // coordinate, then any skinning words) and 16-bit indices, then each shader's physical part (signature, kind).
    Sym g;
    std::vector<uint32_t> vertexAt(n), vertexBytes(n), indexAt(n), indexBytes(n);
    for (size_t k = 0; k < n; ++k) {
        g.align(32);
        vertexAt[k] = g.pos();
        for (size_t i = 0; i < draws[k].vertices.size(); ++i) {
            const WorldVertex& v = draws[k].vertices[i];
            for (float c : v.pos) g.f32(c);
            g.u32(dec3n(v.normal));
            for (float c : v.colour) g.u16(half(c));
            g.f32(v.uv[0]);
            g.f32(v.uv[1]);
            if (skin) g.u32(skin->words[k][i][0]), g.u32(skin->words[k][i][1]);
        }
        const uint32_t indexField =
            shapes ? appendShapes(g, vertexAt[k], uint32_t(draws[k].vertices.size()), kStride, draws[k].shapes, *shapes) : 0;
        if (draws[k].material) appendMaterial(g, vertexAt[k], *draws[k].material);
        vertexBytes[k] = g.pos() - vertexAt[k];
        g.align(32);
        indexAt[k] = g.pos();
        for (uint32_t i : draws[k].indices) g.u16(uint16_t(i));
        indexBytes[k] = g.pos() - indexAt[k];
        if (indexField) g.set(indexField, indexAt[k]), g.set(indexField + 4, uint32_t(draws[k].indices.size()));
    }
    uint32_t physical[2];
    for (uint32_t k = 0; k < 2; ++k) {
        g.align(32);
        physical[k] = g.pos();
        g.str("RNUTPART");
        g.b.pop_back();
        g.u32(sky ? (k == 0 ? 5 : 6) : (k == 0 ? (skin ? 7 : 3) : 4));  // the world (sky, skinned) vertex and pixel shaders
        g.zeros(20);
    }
    constexpr uint32_t kPhysicalSize = 32;
    // The water region's triangles (float3 positions), which the game's water draws as they are.
    uint32_t waterAt = 0;
    if (water) {
        if (water->vertices.empty() || water->vertices.size() % 3 || water->cells.size() != 256)
            fail(name + ": a water region needs whole triangles and its 16 x 16 cells");
        g.align(32);
        waterAt = g.pos();
        for (const auto& v : water->vertices)
            for (float c : v) g.f32(c);
    }

    // ---- render program: the header, three style state tables, the program (empty, or a skinned model's
    // op 0x15 and op 0x12 as the game's own have them), the name.
    Sym s;
    const uint32_t sProgram = s.ptr(kStream);
    const uint32_t sStates = s.ptr(kStream);
    s.u32(0), s.u32(0), s.u32(0);
    const uint32_t sName = s.ptr(kStream);
    s.u32(0);
    s.u32(3);
    s.u32(0);
    static const uint32_t kStates[9][2] = {{0, 0}, {4, 0}, {8, 2}, {0x20, 0}, {0x14, 1}, {0x18, 1}, {0x10, 1}, {0x1C, 0}, {0x24, 1}};
    const uint32_t pairs = s.pos();
    for (auto& p : kStates) s.u32(p[0]), s.u32(p[1]);
    s.set(sStates, s.pos());
    for (int i = 0; i < 3; ++i) {
        s.ptr(kStream, pairs);
        s.u32(9);
    }
    s.set(sProgram, s.pos());
    if (skin) {
        // Op 0x15 {->the next op, 0}; op 0x12 {->list, u16 count, 0}: the bones whose matrices the
        // draws use, each {u16 bone, 0xFFFF, u8 0x7F, 3 pad}, all of them here in their own order.
        const uint32_t nb = uint32_t(skin->bones.size());
        s.u32(0x000C1500);
        s.ptr(kStream, s.pos() + 8);
        s.u32(0);
        s.u32((12 + 8 * nb) << 16 | 0x1200);
        s.ptr(kStream, s.pos() + 8);
        s.u16(uint16_t(nb)), s.u16(0);
        for (uint32_t i = 0; i < nb; ++i) s.u16(uint16_t(i)), s.u16(0xFFFF), s.u32(0x7F000000);
    }
    s.u32(0x00041D00); // end
    s.set(sName, s.pos());
    s.str(name);
    s.align(4);

    // ---- rendergraph data
    Sym d;
    const uint32_t dChunks = d.ptr(kData);
    d.u32(water ? 5 : 4);
    d.zeros(0x18);

    // Chunk 0x05: bounds. Spheres around the box's centre and its bottom's, the largest extent,
    // the radius across x and z, then the box twice.
    std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const WorldDraw& dr : draws)
        for (const WorldVertex& v : dr.vertices)
            for (size_t k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v.pos[k]), hi[k] = std::max(hi[k], v.pos[k]);
    const std::array<float, 3> c{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2}, bottom{c[0], lo[1], c[2]};
    float r = 0, rBottom = 0, rxz = 0;
    for (const WorldDraw& dr : draws)
        for (const WorldVertex& v : dr.vertices) {
            auto dist = [&](const std::array<float, 3>& o) {
                return std::sqrt((v.pos[0] - o[0]) * (v.pos[0] - o[0]) + (v.pos[1] - o[1]) * (v.pos[1] - o[1]) + (v.pos[2] - o[2]) * (v.pos[2] - o[2]));
            };
            r = std::max(r, dist(c));
            rBottom = std::max(rBottom, dist(bottom));
            rxz = std::max(rxz, std::sqrt((v.pos[0] - c[0]) * (v.pos[0] - c[0]) + (v.pos[2] - c[2]) * (v.pos[2] - c[2])));
        }
    d.align(16);
    const uint32_t bounds = d.pos();
    d.f32(r), d.f32(c[0]), d.f32(c[1]), d.f32(c[2]);
    d.f32(rBottom), d.f32(bottom[0]), d.f32(bottom[1]), d.f32(bottom[2]);
    d.f32(std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}));
    d.f32(rxz);
    for (int twice = 0; twice < 2; ++twice) {
        for (float x : lo) d.f32(x);
        for (float x : hi) d.f32(x);
    }

    // Chunk 0x17 (0) and chunk 0x1E (-> the rendergraph).
    const uint32_t chunk17 = d.pos();
    d.u32(0);
    const uint32_t chunk1E = d.pos();
    const uint32_t toRendergraph = d.ptr(kData);
    d.u32(0);

    // The rendergraph header; its texture count and slots are what the game resolves texturefileN
    // into as the model loads (sub_822D7ED0), and draws read their samplers' textures from.
    d.align(16);
    const uint32_t rg = d.pos();
    d.str("rendergraph");
    d.str("26.09.07.0039");
    d.u16(1);
    d.u32(uint32_t(textures.size()));        // +0x1C textures
    const uint32_t rgDictionary = d.ptr(kData);
    d.zeros(0x7C - 0x24);
    const uint32_t rgTextureSlots = d.ptr(kData);
    d.u32(0);
    d.u32(0x38000000);
    d.u32(0xFFFF), d.u32(0xFFFF);
    d.u32(0);                                 // +0x90 the world x view-projection register: c0
    for (int i = 0; i < 18; ++i) d.u32(0xFFFF);
    d.u32(0);
    d.u32(0);                                 // +0xE0 lights
    const uint32_t rgLighting = d.ptr(kData);
    d.u32(0);
    d.ptr(kStream, 0);                        // +0xEC the render program
    const uint32_t rgInstance = d.ptr(kData);
    d.u32(0), d.u32(0);
    const uint32_t rgResources = d.ptr(kData);
    // An instance's data (sub_82BC3488): its copy of the texture slots at 0x50, a word per texture,
    // then its copy of the resources (0x44 bytes), as the game's own models lay it out.
    constexpr uint32_t kInstanceSlots = 0x50;
    const uint32_t instanceResources = kInstanceSlots + 4 * uint32_t(textures.size());
    d.u32(instanceResources);
    const uint32_t rgPose = d.ptr(kData);
    d.u32(0x4441);
    const uint32_t rgStyles = d.ptr(kData);
    // +0x11C: 0x7FFFFF00 in static game models only, 0 in every skinned one.
    d.u32(0), d.u32(0), d.u32(0x80000002), d.u32(skin ? 0 : 0x7FFFFF00), d.u32(0xC0FFEFC0), d.u32(0xF);
    d.zeros(0x148 - 0x128);
    const uint32_t rgLightProbe = d.ptr(kData);
    d.u32(0);
    d.set(toRendergraph, rg);

    // The pose (a skinned model's bones, else none and an empty name table), then the texture slots: a
    // word per texture, set as it loads. Instances get a skin matrix per bone from the pose (sub_822E6820).
    if (skin) {
        d.set(rgPose, detail::pose(d, skin->bones));
    } else {
        d.set(rgPose, d.pos());
        d.str("pose");
        d.zeros(5);
        d.str("19.12.06.0036");
        d.zeros(0x30 - 0x18);
        d.u32(0), d.u32(0);
    }
    d.set(rgTextureSlots, d.pos());
    d.zeros(4 * textures.size());

    // Each draw's vertex stream {stride, ->buffer object, ->data, bytes} and buffer objects (filled at load).
    std::vector<uint32_t> stream(n), vertexBuffer(n), indexBuffer(n);
    for (size_t k = 0; k < n; ++k) {
        d.align(16);
        stream[k] = d.pos();
        d.u32(kStride);
        const uint32_t streamBuffer = d.ptr(kData);
        d.ptr(kGpu, vertexAt[k]);
        d.u32(vertexBytes[k]);
        d.align(16);
        vertexBuffer[k] = d.pos();
        d.zeros(0x20);
        d.set(streamBuffer, vertexBuffer[k]);
        indexBuffer[k] = d.pos();
        d.zeros(0x20);
    }

    // The shaders.
    const uint32_t pixelShader = shader(d, true, kPhysicalSize);
    const uint32_t vertexShader = shader(d, false, kPhysicalSize);

    // Draw field pools: stream bindings (one per draw), extra streams (none), vertex shader, pixel
    // shaders (one, and none for the shadow draws), bucket-3 word, constants (none), samplers, indices.
    d.align(16);
    const uint32_t f0 = d.pos();
    for (size_t k = 0; k < n; ++k) {
        d.ptr(kData, vertexBuffer[k]);
        d.u32(kStride);
    }
    const uint32_t f1 = d.pos();
    d.zeros(0x90);
    const uint32_t f2 = d.pos();
    d.ptr(kData, vertexShader);
    const uint32_t f3 = d.pos();
    d.ptr(kData, pixelShader);
    d.u32(0);
    const uint32_t f4 = d.pos();
    d.u32(0);
    const uint32_t f5 = d.pos();
    d.zeros(0x190);
    // Sampler records (sub_82376878): the shadow draws' (none), then one per lit draw, a sampler
    // {fetch constant, u16 texturefile (a split texture's top), u16 texturefile, clamp u, clamp v}.
    const uint32_t f6 = d.pos();
    d.zeros(kSamplerRecord);
    std::vector<uint32_t> samplers(n);
    for (size_t k = 0; k < n; ++k) {
        // A material's emissive texture is a second sampler, in fetch constant 1.
        const int emissive = draws[k].material ? draws[k].material->emissiveTexture : -1;
        samplers[k] = d.pos();
        d.u32(emissive >= 0 ? 2 : 1);
        d.u32(0);                             // its program in .stream: only array textures need it
        d.u32(0);                             // fetch constant 0
        d.u16(uint16_t(draws[k].texture));
        d.u16(uint16_t(draws[k].texture));
        d.u32(0), d.u32(0);                   // wrap in u and v
        if (emissive >= 0) {
            d.u32(1);
            d.u16(uint16_t(emissive)), d.u16(uint16_t(emissive));
            d.u32(0), d.u32(0);
        }
        d.zeros(kSamplerRecord - (emissive >= 0 ? 40 : 24));
    }
    const uint32_t f7 = d.pos();
    for (size_t k = 0; k < n; ++k) {
        d.ptr(kData, indexBuffer[k]);
        d.u32(uint32_t(draws[k].indices.size()));
    }

    // The draws (8 pointers each): the solid lit ones, the see-through lit ones (a material that blends or
    // adds), then the solid ones' shadows; and their groups {path count, ->path, draw count, ->draws}.
    const uint32_t path = d.pos();
    d.u32(0xFFFFFFFF); // always drawn
    auto seeThrough = [&](size_t k) {
        return draws[k].material && (draws[k].material->alpha == WorldMaterial::Blend || draws[k].material->alpha == WorldMaterial::Add);
    };
    std::vector<size_t> solid, clear;
    for (size_t k = 0; k < n; ++k) (seeThrough(k) ? clear : solid).push_back(k);
    auto drawList = [&](const std::vector<size_t>& which, bool lit) {
        const uint32_t at = d.pos();
        for (size_t k : which) {
            const uint32_t bind = f0 + 8 * uint32_t(k), index = f7 + 8 * uint32_t(k);
            for (uint32_t f : {bind, f1, f2, lit ? f3 : f3 + 4, f4, f5, lit ? samplers[k] : f6, index}) d.ptr(kData, f);
        }
        return at;
    };
    const uint32_t solidList = drawList(solid, true), clearList = drawList(clear, true), shadowList = drawList(solid, false);
    auto group = [&](uint32_t list, size_t count) {
        const uint32_t at = d.pos();
        d.u32(1);
        d.ptr(kData, path);
        d.u32(uint32_t(count));
        d.ptr(kData, list);
        return at;
    };
    const uint32_t solidGroup = group(solidList, solid.size()), clearGroup = group(clearList, clear.size()),
                   shadowGroup = group(shadowList, solid.size());

    // The render list: the solid lit draws in bucket 0 (main pass), the see-through ones in bucket 2 (the
    // game's translucent pass, after the solid one), the shadows in bucket 5; then each pool and its count.
    d.align(16);
    const uint32_t renderList = d.pos();
    for (int bucket = 0; bucket < 8; ++bucket) {
        const bool used = (bucket == 0 && !solid.empty()) || (bucket == 2 && !clear.empty()) || (bucket == 5 && !sky && !solid.empty());
        if (used) {
            d.ptr(kData, bucket == 0 ? solidGroup : bucket == 2 ? clearGroup : shadowGroup);
            d.u32(1);
        } else {
            d.u32(0), d.u32(0);
        }
    }
    for (uint32_t f : {f0, f1, f2, f3, f4, f5, f6, f7}) d.ptr(kData, f);
    for (size_t count : {n, size_t(1), size_t(1), size_t(2), size_t(1), size_t(1), n + 1, n}) d.u32(uint32_t(count));
    d.u32(7);
    d.u32(1); // one instance per batch

    // GPU resources: the shaders (by a word holding each object), their physical parts, the vertex
    // buffers {->stream +4, ->data, bytes} and the index buffers {->object, ->data, bytes, 16-bit}.
    const uint32_t vertexRecords = d.pos();
    for (size_t k = 0; k < n; ++k) {
        d.ptr(kData, stream[k] + 4);
        d.ptr(kGpu, vertexAt[k]);
        d.u32(vertexBytes[k]);
    }
    const uint32_t indexRecords = d.pos();
    for (size_t k = 0; k < n; ++k) {
        d.ptr(kData, indexBuffer[k]);
        d.ptr(kGpu, indexAt[k]);
        d.u32(indexBytes[k]);
        d.u32(1);
    }
    const uint32_t pixelRefs = d.pos();
    d.ptr(kData, f3);
    const uint32_t pixelPhysical = d.pos();
    d.ptr(kGpu, physical[1]);
    const uint32_t vertexRefs = d.pos();
    d.ptr(kData, f2);
    const uint32_t vertexPhysical = d.pos();
    d.ptr(kGpu, physical[0]);
    d.set(rgResources, d.pos());
    d.ptr(kData, pixelRefs), d.ptr(kData, pixelPhysical), d.u32(1);
    d.ptr(kData, vertexRefs), d.ptr(kData, vertexPhysical), d.u32(1);
    d.u32(0), d.u32(0), d.u32(0);
    d.ptr(kData, vertexRecords), d.u32(0), d.u32(uint32_t(n));
    d.ptr(kData, indexRecords), d.u32(uint32_t(n));
    d.u32(0), d.u32(0), d.u32(0);

    // The property dictionary (a skinned model's bone names first, each naming its index; texturefileN
    // names each texture asset) and the shader style names.
    std::vector<std::pair<std::string, std::string>> files;
    for (size_t k = 0; k < textures.size(); ++k) files.push_back({"texturefile" + std::to_string(k), textures[k]});
    std::vector<std::pair<std::string, uint32_t>> properties;
    if (skin)
        for (size_t k = 0; k < skin->bones.size(); ++k) properties.push_back({skin->bones[k].name, uint32_t(k)});
    properties.insert(properties.end(), {{"numTextures", uint32_t(textures.size())}, {"VertexBufferCount", 0}, {"IndexBufferCount", 0}});
    d.set(rgDictionary, dictionary(d, properties, false, files));
    d.set(rgStyles, dictionary(d, {{"RENDERGRAPH_SHADERSTYLE_NORMAL", 0}, {"RENDERGRAPH_SHADERSTYLE_ZONLY", 1},
                                   {"RENDERGRAPH_SHADERSTYLE_REFLECTION", 2}}, true));

    // The instance data: its size, alignment, template and the pointers set in each instance (its
    // texture slots' copy at kInstanceSlots, its resources' copy after them).
    d.align(16);
    const uint32_t instanceTemplate = d.pos();
    d.zeros(instanceResources + 0x44);
    const uint32_t section = d.pos();
    d.u32(instanceResources + 0x44);
    d.u32(0x20);
    d.ptr(kData, instanceTemplate);
    d.u32(1);
    d.u32(2);
    d.u32(0);
    d.u32(0x00), d.u32(kInstanceSlots);
    d.u32(0x48), d.u32(instanceResources);
    const uint32_t dataName = d.pos();
    d.str(".data");
    d.align(4);
    const uint32_t sections = d.pos();
    d.ptr(kData, section);
    d.ptr(kData, dataName);
    d.set(rgInstance, d.pos());
    d.ptr(kData, sections);
    d.u32(1), d.u32(0), d.u32(0);

    // A lighting block with no lights, and no light probe values.
    d.align(4);
    d.set(rgLighting, d.pos());
    d.str("lighting");
    d.str("28.10.05.0034");
    d.align(4);
    d.zeros(0x20);
    d.set(rgLightProbe, d.pos());
    d.zeros(50 * 4);

    const uint32_t chunk26 = water ? waterChunk(d, *water, waterAt) : 0;

    // The chunk directory.
    d.align(16);
    d.set(dChunks, d.pos());
    d.u32(0x05), d.ptr(kData, bounds);
    d.u32(0x17), d.ptr(kData, chunk17);
    d.u32(0x1E), d.ptr(kData, chunk1E);
    d.u32(0x0A), d.ptr(kData, renderList);
    if (water) d.u32(0x26), d.ptr(kData, chunk26);
    d.zeros(water ? 0x78 : 0x80);

    // ---- the container: .data, .gpu and .stream, each one symbol named after the model.
    caff::Caff out;
    out.symbolNames.push_back(name);
    const Sym* syms[3] = {&d, &g, &s};
    const char* const sectionNames[3] = {".data", ".gpu", ".stream"};
    const uint8_t alignLog2[3] = {5, 5, 2};
    for (uint32_t i = 0; i < 3; ++i) {
        caff::Section sec;
        sec.name = sectionNames[i];
        sec.alignLog2 = alignLog2[i];
        sec.firstSymbol = 0;
        sec.data = syms[i]->b;
        while (sec.data.size() % (size_t(1) << alignLog2[i])) sec.data.push_back(0);
        out.sections.push_back(sec);
        out.symbols.push_back({1, 0, uint32_t(syms[i]->b.size()), uint8_t(i + 1), uint8_t(i == 2 ? 2 : 5)});
    }
    for (uint32_t from = 1; from <= 3; ++from) {
        std::map<uint32_t, std::vector<uint32_t>> byTarget;
        for (auto& [at, to] : syms[from - 1]->relocs) byTarget[to].push_back(at);
        for (auto& [to, offsets] : byTarget) out.relocs[1].push_back({from, to, offsets});
    }
    return out.serialize();
}

} // namespace

std::vector<int> detail::boneDepths(const std::vector<SkinBone>& bones) {
    std::vector<int> depth;
    for (size_t i = 0; i < bones.size(); ++i) {
        const int p = bones[i].parent;
        if (p >= int(i) || p < -1) fail("bone " + bones[i].name + " comes before its parent");
        depth.push_back(p < 0 ? 0 : depth[size_t(p)] + 1);
        if (i && depth[i] < depth[i - 1]) fail("bone " + bones[i].name + " is less deep than the bone before it (bones go breadth-first)");
        if (bones[i].name.empty()) fail("a bone has no name");
        for (size_t k = 0; k < i; ++k)
            if (bones[k].name == bones[i].name) fail("two bones are named " + bones[i].name);
    }
    return depth;
}

uint32_t detail::nameTable(Sym& d, const std::vector<SkinBone>& bones, uint32_t pool, const std::vector<uint32_t>& names) {
    const uint32_t n = uint32_t(bones.size());
    if (pool && names.size() != n) fail("a bone name table without its names");
    d.align(4);
    const uint32_t head = d.pos();
    d.u32(n);
    d.ptr(kData, head + 16);
    d.u32(0);
    uint32_t hashes = 0;
    if (pool) d.u32(0);
    else hashes = d.ptr(kData);
    std::vector<uint32_t> keys(n);
    for (uint32_t i = 0; i < n; ++i) {
        keys[i] = d.pos();
        if (pool) d.ptr(pool, names[i]);
        else d.ptr(kData);
        d.u32(0);
        d.u32(i);
    }
    if (pool) return head;
    // The model's: {hash, 0, ->entry} by hash, then the names.
    d.set(hashes, d.pos());
    std::vector<uint32_t> order(n);
    for (uint32_t i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return elfHash(bones[a].name) < elfHash(bones[b].name); });
    for (uint32_t i : order) {
        d.u32(elfHash(bones[i].name));
        d.u32(0);
        d.ptr(kData, keys[i]);
    }
    for (uint32_t i = 0; i < n; ++i) {
        d.set(keys[i], d.pos());
        d.str(bones[i].name);
    }
    d.align(4);
    return head;
}

uint32_t detail::pose(Sym& d, const std::vector<SkinBone>& bones, uint32_t pool, const std::vector<uint32_t>& names) {
    boneDepths(bones);
    const uint16_t n = uint16_t(bones.size());
    d.align(16);
    const uint32_t at = d.pos();
    d.str("pose");
    d.zeros(5);
    d.str("19.12.06.0036");
    d.u32(n);
    d.u32(0);
    const uint32_t table = d.ptr(kData);
    d.u32(0), d.u32(0);
    d.ptr(kData, at + 0x30);
    // Records {local, world, rotation (identity), parent, first child, next sibling, itself, mirror, 0}; a
    // LF_ bone's mirror is its RT_ twin, as the game's skeletons have it.
    auto index = [&](const std::string& name) {
        for (uint16_t k = 0; k < n; ++k)
            if (bones[k].name == name) return k;
        return uint16_t(0xFFFF);
    };
    for (uint16_t i = 0; i < n; ++i) {
        const SkinBone& b = bones[i];
        const std::array<float, 3> up = b.parent < 0 ? std::array<float, 3>{0, 0, 0} : bones[size_t(b.parent)].world;
        for (size_t k = 0; k < 3; ++k) d.f32(b.world[k] - up[k]);
        for (float x : b.world) d.f32(x);
        d.f32(0), d.f32(0), d.f32(0), d.f32(1);
        uint16_t child = 0xFFFF, sibling = 0xFFFF;
        for (uint16_t k = n; k-- > 0;)
            if (bones[k].parent == int(i)) child = k;
        for (uint16_t k = uint16_t(i + 1); k < n && sibling == 0xFFFF; ++k)
            if (bones[k].parent == b.parent) sibling = k;
        d.u16(b.parent < 0 ? uint16_t(0xFFFF) : uint16_t(b.parent));
        d.u16(child);
        d.u16(sibling);
        d.u16(i);
        d.u16(startsWith(b.name, "LF_") ? index("RT_" + b.name.substr(3)) : uint16_t(0xFFFF));
        d.u16(0);
    }
    d.set(table, nameTable(d, bones, pool, names));
    return at;
}

Bytes writeSkinned(const std::string& assetName, const std::vector<WorldDraw>& draws, const std::vector<std::string>& textures,
                   const Skin& skin, const ShapeTable* shapes) {
    return buildWorld(assetName, draws, textures, false, &skin, shapes);
}

Bytes write(const std::string& id, const Mesh& mesh, const ShapeTable* shapes) { return build(id, mesh, false, shapes); }

Bytes writeWorld(const std::string& assetName, const std::vector<WorldDraw>& draws, const std::vector<std::string>& textures,
                 const ShapeTable* shapes, const WaterRegion* water) {
    return buildWorld(assetName, draws, textures, false, nullptr, shapes, water);
}

Bytes writeSky(const std::string& assetName, const std::vector<WorldDraw>& draws, const std::vector<std::string>& textures) {
    return buildWorld(assetName, draws, textures, true);
}

} // namespace nut::blockmodel
