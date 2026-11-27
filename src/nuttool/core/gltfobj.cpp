#include "gltfobj.hpp"

#include "image.hpp"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <tuple>

namespace nut::gltfobj {
namespace {

using Mat = std::array<float, 16>; // column-major, as glTF stores it
using V3 = std::array<float, 3>;

constexpr uint32_t kMagic = 0x46546C67, kChunkJson = 0x4E4F534A, kChunkBin = 0x004E4942;

struct Glb {
    Json json;
    const uint8_t* bin = nullptr;
    size_t binSize = 0;
    std::shared_ptr<Bytes> ownBin;  // a .gltf's buffer, read from its file
};

Bytes dataUri(const std::string& uri);
fs::path uriPath(const std::string& uri);

// A text glTF (.gltf): its JSON and its one buffer, a data URI or a file beside it in `dir`.
Glb openText(const Bytes& f, const fs::path& dir) {
    Glb g;
    g.json = Json::parse(std::string_view(reinterpret_cast<const char*>(f.data()), f.size()));
    if (!g.json.has("buffers") || g.json["buffers"].size() == 0) return g;
    if (g.json["buffers"].size() > 1) fail("gltf: more than one buffer is not supported");
    const Json& b = static_cast<const Json&>(g.json)["buffers"][size_t(0)];
    if (!b.has("uri")) fail("gltf: its buffer has no URI");
    const std::string uri = b["uri"].asString();
    g.ownBin = std::make_shared<Bytes>();
    if (startsWith(uri, "data:")) {
        *g.ownBin = dataUri(uri);
    } else {
        const fs::path file = dir / uriPath(uri);
        if (dir.empty() || !fs::exists(longPath(file))) fail("gltf: no buffer file " + file.string());
        *g.ownBin = readFile(file);
    }
    g.bin = g.ownBin->data();
    g.binSize = g.ownBin->size();
    return g;
}

Glb open(const Bytes& f, const fs::path& dir = {}) {
    if (!isGlb(f)) {
        size_t at = 0;
        while (at < f.size() && (std::isspace(f[at]) || f[at] == 0xEF || f[at] == 0xBB || f[at] == 0xBF)) ++at;
        if (at < f.size() && f[at] == '{') return openText(f, dir);
    }
    if (!isGlb(f) || f.size() < 20) fail("not a glTF (.glb or .gltf) file");
    Glb g;
    for (size_t at = 12; at + 8 <= f.size();) {
        uint32_t size = le32(&f[at]), type = le32(&f[at + 4]);
        if (at + 8 + size > f.size()) fail("glb: chunk past the end of the file");
        if (type == kChunkJson) g.json = Json::parse(std::string_view(reinterpret_cast<const char*>(&f[at + 8]), size));
        else if (type == kChunkBin && !g.bin) {
            g.bin = &f[at + 8];
            g.binSize = size;
        }
        at += 8 + ((size + 3) & ~size_t(3));
    }
    if (g.json.isNull()) fail("glb: no JSON chunk");
    return g;
}

Mat identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat mul(const Mat& a, const Mat& b) {
    Mat r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k) r[size_t(c * 4 + row)] += a[size_t(k * 4 + row)] * b[size_t(c * 4 + k)];
    return r;
}

float num(const Json& j, size_t i, float otherwise) { return j.type() == Json::Array && i < j.size() ? float(j[i].asNumber()) : otherwise; }

// A node's own transform: its matrix, or translation * rotation * scale.
Mat local(const Json& n) {
    if (n.has("matrix")) {
        Mat m{};
        for (size_t i = 0; i < 16; ++i) m[i] = num(n["matrix"], i, 0);
        return m;
    }
    const Json &t = n["translation"], &r = n["rotation"], &s = n["scale"];
    float x = num(r, 0, 0), y = num(r, 1, 0), z = num(r, 2, 0), w = num(r, 3, 1);
    float sx = num(s, 0, 1), sy = num(s, 1, 1), sz = num(s, 2, 1);
    return {(1 - 2 * (y * y + z * z)) * sx, 2 * (x * y + z * w) * sx, 2 * (x * z - y * w) * sx, 0,
            2 * (x * y - z * w) * sy, (1 - 2 * (x * x + z * z)) * sy, 2 * (y * z + x * w) * sy, 0,
            2 * (x * z + y * w) * sz, 2 * (y * z - x * w) * sz, (1 - 2 * (x * x + y * y)) * sz, 0,
            num(t, 0, 0), num(t, 1, 0), num(t, 2, 0), 1};
}

// An accessor's values as floats (positions) or integers (indices), `count` rows of `width`.
std::vector<double> accessor(const Glb& g, size_t index, int width) {
    const Json& a = g.json["accessors"][index];
    if (a.has("sparse")) fail("glb: sparse accessors are not supported");
    const int type = int(a["componentType"].asNumber());
    const size_t count = size_t(a["count"].asNumber());
    const size_t size = type == 5126 || type == 5125 ? 4 : type == 5123 ? 2 : type == 5121 ? 1 : 0;
    if (!size) fail("glb: unsupported accessor component type " + std::to_string(type));
    std::vector<double> out(count * size_t(width));
    if (!a.has("bufferView")) return out;
    const Json& v = g.json["bufferViews"][size_t(a["bufferView"].asNumber())];
    if (v.has("buffer") && v["buffer"].asNumber() != 0) fail("glb: data outside the file's own buffer");
    const size_t stride = v.has("byteStride") ? size_t(v["byteStride"].asNumber()) : size * size_t(width);
    const size_t start = size_t(v.has("byteOffset") ? v["byteOffset"].asNumber() : 0) +
                         size_t(a.has("byteOffset") ? a["byteOffset"].asNumber() : 0);
    if (count && start + (count - 1) * stride + size * size_t(width) > g.binSize) fail("glb: accessor past its buffer");
    for (size_t i = 0; i < count; ++i)
        for (int c = 0; c < width; ++c) {
            const uint8_t* p = g.bin + start + i * stride + size_t(c) * size;
            double& o = out[i * size_t(width) + size_t(c)];
            if (type == 5126) {
                uint32_t b = le32(p);
                float f;
                std::memcpy(&f, &b, 4);
                o = f;
            } else {
                o = type == 5125 ? le32(p) : type == 5123 ? le16(p) : *p;
            }
        }
    return out;
}

// A data: URI's bytes (base64, or plain text with %XX escapes).
Bytes dataUri(const std::string& uri) {
    const size_t comma = uri.find(',');
    if (comma == std::string::npos) fail("glb: a data URI without data");
    const bool base64 = uri.substr(0, comma).find(";base64") != std::string::npos;
    Bytes out;
    if (!base64) {
        for (size_t i = comma + 1; i < uri.size(); ++i) {
            if (uri[i] == '%' && i + 2 < uri.size()) {
                out.push_back(uint8_t(parseHex(uri.substr(i + 1, 2))));
                i += 2;
            } else {
                out.push_back(uint8_t(uri[i]));
            }
        }
        return out;
    }
    uint32_t bits = 0;
    int have = 0;
    for (size_t i = comma + 1; i < uri.size(); ++i) {
        const char ch = uri[i];
        int v = ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 : ch >= '0' && ch <= '9' ? ch - '0' + 52
              : ch == '+' || ch == '-' ? 62 : ch == '/' || ch == '_' ? 63 : -1;
        if (v < 0) continue;  // padding and line breaks
        bits = bits << 6 | uint32_t(v);
        have += 6;
        if (have >= 8) {
            have -= 8;
            out.push_back(uint8_t(bits >> have));
        }
    }
    return out;
}

// A relative URI as a path: %XX escapes undone.
fs::path uriPath(const std::string& uri) {
    std::string s;
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            s += char(parseHex(uri.substr(i + 1, 2)));
            i += 2;
        } else {
            s += uri[i];
        }
    }
    return fs::path(std::u8string(s.begin(), s.end()));
}

// glTF image `index`'s encoded file: from a buffer view of the file's own buffer, a data URI or
// a file beside the .glb in `dir`. `name` says which.
Bytes imageFile(const Glb& g, size_t index, const fs::path& dir, std::string& name) {
    const Json& im = g.json["images"][index];
    name = im.has("name") ? im["name"].asString() : "image " + std::to_string(index);
    if (im.has("bufferView")) {
        const Json& v = g.json["bufferViews"][size_t(im["bufferView"].asNumber())];
        if (v.has("buffer") && v["buffer"].asNumber() != 0) fail("its data is outside the file's own buffer");
        const size_t start = size_t(v.has("byteOffset") ? v["byteOffset"].asNumber() : 0), size = size_t(v["byteLength"].asNumber());
        if (!g.bin || start + size > g.binSize) fail("its buffer view is past the file's buffer");
        return Bytes(g.bin + start, g.bin + start + size);
    }
    if (!im.has("uri")) fail("it has neither a buffer view nor a URI");
    const std::string uri = im["uri"].asString();
    if (startsWith(uri, "data:")) return dataUri(uri);
    const fs::path file = dir / uriPath(uri);
    name = file.string();
    if (dir.empty() || !fs::exists(longPath(file))) fail("no file " + file.string());
    return readFile(file);
}

