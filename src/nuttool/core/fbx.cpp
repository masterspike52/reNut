#include "fbx.hpp"

#include "deflate.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>

namespace nut::fbx {
namespace {

// ---- binary FBX nodes -------------------------------------------------------------------------
// A node record: u32 end offset, u32 property count, u32 property bytes, u8 name length, name,
// properties, child records, and a 13-byte null record closing the child list.

void le32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
void le64(Bytes& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
void put32At(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i));
}
uint64_t bitsOf(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    return u;
}
uint32_t bitsOf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

struct Node {
    std::string name;
    Bytes props;
    uint32_t count = 0;
    std::vector<std::unique_ptr<Node>> kids;
    bool block = false; // always closed with a null record, even without children (objects: readers expect a scope)

    explicit Node(std::string n) : name(std::move(n)) {}
    Node& add(const std::string& n) {
        kids.push_back(std::make_unique<Node>(n));
        return *kids.back();
    }
    Node& I(int32_t v) { return tag('I'), le32(props, uint32_t(v)), *this; }
    Node& L(int64_t v) { return tag('L'), le64(props, uint64_t(v)), *this; }
    Node& D(double v) { return tag('D'), le64(props, bitsOf(v)), *this; }
    Node& C(bool v) { return tag('C'), props.push_back(v ? 1 : 0), *this; }
    Node& S(const std::string& s) {
        tag('S');
        le32(props, uint32_t(s.size()));
        props.insert(props.end(), s.begin(), s.end());
        return *this;
    }
    Node& R(const Bytes& b) {
        tag('R');
        le32(props, uint32_t(b.size()));
        props.insert(props.end(), b.begin(), b.end());
        return *this;
    }
    Node& ai(const std::vector<int32_t>& v) {
        Bytes raw;
        for (int32_t x : v) le32(raw, uint32_t(x));
        return array('i', v.size(), raw);
    }
    Node& al(const std::vector<int64_t>& v) {
        Bytes raw;
        for (int64_t x : v) le64(raw, uint64_t(x));
        return array('l', v.size(), raw);
    }
    Node& af(const std::vector<float>& v) {
        Bytes raw;
        for (float x : v) le32(raw, bitsOf(x));
        return array('f', v.size(), raw);
    }
    Node& ad(const std::vector<double>& v) {
        Bytes raw;
        for (double x : v) le64(raw, bitsOf(x));
        return array('d', v.size(), raw);
    }

private:
    void tag(char t) {
        props.push_back(uint8_t(t));
        ++count;
    }
    // Arrays: u32 length, u32 encoding (0 raw, 1 zlib), u32 byte size, data. Larger ones are compressed.
    Node& array(char t, size_t n, const Bytes& raw) {
        tag(t);
        le32(props, uint32_t(n));
        Bytes z = raw.size() >= 128 ? deflate::zlibCompress(raw.data(), raw.size()) : Bytes();
        bool packed = !z.empty() && z.size() < raw.size();
        const Bytes& data = packed ? z : raw;
        le32(props, packed ? 1 : 0);
        le32(props, uint32_t(data.size()));
        props.insert(props.end(), data.begin(), data.end());
        return *this;
    }
};

void serialize(Bytes& out, const Node& n) {
    size_t start = out.size();
    le32(out, 0);
    le32(out, n.count);
    le32(out, uint32_t(n.props.size()));
    out.push_back(uint8_t(n.name.size()));
    out.insert(out.end(), n.name.begin(), n.name.end());
    out.insert(out.end(), n.props.begin(), n.props.end());
    for (auto& k : n.kids) serialize(out, *k);
    if (!n.kids.empty() || n.count == 0 || n.block) out.insert(out.end(), 13, 0);
    put32At(out, start, uint32_t(out.size()));
}

// Properties70 entries.
Node& P(Node& p70, const char* name, const char* type, const char* label, const char* flags) {
    return p70.add("P").S(name).S(type).S(label).S(flags);
}
void pInt(Node& p, const char* name, int32_t v) { P(p, name, "int", "Integer", "").I(v); }
void pEnum(Node& p, const char* name, int32_t v) { P(p, name, "enum", "", "").I(v); }
void pBool(Node& p, const char* name, bool v) { P(p, name, "bool", "", "").I(v ? 1 : 0); }
void pDouble(Node& p, const char* name, double v) { P(p, name, "double", "Number", "").D(v); }
void pTime(Node& p, const char* name, int64_t v) { P(p, name, "KTime", "Time", "").L(v); }
void pString(Node& p, const char* name, const std::string& v) { P(p, name, "KString", "", "").S(v); }
void pColour(Node& p, const char* name, double r, double g, double b) { P(p, name, "Color", "", "A").D(r).D(g).D(b); }
void pVec(Node& p, const char* name, const char* type, const double* v) { P(p, name, type, "", "A").D(v[0]).D(v[1]).D(v[2]); }

// "name\0\1Class": how object names are stored in binary files.
std::string objName(const std::string& name, const char* cls) { return name + std::string("\0\1", 2) + cls; }

constexpr int64_t kTicksPerSecond = 46186158000LL;
int64_t ktime(double seconds) { return int64_t(std::llround(seconds * double(kTicksPerSecond))); }

// ---- transforms -----------------------------------------------------------------------------

using Mat4 = std::array<double, 16>; // column-major
Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a[size_t(k * 4 + row)] * b[size_t(c * 4 + k)];
            r[size_t(c * 4 + row)] = s;
        }
    return r;
}
void normalize(double q[4]) {
    double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0)
        for (int i = 0; i < 4; ++i) q[i] /= n;
    else
        q[0] = q[1] = q[2] = 0, q[3] = 1;
}
Mat4 trs(const double* t, const double* qIn, const double* s) {
    double q[4] = {qIn[0], qIn[1], qIn[2], qIn[3]};
    normalize(q);
    double x = q[0], y = q[1], z = q[2], w = q[3];
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
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    return m;
}
Mat4 rigidInverse(const Mat4& m) {
    Mat4 r = identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[size_t(j * 4 + i)] = m[size_t(i * 4 + j)];
    for (int i = 0; i < 3; ++i) r[size_t(12 + i)] = -(r[size_t(i)] * m[12] + r[size_t(4 + i)] * m[13] + r[size_t(8 + i)] * m[14]);
    return r;
}
std::vector<double> asArray(const Mat4& m) { return std::vector<double>(m.begin(), m.end()); }

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = 180.0 / kPi;

// XYZ Euler angles (radians) of a quaternion: R = Rz * Ry * Rx, FBX's default rotation order.
std::array<double, 3> euler(const double* qIn) {
    double q[4] = {qIn[0], qIn[1], qIn[2], qIn[3]};
    normalize(q);
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double m00 = 1 - 2 * (y * y + z * z), m10 = 2 * (x * y + z * w), m20 = 2 * (x * z - y * w);
    double m21 = 2 * (y * z + x * w), m22 = 1 - 2 * (x * x + y * y);
    double m12 = 2 * (y * z - x * w), m11 = 1 - 2 * (x * x + z * z);
    double b = std::asin(std::clamp(-m20, -1.0, 1.0));
    if (std::cos(b) > 1e-6) return {std::atan2(m21, m22), b, std::atan2(m10, m00)};
    return {std::atan2(-m12, m11), b, 0.0};
}

// The equivalent angles closest to `prev`, so curves do not jump by 360 degrees.
std::array<double, 3> continuous(std::array<double, 3> e, const std::array<double, 3>& prev) {
    auto wrapNear = [](double a, double ref) { return a + 2 * kPi * std::round((ref - a) / (2 * kPi)); };
    std::array<double, 3> alt = {e[0] + kPi, kPi - e[1], e[2] + kPi};
    double best = 1e300;
    std::array<double, 3> out = e;
    for (auto cand : {e, alt}) {
        double d = 0;
        for (int i = 0; i < 3; ++i) {
            cand[size_t(i)] = wrapNear(cand[size_t(i)], prev[size_t(i)]);
            d += std::abs(cand[size_t(i)] - prev[size_t(i)]);
        }
        if (d < best) best = d, out = cand;
    }
    return out;
}

// A bone's local transform at time t, as the viewer poses it (doc_model.cpp applyPose).
void sampleLocal(const model::Bone& b, const model::Animation& a, int track, double t, double tr[3], double q[4], double s[3]) {
    for (int c = 0; c < 3; ++c) tr[c] = b.local[c], s[c] = 1;
    for (int c = 0; c < 4; ++c) q[c] = b.rotation[c];
    if (track < 0 || uint32_t(track) >= a.bones || a.frames <= 0 || a.keys.size() < size_t(a.frames) * a.bones) return;
    int f0 = 0, f1 = 0;
    double w = 0;
    if (a.frameTimes.size() == size_t(a.frames) && a.frames > 1) {
        auto it = std::upper_bound(a.frameTimes.begin(), a.frameTimes.end(), float(t));
        f1 = std::clamp(int(it - a.frameTimes.begin()), 1, a.frames - 1);
        f0 = f1 - 1;
        double span = a.frameTimes[size_t(f1)] - a.frameTimes[size_t(f0)];
        w = span > 0 ? std::clamp((t - a.frameTimes[size_t(f0)]) / span, 0.0, 1.0) : 0.0;
    }
    const auto& k0 = a.keys[size_t(f0) * a.bones + size_t(track)];
    const auto& k1 = a.keys[size_t(f1) * a.bones + size_t(track)];
    double dot = 0;
    for (int c = 0; c < 4; ++c) dot += double(k0.rotation[c]) * k1.rotation[c];
    double sign = dot < 0 ? -1 : 1;
    for (int c = 0; c < 4; ++c) q[c] = k0.rotation[c] * (1 - w) + sign * k1.rotation[c] * w;
    for (int c = 0; c < 3; ++c) {
        tr[c] += k0.translation[c] * (1 - w) + k1.translation[c] * w;
        s[c] = k0.scale[c] * (1 - w) + k1.scale[c] * w;
    }
}

// Keeps the keys linear interpolation needs to stay within `tol` of every sample (a channel that
// never changes keeps one key).
void reduceKeys(const std::vector<float>& v, const std::vector<int64_t>& t, float tol, std::vector<int64_t>& kt, std::vector<float>& kv) {
    size_t n = v.size();
    if (std::all_of(v.begin(), v.end(), [&](float x) { return std::abs(x - v.front()) <= tol; })) {
        kt = {0};
        kv = {v.front()};
        return;
    }
    size_t a = 0;
    kt.push_back(t[0]);
    kv.push_back(v[0]);
    while (a + 1 < n) {
        // Furthest b whose straight line from a covers every sample between.
        size_t b = a + 1;
        for (size_t c = a + 2; c < n; ++c) {
            bool fits = true;
            for (size_t k = a + 1; k < c && fits; ++k) {
                double f = double(t[k] - t[a]) / double(t[c] - t[a]);
                fits = std::abs(v[a] + (v[c] - v[a]) * f - v[k]) <= tol;
            }
            if (!fits) break;
            b = c;
        }
        kt.push_back(t[b]);
        kv.push_back(v[b]);
        a = b;
    }
}

