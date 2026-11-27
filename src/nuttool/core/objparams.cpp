#include "objparams.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace nut::objparams {
namespace {

using gamescript::NameLookup;
namespace text = gamescript::text;

// Field types: i int, f float, a asset id, x hex word, s string of `size` bytes, r raw bytes.
struct Field {
    uint32_t offset;
    char type;
    uint32_t size;
    std::string name; // empty: named by its offset
};

struct Spec {
    uint32_t offset;
    char type;
    uint32_t size;
    const char* name;
};

const Spec kHeader[] = {{0x002, 's', 64, "objTag"}, {0x042, 's', 64, "objDefId"}};

// The base of every vehicle part. Meanings come from the game's reader (0x82607A38 and the
// functions it calls) and from comparing all 153 parts; the ratings are the parts shop's 0-5 stars.
const Spec kVehiclePart[] = {
    {0x098, 'i', 4, "blockType"},
    {0x0A0, 's', 64, "name"},
    {0x0E0, 's', 64, "description"},
    {0x120, 'a', 4, "garageDescription"},
    {0x124, 'a', 4, "model"},
    {0x128, 'a', 4, "havok"},
    {0x12C, 'a', 4, "physics"},
    {0x130, 's', 32, "paintColour"},
    {0x150, 's', 64, "shopSeenFlag"},
    {0x190, 'f', 4, "mass"},
    {0x194, 'f', 4, "health"},
    {0x198, 'f', 4, "power"},
    {0x19C, 'i', 4, "ratingPower"},
    {0x1A0, 'i', 4, "ratingFuel"},
    {0x1A4, 'i', 4, "ratingFuelUse"},
    {0x1A8, 'i', 4, "ratingAmmo"},
    {0x1AC, 'i', 4, "ratingAmmoUse"},
    {0x1B0, 'i', 4, "ratingFirepower"},
    {0x1B4, 'i', 4, "ratingGrip"},
    {0x1B8, 'i', 4, "ratingStrength"},
    {0x1BC, 'i', 4, "ratingWeight"},
    {0x1C0, 'a', 4, "anim1"},
    {0x1C8, 'a', 4, "anim2"},
    {0x1D0, 'a', 4, "anim3"},
    {0x1D8, 'a', 4, "anim4"},
    {0x1E0, 'a', 4, "anim5"},
    {0x1E8, 'a', 4, "anim6"},
    {0x1F0, 'a', 4, "anim7"},
    {0x1F8, 'a', 4, "anim8"},
    {0x200, 'a', 4, "anim9"},
    {0x208, 'a', 4, "anim10"},
    {0x228, 's', 64, "shopCategory"},
    {0x268, 's', 64, "shopSubcategory"},
    {0x2A8, 's', 64, "shopItem"},
    {0x2E8, 's', 64, "partId"},
    {0x328, 's', 64, "surface"},
    {0x368, 's', 64, "material"},
    {0x3B8, 'a', 4, "texture"},
    {0x3BC, 'x', 4, "flags"},
};

// Class fields with a clear meaning; the rest are listed by offset.
const std::map<std::string, std::vector<Spec>>& classFields() {
    static const std::map<std::string, std::vector<Spec>> m = {
        {"objDefId_vehicleBlockFuel", {{0x3C0, 'f', 4, "fuelCapacity"}}},
        {"objDefId_vehicleBlockAmmo", {{0x3C0, 'f', 4, "ammoCapacity"}}},
    };
    return m;
}

uint32_t recordSize(const Bytes& d) { return be16(d.data()); }

std::string offsetName(uint32_t off) {
    char t[16];
    std::snprintf(t, sizeof t, "0x%03X", off);
    return t;
}

// Fields for bytes no layout names: strings, non-zero words and unaligned leftovers.
void guessFields(const Bytes& d, uint32_t from, uint32_t to, const NameLookup& names, std::vector<Field>& out) {
    uint32_t off = from;
    while (off < to) {
        if (off % 4 || to - off < 4) {
            uint32_t n = off % 4 ? std::min(4 - off % 4, to - off) : to - off;
            if (std::any_of(d.begin() + off, d.begin() + off + n, [](uint8_t b) { return b != 0; })) out.push_back({off, 'r', n, {}});
            off += n;
            continue;
        }
        uint32_t len = 0;
        while (off + len < to && d[off + len] >= 0x20 && d[off + len] < 0x7F) ++len;
        if (len >= 4 && off + len < to && d[off + len] == 0) {
            uint32_t z = off + len + 1;
            while (z < to && !d[z]) ++z;
            uint32_t w = (z - off) / 4 * 4;
            if (w >= len + 1) {
                out.push_back({off, 's', w, {}});
                off += w;
                continue;
            }
        }
        uint32_t v = be32(&d[off]);
        if (v) {
            char t = 'x';
            if (names && v != 0xFFFFFFFF && !names(v).empty()) t = 'a';
            else if (v < 0x100000 || v >= 0xFFF00000) t = 'i';
            else if (uint32_t e = (v >> 23) & 0xFF; e >= 100 && e <= 150) t = 'f';
            out.push_back({off, t, 4, {}});
        }
        off += 4;
    }
}

std::vector<Field> fieldsOf(const Bytes& d, const NameLookup& names) {
    std::vector<Spec> specs(std::begin(kHeader), std::end(kHeader));
    if (isVehiclePart(d)) {
        specs.insert(specs.end(), std::begin(kVehiclePart), std::end(kVehiclePart));
        auto it = classFields().find(className(d));
        if (it != classFields().end()) specs.insert(specs.end(), it->second.begin(), it->second.end());
    }
    std::sort(specs.begin(), specs.end(), [](const Spec& a, const Spec& b) { return a.offset < b.offset; });
    const uint32_t end = recordSize(d);
    std::vector<Field> out;
    uint32_t pos = 2;
    for (const Spec& s : specs) {
        if (s.offset + s.size > end) continue;
        guessFields(d, pos, s.offset, names, out);
        out.push_back({s.offset, s.type, s.size, s.name});
        pos = s.offset + s.size;
    }
    guessFields(d, pos, end, names, out);
    return out;
}

std::string valueText(const Bytes& d, const Field& f, const NameLookup& names) {
    if (f.type == 's') return text::formatString(&d[f.offset], f.size, false);
    if (f.type == 'r') return text::formatRaw(&d[f.offset], f.size);
    return text::formatWord(f.type, be32(&d[f.offset]), names);
}

[[noreturn]] void lineFail(int line, const std::string& msg) { fail("line " + std::to_string(line) + ": " + msg); }

} // namespace