V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float length(const V3& a) { return std::sqrt(dot(a, a)); }

// A node's triangles: positions welded by place (with its and its parents' transforms), each
// face's unit normal, and each corner's normal (the faces at its place within the angle).
struct Geometry {
    std::vector<V3> pos;
    std::vector<std::array<int, 3>> tris;
    std::vector<V3> faceUnit;
    std::vector<std::array<V3, 3>> corner;
    // Shape keys (the mesh's morph targets): names, default weights, and each place's deltas.
    std::vector<std::string> shapeNames;
    std::vector<float> shapeWeights;
    std::vector<std::vector<blockmodel::ShapeDelta>> placeShapes;
};

Geometry geometry(const Bytes& glb, const std::string& name, float smoothDegrees) {
    Glb g = open(glb);
    const Json& nodes = g.json["nodes"];
    size_t node = SIZE_MAX;
    for (size_t i = 0; i < nodes.size() && node == SIZE_MAX; ++i)
        if (nodes[i].has("mesh") && (nodes[i].has("name") ? nodes[i]["name"].asString() : "#" + std::to_string(i)) == name) node = i;
    if (node == SIZE_MAX) {
        std::string list;
        for (auto& n : meshNodes(glb)) list += (list.empty() ? "" : ", ") + n;
        fail("no node named '" + name + "' with a mesh in the file (it has: " + list + ")");
    }

    std::vector<size_t> parent(nodes.size(), SIZE_MAX);
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].has("children"))
            for (auto& c : nodes[i]["children"].items()) parent[size_t(c.asNumber())] = i;
    Mat world = identity();
    for (size_t n = node; n != SIZE_MAX; n = parent[n]) world = mul(local(nodes[n]), world);
    const float det = world[0] * (world[5] * world[10] - world[9] * world[6]) - world[4] * (world[1] * world[10] - world[9] * world[2]) +
                      world[8] * (world[1] * world[6] - world[5] * world[2]);

    Geometry out;
    // Welded by place; a primitive with shape keys keeps its vertices apart (their deltas are their own).
    std::map<std::pair<std::array<long, 3>, long long>, int> weld;
    const Json& meshJson = static_cast<const Json&>(g.json)["meshes"][size_t(nodes[node]["mesh"].asNumber())];
    const Json& prims = meshJson["primitives"];
    auto linear = [&](const V3& d) {
        return V3{world[0] * d[0] + world[4] * d[1] + world[8] * d[2], world[1] * d[0] + world[5] * d[1] + world[9] * d[2],
                  world[2] * d[0] + world[6] * d[1] + world[10] * d[2]};
    };
    long long primIndex = 0;
    for (auto& p : prims.items()) {
        ++primIndex;
        if (p.has("mode") && p["mode"].asNumber() != 4) continue; // lines and points
        if (!p["attributes"].has("POSITION")) continue;
        std::vector<double> v = accessor(g, size_t(p["attributes"]["POSITION"].asNumber()), 3);
        const size_t count = v.size() / 3;
        const bool shaped = p.has("targets") && p["targets"].size() > 0;
        std::vector<int> at(count);
        for (size_t i = 0; i < count; ++i) {
            V3 x{float(v[3 * i]), float(v[3 * i + 1]), float(v[3 * i + 2])};
            V3 w{world[0] * x[0] + world[4] * x[1] + world[8] * x[2] + world[12], world[1] * x[0] + world[5] * x[1] + world[9] * x[2] + world[13],
                 world[2] * x[0] + world[6] * x[1] + world[10] * x[2] + world[14]};
            std::array<long, 3> key{std::lround(w[0] * 1e4f), std::lround(w[1] * 1e4f), std::lround(w[2] * 1e4f)};
            auto [it, fresh] = weld.emplace(std::make_pair(key, shaped ? (primIndex << 32) | (long long)i : -1LL), int(out.pos.size()));
            if (fresh) out.pos.push_back(w), out.placeShapes.emplace_back();
            at[i] = it->second;
        }
        // Its shape keys: each target's deltas, turned as the node turns, under the mesh's names for them.
        if (shaped) {
            const Json& targets = p["targets"];
            const bool named = meshJson.has("extras") && meshJson["extras"].has("targetNames");
            for (size_t ti = 0; ti < targets.size(); ++ti) {
                std::string shapeName = name + "." + std::to_string(ti);
                if (named && ti < meshJson["extras"]["targetNames"].size()) shapeName = meshJson["extras"]["targetNames"][ti].asString();
                auto found = std::find(out.shapeNames.begin(), out.shapeNames.end(), shapeName);
                if (found == out.shapeNames.end()) {
                    float weight = 0;
                    if (nodes[node].has("weights") && ti < nodes[node]["weights"].size()) weight = float(nodes[node]["weights"][ti].asNumber());
                    else if (meshJson.has("weights") && ti < meshJson["weights"].size()) weight = float(meshJson["weights"][ti].asNumber());
                    out.shapeNames.push_back(shapeName), out.shapeWeights.push_back(weight);
                    found = out.shapeNames.end() - 1;
                }
                const uint32_t shape = uint32_t(found - out.shapeNames.begin());
                const Json& tg = targets[ti];
                const std::vector<double> dp = tg.has("POSITION") ? accessor(g, size_t(tg["POSITION"].asNumber()), 3) : std::vector<double>{};
                const std::vector<double> dn = tg.has("NORMAL") ? accessor(g, size_t(tg["NORMAL"].asNumber()), 3) : std::vector<double>{};
                for (size_t i = 0; i < count; ++i) {
                    const V3 d = 3 * i + 2 < dp.size() ? V3{float(dp[3 * i]), float(dp[3 * i + 1]), float(dp[3 * i + 2])} : V3{0, 0, 0};
                    const V3 nd = 3 * i + 2 < dn.size() ? V3{float(dn[3 * i]), float(dn[3 * i + 1]), float(dn[3 * i + 2])} : V3{0, 0, 0};
                    if (d == V3{0, 0, 0} && nd == V3{0, 0, 0}) continue;
                    out.placeShapes[size_t(at[i])].push_back({shape, linear(d), linear(nd)});
                }
            }
        }
        std::vector<double> idx;
        if (p.has("indices")) idx = accessor(g, size_t(p["indices"].asNumber()), 1);
        else for (size_t i = 0; i < count; ++i) idx.push_back(double(i));
        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            std::array<int, 3> tri;
            for (int k = 0; k < 3; ++k) {
                size_t i = size_t(idx[t + size_t(k)]);
                if (i >= count) fail("glb: index past its vertices");
                tri[size_t(k)] = at[i];
            }
            if (det < 0) std::swap(tri[1], tri[2]); // a mirroring transform turns faces inside out
            if (tri[0] != tri[1] && tri[1] != tri[2] && tri[0] != tri[2]) out.tris.push_back(tri);
        }
    }
    if (out.tris.empty()) fail("node '" + name + "' has no triangles");

    std::vector<V3> faceN(out.tris.size());
    out.faceUnit.resize(out.tris.size());
    std::vector<std::vector<size_t>> at(out.pos.size());
    for (size_t t = 0; t < out.tris.size(); ++t) {
        const auto& tr = out.tris[t];
        faceN[t] = cross(sub(out.pos[size_t(tr[1])], out.pos[size_t(tr[0])]), sub(out.pos[size_t(tr[2])], out.pos[size_t(tr[0])]));
        float l = length(faceN[t]);
        out.faceUnit[t] = l > 0 ? V3{faceN[t][0] / l, faceN[t][1] / l, faceN[t][2] / l} : V3{0, 1, 0};
        for (int k : tr) at[size_t(k)].push_back(t);
    }
    const float limit = std::cos(smoothDegrees * 3.14159265f / 180);
    out.corner.resize(out.tris.size());
    for (size_t t = 0; t < out.tris.size(); ++t)
        for (int c = 0; c < 3; ++c) {
            V3 sum{0, 0, 0};
            for (size_t s : at[size_t(out.tris[t][size_t(c)])])
                if (dot(out.faceUnit[s], out.faceUnit[t]) >= limit)
                    for (int q = 0; q < 3; ++q) sum[size_t(q)] += faceN[s][size_t(q)];
            float l = length(sum);
            out.corner[t][size_t(c)] = l > 0 ? V3{sum[0] / l, sum[1] / l, sum[2] / l} : out.faceUnit[t];
        }
    return out;
}

} // namespace