// ---- bone orientation ----------------------------------------------------------------------

using Quat = std::array<double, 4>; // x y z w
using V3 = std::array<double, 3>;

Quat qmul(const Quat& a, const Quat& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
Quat qconj(const Quat& q) { return {-q[0], -q[1], -q[2], q[3]}; }
V3 qrot(const Quat& q, const V3& v) {
    const double zero[3] = {0, 0, 0}, one[3] = {1, 1, 1};
    Mat4 r = trs(zero, q.data(), one);
    return {r[0] * v[0] + r[4] * v[1] + r[8] * v[2], r[1] * v[0] + r[5] * v[1] + r[9] * v[2], r[2] * v[0] + r[6] * v[1] + r[10] * v[2]};
}
double vlen(const V3& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// Per bone, the turn (in its game frame) that points `axis` (0-2 +X/+Y/+Z, 3-5 -X/-Y/-Z) at its main child: the one with
// the most bones under it, or the average of tied ones. Leaves carry on along the line from their parent.
std::vector<Quat> boneFixes(const model::Model& m, const std::vector<Mat4>& world, int axis) {
    size_t nb = m.bones.size();
    auto parentOf = [&](size_t i) {
        int p = m.bones[i].parent;
        return p >= 0 && size_t(p) < nb && size_t(p) != i ? p : -1;
    };
    std::vector<std::vector<size_t>> kids(nb);
    for (size_t i = 0; i < nb; ++i)
        if (int p = parentOf(i); p >= 0) kids[size_t(p)].push_back(i);
    std::vector<int> under(nb, -1);
    std::function<int(size_t, size_t)> count = [&](size_t i, size_t depth) -> int {
        if (under[i] >= 0 || depth > nb) return std::max(0, under[i]);
        int n = 0;
        for (size_t c : kids[i]) n += 1 + count(c, depth + 1);
        return under[i] = n;
    };
    auto pos = [&](size_t i) { return V3{world[i][12], world[i][13], world[i][14]}; };
    const double eps = 1e-4; // children closer than this (metres) give no direction
    std::vector<V3> dir(nb);
    std::vector<char> has(nb, 0), state(nb, 0);
    std::function<void(size_t)> solve = [&](size_t i) {
        if (state[i]) return;
        state[i] = 1;
        V3 p = pos(i), sum{};
        int best = -1;
        for (size_t c : kids[i]) {
            V3 q = pos(c), d = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
            double len = vlen(d);
            if (len < eps) continue;
            int n = count(c, 0);
            if (n > best) best = n, sum = {};
            if (n == best)
                for (int k = 0; k < 3; ++k) sum[size_t(k)] += d[size_t(k)] / len;
        }
        double len = vlen(sum);
        if (best >= 0 && len > 1e-9) {
            dir[i] = {sum[0] / len, sum[1] / len, sum[2] / len};
            has[i] = 1;
        } else if (int pi = parentOf(i); pi >= 0) {
            solve(size_t(pi));
            V3 q = pos(size_t(pi)), d = {p[0] - q[0], p[1] - q[1], p[2] - q[2]};
            double l = vlen(d);
            if (l >= eps) dir[i] = {d[0] / l, d[1] / l, d[2] / l}, has[i] = 1;
            else if (has[size_t(pi)]) dir[i] = dir[size_t(pi)], has[i] = 1;
        }
        state[i] = 2;
    };
    V3 a{};
    a[size_t(axis % 3)] = axis >= 3 ? -1 : 1;
    std::vector<Quat> fix(nb, Quat{0, 0, 0, 1});
    for (size_t i = 0; i < nb; ++i) {
        solve(i);
        if (!has[i]) continue;
        // The direction in the bone's own frame: the transpose of its world rotation applied.
        const Mat4& w = world[i];
        V3 l;
        for (int r = 0; r < 3; ++r) l[size_t(r)] = w[size_t(r * 4)] * dir[i][0] + w[size_t(r * 4 + 1)] * dir[i][1] + w[size_t(r * 4 + 2)] * dir[i][2];
        double ln = vlen(l);
        if (ln < 1e-9) continue;
        for (auto& x : l) x /= ln;
        double dot = a[0] * l[0] + a[1] * l[1] + a[2] * l[2];
        if (dot > 1 - 1e-9) continue;
        Quat q;
        if (dot < -1 + 1e-9) {
            q = axis % 3 == 0 ? Quat{0, 0, 1, 0} : Quat{1, 0, 0, 0}; // a half turn across the axis
        } else {
            q = {a[1] * l[2] - a[2] * l[1], a[2] * l[0] - a[0] * l[2], a[0] * l[1] - a[1] * l[0], 1 + dot}; // the shortest turn
            normalize(q.data());
        }
        fix[i] = q;
    }
    return fix;
}

std::string shortTextureName(const std::string& n) {
    for (const char* p : {"aid_texture_banjox_", "aid_texture_banjoxdlc_", "aid_texture_"})
        if (startsWith(n, p)) return n.substr(std::strlen(p));
    return n;
}

std::string shortModelName(const std::string& n) {
    for (const char* p : {"aid_model_banjox_", "aid_model_banjoxdlc_", "aid_model_"})
        if (startsWith(n, p)) return n.substr(std::strlen(p));
    return n;
}

// Fur layers baked over the meshes' first texture coordinates, as the viewer's fur shader draws them
// (gl_view.cpp): colour from root to tip by height, alpha 255 where the layer has fur.
std::vector<image::Rgba> bakeFur(const model::Model& m, const std::vector<size_t>& meshes, const std::vector<image::Rgba>& volume,
                                 const image::Rgba& root, const image::Rgba& tip, const image::Rgba* thinning, float tiling, uint32_t size,
                                 const std::array<float, 6>& tint) {
    struct Texel {
        bool on = false;
        float vu = 0, vv = 0; // fur volume coordinates
        uint8_t thin = 255;
        uint8_t root[3] = {}, tip[3] = {};
    };
    const int S = int(size);
    std::vector<Texel> tx(size_t(S) * size_t(S));
    auto fetch = [](const image::Rgba& img, float u, float v) -> const uint8_t* {
        static const uint8_t white[4] = {255, 255, 255, 255};
        if (!img.width || !img.height || img.pixels.size() < size_t(img.width) * img.height * 4) return white;
        int w = int(img.width), h = int(img.height);
        int x = int(std::floor(u * float(w))) % w, y = int(std::floor(v * float(h))) % h;
        if (x < 0) x += w;
        if (y < 0) y += h;
        return &img.pixels[(size_t(y) * size_t(w) + size_t(x)) * 4];
    };
    auto edge = [](float ax, float ay, float bx, float by, float cx, float cy) { return (bx - ax) * (cy - ay) - (cx - ax) * (by - ay); };
    for (size_t mi : meshes) {
        const auto& me = m.meshes[mi];
        size_t sets = me.layout.uvSets.size();
        for (size_t t = 0; t + 2 < me.indices.size(); t += 3) {
            const model::Vertex* c[3];
            bool valid = true;
            for (int q = 0; q < 3; ++q) {
                uint32_t i = me.indices[t + size_t(q)];
                valid &= i < me.vertices.size();
                c[q] = valid ? &me.vertices[i] : nullptr;
            }
            if (!valid) continue;
            float px[3], py[3], vu[3], vv[3], tu[3], tv[3];
            for (int q = 0; q < 3; ++q) {
                px[q] = c[q]->uv[0] * float(S);
                py[q] = c[q]->uv[1] * float(S);
                // Two sets: colour and thinning share set 0, the volume tiles over set 1. Three sets: thinning
                // uses set 1 and the volume set 2.
                const float* vs = sets >= 3 ? c[q]->uv3 : sets >= 2 ? c[q]->uv2 : c[q]->uv;
                const float* ts = sets >= 3 ? c[q]->uv2 : c[q]->uv;
                vu[q] = vs[0] * tiling;
                vv[q] = vs[1] * tiling;
                tu[q] = ts[0];
                tv[q] = ts[1];
            }
            float area = edge(px[0], py[0], px[1], py[1], px[2], py[2]);
            if (std::fabs(area) < 1e-9f) continue;
            int x0 = std::max(0, int(std::floor(std::min({px[0], px[1], px[2]})))), x1 = std::min(S - 1, int(std::ceil(std::max({px[0], px[1], px[2]}))));
            int y0 = std::max(0, int(std::floor(std::min({py[0], py[1], py[2]})))), y1 = std::min(S - 1, int(std::ceil(std::max({py[0], py[1], py[2]}))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
                    float w0 = edge(px[1], py[1], px[2], py[2], cx, cy) / area;
                    float w1 = edge(px[2], py[2], px[0], py[0], cx, cy) / area;
                    float w2 = 1 - w0 - w1;
                    if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                    Texel& e = tx[size_t(y) * size_t(S) + size_t(x)];
                    e.on = true;
                    e.vu = w0 * vu[0] + w1 * vu[1] + w2 * vu[2];
                    e.vv = w0 * vv[0] + w1 * vv[1] + w2 * vv[2];
                    if (thinning) e.thin = fetch(*thinning, w0 * tu[0] + w1 * tu[1] + w2 * tu[2], w0 * tv[0] + w1 * tv[1] + w2 * tv[2])[0];
                    const uint8_t *r = fetch(root, cx / float(S), cy / float(S)), *p = fetch(tip, cx / float(S), cy / float(S));
                    std::copy(r, r + 3, e.root);
                    std::copy(p, p + 3, e.tip);
                }
        }
    }
    // A volume without coverage (Banjo's DXT1 fur) gets the viewer's per-texel strand noise instead.
    bool noise = true;
    for (auto& sl : volume)
        for (size_t p = 3; p < sl.pixels.size() && noise; p += 4) noise = sl.pixels[p] == 255;
    auto hash = [](float x, float y) {
        float s = std::sin(x * 127.1f + y * 311.7f) * 43758.5453f;
        return s - std::floor(s);
    };
    std::vector<image::Rgba> out(volume.size());
    for (size_t k = 0; k < volume.size(); ++k) {
        image::Rgba& img = out[k];
        img.width = img.height = uint32_t(S);
        img.pixels.assign(size_t(S) * size_t(S) * 4, 0);
        float h = float(k + 1) / float(volume.size());
        for (size_t i = 0; i < tx.size(); ++i) {
            const Texel& e = tx[i];
            if (!e.on) continue;
            uint8_t* o = &img.pixels[i * 4];
            // The viewer's fur colour: mix(root, tip) * gain + offset.
            for (int ch = 0; ch < 3; ++ch) {
                float v = (float(e.root[ch]) * (1 - h) + float(e.tip[ch]) * h) / 255.0f * tint[size_t(ch)] + tint[size_t(3 + ch)];
                o[ch] = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
            }
            bool cover;
            if (thinning && h > float(e.thin) / 255.0f) cover = false;
            else if (noise) cover = hash(std::floor(e.vu * 96.0f), std::floor(e.vv * 96.0f)) > h * 1.1f;
            else cover = float(fetch(volume[k], e.vu, e.vv)[3]) / 255.0f >= 0.35f;
            o[3] = cover ? 255 : 0;
        }
    }
    return out;
}

// ---- shared by the FBX and OBJ writers ------------------------------------------------------

using Tint = std::array<float, 6>; // colour gain rgb, offset rgb
const Tint kNoTint = {1, 1, 1, 0, 0, 0};

// A mesh's colour tint; none when tinting is off.
Tint meshTint(const model::Mesh& me, bool on) {
    if (!on) return kNoTint;
    return {me.textureGain[0], me.textureGain[1], me.textureGain[2], me.textureOffset[0], me.textureOffset[1], me.textureOffset[2]};
}

// The PNG with `t` baked into its pixels (texture * gain + offset), as the game's shaders draw it.
std::optional<Bytes> tinted(std::optional<Bytes> png, const Tint& t, const std::string& name, std::vector<std::string>& notes) {
    if (!png || t == kNoTint) return png;
    try {
        image::Rgba img = image::decodePng(*png);
        for (size_t p = 0; p + 3 < img.pixels.size(); p += 4)
            for (int c = 0; c < 3; ++c) {
                float v = float(img.pixels[p + size_t(c)]) / 255.0f * t[size_t(c)] + t[size_t(3 + c)];
                img.pixels[p + size_t(c)] = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
            }
        return image::encodePng(img);
    } catch (std::exception& e) {
        notes.push_back("texture " + name + ": tint not applied (" + e.what() + ")");
        return png;
    }
}

double furRadius(const model::Model& m) {
    float dx = m.boundsMax[0] - m.boundsMin[0], dy = m.boundsMax[1] - m.boundsMin[1], dz = m.boundsMax[2] - m.boundsMin[2];
    return std::max(0.01, std::sqrt(double(dx) * dx + double(dy) * dy + double(dz) * dz) / 2);
}

// A mesh's vertex positions, pushed `push` out along their normals (fur shells).
std::vector<std::array<double, 3>> pushedPositions(const model::Mesh& me, double push) {
    std::vector<std::array<double, 3>> pos;
    pos.reserve(me.vertices.size());
    for (auto& v : me.vertices) {
        double n[3] = {v.normal[0], v.normal[1], v.normal[2]};
        double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        double k = push != 0 && len > 0.1 ? push / len : 0;
        pos.push_back({double(v.pos[0]) + n[0] * k, double(v.pos[1]) + n[1] * k, double(v.pos[2]) + n[2] * k});
    }
    return pos;
}

struct FurSet {
    std::vector<size_t> meshes;       // drawn with these layers
    std::vector<image::Rgba> layers;  // baked, root to tip
    std::string name;
};

// Fur draws bind {0 root colour, 1 fur volume, 2 tip colour, 3 thinning}. Meshes with the same textures, UV layout
// and tint share one set of baked layers.
std::vector<FurSet> furSets(const model::Model& m, const Options& opt, const std::function<bool(size_t)>& included) {
    std::map<int, std::optional<std::vector<image::Rgba>>> decoded;
    auto imagesOf = [&](int tex) -> const std::vector<image::Rgba>* {
        if (tex < 0 || size_t(tex) >= m.textures.size()) return nullptr;
        auto it = decoded.find(tex);
        if (it == decoded.end()) it = decoded.emplace(tex, opt.pixels(m.textures[size_t(tex)])).first;
        return it->second && !it->second->empty() ? &*it->second : nullptr;
    };
    auto slotTexture = [](const model::Mesh& me, int slot) {
        for (size_t k = 0; k < me.textures.size() && k < me.textureSlots.size(); ++k)
            if (me.textureSlots[k] == slot) return me.textures[k];
        return -1;
    };
    std::map<std::pair<std::array<int, 5>, Tint>, std::vector<size_t>> sets; // {root, tip, volume, thinning, UV sets}, tint -> meshes
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        if (!included(i)) continue;
        const auto& me = m.meshes[i];
        int vol = slotTexture(me, 1);
        const auto* vi = imagesOf(vol);
        if (!vi || vi->size() < 2) continue; // not a fur volume
        int root = slotTexture(me, 0), tip = slotTexture(me, 2), thin = slotTexture(me, 3);
        if (!imagesOf(root)) root = tip;
        if (!imagesOf(tip)) tip = root;
        if (!imagesOf(root)) continue;
        if (!imagesOf(thin)) thin = -1;
        sets[{{root, tip, vol, thin, int(me.layout.uvSets.size())}, meshTint(me, opt.tint)}].push_back(i);
    }
    std::vector<FurSet> out;
    for (auto& [setKey, meshes] : sets) {
        const auto& key = setKey.first;
        const image::Rgba* thin = key[3] >= 0 ? &imagesOf(key[3])->front() : nullptr;
        FurSet s;
        s.meshes = meshes;
        s.layers = bakeFur(m, meshes, *imagesOf(key[2]), imagesOf(key[0])->front(), imagesOf(key[1])->front(), thin, opt.furTiling,
                           std::max(16u, opt.furSize), setKey.second);
        s.name = "fur" + std::to_string(out.size()) + "_" + shortTextureName(m.textures[size_t(key[2])]);
        out.push_back(std::move(s));
    }
    return out;
}

bool meshIncluded(const model::Model& m, const Options& opt, size_t i) {
    const auto& me = m.meshes[i];
    bool use = i < opt.include.size() ? opt.include[i] : size_t(me.group) < m.groups.size() && m.groups[size_t(me.group)].shown;
    return use && !me.vertices.empty() && me.indices.size() >= 3;
}

std::string meshObjectName(const model::Model& m, size_t i) { return "group" + std::to_string(m.meshes[i].group) + "_mesh" + std::to_string(i); }

// World bind matrices of the bones; they follow the hierarchy, whatever order the bones are listed in.
std::vector<Mat4> bindWorlds(const model::Model& m) {
    size_t nb = m.bones.size();
    std::vector<Mat4> world(nb, identity());
    std::vector<int> state(nb, 0); // 0 todo, 1 in progress, 2 done
    std::function<void(size_t)> solve = [&](size_t i) {
        if (state[i] == 2) return;
        state[i] = 1;
        const auto& b = m.bones[i];
        double t[3] = {b.local[0], b.local[1], b.local[2]}, q[4] = {b.rotation[0], b.rotation[1], b.rotation[2], b.rotation[3]}, one[3] = {1, 1, 1};
        Mat4 local = trs(t, q, one);
        int p = b.parent;
        if (p >= 0 && size_t(p) < nb && state[size_t(p)] != 1) {
            solve(size_t(p));
            world[i] = mul(world[size_t(p)], local);
        } else {
            world[i] = local;
        }
        state[i] = 2;
    };
    for (size_t i = 0; i < nb; ++i) solve(i);
    return world;
}

} // namespace

Bytes write(const model::Model& m, const Options& opt, Report* report) {
    Report rep;
    int64_t nextId = 1000000;
    auto newId = [&] { return nextId++; };

    Node objects("Objects"), connections("Connections");
    auto oo = [&](int64_t child, int64_t parent) { connections.add("C").S("OO").L(child).L(parent); };
    auto op = [&](int64_t child, int64_t parent, const char* prop) { connections.add("C").S("OP").L(child).L(parent).S(prop); };
    std::map<std::string, int> objectCounts; // for Definitions
    auto object = [&](const char* type) -> Node& {
        ++objectCounts[type];
        Node& n = objects.add(type);
        n.block = true;
        return n;
    };
    auto modelNode = [&](int64_t id, const std::string& name, const char* kind) -> Node& {
        Node& n = object("Model").L(id).S(objName(name, "Model")).S(kind);
        n.add("Version").I(232);
        return n;
    };

    // Root: a Null named after the model, holding the skeleton and the meshes.
    std::string rootName = shortModelName(m.name);
    int64_t rootId = newId();
    {
        int64_t attr = newId();
        object("NodeAttribute").L(attr).S(objName(rootName, "NodeAttribute")).S("Null").add("TypeFlags").S("Null");
        Node& n = modelNode(rootId, rootName, "Null");
        n.add("Properties70");
        n.add("Shading").C(true);
        n.add("Culling").S("CullingOff");
        oo(attr, rootId);
        oo(rootId, 0);
    }

    // Skeleton. World bind matrices follow the hierarchy, whatever order the bones are listed in.
    size_t nb = m.bones.size();
    std::vector<int64_t> boneId(nb);
    std::vector<Mat4> bindWorld = bindWorlds(m);
    // Reoriented bones: each written frame is the game's turned by fix[i], so locals become fix[p]^-1 * local * fix[i].
    std::vector<Quat> fix(nb, Quat{0, 0, 0, 1});
    if (opt.reorient && nb) fix = boneFixes(m, bindWorld, std::clamp(opt.boneAxis, 0, 5));
    for (size_t i = 0; i < nb; ++i) {
        const double zero[3] = {0, 0, 0}, one[3] = {1, 1, 1};
        bindWorld[i] = mul(bindWorld[i], trs(zero, fix[i].data(), one));
    }
    // Scale axes follow the turn to the nearest game axis (exact for uniform scale).
    std::vector<std::array<int, 3>> scaleAxis(nb, {0, 1, 2});
    for (size_t i = 0; i < nb; ++i)
        for (int k = 0; k < 3; ++k) {
            V3 e{};
            e[size_t(k)] = 1;
            V3 g = qrot(fix[i], e);
            int best = 0;
            for (int j = 1; j < 3; ++j)
                if (std::abs(g[size_t(j)]) > std::abs(g[size_t(best)])) best = j;
            scaleAxis[i][size_t(k)] = best;
        }
    auto written = [&](size_t i, const double* t, const double* q, const double* s, double to[3], double qo[4], double so[3]) {
        int p = m.bones[i].parent;
        Quat fp = p >= 0 && size_t(p) < nb && size_t(p) != i ? fix[size_t(p)] : Quat{0, 0, 0, 1};
        V3 tt = qrot(qconj(fp), V3{t[0], t[1], t[2]});
        Quat qq = qmul(qmul(qconj(fp), Quat{q[0], q[1], q[2], q[3]}), fix[i]);
        for (int c = 0; c < 3; ++c) to[c] = tt[size_t(c)], so[c] = s[scaleAxis[i][size_t(c)]];
        for (int c = 0; c < 4; ++c) qo[c] = qq[size_t(c)];
    };
    for (size_t i = 0; i < nb; ++i) {
        const auto& b = m.bones[i];
        boneId[i] = newId();
        int64_t attr = newId();
        std::string name = b.name.empty() ? "bone" + std::to_string(i) : b.name;
        Node& a = object("NodeAttribute").L(attr).S(objName(name, "NodeAttribute")).S("LimbNode");
        a.add("TypeFlags").S("Skeleton");
        Node& n = modelNode(boneId[i], name, "LimbNode");
        Node& p70 = n.add("Properties70");
        double gt[3] = {b.local[0], b.local[1], b.local[2]}, gq[4] = {b.rotation[0], b.rotation[1], b.rotation[2], b.rotation[3]}, one[3] = {1, 1, 1};
        double t[3], q[4], s[3];
        written(i, gt, gq, one, t, q, s);
        auto e = euler(q);
        double r[3] = {e[0] * kDeg, e[1] * kDeg, e[2] * kDeg};
        pVec(p70, "Lcl Translation", "Lcl Translation", t);
        pVec(p70, "Lcl Rotation", "Lcl Rotation", r);
        n.add("Shading").C(true);
        n.add("Culling").S("CullingOff");
        oo(attr, boneId[i]);
    }
    for (size_t i = 0; i < nb; ++i) {
        int p = m.bones[i].parent;
        oo(boneId[i], p >= 0 && size_t(p) < nb && size_t(p) != i ? boneId[size_t(p)] : rootId);
    }
    rep.bones = nb;

    // End bones: a short child along the bone axis for each bone without one, since Blender sizes a bone by its children.
    std::vector<std::pair<int64_t, Mat4>> ends;
    if (opt.boneEnds && nb) {
        std::vector<char> hasChild(nb, 0);
        for (size_t i = 0; i < nb; ++i) {
            int p = m.bones[i].parent;
            if (p >= 0 && size_t(p) < nb && size_t(p) != i) hasChild[size_t(p)] = 1;
        }
        double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300}, extent = 0;
        for (auto& w : bindWorld)
            for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], w[size_t(12 + k)]), hi[k] = std::max(hi[k], w[size_t(12 + k)]);
        for (int k = 0; k < 3; ++k) extent = std::max(extent, hi[k] - lo[k]);
        double len = std::max(1e-4, double(opt.boneEndSize) * (extent > 0 ? extent : 1));
        int axis = opt.reorient ? std::clamp(opt.boneAxis, 0, 5) : 1;
        double t[3] = {0, 0, 0}, q0[4] = {0, 0, 0, 1}, one[3] = {1, 1, 1};
        t[axis % 3] = axis >= 3 ? -len : len;
        Mat4 local = trs(t, q0, one);
        for (size_t i = 0; i < nb; ++i) {
            if (hasChild[i]) continue;
            int64_t id = newId(), attr = newId();
            std::string name = (m.bones[i].name.empty() ? "bone" + std::to_string(i) : m.bones[i].name) + "_end";
            object("NodeAttribute").L(attr).S(objName(name, "NodeAttribute")).S("LimbNode").add("TypeFlags").S("Skeleton");
            Node& n = modelNode(id, name, "LimbNode");
            Node& p70 = n.add("Properties70");
            pVec(p70, "Lcl Translation", "Lcl Translation", t);
            n.add("Shading").C(true);
            n.add("Culling").S("CullingOff");
            oo(attr, id);
            oo(id, boneId[i]);
            ends.push_back({id, mul(bindWorld[i], local)});
        }
    }

    // A texture object with its video (the PNG, embedded when given).
    auto makeTexture = [&](const std::string& name, const std::string& file, const std::optional<Bytes>& png, bool alpha) -> int64_t {
        int64_t id = newId();
        Node& t = object("Texture").L(id).S(objName(name, "Texture")).S("");
        t.add("Type").S("TextureVideoClip");
        t.add("Version").I(202);
        t.add("TextureName").S(objName(name, "Texture"));
        Node& p70 = t.add("Properties70");
        pBool(p70, "UseMaterial", true);
        t.add("Media").S(objName(name, "Video"));
        t.add("FileName").S(file);
        t.add("RelativeFilename").S(file);
        t.add("ModelUVTranslation").D(0).D(0);
        t.add("ModelUVScaling").D(1).D(1);
        t.add("Texture_Alpha_Source").S(alpha ? "Alpha_Black" : "None");
        t.add("Cropping").I(0).I(0).I(0).I(0);
        int64_t vid = newId();
        Node& v = object("Video").L(vid).S(objName(name, "Video")).S("Clip");
        v.add("Type").S("Clip");
        Node& vp = v.add("Properties70");
        P(vp, "Path", "KString", "XRefUrl", "").S(file);
        v.add("UseMipMap").I(0);
        v.add("Filename").S(file);
        v.add("RelativeFilename").S(file);
        if (png) {
            v.add("Content").R(*png);
            ++rep.embedded;
        }
        oo(vid, id);
        ++rep.textures;
        return id;
    };
    auto tintOf = [&](const model::Mesh& me) { return meshTint(me, opt.tint); };
    const Tint noTint = kNoTint;
    // Colour maps, created on first use: one per texture and tint, the tint baked into its pixels.
    std::map<std::pair<int, Tint>, int64_t> textureId;
    std::map<int, int> tintCount;
    auto textureFor = [&](int tex, const Tint& tint) -> int64_t {
        auto it = textureId.find({tex, tint});
        if (it != textureId.end()) return it->second;
        const std::string& full = m.textures[size_t(tex)];
        std::optional<Bytes> png = opt.texture ? opt.texture(full) : std::nullopt;
        if (!png) rep.notes.push_back("texture " + full + " not embedded (not found)");
        std::string name = shortTextureName(full), file = full;
        if (tint != noTint) {
            int k = ++tintCount[tex];
            name += "_tint" + std::to_string(k);
            file += "_tint" + std::to_string(k);
            png = tinted(std::move(png), tint, full, rep.notes);
        }
        int64_t id = makeTexture(name, file + ".png", png, false);
        textureId[{tex, tint}] = id;
        return id;
    };

    // Materials: one per distinct colour map + tint + colour.
    std::map<std::tuple<int, Tint, float, float, float, std::string>, int64_t> materialId;
    std::map<std::string, int> materialNames;
    auto materialFor = [&](const model::Mesh& me) -> int64_t {
        int tex = me.texture >= 0 && size_t(me.texture) < m.textures.size() ? me.texture : -1;
        float r = 1, g = 1, b = 1;
        if (me.hasColour || tex < 0) r = me.colour[0], g = me.colour[1], b = me.colour[2];
        Tint tint = tex >= 0 ? tintOf(me) : noTint;
        auto key = std::make_tuple(tex, tint, r, g, b, me.material);
        auto it = materialId.find(key);
        if (it != materialId.end()) return it->second;
        std::string base = tex >= 0 ? shortTextureName(m.textures[size_t(tex)]) : me.material.empty() ? std::string("material") : me.material;
        int k = materialNames[base]++;
        std::string name = k ? base + "_" + std::to_string(k) : base;
        int64_t id = newId();
        Node& n = object("Material").L(id).S(objName(name, "Material")).S("");
        n.add("Version").I(102);
        n.add("ShadingModel").S("phong");
        n.add("MultiLayer").I(0);
        Node& p70 = n.add("Properties70");
        pColour(p70, "DiffuseColor", r, g, b);
        pDouble(p70, "DiffuseFactor", 1);
        pColour(p70, "SpecularColor", me.specular[0], me.specular[1], me.specular[2]);
        pDouble(p70, "SpecularFactor", 1);
        pDouble(p70, "Shininess", me.specularPower);
        pDouble(p70, "ShininessExponent", me.specularPower);
        pDouble(p70, "Opacity", 1);
        if (tex >= 0) op(textureFor(tex, tint), id, "DiffuseColor");
        ++rep.materials;
        materialId[key] = id;
        return id;
    };

    // Meshes. `push` moves every vertex out along its normal (fur shells); `second` writes the second UV set.
    std::vector<int64_t> meshModels;
    auto emitMesh = [&](const model::Mesh& me, const std::string& name, double push, int64_t material, bool second, bool shell) {
        size_t nv = me.vertices.size(), ni = me.indices.size() / 3 * 3;

        std::vector<double> pos, normals, uv, uv2;
        pos.reserve(nv * 3);
        for (auto& p : pushedPositions(me, push)) pos.insert(pos.end(), p.begin(), p.end());
        std::vector<int32_t> poly(ni), uvIndex(ni);
        normals.reserve(ni * 3);
        for (size_t k = 0; k < ni; ++k) {
            uint32_t vi = me.indices[k] < nv ? me.indices[k] : 0;
            poly[k] = k % 3 == 2 ? ~int32_t(vi) : int32_t(vi);
            uvIndex[k] = int32_t(vi);
            const auto& v = me.vertices[vi];
            normals.insert(normals.end(), {double(v.normal[0]), double(v.normal[1]), double(v.normal[2])});
        }
        for (auto& v : me.vertices) {
            uv.insert(uv.end(), {double(v.uv[0]), 1.0 - double(v.uv[1])});
            if (second) uv2.insert(uv2.end(), {double(v.uv2[0]), 1.0 - double(v.uv2[1])});
        }

        int64_t geo = newId();
        Node& g = object("Geometry").L(geo).S(objName(name, "Geometry")).S("Mesh");
        g.add("Properties70");
        g.add("GeometryVersion").I(124);
        g.add("Vertices").ad(pos);
        g.add("PolygonVertexIndex").ai(poly);
        {
            Node& ln = g.add("LayerElementNormal").I(0);
            ln.add("Version").I(101);
            ln.add("Name").S("");
            ln.add("MappingInformationType").S("ByPolygonVertex");
            ln.add("ReferenceInformationType").S("Direct");
            ln.add("Normals").ad(normals);
        }
        auto uvLayer = [&](int index, const char* layerName, const std::vector<double>& data) {
            Node& lu = g.add("LayerElementUV").I(index);
            lu.add("Version").I(101);
            lu.add("Name").S(layerName);
            lu.add("MappingInformationType").S("ByPolygonVertex");
            lu.add("ReferenceInformationType").S("IndexToDirect");
            lu.add("UV").ad(data);
            lu.add("UVIndex").ai(uvIndex);
        };
        uvLayer(0, "UVMap", uv);
        if (second) uvLayer(1, "UVMap2", uv2);
        {
            Node& lm = g.add("LayerElementMaterial").I(0);
            lm.add("Version").I(101);
            lm.add("Name").S("");
            lm.add("MappingInformationType").S("AllSame");
            lm.add("ReferenceInformationType").S("IndexToDirect");
            lm.add("Materials").ai({0});
        }
        {
            Node& layer = g.add("Layer").I(0);
            layer.add("Version").I(100);
            for (const char* t : {"LayerElementNormal", "LayerElementMaterial", "LayerElementUV"}) {
                Node& e = layer.add("LayerElement");
                e.add("Type").S(t);
                e.add("TypedIndex").I(0);
            }
        }
        if (second) {
            Node& layer = g.add("Layer").I(1);
            layer.add("Version").I(100);
            Node& e = layer.add("LayerElement");
            e.add("Type").S("LayerElementUV");
            e.add("TypedIndex").I(1);
        }

        int64_t mid = newId();
        Node& mn = modelNode(mid, name, "Mesh");
        mn.add("Properties70");
        mn.add("Shading").C(true);
        mn.add("Culling").S("CullingOff");
        oo(mid, rootId);
        oo(geo, mid);
        oo(material, mid);
        meshModels.push_back(mid);

        // Skin: one cluster per bone the mesh uses.
        if (me.layout.skinned && nb) {
            std::map<size_t, std::map<int32_t, double>> perBone;
            for (size_t v = 0; v < nv; ++v)
                for (int k = 0; k < 4; ++k) {
                    float w = me.vertices[v].weights[k];
                    size_t b = me.vertices[v].bones[k];
                    if (w > 0 && b < nb) perBone[b][int32_t(v)] += w;
                }
            if (!perBone.empty()) {
                int64_t skin = newId();
                Node& s = object("Deformer").L(skin).S(objName(name, "Deformer")).S("Skin");
                s.add("Version").I(101);
                s.add("Link_DeformAcuracy").D(50);
                oo(skin, geo);
                for (auto& [b, weights] : perBone) {
                    std::vector<int32_t> idx;
                    std::vector<double> wts;
                    for (auto& [v, w] : weights) idx.push_back(v), wts.push_back(w);
                    int64_t cl = newId();
                    Node& c = object("Deformer").L(cl).S(objName(m.bones[b].name, "SubDeformer")).S("Cluster");
                    c.add("Version").I(100);
                    c.add("UserData").S("").S("");
                    c.add("Indexes").ai(idx);
                    c.add("Weights").ad(wts);
                    // As other exporters write it: the mesh relative to the bone, and the bone's world matrix, at bind time.
                    c.add("Transform").ad(asArray(rigidInverse(bindWorld[b])));
                    c.add("TransformLink").ad(asArray(bindWorld[b]));
                    oo(cl, skin);
                    oo(boneId[b], cl);
                }
            }
        }
        if (shell) {
            ++rep.furShells;
            return;
        }
        ++rep.meshes;
        rep.vertices += nv;
        rep.triangles += ni / 3;
    };
    auto included = [&](size_t i) { return meshIncluded(m, opt, i); };
    auto meshName = [&](size_t i) { return meshObjectName(m, i); };
    for (size_t i = 0; i < m.meshes.size(); ++i)
        if (included(i)) emitMesh(m.meshes[i], meshName(i), 0, materialFor(m.meshes[i]), m.meshes[i].layout.uvSets.size() >= 2, false);
    if (!rep.meshes) rep.notes.push_back("no meshes written");

    // Fur shells: one shell mesh per baked layer, pushed out by its height.
    if (opt.fur && opt.pixels) {
        double radius = furRadius(m);
        std::vector<FurSet> sets = furSets(m, opt, included);
        for (auto& set : sets) {
            const auto& layers = set.layers;
            const auto& meshes = set.meshes;
            for (size_t k = 0; k < layers.size(); ++k) {
                std::string name = set.name + "_layer" + std::to_string(k + 1);
                int64_t tex = makeTexture(name, name + ".png", image::encodePng(layers[k]), true);
                int64_t mat = newId();
                Node& n = object("Material").L(mat).S(objName(name, "Material")).S("");
                n.add("Version").I(102);
                n.add("ShadingModel").S("phong");
                n.add("MultiLayer").I(0);
                Node& p70 = n.add("Properties70");
                pColour(p70, "DiffuseColor", 1, 1, 1);
                pDouble(p70, "DiffuseFactor", 1);
                pDouble(p70, "Opacity", 1);
                op(tex, mat, "DiffuseColor");
                op(tex, mat, "TransparencyFactor"); // its alpha is the fur's coverage
                ++rep.materials;
                double h = double(k + 1) / double(layers.size());
                for (size_t i : meshes) emitMesh(m.meshes[i], meshName(i) + "_fur" + std::to_string(k + 1), opt.furLength * radius * h, mat, false, true);
            }
        }
        if (sets.empty()) rep.notes.push_back("no fur: none of the written meshes binds a fur volume");
    }

    // Bind pose.
    if (nb) {
        int64_t id = newId();
        Node& p = object("Pose").L(id).S(objName(rootName, "Pose")).S("BindPose");
        p.add("Type").S("BindPose");
        p.add("Version").I(100);
        p.add("NbPoseNodes").I(int32_t(1 + nb + ends.size() + meshModels.size()));
        auto poseNode = [&](int64_t node, const Mat4& mat) {
            Node& pn = p.add("PoseNode");
            pn.add("Node").L(node);
            pn.add("Matrix").ad(asArray(mat));
        };
        poseNode(rootId, identity());
        for (size_t i = 0; i < nb; ++i) poseNode(boneId[i], bindWorld[i]);
        for (auto& [eid, ew] : ends) poseNode(eid, ew);
        for (int64_t mm : meshModels) poseNode(mm, identity());
    }

    // Animations: one stack + base layer each; curves only for matched bones.
    double longest = 0;
    std::string firstTake;
    const float fps = opt.fps > 0 ? opt.fps : 30;
    for (auto& na : opt.animations) {
        if (!na.anim) continue;
        const model::Animation& a = *na.anim;
        std::map<std::string, int> byName;
        for (size_t t = 0; t < a.boneNames.size(); ++t) byName.emplace(a.boneNames[t], int(t));
        double dur = std::max(0.0f, a.duration);
        int frames = dur > 0 ? std::max(1, int(std::lround(dur * fps))) + 1 : 1;
        std::vector<int64_t> times(static_cast<size_t>(frames));
        std::vector<double> secs(static_cast<size_t>(frames));
        for (int f = 0; f < frames; ++f) {
            secs[size_t(f)] = std::min(dur, f / double(fps));
            times[size_t(f)] = ktime(secs[size_t(f)]);
        }
        int64_t stack = newId(), layer = newId();
        Node& st = object("AnimationStack").L(stack).S(objName(na.name, "AnimStack")).S("");
        Node& sp = st.add("Properties70");
        pTime(sp, "LocalStart", 0);
        pTime(sp, "LocalStop", ktime(dur));
        pTime(sp, "ReferenceStart", 0);
        pTime(sp, "ReferenceStop", ktime(dur));
        object("AnimationLayer").L(layer).S(objName("BaseLayer", "AnimLayer")).S("");
        oo(layer, stack);
        if (firstTake.empty()) firstTake = na.name;
        longest = std::max(longest, dur);

        int matched = 0;
        for (size_t b = 0; b < nb; ++b) {
            auto it = byName.find(m.bones[b].name);
            if (it == byName.end()) continue;
            ++matched;
            std::vector<float> ch[9]; // tx ty tz, rx ry rz (degrees), sx sy sz
            for (auto& c : ch) c.reserve(size_t(frames));
            std::array<double, 3> prev{};
            for (int f = 0; f < frames; ++f) {
                double gt[3], gq[4], gs[3], tr[3], q[4], s[3];
                sampleLocal(m.bones[b], a, it->second, secs[size_t(f)], gt, gq, gs);
                written(b, gt, gq, gs, tr, q, s);
                auto e = euler(q);
                if (f) e = continuous(e, prev);
                prev = e;
                for (int c = 0; c < 3; ++c) {
                    ch[c].push_back(float(tr[c]));
                    ch[3 + c].push_back(float(e[size_t(c)] * kDeg));
                    ch[6 + c].push_back(float(s[c]));
                }
            }
            const char* props[3] = {"Lcl Translation", "Lcl Rotation", "Lcl Scaling"};
            const char* nodeNames[3] = {"T", "R", "S"};
            const char* axes[3] = {"d|X", "d|Y", "d|Z"};
            // Channels that stay at the bone's rest transform need no curves: importers take those
            // values from the bone (Blender fills every un-keyed channel of an animated bone that way).
            const auto& bone = m.bones[b];
            double gt[3] = {bone.local[0], bone.local[1], bone.local[2]}, gq[4] = {bone.rotation[0], bone.rotation[1], bone.rotation[2], bone.rotation[3]};
            double one[3] = {1, 1, 1}, rt[3], rq[4], rs[3];
            written(b, gt, gq, one, rt, rq, rs);
            auto re = euler(rq);
            const double rest[9] = {rt[0], rt[1], rt[2], re[0] * kDeg, re[1] * kDeg, re[2] * kDeg, 1, 1, 1};
            for (int kind = 0; kind < 3; ++kind) {
                float tol = kind == 1 ? 0.01f : 1e-4f;
                bool atRest = true;
                for (int c = 0; c < 3 && atRest; ++c)
                    for (float x : ch[kind * 3 + c])
                        if (std::abs(double(x) - rest[kind * 3 + c]) > tol) {
                            atRest = false;
                            break;
                        }
                if (atRest) continue;
                int64_t cn = newId();
                Node& n = object("AnimationCurveNode").L(cn).S(objName(nodeNames[kind], "AnimCurveNode")).S("");
                Node& p70 = n.add("Properties70");
                for (int c = 0; c < 3; ++c) P(p70, axes[c], "Number", "", "A").D(ch[kind * 3 + c].front());
                oo(cn, layer);
                op(cn, boneId[b], props[kind]);
                for (int c = 0; c < 3; ++c) {
                    const auto& v = ch[kind * 3 + c];
                    std::vector<int64_t> kt;
                    std::vector<float> kv;
                    reduceKeys(v, times, tol, kt, kv);
                    int64_t cv = newId();
                    Node& curve = object("AnimationCurve").L(cv).S(objName("", "AnimCurve")).S("");
                    curve.add("Default").D(v.front());
                    curve.add("KeyVer").I(4009);
                    curve.add("KeyTime").al(kt);
                    curve.add("KeyValueFloat").af(kv);
                    // Linear interpolation, as Blender's exporter writes it.
                    curve.add("KeyAttrFlags").ai({24836});
                    float extra;
                    uint32_t bits = 218434821u;
                    std::memcpy(&extra, &bits, 4);
                    curve.add("KeyAttrDataFloat").af({0, 0, extra, 0});
                    curve.add("KeyAttrRefCount").ai({int32_t(kt.size())});
                    op(cv, cn, axes[c]);
                }
            }
        }
        if (!matched) rep.notes.push_back("animation " + na.name + " matches none of the model's bones");
        ++rep.animations;
    }

    // ---- file ----
    Bytes out;
    const char magic[] = "Kaydara FBX Binary  ";
    out.insert(out.end(), magic, magic + 20);
    out.insert(out.end(), {0x00, 0x1a, 0x00});
    le32(out, 7400);

    {
        Node h("FBXHeaderExtension");
        h.add("FBXHeaderVersion").I(1003);
        h.add("FBXVersion").I(7400);
        h.add("EncryptionType").I(0);
        Node& ts = h.add("CreationTimeStamp");
        ts.add("Version").I(1000);
        static const std::pair<const char*, int> stamp[] = {{"Year", 1970}, {"Month", 1}, {"Day", 1}, {"Hour", 10}, {"Minute", 0}, {"Second", 0}, {"Millisecond", 0}};
        for (auto& [k, v] : stamp) ts.add(k).I(v);
        h.add("Creator").S("NutTool");
        serialize(out, h);
    }
    // The file id and creation time pair with the footer below (as Blender writes them).
    {
        static const uint8_t fileId[16] = {0x28, 0xb3, 0x2a, 0xeb, 0xb6, 0x24, 0xcc, 0xc2, 0xbf, 0xc8, 0xb0, 0x2a, 0xa9, 0x2b, 0xfc, 0xf1};
        Node f("FileId");
        f.R(Bytes(fileId, fileId + 16));
        serialize(out, f);
        Node t("CreationTime");
        t.S("1970-01-01 10:00:00:000");
        serialize(out, t);
        Node c("Creator");
        c.S("NutTool");
        serialize(out, c);
    }
    {
        Node gs("GlobalSettings");
        gs.add("Version").I(1000);
        Node& p = gs.add("Properties70");
        pInt(p, "UpAxis", 1);
        pInt(p, "UpAxisSign", 1);
        pInt(p, "FrontAxis", 2);
        pInt(p, "FrontAxisSign", 1);
        pInt(p, "CoordAxis", 0);
        pInt(p, "CoordAxisSign", 1);
        pInt(p, "OriginalUpAxis", 1);
        pInt(p, "OriginalUpAxisSign", 1);
        pDouble(p, "UnitScaleFactor", opt.unitScale);
        pDouble(p, "OriginalUnitScaleFactor", opt.unitScale);
        pColour(p, "AmbientColor", 0, 0, 0);
        pString(p, "DefaultCamera", "Producer Perspective");
        pEnum(p, "TimeMode", 6); // 30 frames a second
        pTime(p, "TimeSpanStart", 0);
        pTime(p, "TimeSpanStop", ktime(longest > 0 ? longest : 1));
        pDouble(p, "CustomFrameRate", fps);
        serialize(out, gs);
    }
    {
        Node d("Documents");
        d.add("Count").I(1);
        int64_t docId = newId();
        Node& doc = d.add("Document").L(docId).S("").S("Scene");
        Node& p = doc.add("Properties70");
        P(p, "SourceObject", "object", "", "");
        pString(p, "ActiveAnimStackName", firstTake);
        doc.add("RootNode").L(0);
        serialize(out, d);
    }
    {
        Node r("References");
        serialize(out, r);
    }
    {
        Node d("Definitions");
        d.add("Version").I(100);
        int total = 1;
        for (auto& [k, v] : objectCounts) total += v;
        d.add("Count").I(total);
        d.add("ObjectType").S("GlobalSettings").add("Count").I(1);
        for (auto& [k, v] : objectCounts) d.add("ObjectType").S(k).add("Count").I(v);
        serialize(out, d);
    }
    serialize(out, objects);
    serialize(out, connections);
    {
        Node t("Takes");
        t.add("Current").S(firstTake);
        for (auto& na : opt.animations) {
            if (!na.anim) continue;
            Node& take = t.add("Take").S(na.name);
            take.add("FileName").S(na.name + ".tak");
            int64_t stop = ktime(std::max(0.0f, na.anim->duration));
            take.add("LocalTime").L(0).L(stop);
            take.add("ReferenceTime").L(0).L(stop);
        }
        serialize(out, t);
    }
    out.insert(out.end(), 13, 0); // end of the top-level records

    // Footer.
    static const uint8_t footId[16] = {0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66, 0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e};
    out.insert(out.end(), footId, footId + 16);
    out.insert(out.end(), 4, 0);
    size_t pad = ((out.size() + 15) & ~size_t(15)) - out.size();
    out.insert(out.end(), pad ? pad : 16, 0);
    le32(out, 7400);
    out.insert(out.end(), 120, 0);
    static const uint8_t footMagic[16] = {0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e, 0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b};
    out.insert(out.end(), footMagic, footMagic + 16);

    if (report) *report = std::move(rep);
    return out;
}