bool isObjParams(const Bytes& d) {
    if (d.size() < 0x84) return false;
    uint32_t size = recordSize(d);
    return size >= 0x84 && size <= d.size() && d.size() - size < 16 && std::memcmp(&d[0x42], "objDefId_", 9) == 0;
}

std::string className(const Bytes& d) {
    if (d.size() < 0x82) return {};
    size_t len = 0;
    while (len < 64 && d[0x42 + len]) ++len;
    return std::string(reinterpret_cast<const char*>(&d[0x42]), len);
}

bool isVehiclePart(const Bytes& d) { return isObjParams(d) && recordSize(d) >= 0x3C0 && startsWith(className(d), "objDefId_vehicleBlock"); }

std::string toText(const Bytes& d, const NameLookup& names) {
    if (!isObjParams(d)) fail("not an objparams record");
    std::string out = "# " + className(d) + (isVehiclePart(d) ? " (vehicle part)" : "") + ", " + std::to_string(recordSize(d)) +
                      " bytes: one field per line, name = value.\n"
                      "# Fields without a name go by offset; words that are 0 are not listed (add `0x3C4 = 1.5` to set one).\n"
                      "# Values: 12 or -3 (int), 1.5 (float), 0x1A2B3C4D (hex), @aid_name (asset id), \"text\" (string), <0a0b> (raw bytes).\n";
    for (const Field& f : fieldsOf(d, names)) {
        std::string name = f.name.empty() ? offsetName(f.offset) : f.name;
        if (name.size() < 18) name.resize(18, ' ');
        out += name + " = " + valueText(d, f, names);
        if (!f.name.empty()) out += "  # " + offsetName(f.offset);
        out += '\n';
    }
    return out;
}