bool isWaterName(const std::string& name, const std::vector<std::string>& extra) {
    const std::string n = lower(name);
    if (n.find("_water") != std::string::npos) return true;
    for (const std::string& e : extra)
        if (!e.empty() && n.find(lower(e)) != std::string::npos) return true;
    return false;
}

int addMaterial(std::vector<SceneMesh::Material>& materials, const std::string& name, const blockmodel::WorldMaterial& look) {
    for (size_t i = 0; i < materials.size(); ++i)
        if (materials[i].name == name) return int(i);
    materials.push_back({name, look, std::nullopt});
    return int(materials.size() - 1);
}

int addWater(std::vector<SceneMesh::Water>& water, const std::string& name, const std::array<V3, 3>& tri) {
    auto it = std::find_if(water.begin(), water.end(), [&](const SceneMesh::Water& w) { return w.name == name; });
    if (it == water.end()) it = water.insert(water.end(), SceneMesh::Water{name, {}});
    it->triangles.push_back(tri);
    return int(it - water.begin());
}

bool isGlb(const Bytes& file) { return file.size() >= 12 && le32(file.data()) == kMagic; }

std::vector<std::string> meshNodes(const Bytes& glb) {
    Glb g = open(glb);
    std::vector<std::string> out;
    const Json& nodes = g.json["nodes"];
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].has("mesh")) out.push_back(nodes[i].has("name") ? nodes[i]["name"].asString() : "#" + std::to_string(i));
    return out;
}

MeshData mesh(const Bytes& glb, const std::string& node, float smoothDegrees) {
    const Geometry geo = geometry(glb, node, smoothDegrees);
    // One vertex per place and normal.
    MeshData out;
    std::map<std::pair<int, std::array<long, 3>>, uint32_t> unique;
    for (size_t t = 0; t < geo.tris.size(); ++t)
        for (int c = 0; c < 3; ++c) {
            const int p = geo.tris[t][size_t(c)];
            const V3& n = geo.corner[t][size_t(c)];
            const std::array<long, 3> key{std::lround(n[0] * 1e4f), std::lround(n[1] * 1e4f), std::lround(n[2] * 1e4f)};
            auto [it, fresh] = unique.emplace(std::make_pair(p, key), uint32_t(out.positions.size()));
            if (fresh) {
                out.positions.push_back(geo.pos[size_t(p)]);
                out.normals.push_back(n);
                out.shapes.push_back(geo.placeShapes[size_t(p)]);
            }
            out.indices.push_back(it->second);
        }
    out.shapeNames = geo.shapeNames;
    out.shapeWeights = geo.shapeWeights;
    if (out.shapeNames.empty()) out.shapes.clear();
    return out;
}