// ---- OBJ ------------------------------------------------------------------------------------

ObjFiles writeObj(const model::Model& m, const Options& opt, const std::string& base, Report* report) {
    Report rep;
    ObjFiles f;
    std::string& o = f.obj;
    std::string& mt = f.mtl;
    char t[256];
    o = "# " + m.name + " (exported by nuttool)\nmtllib " + base + ".mtl\n";
    mt = "# " + m.name + " (exported by nuttool)\n";

    // Colour maps: one PNG per texture and tint, the tint baked in, written next to the .obj.
    std::map<std::pair<int, Tint>, std::string> fileOf;
    std::map<int, int> tintCount;
    auto textureFile = [&](int tex, const Tint& tint) -> std::string {
        auto it = fileOf.find({tex, tint});
        if (it != fileOf.end()) return it->second;
        const std::string& full = m.textures[size_t(tex)];
        std::string file = full + (tint != kNoTint ? "_tint" + std::to_string(++tintCount[tex]) : std::string()) + ".png";
        std::optional<Bytes> png = tinted(opt.texture ? opt.texture(full) : std::nullopt, tint, full, rep.notes);
        if (png) {
            f.files.push_back({file, std::move(*png)});
            ++rep.textures;
        } else {
            rep.notes.push_back("texture " + full + " not written (not found)");
        }
        return fileOf[{tex, tint}] = file;
    };
    std::map<std::string, int> materialNames;
    auto newMaterial = [&](const std::string& b) {
        int k = materialNames[b]++;
        std::string name = k ? b + "_" + std::to_string(k) : b;
        mt += "\nnewmtl " + name + "\n";
        ++rep.materials;
        return name;
    };
    // Materials: one per distinct colour map + tint + colour, as in the FBX.
    std::map<std::tuple<int, Tint, float, float, float, std::string>, std::string> materialOf;
    auto materialFor = [&](const model::Mesh& me) -> std::string {
        int tex = me.texture >= 0 && size_t(me.texture) < m.textures.size() ? me.texture : -1;
        float r = 1, g = 1, b = 1;
        if (me.hasColour || tex < 0) r = me.colour[0], g = me.colour[1], b = me.colour[2];
        Tint tint = tex >= 0 ? meshTint(me, opt.tint) : kNoTint;
        auto key = std::make_tuple(tex, tint, r, g, b, me.material);
        auto it = materialOf.find(key);
        if (it != materialOf.end()) return it->second;
        std::string name = newMaterial(tex >= 0 ? shortTextureName(m.textures[size_t(tex)]) : me.material.empty() ? std::string("material") : me.material);
        std::snprintf(t, sizeof t, "Kd %g %g %g\nKs %g %g %g\nNs %g\nd 1\nillum 2\n", r, g, b, me.specular[0], me.specular[1], me.specular[2],
                      std::clamp(double(me.specularPower), 0.0, 1000.0));
        mt += t;
        if (tex >= 0) mt += "map_Kd " + textureFile(tex, tint) + "\n";
        return materialOf[key] = name;
    };

    size_t next = 1; // OBJ indices count from 1 across the file
    auto emit = [&](const model::Mesh& me, const std::string& name, double push, const std::string& material, bool shell) {
        size_t nv = me.vertices.size(), ni = me.indices.size() / 3 * 3;
        o += "o " + name + "\n";
        for (auto& p : pushedPositions(me, push)) {
            std::snprintf(t, sizeof t, "v %.9g %.9g %.9g\n", p[0], p[1], p[2]);
            o += t;
        }
        for (auto& v : me.vertices) {
            std::snprintf(t, sizeof t, "vt %.9g %.9g\n", v.uv[0], 1.0f - v.uv[1]);
            o += t;
        }
        for (auto& v : me.vertices) {
            std::snprintf(t, sizeof t, "vn %g %g %g\n", v.normal[0], v.normal[1], v.normal[2]);
            o += t;
        }
        o += "usemtl " + material + "\n";
        for (size_t k = 0; k < ni; k += 3) {
            size_t c[3];
            for (int q = 0; q < 3; ++q) c[q] = next + (me.indices[k + size_t(q)] < nv ? me.indices[k + size_t(q)] : 0);
            std::snprintf(t, sizeof t, "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n", c[0], c[0], c[0], c[1], c[1], c[1], c[2], c[2], c[2]);
            o += t;
        }
        next += nv;
        if (shell) {
            ++rep.furShells;
            return;
        }
        ++rep.meshes;
        rep.vertices += nv;
        rep.triangles += ni / 3;
    };
    auto included = [&](size_t i) { return meshIncluded(m, opt, i); };
    for (size_t i = 0; i < m.meshes.size(); ++i)
        if (included(i)) emit(m.meshes[i], meshObjectName(m, i), 0, materialFor(m.meshes[i]), false);
    if (!rep.meshes) rep.notes.push_back("no meshes written");

    // Fur shells, as in the FBX; each layer's alpha (map_d) is where it has strands.
    if (opt.fur && opt.pixels) {
        double radius = furRadius(m);
        std::vector<FurSet> sets = furSets(m, opt, included);
        for (auto& set : sets)
            for (size_t k = 0; k < set.layers.size(); ++k) {
                std::string name = set.name + "_layer" + std::to_string(k + 1), file = name + ".png";
                f.files.push_back({file, image::encodePng(set.layers[k])});
                ++rep.textures;
                std::string mat = newMaterial(name);
                mt +="Kd 1 1 1\nKs 0 0 0\nd 1\nillum 1\nmap_Kd " + file + "\nmap_d " + file + "\n";
                double h = double(k + 1) / double(set.layers.size());
                for (size_t i : set.meshes) emit(m.meshes[i], meshObjectName(m, i) + "_fur" + std::to_string(k + 1), opt.furLength * radius * h, mat, true);
            }
        if (sets.empty()) rep.notes.push_back("no fur: none of the written meshes binds a fur volume");
    }

    if (report) *report = std::move(rep);
    return f;
}

