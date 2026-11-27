#include "renutvehicle.hpp"

#include "assetdb.hpp"
#include "json.hpp"

#include <cmath>

namespace nut::renutvehicle {
namespace {

// The blueprint: a header, then a record per part (layout as saved vehicles have it).
constexpr uint8_t kMagic[8] = {0x3F, 0x9A, 0xE1, 0x48, 0x40, 0x54, 0x7A, 0xE1};
constexpr size_t kHeader = 0x84, kPart = 36, kNameChars = 31;
// Header: part count (u16), name (UTF-16), total mass, the parts on the A, B and X buttons.
constexpr size_t kCount = 0x08, kName = 0x28, kMassTotal = 0x18, kButtonParts = 0x6C;
// Record: cell (3 bytes), block type + 1, objparams aid, rotation (3 f32 radians), colour (RGBA),
// its button.
constexpr size_t kCell = 0, kType = 6, kAid = 8, kRotation = 12, kColour = 24, kButton = 28;
// A part record: block type (u32), mass (f32).
constexpr uint32_t kBlockType = 0x098, kMass = 0x190;
constexpr std::string_view kPartPrefix = "aid_objparams_banjox_vehicleblock_";

Bytes partRecord(const caff::Caff& c, const std::string& asset) {
    for (size_t i = 0; i < c.symbols.size(); ++i)
        if (assetdb::baseAssetName(c.symbolName(i)) == asset && c.sectionOf(c.symbols[i]).name == ".data") return c.symbolData(i);
    fail(asset + " is not in the bundle (is it a part of the game?)");
}

void putF32(uint8_t* p, double v) {
    float f = float(v);
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    putBe32(p, bits);
}

std::string partName(std::string s) {
    s = lower(trim(s));
    if (startsWith(s, kPartPrefix)) s.erase(0, kPartPrefix.size());
    return s;
}

} // namespace

Bytes blueprint(const caff::Caff& c, const std::string& text) {
    Json j = Json::parse(text);
    if (!j.has("parts")) fail("the vehicle file needs \"parts\"");
    const auto& parts = j["parts"].items();
    if (parts.empty() || parts.size() > 0xFFFF) fail("a vehicle has 1-65535 parts");
    Bytes out(kHeader + parts.size() * kPart, 0);
    std::memcpy(out.data(), kMagic, sizeof kMagic);
    putBe16(&out[kCount], uint16_t(parts.size()));
    // As every saved vehicle has them.
    putBe16(&out[0x0A], 0x0100);
    putBe32(&out[0x20], 0x0000085B);
    putBe32(&out[0x24], 1);
    putBe32(&out[0x80], 0x01000000);
    std::u16string name = utf8ToUtf16(j.has("name") ? j["name"].asString() : "MOD VEHICLE");
    if (name.size() > kNameChars) name.resize(kNameChars);
    for (size_t i = 0; i < name.size(); ++i) putBe16(&out[kName + 2 * i], uint16_t(name[i]));

    double mass = 0;
    for (size_t k = 0; k < parts.size(); ++k) {
        const Json& p = parts[k];
        const std::string where = "part " + std::to_string(k + 1);
        if (!p.has("part")) fail(where + " needs \"part\"");
        std::string part = partName(p["part"].asString()), base = p.has("base") ? partName(p["base"].asString()) : "";
        std::string asset = std::string(kPartPrefix) + (base.empty() ? part : "renut_" + part);
        auto aid = assetdb::idFromName(asset);
        if (!aid) fail(where + ": no asset id for " + asset);
        Bytes rec = partRecord(c, std::string(kPartPrefix) + (base.empty() ? part : base));
        if (rec.size() < kMass + 4) fail(where + ": " + part + " is not a vehicle part");
        mass += bef32(&rec[kMass]);

        uint8_t* r = &out[kHeader + k * kPart];
        if (!p.has("cell") || p["cell"].size() != 3) fail(where + " needs \"cell\": [x, y, z]");
        for (size_t a = 0; a < 3; ++a) {
            double v = p["cell"][a].asNumber();
            if (v < 0 || v > 255) fail(where + ": a cell is 0-255 on each axis");
            r[kCell + a] = uint8_t(v);
        }
        r[kType] = uint8_t(be32(&rec[kBlockType]) + 1);
        putBe32(r + kAid, *aid);
        if (p.has("rotation")) {
            if (p["rotation"].size() != 3) fail(where + ": \"rotation\" is [x, y, z] in degrees");
            for (size_t a = 0; a < 3; ++a) putF32(r + kRotation + 4 * a, p["rotation"][a].asNumber() * 3.14159265358979323846 / 180);
        }
        uint32_t colour = p.has("colour") ? parseHex(p["colour"].asString()) : 0xE09A07;
        putBe32(r + kColour, colour << 8 | 0xFF);
        if (p.has("button")) {
            std::string b = lower(p["button"].asString());
            int slot = b == "a" ? 0 : b == "b" ? 1 : b == "x" ? 2 : -1;
            if (slot < 0) fail(where + ": \"button\" is \"A\", \"B\" or \"X\"");
            putBe32(r + kButton, 0x1000u << slot);
            putBe32(&out[kButtonParts + 4 * size_t(slot)], *aid);
        }
    }
    putF32(&out[kMassTotal], mass);
    return out;
}

} // namespace nut::renutvehicle