SceneMesh scene(const Bytes& glb, float smoothDegrees, const fs::path& dir, const std::vector<std::string>& waterNames) {
    Glb g = open(glb, dir);
    const Json& nodes = g.json["nodes"];
    std::vector<size_t> parent(nodes.size(), SIZE_MAX);
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].has("children"))
            for (auto& c : nodes[i]["children"].items()) parent[size_t(c.asNumber())] = i;

    // The file's images, each decoded once as a material first shows it (-1: unreadable).
    std::vector<image::Rgba> images;
    std::vector<std::string> imageNames, warnings;
    std::map<size_t, int> imageOf;
    auto imageAt = [&](size_t index) {
        auto [it, fresh] = imageOf.emplace(index, -1);
        if (!fresh) return it->second;
        std::string name = "image " + std::to_string(index);
        try {
            image::Rgba img = image::decodeImage(imageFile(g, index, dir, name));
            if (!img.width || !img.height || img.pixels.size() < size_t(img.width) * img.height * 4) fail("it has no pixels");
            it->second = int(images.size());
            images.push_back(std::move(img));
            imageNames.push_back(name);
        } catch (const std::exception& e) {
            warnings.push_back("glTF " + name + " was not read (" + e.what() + "); its materials are untextured");
        }
        return it->second;
    };

    // Every triangle's corners: a welded place (for smoothing), a colour and a texture coordinate.
    std::vector<V3> pos;
    std::map<std::array<long, 3>, int> weld;
    std::vector<std::array<int, 3>> tris;
    std::vector<std::array<Corner, 3>> triCorners;
    std::vector<int> triImage, triWater, triMaterial;
    std::vector<SceneMesh::Water> water;
    std::vector<SceneMesh::Material> materials;
    std::vector<std::string> shapeNames;  // shape keys across the file, by name
    std::vector<float> shapeWeights;
    std::map<std::string, uint32_t> shapeOf;
    auto toSrgb = [](float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f; };
    auto worldOf = [&](size_t node) {
        Mat world = identity();
        for (size_t n = node; n != SIZE_MAX; n = parent[n]) world = mul(local(nodes[n]), world);
        return world;
    };
    auto determinant = [](const Mat& m) {
        return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
    };
    auto apply = [](const Mat& m, const V3& x) {
        return V3{m[0] * x[0] + m[4] * x[1] + m[8] * x[2] + m[12], m[1] * x[0] + m[5] * x[1] + m[9] * x[2] + m[13],
                  m[2] * x[0] + m[6] * x[1] + m[10] * x[2] + m[14]};
    };
    // A skin's joints at rest: each joint's world transform times its inverse bind matrix.
    std::map<size_t, std::vector<Mat>> skinJoints;
    auto jointsOf = [&](size_t skin) -> const std::vector<Mat>& {
        auto [it, fresh] = skinJoints.emplace(skin, std::vector<Mat>{});
        if (!fresh) return it->second;
        const Json& s = g.json["skins"][skin];
        std::vector<double> ibm;
        if (s.has("inverseBindMatrices")) ibm = accessor(g, size_t(s["inverseBindMatrices"].asNumber()), 16);
        size_t j = 0;
        for (auto& joint : s["joints"].items()) {
            Mat inv = identity();
            if (16 * (j + 1) <= ibm.size())
                for (size_t k = 0; k < 16; ++k) inv[k] = float(ibm[16 * j + k]);
            it->second.push_back(mul(worldOf(size_t(joint.asNumber())), inv));
            ++j;
        }
        return it->second;
    };
    for (size_t node = 0; node < nodes.size(); ++node) {
        if (!nodes[node].has("mesh")) continue;
        const Mat world = worldOf(node);
        const float det = determinant(world);
        const std::vector<Mat>* joints = nodes[node].has("skin") ? &jointsOf(size_t(nodes[node]["skin"].asNumber())) : nullptr;
        const Json& meshJson = static_cast<const Json&>(g.json)["meshes"][size_t(nodes[node]["mesh"].asNumber())];
        const Json& prims = meshJson["primitives"];
        const std::string nodeName = nodes[node].has("name") ? nodes[node]["name"].asString() : std::string();
        const std::string meshName = meshJson.has("name") ? meshJson["name"].asString() : std::string();
        const std::string nodeWater = isWaterName(nodeName, waterNames)   ? nodeName
                                      : isWaterName(meshName, waterNames) ? meshName
                                                                          : std::string();
        for (auto& p : prims.items()) {
            if (p.has("mode") && p["mode"].asNumber() != 4) continue;
            std::string waterName = nodeWater;
            if (!p["attributes"].has("POSITION")) continue;
            std::array<float, 4> base{1, 1, 1, 1};
            int image = -1, texCoord = 0, material = -1;
            if (p.has("material")) {
                const Json& m = g.json["materials"][size_t(p["material"].asNumber())];
                if (m.has("pbrMetallicRoughness")) {
                    const Json& pbr = m["pbrMetallicRoughness"];
                    if (pbr.has("baseColorFactor"))
                        for (size_t k = 0; k < 4; ++k) base[k] = num(pbr["baseColorFactor"], k, 1);
                    if (pbr.has("baseColorTexture") && pbr["baseColorTexture"].has("index")) {
                        const Json& t = static_cast<const Json&>(g.json)["textures"][size_t(pbr["baseColorTexture"]["index"].asNumber())];
                        if (pbr["baseColorTexture"].has("texCoord")) texCoord = int(pbr["baseColorTexture"]["texCoord"].asNumber());
                        if (t.has("source")) image = imageAt(size_t(t["source"].asNumber()));
                    }
                }
                // Water by its material's name, or its texture's.
                const std::string materialName = m.has("name") ? m["name"].asString() : std::string();
                if (waterName.empty() && isWaterName(materialName, waterNames)) waterName = materialName;
                // Its look in reNut: alpha mode (none given: cut out under half, as before materials),
                // emissive (linear in glTF, sRGB here), double-sided and unlit.
                blockmodel::WorldMaterial look;
                if (m.has("alphaMode")) {
                    const std::string mode = m["alphaMode"].asString();
                    look.alpha = mode == "BLEND" ? blockmodel::WorldMaterial::Blend
                                 : mode == "MASK" ? blockmodel::WorldMaterial::Cutout
                                                  : blockmodel::WorldMaterial::Opaque;
                }
                if (m.has("alphaCutoff")) look.cutoff = float(m["alphaCutoff"].asNumber());
                if (m.has("doubleSided")) look.doubleSided = m["doubleSided"].asBool();
                if (m.has("emissiveFactor"))
                    for (size_t k = 0; k < 3; ++k) look.emissive[k] = toSrgb(std::clamp(num(m["emissiveFactor"], k, 0), 0.0f, 1.0f));
                if (m.has("emissiveTexture") && m["emissiveTexture"].has("index")) {
                    const Json& t = static_cast<const Json&>(g.json)["textures"][size_t(m["emissiveTexture"]["index"].asNumber())];
                    if (t.has("source")) look.emissiveTexture = imageAt(size_t(t["source"].asNumber()));
                }
                if (m.has("extensions")) {
                    const Json& ext = m["extensions"];
                    if (ext.has("KHR_materials_emissive_strength") && ext["KHR_materials_emissive_strength"].has("emissiveStrength"))
                        look.strength = float(ext["KHR_materials_emissive_strength"]["emissiveStrength"].asNumber());
                    look.unlit = ext.has("KHR_materials_unlit");
                }
                const std::string key = materialName.empty() ? "material" + std::to_string(size_t(p["material"].asNumber())) : materialName;
                material = addMaterial(materials, key, look);
            }
            if (waterName.empty() && image >= 0 && isWaterName(imageNames[size_t(image)], waterNames))
                waterName = imageNames[size_t(image)];
            std::vector<double> v = accessor(g, size_t(p["attributes"]["POSITION"].asNumber()), 3);
            const size_t count = v.size() / 3;
            // TEXCOORD_n: floats, or normalised integers.
            std::vector<std::array<float, 2>> uv(count, std::array<float, 2>{0, 0});
            const std::string uvName = "TEXCOORD_" + std::to_string(texCoord);
            if (image >= 0 && p["attributes"].has(uvName)) {
                const size_t ui = size_t(p["attributes"][uvName].asNumber());
                const int type = int(static_cast<const Json&>(g.json)["accessors"][ui]["componentType"].asNumber());
                const double scale = type == 5121 ? 1.0 / 255 : type == 5123 ? 1.0 / 65535 : 1.0;
                std::vector<double> c = accessor(g, ui, 2);
                for (size_t i = 0; i < count && 2 * i + 1 < c.size(); ++i) uv[i] = {float(c[2 * i] * scale), float(c[2 * i + 1] * scale)};
            } else if (image >= 0) {
                image = -1;  // nothing to place the texture by
            }
            // COLOR_0: three or four components, normalised when they are integers.
            std::vector<std::array<float, 4>> vertexColour(count, std::array<float, 4>{1, 1, 1, 1});
            if (p["attributes"].has("COLOR_0")) {
                const size_t ci = size_t(p["attributes"]["COLOR_0"].asNumber());
                const Json& a = g.json["accessors"][ci];
                const int width = a["type"].asString() == "VEC3" ? 3 : 4;
                const int type = int(a["componentType"].asNumber());
                const double scale = type == 5121 ? 1.0 / 255 : type == 5123 ? 1.0 / 65535 : 1.0;
                std::vector<double> c = accessor(g, ci, width);
                for (size_t i = 0; i < count && (i + 1) * size_t(width) <= c.size(); ++i)
                    for (int k = 0; k < width; ++k) vertexColour[i][size_t(k)] = float(c[i * size_t(width) + size_t(k)] * scale);
            }
            // A skinned primitive is placed by its joints at rest (glTF ignores its node's transform).
            std::vector<double> jointIds, jointWeights;
            const bool skinned = joints && !joints->empty() && p["attributes"].has("JOINTS_0") && p["attributes"].has("WEIGHTS_0");
            if (skinned) {
                jointIds = accessor(g, size_t(p["attributes"]["JOINTS_0"].asNumber()), 4);
                const size_t wi = size_t(p["attributes"]["WEIGHTS_0"].asNumber());
                const int type = int(static_cast<const Json&>(g.json)["accessors"][wi]["componentType"].asNumber());
                const double scale = type == 5121 ? 1.0 / 255 : type == 5123 ? 1.0 / 65535 : 1.0;
                jointWeights = accessor(g, wi, 4);
                for (double& w : jointWeights) w *= scale;
            }
            // Where vertex i's point x goes: by its joints at rest when skinned, else by its node.
            auto place = [&](size_t i, const V3& x) {
                if (skinned && 4 * i + 3 < jointIds.size()) {
                    V3 sum{0, 0, 0};
                    double total = 0;
                    for (size_t k = 0; k < 4; ++k) {
                        const double wt = jointWeights[4 * i + k];
                        const size_t jt = size_t(jointIds[4 * i + k]);
                        if (wt <= 0 || jt >= joints->size()) continue;
                        const V3 p2 = apply((*joints)[jt], x);
                        for (size_t c = 0; c < 3; ++c) sum[c] += float(wt) * p2[c];
                        total += wt;
                    }
                    if (total > 0) {
                        for (float& c : sum) c = float(c / total);
                        return sum;
                    }
                }
                return apply(world, x);
            };
            // A skinned primitive's winding: its first vertex's heaviest joint's.
            float primDet = det;
            if (skinned && jointIds.size() >= 4) {
                size_t heaviest = 4;
                for (size_t k = 0; k < 4; ++k)
                    if (jointWeights[k] > 0 && size_t(jointIds[k]) < joints->size() && (heaviest == 4 || jointWeights[k] > jointWeights[heaviest]))
                        heaviest = k;
                if (heaviest < 4) primDet = determinant((*joints)[size_t(jointIds[heaviest])]);
            }
            std::vector<int> at(count);
            for (size_t i = 0; i < count; ++i) {
                const V3 w = place(i, V3{float(v[3 * i]), float(v[3 * i + 1]), float(v[3 * i + 2])});
                std::array<long, 3> key{std::lround(w[0] * 1e4f), std::lround(w[1] * 1e4f), std::lround(w[2] * 1e4f)};
                auto [it, fresh] = weld.emplace(key, int(pos.size()));
                if (fresh) pos.push_back(w);
                at[i] = it->second;
            }
            // Shape keys (morph targets): each target's position and normal deltas, placed as the vertex
            // is, under the mesh's names for them (extras.targetNames) across the file.
            std::vector<std::vector<blockmodel::ShapeDelta>> primShapes;
            if (p.has("targets") && p["targets"].size() > 0) {
                primShapes.resize(count);
                const Json& targets = p["targets"];
                const bool named = meshJson.has("extras") && meshJson["extras"].has("targetNames");
                for (size_t ti = 0; ti < targets.size(); ++ti) {
                    const Json& tg = targets[ti];
                    std::string name = meshName.empty() ? "shape" : meshName;
                    name += "." + std::to_string(ti);
                    if (named && ti < meshJson["extras"]["targetNames"].size()) name = meshJson["extras"]["targetNames"][ti].asString();
                    float weight = 0;
                    if (nodes[node].has("weights") && ti < nodes[node]["weights"].size()) weight = float(nodes[node]["weights"][ti].asNumber());
                    else if (meshJson.has("weights") && ti < meshJson["weights"].size()) weight = float(meshJson["weights"][ti].asNumber());
                    auto [found, added] = shapeOf.emplace(name, uint32_t(shapeNames.size()));
                    if (added) shapeNames.push_back(name), shapeWeights.push_back(weight);
                    const std::vector<double> dp = tg.has("POSITION") ? accessor(g, size_t(tg["POSITION"].asNumber()), 3) : std::vector<double>{};
                    const std::vector<double> dn = tg.has("NORMAL") ? accessor(g, size_t(tg["NORMAL"].asNumber()), 3) : std::vector<double>{};
                    for (size_t i = 0; i < count; ++i) {
                        blockmodel::ShapeDelta sd;
                        sd.shape = found->second;
                        bool any = false;
                        if (3 * i + 2 < dp.size() && (dp[3 * i] != 0 || dp[3 * i + 1] != 0 || dp[3 * i + 2] != 0)) {
                            const V3 x{float(v[3 * i]), float(v[3 * i + 1]), float(v[3 * i + 2])};
                            const V3 moved = place(i, V3{x[0] + float(dp[3 * i]), x[1] + float(dp[3 * i + 1]), x[2] + float(dp[3 * i + 2])});
                            const V3 here = place(i, x);
                            sd.position = {moved[0] - here[0], moved[1] - here[1], moved[2] - here[2]};
                            any = true;
                        }
                        if (3 * i + 2 < dn.size() && (dn[3 * i] != 0 || dn[3 * i + 1] != 0 || dn[3 * i + 2] != 0)) {
                            const V3 tip = apply(world, V3{float(dn[3 * i]), float(dn[3 * i + 1]), float(dn[3 * i + 2])}), origin = apply(world, V3{0, 0, 0});
                            sd.normal = {tip[0] - origin[0], tip[1] - origin[1], tip[2] - origin[2]};
                            any = true;
                        }
                        if (any) primShapes[i].push_back(sd);
                    }
                }
            }
            std::vector<double> idx;
            if (p.has("indices")) idx = accessor(g, size_t(p["indices"].asNumber()), 1);
            else for (size_t i = 0; i < count; ++i) idx.push_back(double(i));
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                std::array<size_t, 3> corner{};
                for (int k = 0; k < 3; ++k) {
                    corner[size_t(k)] = size_t(idx[t + size_t(k)]);
                    if (corner[size_t(k)] >= count) fail("glb: index past its vertices");
                }
                if (primDet < 0) std::swap(corner[1], corner[2]);
                std::array<int, 3> tri{at[corner[0]], at[corner[1]], at[corner[2]]};
                if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) continue;
                triWater.push_back(waterName.empty()
                                       ? -1
                                       : addWater(water, waterName, {pos[size_t(tri[0])], pos[size_t(tri[1])], pos[size_t(tri[2])]}));
                std::array<Corner, 3> corners;
                for (int k = 0; k < 3; ++k) {
                    for (size_t q = 0; q < 4; ++q) {
                        const float c = std::clamp(base[q] * vertexColour[corner[size_t(k)]][q], 0.0f, 1.0f);
                        corners[size_t(k)].colour[q] = q < 3 ? toSrgb(c) : c;
                    }
                    corners[size_t(k)].uv = uv[corner[size_t(k)]];
                    if (!primShapes.empty()) corners[size_t(k)].shapes = primShapes[corner[size_t(k)]];
                }
                tris.push_back(tri);
                triCorners.push_back(corners);
                triImage.push_back(image);
                triMaterial.push_back(material);
            }
        }
    }
    if (tris.empty()) fail("the file has no triangles");
    SceneMesh out = sceneFromTriangles(pos, tris, triCorners, triImage, smoothDegrees);
    out.images = std::move(images);
    out.imageNames = std::move(imageNames);
    out.warnings = std::move(warnings);
    out.water = std::move(water);
    out.triangleWater = std::move(triWater);
    out.materials = std::move(materials);
    out.triangleMaterial = std::move(triMaterial);
    out.shapeNames = std::move(shapeNames);
    out.shapeWeights = std::move(shapeWeights);
    return out;
}