// ---- reading --------------------------------------------------------------------------------

namespace {

struct RProp {
    char type = 0;
    int64_t i = 0;
    double d = 0;
    std::string s;            // S and R
    std::vector<double> nums; // arrays
};

struct RNode {
    std::string name;
    std::vector<RProp> props;
    std::vector<RNode> kids;
    const RNode* kid(const char* n) const {
        for (auto& k : kids)
            if (k.name == n) return &k;
        return nullptr;
    }
    std::string str(size_t k) const { return k < props.size() ? props[k].s : std::string(); }
    int64_t num(size_t k) const { return k < props.size() ? props[k].i : 0; }
};

uint64_t rdLe(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t k = n; k-- > 0;) v = v << 8 | p[k];
    return v;
}

// One node record and its children; false for the null record that closes a child list.
bool readNode(const Bytes& b, size_t& at, bool wide, RNode& out, int depth) {
    const size_t w = wide ? 8 : 4, hdr = 3 * w + 1;
    if (at > b.size() || hdr > b.size() - at) fail("FBX: the file is cut short");
    uint64_t end = rdLe(&b[at], w), count = rdLe(&b[at + w], w), plen = rdLe(&b[at + 2 * w], w);
    uint8_t nlen = b[at + 3 * w];
    if (!end && !count && !plen && !nlen) {
        at += hdr;
        return false;
    }
    if (end > b.size() || plen > b.size() || end < at + hdr + nlen + plen || depth > 64) fail("FBX: broken node record");
    size_t p = at + hdr;
    out.name.assign(reinterpret_cast<const char*>(&b[p]), nlen);
    p += nlen;
    const size_t propsEnd = p + size_t(plen);
    for (uint64_t k = 0; k < count; ++k) {
        if (p >= propsEnd) fail("FBX: broken property list in " + out.name);
        RProp pr;
        pr.type = char(b[p++]);
        auto scalar = [&](size_t n) {
            if (n > propsEnd - p) fail("FBX: broken property list in " + out.name);
            uint64_t v = rdLe(&b[p], n);
            p += n;
            return v;
        };
        switch (pr.type) {
        case 'C': pr.i = int64_t(scalar(1)); break;
        case 'Y': pr.i = int16_t(scalar(2)); break;
        case 'I': pr.i = int32_t(scalar(4)); break;
        case 'L': pr.i = int64_t(scalar(8)); break;
        case 'F': {
            uint32_t u = uint32_t(scalar(4));
            float f;
            std::memcpy(&f, &u, 4);
            pr.d = f;
            break;
        }
        case 'D': {
            uint64_t u = scalar(8);
            std::memcpy(&pr.d, &u, 8);
            break;
        }
        case 'S':
        case 'R': {
            size_t n = size_t(scalar(4));
            if (n > propsEnd - p) fail("FBX: broken string in " + out.name);
            pr.s.assign(reinterpret_cast<const char*>(b.data() + p), n);
            p += n;
            break;
        }
        case 'f':
        case 'd':
        case 'l':
        case 'i':
        case 'b': {
            size_t n = size_t(scalar(4)), enc = size_t(scalar(4)), clen = size_t(scalar(4));
            size_t es = pr.type == 'd' || pr.type == 'l' ? 8 : pr.type == 'b' ? 1 : 4;
            if (clen > propsEnd - p || n > (size_t(1) << 31) / es) fail("FBX: broken array in " + out.name);
            Bytes raw;
            if (enc == 1) raw = deflate::zlibDecompress(b.data() + p, clen, nullptr, n * es);
            else if (enc == 0) raw.assign(b.begin() + std::ptrdiff_t(p), b.begin() + std::ptrdiff_t(p + clen));
            else fail("FBX: unknown array encoding in " + out.name);
            if (raw.size() < n * es) fail("FBX: array shorter than its length in " + out.name);
            pr.nums.resize(n);
            for (size_t e = 0; e < n; ++e) {
                const uint8_t* q = raw.data() + e * es;
                switch (pr.type) {
                case 'f': {
                    uint32_t u = uint32_t(rdLe(q, 4));
                    float f;
                    std::memcpy(&f, &u, 4);
                    pr.nums[e] = f;
                    break;
                }
                case 'd': {
                    uint64_t u = rdLe(q, 8);
                    std::memcpy(&pr.nums[e], &u, 8);
                    break;
                }
                case 'l': pr.nums[e] = double(int64_t(rdLe(q, 8))); break;
                case 'i': pr.nums[e] = double(int32_t(uint32_t(rdLe(q, 4)))); break;
                default: pr.nums[e] = q[0]; break;
                }
            }
            p += clen;
            break;
        }
        default: fail(std::string("FBX: unknown property type '") + pr.type + "' in " + out.name);
        }
        if (pr.type == 'F' || pr.type == 'D') pr.i = std::isfinite(pr.d) && std::fabs(pr.d) < 9e18 ? int64_t(pr.d) : 0;
        else pr.d = double(pr.i);
        out.props.push_back(std::move(pr));
    }
    p = propsEnd;
    while (p < end) {
        RNode k;
        if (!readNode(b, p, wide, k, depth + 1)) break;
        out.kids.push_back(std::move(k));
    }
    at = size_t(end);
    return true;
}

