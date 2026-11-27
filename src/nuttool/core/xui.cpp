#include "xui.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <set>

namespace nut::xui {
namespace {

enum : uint8_t { Bool = 1, Int, Unsigned, Float, String, Color, Vector, Quaternion, Object, Custom };

struct Prop {
    uint8_t type;
    bool indexed = false;
    const std::vector<Prop>* object = nullptr;
};

// Property tables of the classes adjusted here, in the game's order (registrations at 0x82A214F0,
// 0x82A1E780, 0x82A1A6A0, 0x82A1A248, 0x82A19AE8, 0x82A19D00).
const std::vector<Prop> kElement = {{String}, {Float}, {Float}, {Vector}, {Vector}, {Quaternion}, {Float}, {Unsigned}, {Vector},
                                    {Bool}, {Unsigned}, {Bool}, {Bool}, {Unsigned}, {Bool}, {Bool}, {Bool}};
enum { ElemId = 0, ElemWidth = 1, ElemHeight = 2 };
const std::vector<Prop> kImage = {{Unsigned}, {String}, {Unsigned}}; // SizeMode, ImagePath, BrushFlags
enum { ImageSizeMode = 0, ImagePath = 1 };
const std::vector<Prop> kStroke = {{Float}, {Color}};
const std::vector<Prop> kGradient = {{Bool}, {Int}, {Color, true}, {Float, true}};
// FillType, FillColor, TextureFileName, Gradient, Translation, Scale, Rotation, two wrap modes, BrushFlags, one more
const std::vector<Prop> kFill = {{Unsigned}, {Color}, {String}, {Object, false, &kGradient}, {Vector}, {Vector}, {Float},
                                 {Unsigned}, {Unsigned}, {Unsigned}, {Unsigned}};
enum { FillType = 0, FillTexture = 2, FillTranslation = 4, FillScale = 5, FillRotation = 6, FillBrushMode = 10 };
const std::vector<Prop> kFigure = {{Object, false, &kStroke}, {Object, false, &kFill}, {Bool}, {Custom}};
enum { FigureFill = 1 };

constexpr uint32_t kStretch = 4, kAnyStretch = 4 | 8 | 16;
constexpr uint32_t kTextureFill = 4;

// One parsed property block.
struct Block {
    size_t start = 0, end = 0;
    uint32_t mask = 0;
    int maskBytes = 0;
    std::vector<std::pair<size_t, size_t>> values; // per property: its bytes, [begin, end); empty when absent
    unsigned count = 0;                             // values (array elements each, objects once)
    unsigned records = 0;                           // count plus the values of nested objects