SceneMesh sceneFromTriangles(const std::vector<std::array<float, 3>>& pos, const std::vector<std::array<int, 3>>& tris,
                             const std::vector<std::array<Corner, 3>>& triCorners, const std::vector<int>& triImage, float smoothDegrees) {
    // Each corner's normal: the faces at its place within the angle, as mesh() smooths them.
    std::vector<V3> faceN(tris.size()), faceUnit(tris.size());
    std::vector<std::vector<size_t>> around(pos.size());
    for (size_t t = 0; t < tris.size(); ++t) {
        faceN[t] = cross(sub(pos[size_t(tris[t][1])], pos[size_t(tris[t][0])]), sub(pos[size_t(tris[t][2])], pos[size_t(tris[t][0])]));
        const float l = length(faceN[t]);
        faceUnit[t] = l > 0 ? V3{faceN[t][0] / l, faceN[t][1] / l, faceN[t][2] / l} : V3{0, 1, 0};
        for (int k : tris[t]) around[size_t(k)].push_back(t);
    }
    const float limit = std::cos(smoothDegrees * 3.14159265f / 180);
    SceneMesh out;
    // A vertex per place, normal, colour, texture coordinate and image (vertices never span two images).
    std::map<std::tuple<int, std::array<long, 3>, std::array<long, 4>, std::array<long, 2>, int, long long>, uint32_t> unique;
    bool anyShapes = false;
    for (size_t t = 0; t < tris.size(); ++t) {
        const int image = t < triImage.size() ? triImage[t] : -1;
        for (int c = 0; c < 3; ++c) {
            const int p = tris[t][size_t(c)];
            V3 sum{0, 0, 0};
            for (size_t s : around[size_t(p)])
                if (dot(faceUnit[s], faceUnit[t]) >= limit)
                    for (int q = 0; q < 3; ++q) sum[size_t(q)] += faceN[s][size_t(q)];
            const float l = length(sum);
            const V3 n = l > 0 ? V3{sum[0] / l, sum[1] / l, sum[2] / l} : faceUnit[t];
            const Corner& corner = triCorners[t][size_t(c)];
            const std::array<float, 4>& col = corner.colour;
            const std::array<float, 2> uv = image >= 0 ? corner.uv : std::array<float, 2>{0, 0};
            const std::array<long, 3> nk{std::lround(n[0] * 1e4f), std::lround(n[1] * 1e4f), std::lround(n[2] * 1e4f)};
            const std::array<long, 4> ck{std::lround(col[0] * 255), std::lround(col[1] * 255), std::lround(col[2] * 255),
                                         std::lround(col[3] * 255)};
            const std::array<long, 2> uk{std::lround(uv[0] * 1e5f), std::lround(uv[1] * 1e5f)};
            // A corner with shape keys keeps a vertex of its own (its deltas are its own).
            const long long own = corner.shapes.empty() ? -1 : (long long)(3 * t + size_t(c));
            auto [it, fresh] = unique.emplace(std::make_tuple(p, nk, ck, uk, image, own), uint32_t(out.positions.size()));
            if (fresh) {
                out.positions.push_back(pos[size_t(p)]);
                out.normals.push_back(n);
                out.colours.push_back(col);
                out.uvs.push_back(uv);
                out.vertexShapes.push_back(corner.shapes);
                anyShapes |= !corner.shapes.empty();
            }
            out.indices.push_back(it->second);
        }
        out.triangleImage.push_back(image);
    }
    if (!anyShapes) out.vertexShapes.clear();
    return out;
}