const std::vector<double>* arrayOf(const RNode* n, const char* kid) {
    const RNode* k = n ? n->kid(kid) : nullptr;
    return k && !k->props.empty() ? &k->props[0].nums : nullptr;
}

// Properties70 values: P name, type, label, flags, values...
const RNode* findP(const RNode* obj, const char* name) {
    const RNode* p70 = obj ? obj->kid("Properties70") : nullptr;
    if (!p70) return nullptr;
    for (auto& p : p70->kids)
        if (p.name == "P" && p.str(0) == name) return &p;
    return nullptr;
}
std::array<double, 3> pVec3(const RNode* obj, const char* name, std::array<double, 3> def) {
    const RNode* p = findP(obj, name);
    if (!p || p->props.size() < 7) return def;
    return {p->props[4].d, p->props[5].d, p->props[6].d};
}
double pNum(const RNode* obj, const char* name, double def) {
    const RNode* p = findP(obj, name);
    return p && p->props.size() >= 5 ? p->props[4].d : def;
}

std::string plainName(const std::string& s) {
    size_t k = s.find(std::string("\0\1", 2));
    return k == std::string::npos ? s : s.substr(0, k);
}

Mat4 translate(const std::array<double, 3>& t) {
    Mat4 m = identity();
    m[12] = t[0], m[13] = t[1], m[14] = t[2];
    return m;
}
Mat4 scaling(const std::array<double, 3>& s) {
    Mat4 m = identity();
    m[0] = s[0], m[5] = s[1], m[10] = s[2];
    return m;
}
std::array<double, 3> negated(std::array<double, 3> v) {
    for (double& x : v) x = -x;
    return v;
}
// Euler angles in degrees; FBX's RotationOrder (0 XYZ: X turns first, so R = Rz * Ry * Rx).
Mat4 rotation(const std::array<double, 3>& deg, int order) {
    Mat4 ax[3];
    for (int k = 0; k < 3; ++k) {
        double a = deg[size_t(k)] / kDeg, c = std::cos(a), s = std::sin(a);
        Mat4 m = identity();
        int i = (k + 1) % 3, j = (k + 2) % 3;
        m[size_t(i * 4 + i)] = c, m[size_t(i * 4 + j)] = s, m[size_t(j * 4 + i)] = -s, m[size_t(j * 4 + j)] = c;
        ax[k] = m;
    }
    static const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 2, 0}, {1, 0, 2}, {2, 0, 1}, {2, 1, 0}};
    const int* o = orders[order >= 0 && order < 6 ? order : 0];
    return mul(ax[o[2]], mul(ax[o[1]], ax[o[0]]));
}