    bool has(int i) const { return mask >> i & 1; }
};

bool readBlock(const Bytes& d, size_t p, size_t end, const std::vector<Prop>& props, Block& b) {
    if (p >= end) return false;
    uint8_t first = d[p];
    int n = first & 7;
    if (n != 0 && n != 1 && n != 2 && n != 4) return false;
    if (p + 1 + size_t(n) > end) return false;
    uint32_t mask = 0;
    for (int i = 0; i < n; ++i) mask = mask << 8 | d[p + 1 + size_t(i)];
    if (props.size() < 32 && mask >> props.size()) return false;
    if (mask ? (first >> 3) != unsigned(std::popcount(mask)) - 1 : first != 0) return false;
    b = Block{};
    b.start = p;
    b.mask = mask;
    b.maskBytes = n;
    b.values.assign(props.size(), {0, 0});
    size_t q = p + 1 + size_t(n);
    for (size_t i = 0; i < props.size(); ++i) {
        if (!(mask >> i & 1)) continue;
        size_t begin = q;
        uint32_t reps = 1;
        if (props[i].indexed) {
            if (q >= end) return false;
            uint8_t c = d[q++];
            if (c & 0x80) {
                int k = c & 0x7f;
                if ((k != 1 && k != 2 && k != 4) || q + size_t(k) > end) return false;
                reps = 0;
                for (int j = 0; j < k; ++j) reps = reps << 8 | d[q++];
            } else {
                reps = c;
            }
        }
        for (uint32_t r = 0; r < reps; ++r) {
            switch (props[i].type) {
                case Bool: q += 1; break;
                case String: q += 2; break;
                case Object: {
                    if (q + 2 > end) return false;
                    unsigned cnt = be16(&d[q]);
                    Block nested;
                    if (!readBlock(d, q + 2, end, *props[i].object, nested) || nested.count != cnt) return false;
                    q = nested.end;
                    b.records += nested.records;
                    break;
                }
                default: q += 4; break;
            }
            if (q > end) return false;
            ++b.count;
        }
        b.values[i] = {begin, q};
    }
    b.end = q;
    b.records += b.count;
    return true;
}

// The block again, with the values in `set` added or replaced (plain, non-indexed properties).
Bytes writeBlock(const Bytes& d, const Block& b, const std::map<int, Bytes>& set, unsigned* count) {
    uint32_t mask = b.mask;
    for (auto& [i, v] : set) mask |= 1u << i;
    int need = mask >> 16 ? 4 : mask >> 8 ? 2 : mask ? 1 : 0;
    int n = std::max(b.maskBytes, need);
    Bytes out;
    out.push_back(mask ? uint8_t((std::popcount(mask) - 1) << 3 | n) : 0);
    for (int i = n - 1; i >= 0; --i) out.push_back(uint8_t(mask >> (8 * i)));
    *count = b.count;
    for (size_t i = 0; i < b.values.size(); ++i) {
        auto it = set.find(int(i));
        if (it != set.end()) {
            out.insert(out.end(), it->second.begin(), it->second.end());
            if (!b.has(int(i))) ++*count;
        } else if (b.has(int(i))) {
            out.insert(out.end(), d.begin() + std::ptrdiff_t(b.values[i].first), d.begin() + std::ptrdiff_t(b.values[i].second));
        }
    }
    return out;
}

Bytes u16Bytes(uint16_t v) { return {uint8_t(v >> 8), uint8_t(v)}; }
Bytes u32Bytes(uint32_t v) { return {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; }
float f32At(const Bytes& d, size_t p) { return bef32(&d[p]); }

// An element of a known class: u16 class, u8 flags, u16 property count, XuiElement block, own block.
struct Element {
    size_t start = 0;
    Block base, own;
};

bool readElement(const Bytes& d, size_t p, uint16_t cls, const std::vector<Prop>& own, Element& e) {
    if (p + 5 > d.size() || be16(&d[p]) != cls) return false;
    uint8_t flags = d[p + 2];
    if (flags > 7 || !(flags & 1)) return false;
    unsigned count = be16(&d[p + 3]);
    e.start = p;
    return readBlock(d, p + 5, d.size(), kElement, e.base) && readBlock(d, e.base.end, d.size(), own, e.own) &&
           e.base.count + e.own.count == count;
}

struct Section {
    std::string name;
    uint32_t offset = 0, size = 0;
    size_t tableAt = 0; // where its entry is in the header
};

struct Edit {
    size_t start, end;
    Bytes bytes;
};

std::string lowerAscii(const std::u16string& s) {
    std::string out;
    for (char16_t c : s) out.push_back(c < 128 ? char(std::tolower(int(c))) : '?');
    return out;
}

// Adjusts one XUIB scene; returns it unchanged when nothing applies.
Bytes adaptScene(const Bytes& x, const std::string& scene, const std::map<std::string, Resize>& resized, std::vector<std::string>& log,
                 int& changed) {
    if (x.size() < 20 || std::memcmp(x.data(), "XUIB", 4) != 0 || be32(&x[4]) != 5 || be32(&x[14]) != x.size()) return x;
    bool counts = be32(&x[8]) & 1;
    size_t table = counts ? 60 : 20;
    unsigned nsec = be16(&x[18]);
    if (table + 12 * size_t(nsec) > x.size()) return x;
    std::vector<Section> secs;
    const Section *strn = nullptr, *vect = nullptr, *data = nullptr;
    for (unsigned i = 0; i < nsec; ++i) {
        size_t t = table + 12 * size_t(i);
        Section s{std::string(reinterpret_cast<const char*>(&x[t]), 4), be32(&x[t + 4]), be32(&x[t + 8]), t};
        if (uint64_t(s.offset) + s.size > x.size()) return x;
        secs.push_back(s);
    }
    for (auto& s : secs) {
        if (s.name == "STRN") strn = &s;
        else if (s.name == "VECT") vect = &s;
        else if (s.name == "DATA") data = &s;
    }
    if (!strn || !data) return x;

    // Strings (1-based); which ones name a resized texture.
    std::vector<std::string> strings;
    for (size_t q = strn->offset; q + 2 <= size_t(strn->offset) + strn->size;) {
        size_t n = be16(&x[q]);
        std::u16string s;
        for (size_t i = 0; i < n && q + 4 + 2 * i <= x.size(); ++i) s.push_back(char16_t(be16(&x[q + 2 + 2 * i])));
        strings.push_back(lowerAscii(s));
        q += 2 + 2 * n;
    }
    auto textureOf = [&](uint16_t idx) -> const std::pair<const std::string, Resize>* {
        if (idx == 0 || idx > strings.size()) return nullptr;
        const std::string& s = strings[idx - 1];
        if (!startsWith(s, "file://")) return nullptr;
        auto it = resized.find(s.substr(7));
        return it == resized.end() ? nullptr : &*it;
    };
    std::set<uint16_t> wanted;
    for (size_t i = 0; i < strings.size(); ++i)
        if (textureOf(uint16_t(i + 1))) wanted.insert(uint16_t(i + 1));
    if (wanted.empty()) return x;
    auto classIndex = [&](const char* name) {
        for (size_t i = 0; i < strings.size(); ++i)
            if (strings[i] == lower(name)) return uint16_t(i + 1);
        return uint16_t(0);
    };
    uint16_t imageClass = classIndex("XuiImage"), figureClass = classIndex("XuiFigure");

    std::vector<std::array<float, 3>> vectors;
    if (vect)
        for (size_t q = vect->offset; q + 12 <= size_t(vect->offset) + vect->size; q += 12)
            vectors.push_back({bef32(&x[q]), bef32(&x[q + 4]), bef32(&x[q + 8])});
    size_t oldVectors = vectors.size();
    auto vectorAt = [&](const Bytes& d, const Block& b, int i, std::array<float, 3> def) {
        if (!b.has(i)) return def;
        uint32_t idx = be32(&d[b.values[size_t(i)].first]);
        return idx < oldVectors ? vectors[idx] : def;
    };
    auto addVector = [&](std::array<float, 3> v) {
        vectors.push_back(v);
        return u32Bytes(uint32_t(vectors.size() - 1));
    };

    Bytes d(x.begin() + data->offset, x.begin() + data->offset + data->size);
    std::vector<Edit> edits;
    unsigned addedRecords = 0;
    std::set<uint16_t> handled;
    auto label = [&](const Element& e) {
        uint16_t si = e.base.has(ElemId) ? be16(&d[e.base.values[ElemId].first]) : 0;
        return scene + ": " + (si && si <= strings.size() ? strings[si - 1] : std::string("(unnamed)"));
    };
    auto boxOf = [&](const Element& e, float& w, float& h) {
        if (!e.base.has(ElemWidth) || !e.base.has(ElemHeight)) return false;
        w = f32At(d, e.base.values[ElemWidth].first);
        h = f32At(d, e.base.values[ElemHeight].first);
        return true;
    };

    for (size_t p = 0; p + 5 <= d.size(); ++p) {
        uint16_t cls = be16(&d[p]);
        if (cls == 0 || (cls != imageClass && cls != figureClass)) continue;
        Element e;
        if (cls == imageClass && readElement(d, p, cls, kImage, e)) {
            if (!e.own.has(ImagePath)) continue;
            uint16_t path = be16(&d[e.own.values[ImagePath].first]);
            auto* tex = textureOf(path);
            if (!tex) continue;
            handled.insert(path);
            const Resize& r = tex->second;
            uint32_t mode = e.own.has(ImageSizeMode) ? be32(&d[e.own.values[ImageSizeMode].first]) : 0;
            p = e.own.end - 1;
            if (mode & kAnyStretch) continue; // already scales with its box
            float w = 0, h = 0;
            if (!boxOf(e, w, h) || std::fabs(w - float(r.fromW)) > 0.5f || std::fabs(h - float(r.fromH)) > 0.5f) {
                log.push_back(label(e) + ": its box is not the texture's original size, so it was left as it is");
                continue;
            }
            unsigned count = 0;
            Bytes own = writeBlock(d, e.own, {{ImageSizeMode, u32Bytes(kStretch)}}, &count);
            Bytes head(d.begin() + std::ptrdiff_t(e.start), d.begin() + std::ptrdiff_t(e.own.start));
            putBe16(&head[3], uint16_t(e.base.count + count));
            head.insert(head.end(), own.begin(), own.end());
            addedRecords += count - e.own.count;
            edits.push_back({e.start, e.own.end, std::move(head)});
            ++changed;
        } else if (cls == figureClass && readElement(d, p, cls, kFigure, e)) {
            if (!e.own.has(FigureFill)) continue;
            size_t fillAt = e.own.values[FigureFill].first;
            Block fill;
            if (!readBlock(d, fillAt + 2, d.size(), kFill, fill) || !fill.has(FillTexture)) continue;
            uint16_t path = be16(&d[fill.values[FillTexture].first]);
            auto* tex = textureOf(path);
            if (!tex) continue;
            handled.insert(path);
            p = e.own.end - 1;
            if (!fill.has(FillType) || be32(&d[fill.values[FillType].first]) != kTextureFill) continue;
            const Resize& r = tex->second;
            float w = 0, h = 0;
            if (!boxOf(e, w, h) && !(fill.has(FillBrushMode) && be32(&d[fill.values[FillBrushMode].first]))) {
                log.push_back(label(e) + ": its size is not stored, so its texture fill was left as it is");
                continue;
            }
            float kx = float(r.toW) / float(r.fromW), ky = float(r.toH) / float(r.fromH);
            auto s0 = vectorAt(d, fill, FillScale, {1, 1, 1});
            std::array<float, 3> s1 = {s0[0] * kx, s0[1] * ky, s0[2]};
            std::map<int, Bytes> set;
            // The fill's flags word bits 8-9 (FillBrushMode): set, the mapping is centred on the box,
            // UV = position * rotation * Scale / texture size + offsets (0x821C2B20), and Translation
            // counts in box spans, so only Scale has to grow with the texture. Clear, the scale is
            // about the box centre before dividing by the size and Translation counts in texture
            // units, so it absorbs the shift (0x82A312C8).
            uint32_t brush = fill.has(FillBrushMode) ? be32(&d[fill.values[FillBrushMode].first]) : 0;
            if (!brush) {
                float rot = fill.has(FillRotation) ? f32At(d, fill.values[FillRotation].first) : 0.0f;
                if (rot != 0.0f && std::fabs(kx - ky) > 1e-6f) {
                    log.push_back(label(e) + ": a rotated texture fill cannot follow a change of aspect ratio, so it was left as it is");
                    continue;
                }
                auto t0 = vectorAt(d, fill, FillTranslation, {0, 0, 0});
                std::array<float, 3> t1 = {t0[0] + w / 2 * (1.0f / float(r.fromW) - 1.0f / float(r.toW)),
                                           t0[1] + h / 2 * (1.0f / float(r.fromH) - 1.0f / float(r.toH)), t0[2]};
                set[FillTranslation] = addVector(t1);
            }
            set[FillScale] = addVector(s1);
            unsigned fillCount = 0, figCount = 0;
            Bytes newFill = writeBlock(d, fill, set, &fillCount);
            Bytes fillObject = u16Bytes(uint16_t(fillCount));
            fillObject.insert(fillObject.end(), newFill.begin(), newFill.end());
            Bytes own = writeBlock(d, e.own, {{FigureFill, fillObject}}, &figCount);
            addedRecords += fillCount - fill.count;
            edits.push_back({e.own.start, e.own.end, std::move(own)});
            ++changed;
        }
    }
    for (uint16_t s : wanted)
        if (!handled.count(s))
            log.push_back(scene + ": " + strings[s - 1].substr(7) + " is also drawn by elements NutTool does not adjust (nine-grids or others)");
    if (edits.empty()) return x;

    // Splice the element edits into DATA, back to front.
    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.start > b.start; });
    for (auto& ed : edits) {
        d.erase(d.begin() + std::ptrdiff_t(ed.start), d.begin() + std::ptrdiff_t(ed.end));
        d.insert(d.begin() + std::ptrdiff_t(ed.start), ed.bytes.begin(), ed.bytes.end());
    }
    Bytes newVect;
    if (vect) newVect.assign(x.begin() + vect->offset, x.begin() + vect->offset + vect->size);
    for (size_t i = oldVectors; i < vectors.size(); ++i)
        for (float f : vectors[i]) {
            uint32_t v;
            std::memcpy(&v, &f, 4);
            Bytes b = u32Bytes(v);
            newVect.insert(newVect.end(), b.begin(), b.end());
        }
    if (!vect && vectors.size() > oldVectors) {
        log.push_back(scene + ": has no vector table, so it was left as it is");
        changed -= int(edits.size());
        return x;
    }