SceneMesh objScene(const fs::path& obj, float smoothDegrees, const std::vector<std::string>& waterNames) {
    const fs::path dir = obj.parent_path();
    // A line's words after its keyword, and the rest of it (a path may hold spaces).
    auto words = [](const std::string& line) {
        std::vector<std::string> out;
        for (auto& w : split(line, ' '))
            if (!trim(w).empty()) out.push_back(trim(w));
        return out;
    };

    // The materials: their Kd and texture (an index into `out.images`, -1 for none), and their look in
    // reNut (Ke and map_Ke its emissive, d or Tr below 1 blending).
    struct Material {
        std::array<float, 4> kd{1, 1, 1, 1};
        int image = -1;
        blockmodel::WorldMaterial look;
    };
    SceneMesh out;
    std::map<std::string, Material> materials;
    std::map<std::string, int> imageOf;  // by texture file
    auto loadImage = [&](const std::string& file) {
        auto [it, fresh] = imageOf.emplace(lower(file), -1);
        if (!fresh) return it->second;
        const fs::path path = dir / file;
        try {
            image::Rgba img = image::decodeImage(readFile(path));
            if (!img.width || !img.height || img.pixels.size() < size_t(img.width) * img.height * 4) fail("it has no pixels");
            it->second = int(out.images.size());
            out.images.push_back(std::move(img));
            out.imageNames.push_back(path.string());
        } catch (const std::exception& e) {
            out.warnings.push_back(path.string() + " was not read (" + e.what() + "); its materials are untextured");
        }
        return it->second;
    };
    auto loadMtl = [&](const fs::path& mtl) {
        if (!fs::exists(longPath(mtl))) {
            out.warnings.push_back("no material library " + mtl.string() + "; the model is untextured");
            return;
        }
        std::string current;
        for (auto& raw : split(readText(mtl), '\n')) {
            const std::string line = trim(raw);
            auto w = words(line);
            if (w.empty()) continue;
            if (w[0] == "newmtl" && w.size() > 1) {
                current = trim(line.substr(6));
                materials[current] = Material{};
            } else if (w[0] == "Kd" && w.size() >= 4 && !current.empty()) {
                for (size_t k = 0; k < 3; ++k) materials[current].kd[k] = std::clamp(float(std::atof(w[k + 1].c_str())), 0.0f, 1.0f);
            } else if (w[0] == "map_Kd" && w.size() > 1 && !current.empty()) {
                // Options (-s 1 1 1, -bm 1...) come before the file name.
                const std::string file = w[1][0] == '-' ? w.back() : trim(line.substr(6));
                materials[current].image = loadImage(file);
            } else if (w[0] == "Ke" && w.size() >= 4 && !current.empty()) {
                for (size_t k = 0; k < 3; ++k)
                    materials[current].look.emissive[k] = std::clamp(float(std::atof(w[k + 1].c_str())), 0.0f, 1.0f);
            } else if (w[0] == "map_Ke" && w.size() > 1 && !current.empty()) {
                const std::string file = w[1][0] == '-' ? w.back() : trim(line.substr(6));
                Material& m = materials[current];
                m.look.emissiveTexture = loadImage(file);
                // A texture alone gives off its own colour.
                if (m.look.emissiveTexture >= 0 && m.look.emissive == std::array<float, 3>{0, 0, 0}) m.look.emissive = {1, 1, 1};
            } else if ((w[0] == "d" || w[0] == "Tr") && w.size() > 1 && !current.empty()) {
                float d = std::clamp(float(std::atof(w[1].c_str())), 0.0f, 1.0f);
                if (w[0] == "Tr") d = 1 - d;
                Material& m = materials[current];
                m.look.opacity = d;
                if (d < 1) m.look.alpha = blockmodel::WorldMaterial::Blend;
            }
        }
    };

    std::vector<V3> pos;              // welded places
    std::vector<int> placeOf;         // an OBJ vertex's place
    std::vector<std::array<float, 3>> vertexColour;
    std::vector<std::array<float, 2>> texCoords;  // vt, v from the image's top
    std::map<std::array<long, 3>, int> weld;
    std::vector<std::array<int, 3>> tris;
    std::vector<std::array<Corner, 3>> triCorners;
    std::vector<int> triImage, triWater, triMaterial;
    std::vector<SceneMesh::Water> water;
    std::vector<SceneMesh::Material> sceneMaterials;
    // The current object or group's name, and material's (or its texture's), when they are water.
    std::string objectWater, materialWater;
    Material material;
    int materialIndex = -1;
    for (auto& raw : split(readText(obj), '\n')) {
        const std::string line = trim(raw);
        auto w = words(line);
        if (w.empty() || w[0][0] == '#') continue;
        if ((w[0] == "o" || w[0] == "g") && w.size() > 1) {
            const std::string name = trim(line.substr(1));
            objectWater = isWaterName(name, waterNames) ? name : std::string();
        } else if (w[0] == "mtllib" && w.size() > 1) {
            loadMtl(dir / trim(line.substr(6)));
        } else if (w[0] == "usemtl") {
            const std::string name = w.size() > 1 ? trim(line.substr(6)) : std::string();
            auto it = materials.find(name);
            material = it == materials.end() ? Material{} : it->second;
            materialIndex = it == materials.end() ? -1 : addMaterial(sceneMaterials, name, material.look);
            const std::string texture = material.image >= 0 ? out.imageNames[size_t(material.image)] : std::string();
            materialWater = isWaterName(name, waterNames) ? name : isWaterName(texture, waterNames) ? texture : std::string();
        } else if (w[0] == "vt" && w.size() >= 3) {
            texCoords.push_back({float(std::atof(w[1].c_str())), 1 - float(std::atof(w[2].c_str()))});
        } else if (w[0] == "v" && w.size() >= 4) {
            const V3 p{float(std::atof(w[1].c_str())), float(std::atof(w[2].c_str())), float(std::atof(w[3].c_str()))};
            std::array<long, 3> key{std::lround(p[0] * 1e4f), std::lround(p[1] * 1e4f), std::lround(p[2] * 1e4f)};
            auto [it, fresh] = weld.emplace(key, int(pos.size()));
            if (fresh) pos.push_back(p);
            placeOf.push_back(it->second);
            // Vertex colours some exporters add after the position.
            std::array<float, 3> c{1, 1, 1};
            if (w.size() >= 7)
                for (size_t k = 0; k < 3; ++k) c[k] = std::clamp(float(std::atof(w[4 + k].c_str())), 0.0f, 1.0f);
            vertexColour.push_back(c);
        } else if (w[0] == "f" && w.size() >= 4) {
            // A polygon as a fan; indices from 1, negative ones from the end.
            std::vector<size_t> corner;
            std::vector<long> texCorner;  // -1: none
            for (size_t k = 1; k < w.size(); ++k) {
                const std::vector<std::string> parts = split(w[k], '/');
                long i = std::atol(parts[0].c_str());
                if (i < 0) i += long(placeOf.size()) + 1;
                if (i < 1 || size_t(i) > placeOf.size()) fail("obj: a face names a vertex it doesn't have: " + line);
                corner.push_back(size_t(i - 1));
                long t = parts.size() > 1 && !parts[1].empty() ? std::atol(parts[1].c_str()) : 0;
                if (t < 0) t += long(texCoords.size()) + 1;
                texCorner.push_back(t >= 1 && size_t(t) <= texCoords.size() ? t - 1 : -1);
            }
            for (size_t k = 1; k + 1 < corner.size(); ++k) {
                const std::array<size_t, 3> at{0, k, k + 1};
                const std::array<size_t, 3> c{corner[at[0]], corner[at[1]], corner[at[2]]};
                std::array<int, 3> tri{placeOf[c[0]], placeOf[c[1]], placeOf[c[2]]};
                if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) continue;
                triWater.push_back(objectWater.empty() && materialWater.empty()
                                       ? -1
                                       : addWater(water, !objectWater.empty() ? objectWater : materialWater,
                                                  {pos[size_t(tri[0])], pos[size_t(tri[1])], pos[size_t(tri[2])]}));
                // A textured material shows its texture in white: exporters' Kd is a dim grey under it.
                int image = material.image;
                for (size_t q : at)
                    if (texCorner[q] < 0) image = -1;
                std::array<Corner, 3> corners;
                for (size_t q = 0; q < 3; ++q) {
                    for (size_t r = 0; r < 4; ++r)
                        corners[q].colour[r] = r < 3 ? (image >= 0 ? 1.0f : material.kd[r]) * vertexColour[c[q]][r] : material.kd[3];
                    if (image >= 0) corners[q].uv = texCoords[size_t(texCorner[at[q]])];
                }
                tris.push_back(tri);
                triCorners.push_back(corners);
                triImage.push_back(image);
                triMaterial.push_back(materialIndex);
            }
        }
    }
    if (tris.empty()) fail("the file has no faces");
    SceneMesh result = sceneFromTriangles(pos, tris, triCorners, triImage, smoothDegrees);
    result.images = std::move(out.images);
    result.imageNames = std::move(out.imageNames);
    result.warnings = std::move(out.warnings);
    result.water = std::move(water);
    result.triangleWater = std::move(triWater);
    result.materials = std::move(sceneMaterials);
    result.triangleMaterial = std::move(triMaterial);
    return result;
}

std::string toObj(const Bytes& glb, const Options& o) {
    if (o.into.empty()) fail("no meshes of the model to put the node's mesh in");
    const Geometry geo = geometry(glb, o.node, o.smoothDegrees);

    std::string out = "# " + o.node + " (glTF), as NutTool's OBJ for model import\n";
    char line[160];
    auto put = [&](const char* fmt, auto... args) {
        std::snprintf(line, sizeof line, fmt, args...);
        out += line;
    };
    put("o mesh%d\n", o.into[0]);
    if (!o.skinBone.empty()) out += "skin " + o.skinBone + "\n";
    if (o.flatUv2) put("uv2 %.6g %.6g\n", o.flatUv2At[0], 1 - o.flatUv2At[1]);
    for (auto& p : geo.pos) put("v %.6g %.6g %.6g\n", p[0], p[1], p[2]);
    if (o.flatUv2) out += "uv2\n";
    if (!o.skinBone.empty()) out += "skin\n";
    for (size_t t = 0; t < geo.tris.size(); ++t) {
        // Texture coordinates: the face's two other axes than the one it faces most along.
        const V3& n = geo.faceUnit[t];
        int axis = std::fabs(n[0]) > std::fabs(n[1]) ? (std::fabs(n[0]) > std::fabs(n[2]) ? 0 : 2) : (std::fabs(n[1]) > std::fabs(n[2]) ? 1 : 2);
        int u = axis == 0 ? 2 : 0, v = axis == 1 ? 2 : 1;
        for (int c = 0; c < 3; ++c) {
            const int k = geo.tris[t][size_t(c)];
            const V3& nn = geo.corner[t][size_t(c)];
            if (o.flat) put("vt %.6g %.6g\n", o.flatUv[0], 1 - o.flatUv[1]);
            else put("vt %.6g %.6g\n", geo.pos[size_t(k)][size_t(u)] + 0.5f, geo.pos[size_t(k)][size_t(v)] + 0.5f);
            put("vn %.6g %.6g %.6g\n", nn[0], nn[1], nn[2]);
        }
    }
    for (size_t i = 0; i < o.into.size(); ++i) {
        if (i) put("o mesh%d\n", o.into[i]);
        for (size_t t = 0; t < geo.tris.size(); ++t) {
            size_t c = 3 * t + 1;
            put("f %d/%zu/%zu %d/%zu/%zu %d/%zu/%zu\n", geo.tris[t][0] + 1, c, c, geo.tris[t][1] + 1, c + 1, c + 1, geo.tris[t][2] + 1, c + 2, c + 2);
        }
    }
    for (int h : o.hide) put("o mesh%d\nv 0 0 0\n", h);
    return out;
}