// A Model's local transform: T * Roff * Rp * Rpre * R * Rpost^-1 * Rp^-1 * Soff * Sp * S * Sp^-1.
Mat4 localOf(const RNode* mdl) {
    const std::array<double, 3> zero{0, 0, 0}, one{1, 1, 1};
    auto rp = pVec3(mdl, "RotationPivot", zero), sp = pVec3(mdl, "ScalingPivot", zero);
    Mat4 m = translate(pVec3(mdl, "Lcl Translation", zero));
    m = mul(m, translate(pVec3(mdl, "RotationOffset", zero)));
    m = mul(m, translate(rp));
    m = mul(m, rotation(pVec3(mdl, "PreRotation", zero), 0));
    m = mul(m, rotation(pVec3(mdl, "Lcl Rotation", zero), int(pNum(mdl, "RotationOrder", 0))));
    m = mul(m, rigidInverse(rotation(pVec3(mdl, "PostRotation", zero), 0)));
    m = mul(m, translate(negated(rp)));
    m = mul(m, translate(pVec3(mdl, "ScalingOffset", zero)));
    m = mul(m, translate(sp));
    m = mul(m, scaling(pVec3(mdl, "Lcl Scaling", one)));
    m = mul(m, translate(negated(sp)));
    return m;
}

// Value of a layer element (normals, UVs) at a corner; false when the layer has none there.
bool layerValue(const RNode* layer, const char* dataName, const char* indexName, size_t comps, size_t corner, size_t point, size_t polygon, double* out) {
    const auto* data = arrayOf(layer, dataName);
    if (!data) return false;
    const RNode* mapNode = layer->kid("MappingInformationType");
    const RNode* refNode = layer->kid("ReferenceInformationType");
    std::string map = mapNode ? mapNode->str(0) : "ByPolygonVertex", ref = refNode ? refNode->str(0) : "Direct";
    size_t k;
    if (map == "ByPolygonVertex") k = corner;
    else if (map == "ByControlPoint" || map == "ByVertice" || map == "ByVertex") k = point;
    else if (map == "ByPolygon") k = polygon;
    else if (map == "AllSame") k = 0;
    else return false;
    if (ref == "IndexToDirect" || ref == "Index") {
        const auto* idx = arrayOf(layer, indexName);
        if (!idx || k >= idx->size() || (*idx)[k] < 0) return false;
        k = size_t((*idx)[k]);
    }
    if (k >= data->size() / comps) return false;
    for (size_t c = 0; c < comps; ++c) out[c] = (*data)[k * comps + c];
    return true;
}