Bytes fromText(const Bytes& base, std::string_view src) {
    if (!isObjParams(base)) fail("not an objparams record");
    Bytes out = base;
    const uint32_t end = recordSize(base);
    std::map<std::string, Field> byName;
    for (const Field& f : fieldsOf(base, {})) byName.emplace(f.name.empty() ? offsetName(f.offset) : f.name, f);
    int lineNo = 0;
    size_t pos = 0;
    while (pos <= src.size()) {
        size_t nl = src.find('\n', pos);
        if (nl == std::string_view::npos) nl = src.size();
        std::string_view line = src.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) lineFail(lineNo, "expected name = value");
        std::string name = trim(std::string_view(t).substr(0, eq));
        std::vector<text::Token> toks = text::parseLine(std::string_view(t).substr(eq + 1), lineNo);
        if (toks.size() != 1) lineFail(lineNo, toks.empty() ? "no value for " + name : name + " takes one value");
        const text::Token& v = toks[0];
        Field f{0, '?', 0, name};
        auto it = byName.find(name);
        if (it != byName.end()) {
            f = it->second;
        } else if (startsWith(lower(name), "0x")) {
            char* stop = nullptr;
            unsigned long off = std::strtoul(name.c_str() + 2, &stop, 16);
            if (name.size() == 2 || *stop) lineFail(lineNo, "bad offset '" + name + "'");
            f.offset = uint32_t(off);
        } else {
            lineFail(lineNo, "no field '" + name + "' in " + className(base));
        }
        if (f.name == "objDefId") {
            bool same = v.type == text::Token::String ? v.text == className(base)
                                                      : v.type == text::Token::Raw && v.raw == Bytes(base.begin() + 0x42, base.begin() + 0x82);
            if (!same) lineFail(lineNo, "objDefId can't change: the fields of another class would not fit");
            continue;
        }
        Bytes bytes;
        if (v.type == text::Token::String) {
            uint32_t n = f.type == 's' ? f.size : v.size;
            if (!n) lineFail(lineNo, "give the string's size, as in \"text\":64");
            if (v.text.size() >= n) lineFail(lineNo, name + " holds " + std::to_string(n - 1) + " characters");
            bytes.assign(v.text.begin(), v.text.end());
            bytes.resize(n, 0);
        } else if (v.type == text::Token::Raw) {
            bytes = v.raw;
        } else if (v.type == text::Token::Word) {
            bytes.resize(4);
            putBe32(bytes.data(), v.word);
        } else {
            lineFail(lineNo, "bad value for " + name);
        }
        if (f.type != '?' && bytes.size() != f.size)
            lineFail(lineNo, name + " is " + std::to_string(f.size) + " bytes; the value is " + std::to_string(bytes.size()));
        if (f.offset < 2 || f.offset + bytes.size() > end)
            lineFail(lineNo, name + " is outside the " + std::to_string(end) + "-byte record");
        std::copy(bytes.begin(), bytes.end(), out.begin() + f.offset);
    }
    return out;
}

std::vector<Change> changes(const Bytes& original, const Bytes& edited) {
    if (!isObjParams(original) || !isObjParams(edited)) fail("not an objparams record");
    if (className(original) != className(edited) || recordSize(original) != recordSize(edited))
        fail("the records are of different classes");
    // A field either record names (a word set to 0 is listed by the other one only), named ones first.
    std::map<uint32_t, Field> fields;
    for (const Bytes* d : {&original, &edited})
        for (const Field& f : fieldsOf(*d, {})) {
            auto [it, added] = fields.emplace(f.offset, f);
            if (!added && it->second.name.empty() && !f.name.empty()) it->second = f;
        }
    const uint32_t end = recordSize(original);
    std::vector<Change> out;
    std::vector<bool> covered(end, false);
    for (const auto& [offset, f] : fields) {
        if (offset + f.size > end) continue;
        std::fill(covered.begin() + offset, covered.begin() + offset + f.size, true);
        if (std::equal(original.begin() + offset, original.begin() + offset + f.size, edited.begin() + offset)) continue;
        out.push_back({offset, f.type, f.size, f.name});
    }
    // Bytes no field covers that changed anyway, as raw bytes.
    for (uint32_t i = 2; i < end; ++i)
        if (!covered[i] && original[i] != edited[i]) out.push_back({i, 'r', 1, {}});
    std::sort(out.begin(), out.end(), [](const Change& a, const Change& b) { return a.offset < b.offset; });
    return out;
}

} // namespace nut::objparams