    // Rebuild the file with the sections in their order, gaps kept.
    std::vector<const Section*> order;
    for (auto& s : secs) order.push_back(&s);
    std::sort(order.begin(), order.end(), [](auto* a, auto* b) { return a->offset < b->offset; });
    Bytes out(x.begin(), x.begin() + order.front()->offset);
    size_t tableShift = 0;
    if (counts) {
        putBe32(&out[24], be32(&out[24]) + addedRecords);
    } else if (addedRecords) {
        // Without counts the loader's arena is a fixed 16 KB (0x82A45F68), which the added records
        // might not fit. Counts make it 16 bytes per counts[1]; the others only add to that size.
        Bytes c(40, 0);
        putBe32(&c[4], 0x4000 / 16 + addedRecords);
        out.insert(out.begin() + 20, c.begin(), c.end());
        putBe32(&out[8], be32(&out[8]) | 1);
        tableShift = 40;
    }
    size_t prevEnd = order.front()->offset;
    for (auto* s : order) {
        out.insert(out.end(), x.begin() + std::ptrdiff_t(prevEnd), x.begin() + s->offset);
        const Bytes* body = s == data ? &d : s == vect ? &newVect : nullptr;
        uint32_t off = uint32_t(out.size());
        if (body) out.insert(out.end(), body->begin(), body->end());
        else out.insert(out.end(), x.begin() + s->offset, x.begin() + s->offset + s->size);
        putBe32(&out[s->tableAt + tableShift + 4], off);
        putBe32(&out[s->tableAt + tableShift + 8], uint32_t(body ? body->size() : s->size));
        prevEnd = size_t(s->offset) + s->size;
    }
    out.insert(out.end(), x.begin() + std::ptrdiff_t(prevEnd), x.end());
    putBe32(&out[14], uint32_t(out.size()));
    return out;
}

} // namespace