std::vector<SkinnedMesh> skinnedMeshes(const Bytes& file) {
    const Glb g = open(file);
    std::vector<SkinnedMesh> out;
    if (!g.json.has("nodes")) return out;
    const auto& nodes = g.json["nodes"].items();
    std::vector<size_t> parent(nodes.size(), SIZE_MAX);
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].has("children"))
            for (auto& c : nodes[i]["children"].items())
                if (size_t(c.asNumber()) < nodes.size()) parent[size_t(c.asNumber())] = i;
    auto worldOf = [&](size_t node) {
        Mat w = identity();
        for (size_t n = node, depth = 0; n != SIZE_MAX && depth < 256; n = parent[n], ++depth) w = mul(local(nodes[n]), w);
        return w;
    };
    // x through m: a point (with its translation) or a direction (without).
    auto apply = [](const Mat& m, const V3& x, float w) {
        return V3{m[0] * x[0] + m[4] * x[1] + m[8] * x[2] + w * m[12], m[1] * x[0] + m[5] * x[1] + m[9] * x[2] + w * m[13],
                  m[2] * x[0] + m[6] * x[1] + m[10] * x[2] + w * m[14]};
    };
    auto numbers = [&](const Json& attrs, const char* name, int width, bool normalised) {
        std::vector<double> v;
        if (!attrs.has(name)) return v;
        const size_t i = size_t(attrs[name].asNumber());
        v = accessor(g, i, width);
        if (normalised) {
            const int type = int(g.json["accessors"][i]["componentType"].asNumber());
            const double scale = type == 5121 ? 1.0 / 255 : type == 5123 ? 1.0 / 65535 : 1.0;
            for (double& x : v) x *= scale;
        }
        return v;
    };
    for (size_t node = 0; node < nodes.size(); ++node) {
        if (!nodes[node].has("mesh")) continue;
        SkinnedMesh sm;
        sm.name = nodes[node].has("name") ? nodes[node]["name"].asString() : std::string();
        const Mat world = worldOf(node);
        // A skin's joints at rest (world transform times inverse bind matrix) and names.
        std::vector<Mat> joints;
        std::vector<std::string> jointNames;
        if (nodes[node].has("skin")) {
            const Json& s = g.json["skins"][size_t(nodes[node]["skin"].asNumber())];
            std::vector<double> ibm;
            if (s.has("inverseBindMatrices")) ibm = accessor(g, size_t(s["inverseBindMatrices"].asNumber()), 16);
            size_t k = 0;
            for (auto& joint : s["joints"].items()) {
                const size_t jn = size_t(joint.asNumber());
                Mat inv = identity();
                if (16 * (k + 1) <= ibm.size())
                    for (size_t e = 0; e < 16; ++e) inv[e] = float(ibm[16 * k + e]);
                joints.push_back(jn < nodes.size() ? mul(worldOf(jn), inv) : identity());
                jointNames.push_back(jn < nodes.size() && nodes[jn].has("name") ? nodes[jn]["name"].asString() : std::string());
                ++k;
            }
        }
        const Json& prims = g.json["meshes"][size_t(nodes[node]["mesh"].asNumber())]["primitives"];
        for (auto& p : prims.items()) {
            if ((p.has("mode") && p["mode"].asNumber() != 4) || !p["attributes"].has("POSITION")) continue;
            if (!sm.plainColour && sm.colourImage.empty() && p.has("material")) {
                const Json& m = g.json["materials"][size_t(p["material"].asNumber())];
                const Json* pbr = m.has("pbrMetallicRoughness") ? &m["pbrMetallicRoughness"] : nullptr;
                if (!pbr || !pbr->has("baseColorTexture")) {
                    std::array<float, 3> c{1, 1, 1};
                    if (pbr && pbr->has("baseColorFactor"))
                        for (size_t k = 0; k < 3; ++k) c[k] = num((*pbr)["baseColorFactor"], k, 1);
                    sm.plainColour = c;
                } else {
                    // The texture's image, when the file holds it (an image beside the .glb isn't read here).
                    const Json& t = g.json["textures"][size_t((*pbr)["baseColorTexture"]["index"].asNumber())];
                    if (t.has("source")) {
                        try {
                            sm.colourImage = imageFile(g, size_t(t["source"].asNumber()), {}, sm.colourImageName);
                        } catch (const std::exception&) {
                        }
                    }
                }
            }
            const Json& attrs = p["attributes"];
            const std::vector<double> pos = numbers(attrs, "POSITION", 3, false), nor = numbers(attrs, "NORMAL", 3, false),
                                      uv = numbers(attrs, "TEXCOORD_0", 2, true), ji = numbers(attrs, "JOINTS_0", 4, false),
                                      jw = numbers(attrs, "WEIGHTS_0", 4, true);
            const size_t count = pos.size() / 3, base = sm.points.size();
            const bool skinned = !joints.empty() && ji.size() >= count * 4 && jw.size() >= count * 4;
            for (size_t i = 0; i < count; ++i) {
                // Its joints' blended rest transform when skinned, else its node's.
                Mat m = world;
                std::vector<std::pair<std::string, float>> weights;
                if (skinned) {
                    Mat sum{};
                    double total = 0;
                    for (size_t k = 0; k < 4; ++k) {
                        const double w = jw[4 * i + k];
                        const size_t j = size_t(ji[4 * i + k]);
                        if (w <= 0 || j >= joints.size()) continue;
                        for (size_t e = 0; e < 16; ++e) sum[e] += float(w) * joints[j][e];
                        total += w;
                        weights.push_back({jointNames[j], float(w)});
                    }
                    if (total > 0) {
                        for (float& e : sum) e = float(e / total);
                        m = sum;
                    }
                }
                sm.points.push_back(apply(m, V3{float(pos[3 * i]), float(pos[3 * i + 1]), float(pos[3 * i + 2])}, 1));
                if (nor.size() >= 3 * count) {
                    V3 n = apply(m, V3{float(nor[3 * i]), float(nor[3 * i + 1]), float(nor[3 * i + 2])}, 0);
                    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    sm.normals.push_back(len > 0 ? V3{n[0] / len, n[1] / len, n[2] / len} : V3{0, 1, 0});
                }
                if (uv.size() >= 2 * count) sm.uvs.push_back({float(uv[2 * i]), 1 - float(uv[2 * i + 1])});
                sm.skin.push_back(std::move(weights));
            }
            std::vector<double> idx = p.has("indices") ? accessor(g, size_t(p["indices"].asNumber()), 1) : std::vector<double>{};
            if (idx.empty())
                for (size_t i = 0; i < count; ++i) idx.push_back(double(i));
            for (size_t t = 0; t + 2 < idx.size(); t += 3)
                if (idx[t] < double(count) && idx[t + 1] < double(count) && idx[t + 2] < double(count))
                    sm.triangles.push_back({uint32_t(base + size_t(idx[t])), uint32_t(base + size_t(idx[t + 1])), uint32_t(base + size_t(idx[t + 2]))});
        }
        // A primitive without normals or texture coordinates drops them for the whole node.
        if (sm.normals.size() != sm.points.size()) sm.normals.clear();
        if (sm.uvs.size() != sm.points.size()) sm.uvs.clear();
        if (!sm.triangles.empty()) out.push_back(std::move(sm));
    }
    return out;
}

namespace {

Mat trs(const std::array<float, 3>& t, const std::array<float, 4>& q, const std::array<float, 3>& s) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {(1 - 2 * (y * y + z * z)) * s[0], 2 * (x * y + z * w) * s[0], 2 * (x * z - y * w) * s[0], 0,
            2 * (x * y - z * w) * s[1], (1 - 2 * (x * x + z * z)) * s[1], 2 * (y * z + x * w) * s[1], 0,
            2 * (x * z + y * w) * s[2], 2 * (y * z - x * w) * s[2], (1 - 2 * (x * x + y * y)) * s[2], 0,
            t[0], t[1], t[2], 1};
}