// The layer element of a kind with the lowest layer index.
const RNode* firstLayer(const RNode* geo, const char* kind) {
    const RNode* best = nullptr;
    for (auto& k : geo->kids)
        if (k.name == kind && (!best || k.num(0) < best->num(0))) best = &k;
    return best;
}

// A binary FBX file's objects by id, their object-object connections and the transform from its axes and unit
// to the game's (Y up, +Z front, +X right, metres).
struct Scene {
    std::vector<RNode> top;
    std::map<int64_t, const RNode*> byId;
    std::multimap<int64_t, int64_t> parents, children;
    std::multimap<int64_t, int64_t> propChildren;  // object-property connections (a material's textures), by parent
    Mat4 axes = identity();

    const RNode* topNode(const char* n) const {
        for (auto& t : top)
            if (t.name == n) return &t;
        return nullptr;
    }
    bool isA(int64_t id, const char* cls, const char* kind) const {
        auto it = byId.find(id);
        return it != byId.end() && it->second->name == cls && (!kind || it->second->str(2) == kind);
    }
    int64_t modelParent(int64_t id) const {
        auto r = parents.equal_range(id);
        for (auto it = r.first; it != r.second; ++it)
            if (isA(it->second, "Model", nullptr)) return it->second;
        return 0;
    }
    // A Model's world transform in the file's space.
    Mat4 global(int64_t id, int depth = 0) const {
        if (!isA(id, "Model", nullptr)) return identity();
        Mat4 l = localOf(byId.at(id));
        int64_t p = modelParent(id);
        return p && depth < 256 ? mul(global(p, depth + 1), l) : l;
    }
};