bool isPackage(const Bytes& d) { return d.size() >= 22 && std::memcmp(d.data(), "XUIZ", 4) == 0; }

Bytes adaptPackage(const Bytes& z, const std::map<std::string, Resize>& resized, std::vector<std::string>& log, int* changed) {
    int count = 0;
    if (changed) *changed = 0;
    if (!isPackage(z) || resized.empty()) return z;
    size_t dirSize = be32(&z[16]), base = 22 + dirSize;
    unsigned n = be16(&z[20]);
    if (base > z.size()) return z;
    struct Entry {
        size_t dirAt;
        uint32_t size, offset;
        std::string name;
        Bytes data;
    };
    std::vector<Entry> entries;
    size_t q = 22;
    for (unsigned i = 0; i < n; ++i) {
        if (q + 9 > base) return z;
        Entry e{q, be32(&z[q]), be32(&z[q + 4]), {}, {}};
        size_t len = z[q + 8];
        for (size_t c = 0; c < len && q + 10 + 2 * c < z.size(); ++c) e.name.push_back(char(be16(&z[q + 9 + 2 * c]) & 0x7f));
        q += 9 + 2 * len;
        if (base + uint64_t(e.offset) + e.size > z.size()) return z;
        e.data.assign(z.begin() + std::ptrdiff_t(base + e.offset), z.begin() + std::ptrdiff_t(base + e.offset + e.size));
        entries.push_back(std::move(e));
    }
    bool any = false;
    std::vector<Bytes> adapted;
    for (auto& e : entries) {
        int c = 0;
        adapted.push_back(adaptScene(e.data, e.name, resized, log, c));
        count += c;
        any |= adapted.back() != e.data;
    }
    if (changed) *changed = count;
    if (!any) return z;

    // Lay the scenes out again in their original order, gaps kept.
    std::vector<size_t> order(entries.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return entries[a].offset < entries[b].offset; });
    Bytes out(z.begin(), z.begin() + std::ptrdiff_t(base));
    size_t prevEnd = base;
    for (size_t i : order) {
        const Entry& e = entries[i];
        size_t at = base + e.offset;
        if (at > prevEnd) out.insert(out.end(), z.begin() + std::ptrdiff_t(prevEnd), z.begin() + std::ptrdiff_t(at));
        putBe32(&out[e.dirAt], uint32_t(adapted[i].size()));
        putBe32(&out[e.dirAt + 4], uint32_t(out.size() - base));
        out.insert(out.end(), adapted[i].begin(), adapted[i].end());
        prevEnd = std::max(prevEnd, at + e.size);
    }
    if (prevEnd < z.size()) out.insert(out.end(), z.begin() + std::ptrdiff_t(prevEnd), z.end());
    putBe32(&out[8], be32(&z[8]) + uint32_t(out.size()) - uint32_t(z.size()));
    return out;
}

} // namespace nut::xui