// The skin a skeleton comes from: the first skin's joint nodes in skeleton() order, their parents there, and binds:
// each joint's rest transform in world space, where skinnedMeshes puts the vertices.
struct SkinJoints {
    std::vector<size_t> nodes;
    std::vector<Joint> joints;
    std::vector<size_t> parentNode;  // every node's parent, SIZE_MAX for roots
};

SkinJoints skinJoints(const Glb& g) {
    SkinJoints sj;
    if (!g.json.has("skins") || g.json["skins"].size() == 0) fail("the file has no skin (no rigged mesh)");
    const auto& nodes = g.json["nodes"].items();
    sj.parentNode.assign(nodes.size(), SIZE_MAX);
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].has("children"))
            for (auto& c : nodes[i]["children"].items())
                if (size_t(c.asNumber()) < nodes.size()) sj.parentNode[size_t(c.asNumber())] = i;
    const Json& s = g.json["skins"][size_t(0)];
    std::vector<size_t> jn;
    std::vector<Mat> bind;
    for (auto& joint : s["joints"].items()) {
        const size_t n = size_t(joint.asNumber());
        if (n >= nodes.size()) fail("a skin joint is not a node");
        Mat w = identity();
        for (size_t p = n, guard = 0; p != SIZE_MAX && guard < 1024; p = sj.parentNode[p], ++guard) w = mul(local(nodes[p]), w);
        jn.push_back(n);
        bind.push_back(w);
    }
    // Each joint's parent: its nearest ancestor that is a joint; its depth, for the order.
    std::map<size_t, size_t> index;
    for (size_t k = 0; k < jn.size(); ++k) index[jn[k]] = k;
    std::vector<int> parent(jn.size(), -1), depth(jn.size(), 0);
    for (size_t k = 0; k < jn.size(); ++k)
        for (size_t p = sj.parentNode[jn[k]], guard = 0; p != SIZE_MAX && guard < 1024; p = sj.parentNode[p], ++guard)
            if (index.count(p)) {
                parent[k] = int(index[p]);
                break;
            }
    for (size_t k = 0; k < jn.size(); ++k)
        for (int p = parent[k], guard = 0; p >= 0 && guard < 1024; p = parent[size_t(p)], ++guard) ++depth[k];
    std::vector<size_t> order(jn.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = k;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return depth[x] < depth[y]; });
    std::vector<int> at(jn.size());
    for (size_t k = 0; k < order.size(); ++k) at[order[k]] = int(k);
    for (size_t k : order) {
        Joint j;
        j.name = nodes[jn[k]].has("name") ? nodes[jn[k]]["name"].asString() : "joint" + std::to_string(k);
        j.parent = parent[k] < 0 ? -1 : at[size_t(parent[k])];
        j.bind = bind[k];
        sj.nodes.push_back(jn[k]);
        sj.joints.push_back(std::move(j));
    }
    return sj;
}

} // namespace

std::vector<Joint> skeleton(const Bytes& file) { return skinJoints(open(file)).joints; }

std::vector<JointAnimation> jointAnimations(const Bytes& file, float fps) {
    const Glb g = open(file);
    const SkinJoints sj = skinJoints(g);
    std::vector<JointAnimation> out;
    if (!g.json.has("animations")) return out;
    const auto& nodes = g.json["nodes"].items();
    // Every node's rest translation, rotation and scale (a node given as a matrix keeps it, unanimated).
    struct Trs {
        std::array<float, 3> t{0, 0, 0}, s{1, 1, 1};
        std::array<float, 4> r{0, 0, 0, 1};
    };
    std::vector<Trs> rest(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        const Json& n = nodes[i];
        for (size_t k = 0; k < 3; ++k) rest[i].t[k] = num(n["translation"], k, 0), rest[i].s[k] = num(n["scale"], k, 1);
        for (size_t k = 0; k < 4; ++k) rest[i].r[k] = num(n["rotation"], k, k == 3 ? 1.0f : 0.0f);
    }
    for (size_t ai = 0; ai < g.json["animations"].size(); ++ai) {
        const Json& a = g.json["animations"][ai];
        struct Channel {
            size_t node;
            int path;  // 0 translation, 1 rotation, 2 scale
            std::vector<double> times, values;
            int width;
            bool step, cubic;
        };
        std::vector<Channel> channels;
        float seconds = 0;
        for (auto& c : a["channels"].items()) {
            if (!c["target"].has("node")) continue;
            const std::string path = c["target"]["path"].asString();
            const int p = path == "translation" ? 0 : path == "rotation" ? 1 : path == "scale" ? 2 : -1;
            if (p < 0) continue;
            const Json& smp = a["samplers"][size_t(c["sampler"].asNumber())];
            Channel ch;
            ch.node = size_t(c["target"]["node"].asNumber());
            ch.path = p;
            ch.width = p == 1 ? 4 : 3;
            ch.times = accessor(g, size_t(smp["input"].asNumber()), 1);
            ch.values = accessor(g, size_t(smp["output"].asNumber()), ch.width);
            const std::string interp = smp.has("interpolation") ? smp["interpolation"].asString() : "LINEAR";
            ch.step = interp == "STEP";
            ch.cubic = interp == "CUBICSPLINE";
            if (ch.node >= nodes.size() || ch.times.empty()) continue;
            // Rotations stored as normalised integers.
            const int type = int(g.json["accessors"][size_t(smp["output"].asNumber())]["componentType"].asNumber());
            if (p == 1 && type != 5126)
                for (double& v : ch.values) v /= type == 5121 ? 255.0 : type == 5123 ? 65535.0 : 1.0;
            seconds = std::max(seconds, float(ch.times.back()));
            channels.push_back(std::move(ch));
        }
        if (channels.empty()) continue;
        JointAnimation ja;
        ja.name = a.has("name") ? a["name"].asString() : "animation" + std::to_string(ai);
        ja.seconds = seconds;
        const size_t frames = std::max<size_t>(2, size_t(std::lround(double(seconds) * fps)) + 1);
        for (size_t f = 0; f < frames; ++f) {
            const double t = std::min(double(seconds), double(f) / fps);
            std::vector<Trs> pose = rest;
            for (const Channel& ch : channels) {
                // Its keys around t (each a value, or for cubic splines {in tangent, value, out tangent}).
                const size_t n = ch.times.size();
                size_t k = 0;
                while (k + 1 < n && ch.times[k + 1] <= t) ++k;
                const size_t k1 = std::min(k + 1, n - 1);
                const double span = ch.times[k1] - ch.times[k];
                const double u = ch.step || span <= 0 ? 0.0 : std::clamp((t - ch.times[k]) / span, 0.0, 1.0);
                const size_t stride = ch.cubic ? 3 : 1, mid = ch.cubic ? 1 : 0;
                auto value = [&](size_t key, size_t c) {
                    const size_t at = (key * stride + mid) * size_t(ch.width) + c;
                    return at < ch.values.size() ? ch.values[at] : 0.0;
                };
                if (ch.path == 1) {
                    double q0[4], q1[4], dot = 0;
                    for (size_t c = 0; c < 4; ++c) q0[c] = value(k, c), q1[c] = value(k1, c), dot += q0[c] * q1[c];
                    const double sign = dot < 0 ? -1 : 1;
                    double q[4], len = 0;
                    for (size_t c = 0; c < 4; ++c) q[c] = q0[c] * (1 - u) + sign * q1[c] * u, len += q[c] * q[c];
                    len = len > 0 ? std::sqrt(len) : 1;
                    for (size_t c = 0; c < 4; ++c) pose[ch.node].r[c] = float(q[c] / len);
                } else {
                    auto& dst = ch.path == 0 ? pose[ch.node].t : pose[ch.node].s;
                    for (size_t c = 0; c < 3; ++c) dst[c] = float(value(k, c) * (1 - u) + value(k1, c) * u);
                }
            }
            std::vector<Mat> world(nodes.size());
            std::vector<char> done(nodes.size(), 0);
            std::function<const Mat&(size_t)> worldOf = [&](size_t i) -> const Mat& {
                if (!done[i]) {
                    const Mat own = nodes[i].has("matrix") ? local(nodes[i]) : trs(pose[i].t, pose[i].r, pose[i].s);
                    world[i] = sj.parentNode[i] == SIZE_MAX ? own : mul(worldOf(sj.parentNode[i]), own);
                    done[i] = 1;
                }
                return world[i];
            };
            std::vector<std::array<float, 16>> row;
            for (size_t n : sj.nodes) row.push_back(worldOf(n));
            ja.frames.push_back(std::move(row));
        }
        out.push_back(std::move(ja));
    }
    return out;
}

} // namespace nut::gltfobj