void loadScene(Scene& s, const Bytes& file) {
    if (!isFbx(file)) fail("not an FBX file");
    if (file.size() < 27 || std::memcmp(file.data(), "Kaydara FBX Binary  ", 20) != 0)
        fail("this is a text (ASCII) FBX file; export it as binary FBX");
    uint32_t version = uint32_t(rdLe(&file[23], 4));
    if (version < 7000) fail("FBX version " + std::to_string(version) + " is too old; export as FBX 7.x (2011 or later)");
    const bool wide = version >= 7500;
    for (size_t at = 27, hdr = wide ? 25 : 13; at + hdr <= file.size();) {
        RNode n;
        if (!readNode(file, at, wide, n, 0)) break;
        s.top.push_back(std::move(n));
    }
    const RNode* objects = s.topNode("Objects");
    const RNode* connections = s.topNode("Connections");
    if (!objects) fail("FBX: the file has no objects");
    for (auto& o : objects->kids)
        if (!o.props.empty()) s.byId[o.num(0)] = &o;
    if (connections) {
        for (auto& c : connections->kids) {
            if (c.name == "C" && c.str(0) == "OO") {
                s.parents.emplace(c.num(1), c.num(2));
                s.children.emplace(c.num(2), c.num(1));
            } else if (c.name == "C" && c.str(0) == "OP") {
                s.propChildren.emplace(c.num(2), c.num(1));
            }
        }
    }

    const RNode* gs = s.topNode("GlobalSettings");
    auto axis = [&](const char* n, int def) {
        int a = int(pNum(gs, n, def));
        return a >= 0 && a < 3 ? a : def;
    };
    auto sign = [&](const char* n) { return pNum(gs, n, 1) < 0 ? -1.0 : 1.0; };
    int up = axis("UpAxis", 1), front = axis("FrontAxis", 2), coord = axis("CoordAxis", 0);
    double unit = pNum(gs, "UnitScaleFactor", 1) / 100;
    if (!(unit > 0)) unit = 0.01;
    if (up != front && up != coord && front != coord) {
        s.axes = Mat4{};
        s.axes[size_t(coord * 4 + 0)] = sign("CoordAxisSign");
        s.axes[size_t(up * 4 + 1)] = sign("UpAxisSign");
        s.axes[size_t(front * 4 + 2)] = sign("FrontAxisSign");
        s.axes[15] = 1;
    }
    s.axes = mul(scaling({unit, unit, unit}), s.axes);
}

} // namespace

bool isFbx(const Bytes& f) {
    if (f.size() >= 27 && std::memcmp(f.data(), "Kaydara FBX Binary  ", 20) == 0) return true;
    std::string head(f.begin(), f.begin() + std::ptrdiff_t(std::min<size_t>(f.size(), 4096)));
    return startsWith(head, "; FBX") || head.find("FBXHeaderExtension:") != std::string::npos;
}

std::vector<MeshData> readMeshes(const Bytes& file) {
    Scene s;
    loadScene(s, file);
    const auto& byId = s.byId;
    const auto& children = s.children;
    const auto& parents = s.parents;
    const Mat4& axes = s.axes;
    auto isA = [&](int64_t id, const char* cls, const char* kind) { return s.isA(id, cls, kind); };

    std::vector<MeshData> out;
    for (auto& [geoId, geo] : byId) {
        if (geo->name != "Geometry" || geo->str(2) != "Mesh") continue;
        const auto* verts = arrayOf(geo, "Vertices");
        const auto* pvi = arrayOf(geo, "PolygonVertexIndex");
        if (!verts) continue;
        size_t np = verts->size() / 3;

        // Skin: Skin deformer -> clusters -> bone models.
        std::vector<std::vector<std::pair<std::string, float>>> skin;
        auto dr = children.equal_range(geoId);
        for (auto d = dr.first; d != dr.second; ++d) {
            if (!isA(d->second, "Deformer", "Skin")) continue;
            skin.assign(np, {});
            auto cr = children.equal_range(d->second);
            for (auto c = cr.first; c != cr.second; ++c) {
                if (!isA(c->second, "Deformer", "Cluster")) continue;
                std::string bone;
                auto br = children.equal_range(c->second);
                for (auto b = br.first; b != br.second; ++b)
                    if (isA(b->second, "Model", nullptr)) bone = plainName(byId.at(b->second)->str(1));
                const RNode* cl = byId.at(c->second);
                const auto* idx = arrayOf(cl, "Indexes");
                const auto* wts = arrayOf(cl, "Weights");
                if (bone.empty() || !idx || !wts) continue;
                for (size_t k = 0; k < idx->size() && k < wts->size(); ++k) {
                    double v = (*idx)[k];
                    if (v >= 0 && size_t(v) < np && (*wts)[k] > 0) skin[size_t(v)].push_back({bone, float((*wts)[k])});
                }
            }
            break;
        }

        auto mr = parents.equal_range(geoId);
        for (auto mi = mr.first; mi != mr.second; ++mi) {
            if (!isA(mi->second, "Model", nullptr)) continue;
            const RNode* mdl = byId.at(mi->second);
            const std::array<double, 3> zero{0, 0, 0}, one{1, 1, 1};
            Mat4 geom = mul(translate(pVec3(mdl, "GeometricTranslation", zero)),
                            mul(rotation(pVec3(mdl, "GeometricRotation", zero), 0), scaling(pVec3(mdl, "GeometricScaling", one))));
            Mat4 world = mul(axes, mul(s.global(mi->second), geom));
            // Normals by the cofactor of the linear part; a mirroring transform also reverses the winding.
            std::array<double, 3> a{world[0], world[1], world[2]}, b{world[4], world[5], world[6]}, c{world[8], world[9], world[10]};
            auto crossD = [](const std::array<double, 3>& x, const std::array<double, 3>& y) {
                return std::array<double, 3>{x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
            };
            auto bc = crossD(b, c), ca = crossD(c, a), ab = crossD(a, b);
            double det = a[0] * bc[0] + a[1] * bc[1] + a[2] * bc[2];
            double flip = det < 0 ? -1 : 1;

            MeshData md;
            md.name = plainName(mdl->str(1));
            md.skin = skin;
            // Its first material: a plain colour when no texture is connected to it.
            auto matr = children.equal_range(mi->second);
            for (auto m = matr.first; m != matr.second; ++m) {
                if (!isA(m->second, "Material", nullptr)) continue;
                bool textured = false;
                for (const auto* links : {&s.children, &s.propChildren}) {
                    auto tr = links->equal_range(m->second);
                    for (auto t = tr.first; t != tr.second; ++t) textured |= isA(t->second, "Texture", nullptr);
                }
                const RNode* mat = byId.at(m->second);
                const std::array<double, 3> none{-1, -1, -1};
                std::array<double, 3> col = pVec3(mat, "DiffuseColor", none);
                if (col[0] < 0) col = pVec3(mat, "Diffuse", none);
                if (!textured && col[0] >= 0) md.plainColour = std::array<float, 3>{float(col[0]), float(col[1]), float(col[2])};
                break;
            }
            md.points.reserve(np);
            for (size_t k = 0; k < np; ++k) {
                double x = (*verts)[3 * k], y = (*verts)[3 * k + 1], z = (*verts)[3 * k + 2];
                md.points.push_back({float(world[0] * x + world[4] * y + world[8] * z + world[12]), float(world[1] * x + world[5] * y + world[9] * z + world[13]),
                                     float(world[2] * x + world[6] * y + world[10] * z + world[14])});
            }
            const RNode* nl = firstLayer(geo, "LayerElementNormal");
            const RNode* ul = firstLayer(geo, "LayerElementUV");
            bool haveN = nl != nullptr, haveUv = ul != nullptr;
            std::vector<int> poly;
            size_t polyNo = 0;
            for (size_t k = 0; pvi && k < pvi->size(); ++k) {
                double v = (*pvi)[k];
                bool last = v < 0;
                int64_t point = last ? ~int64_t(v) : int64_t(v);
                if (point < 0 || size_t(point) >= np) fail("FBX: mesh " + md.name + " has a polygon corner outside its vertices");
                size_t corner = md.cornerPoint.size();
                md.cornerPoint.push_back(int(point));
                double n[3], uv[2];
                if (haveN && layerValue(nl, "Normals", "NormalsIndex", 3, corner, size_t(point), polyNo, n)) {
                    std::array<double, 3> t{};
                    for (int q = 0; q < 3; ++q) t[size_t(q)] = flip * (bc[size_t(q)] * n[0] + ca[size_t(q)] * n[1] + ab[size_t(q)] * n[2]);
                    double len = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
                    if (len > 0) md.cornerNormal.push_back({float(t[0] / len), float(t[1] / len), float(t[2] / len)});
                    else md.cornerNormal.push_back({0, 1, 0});
                } else {
                    haveN = false;
                }
                if (haveUv && layerValue(ul, "UV", "UVIndex", 2, corner, size_t(point), polyNo, uv)) md.cornerUv.push_back({float(uv[0]), float(uv[1])});
                else haveUv = false;
                poly.push_back(int(corner));
                if (last) {
                    if (det < 0) std::reverse(poly.begin(), poly.end());
                    if (poly.size() >= 3) md.polygons.push_back(poly);
                    poly.clear();
                    ++polyNo;
                }
            }
            if (!haveN) md.cornerNormal.clear();
            if (!haveUv) md.cornerUv.clear();
            out.push_back(std::move(md));
        }
    }
    return out;
}

std::vector<BoneData> readBones(const Bytes& file) {
    Scene s;
    loadScene(s, file);
    // Bind matrices: a skin cluster's TransformLink, else the bind pose's matrix (both world, file space).
    std::map<int64_t, Mat4> bind;
    auto matrixOf = [](const std::vector<double>* a, Mat4& m) {
        if (!a || a->size() != 16) return false;
        std::copy(a->begin(), a->end(), m.begin());
        return true;
    };
    for (auto& [id, obj] : s.byId) {
        if (!s.isA(id, "Deformer", "Cluster")) continue;
        Mat4 m;
        if (!matrixOf(arrayOf(obj, "TransformLink"), m)) continue;
        auto br = s.children.equal_range(id);
        for (auto b = br.first; b != br.second; ++b)
            if (s.isA(b->second, "Model", nullptr)) bind.emplace(b->second, m);
    }
    for (auto& [id, obj] : s.byId) {
        if (obj->name != "Pose" || obj->str(2) != "BindPose") continue;
        for (auto& pn : obj->kids) {
            Mat4 m;
            const RNode* node = pn.kid("Node");
            if (pn.name == "PoseNode" && node && !node->props.empty() && matrixOf(arrayOf(&pn, "Matrix"), m)) bind.emplace(node->num(0), m);
        }
    }

    std::vector<BoneData> out;
    for (auto& [id, obj] : s.byId) {
        if (obj->name != "Model" || obj->str(2) != "LimbNode") continue;
        BoneData b;
        b.name = plainName(obj->str(1));
        auto it = bind.find(id);
        b.fromBind = it != bind.end();
        Mat4 w = mul(s.axes, b.fromBind ? it->second : s.global(id));
        b.position = {float(w[12]), float(w[13]), float(w[14])};
        if (int64_t p = s.modelParent(id); p && s.isA(p, "Model", "LimbNode")) b.parent = plainName(s.byId.at(p)->str(1));
        out.push_back(std::move(b));
    }
    return out;
}

} // namespace nut::fbx
