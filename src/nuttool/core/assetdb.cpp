#include "assetdb.hpp"

#include <cctype>
#include <fstream>

namespace nut::assetdb {
namespace {

// Recovered from the type table at 0x820CDAD8 in default.xex (16-byte entries:
// name, expected content version, ...). The index is the id's top byte.
struct TypeInfo {
    const char* name;
    float version;
};

const TypeInfo kTypes[] = {
    {"vehicle", 3.32f}, {"texture", 2.54f}, {"anim", 2.63f}, {"pfont", 2.23f}, {"model", 5.42f},
    {"havok", 1.18f}, {"animevents", 3.38f}, {"pathengine", 1.02f}, {"cutscene", 3.5f}, {"cutsceneevents", 3.47f},
    {"movie", 2.0f}, {"misc", 9.73f}, {"actorgoals", 4.38f}, {"marker", 4.67f}, {"callout", 3.49f},
    {"aidlist", 3.29f}, {"accessorymodel", 5.42f}, {"loctext", 2.33f}, {"xwavebank", 4.54f}, {"xwavebankloc", 4.54f},
    {"avatarhavokdata", 3.1f}, {"xcuelist", 5.89f}, {"font", 2.63f}, {"vehicleblockdata", 3.11f}, {"astarregion", 1.0f},
    {"script", 4.9f}, {"actorattribs", 3.32f}, {"fxemitter", 3.38f}, {"fxparticle", 3.29f}, {"fxrumble", 3.28f},
    {"fxcamshake", 3.13f}, {"objparams", 10.97f}, {"animtable", 3.35f}, {"attackdata", 3.13f}, {"hitreaction", 3.13f},
    {"scripttable", 3.22f}, {"statetable", 3.88f}, {"livesessionparams", 3.13f}, {"livetemplate", 3.13f}, {"lightprobes", 1.03f},
    {"uimenuitems", 3.25f}, {"uipageselect", 3.13f}, {"xuipackage", 7.06f}, {"xuicachefile", 4.5f}, {"xuiscene", 5.47f},
    {"xuiskin", 4.48f}, {"xuifont", 4.17f}, {"xuiloadlist", 4.27f}, {"xuiextension", 4.18f}, {"truetypefont", 2.01f},
    {"entrypoint", 3.14f}, {"taggroup", 3.17f}, {"vincedata", 1.02f}, {"xlsdata", 3.77f}, {"xmv", 1.03f},
    {"luascript", 1.14f}, {"astar", 1.0f}, {"testingtasks", 3.34f}, {"actionxls", 1.41f}, {"action", 3.45f},
    {"skeleton", 3.18f}, {"challenge", 4.23f}, {"pip", 3.34f}, {"turningcircle", 3.23f}, {"modelextras", 5.42f},
    {"vertexshader", 1.07f}, {"pixelshader", 1.12f}, {"dialog", 3.24f}, {"chardata", 3.14f}, {"gpuparticleeffect", 3.31f},
    {"cutcam", 3.06f}, {"blobsdropleteffect", 3.07f}, {"explosioneffect", 3.04f}, {"garagetutorial", 3.09f}, {"3dgpuparticleeffect", 3.09f},
    {"compositeeffect", 3.04f}, {"bundlebodge", 3.03f}, {"ddstexture", 1.04f}, {"pathenginepreprocess", 1.02f}, {"bundle", -1.0f},
    {"streambundle", -1.0f}, {"replay", 1.01f}, {"aidfilter", 1.01f}, {"xuifontcachemeta", 4.13f}, {"xuifontcachefont", 4.13f},
    {"multiplayerseries", 1.01f}, {"versionedmultiplayerseries", 1.01f}, {"level", 1.01f},
};

} // namespace

size_t typeCount() { return std::size(kTypes); }

std::string typeName(uint8_t type) {
    if (type < std::size(kTypes)) return kTypes[type].name;
    return "type_" + hex8(type);
}

float typeVersion(uint8_t type) { return type < std::size(kTypes) ? kTypes[type].version : 0.0f; }

std::optional<uint8_t> typeIndex(std::string_view name) {
    for (size_t i = 0; i < std::size(kTypes); ++i)
        if (name == kTypes[i].name) return uint8_t(i);
    return std::nullopt;
}

std::string baseAssetName(std::string_view s) {
    // Texture symbols look like "D:\LocalLibrary\BanjoX\aid_texture_x\default.rtx".
    size_t aid = s.find("aid_");
    if (aid != std::string_view::npos && aid > 0 && (s[aid - 1] == '\\' || s[aid - 1] == '/')) {
        s.remove_prefix(aid);
        size_t slash = s.find_first_of("\\/");
        if (slash != std::string_view::npos) s = s.substr(0, slash);
    }
    size_t comma = s.find(',');
    if (comma != std::string_view::npos) s = s.substr(0, comma);
    return std::string(s);
}

std::optional<uint32_t> idFromName(std::string_view name) {
    std::string base = baseAssetName(name);
    std::string_view s = base;
    if (startsWith(s, "aid_")) s.remove_prefix(4);
    // The type name may itself contain no underscores, but "3dgpuparticleeffect" etc. are
    // unambiguous; pick the longest table entry that prefixes the string.
    std::optional<uint8_t> best;
    size_t bestLen = 0;
    for (size_t i = 0; i < std::size(kTypes); ++i) {
        std::string_view t = kTypes[i].name;
        if (s.size() > t.size() && startsWith(s, t) && s[t.size()] == '_' && t.size() > bestLen) {
            best = uint8_t(i);
            bestLen = t.size();
        }
    }
    if (!best) return std::nullopt;
    std::string_view rest = s.substr(bestLen + 1);
    uint32_t h = crc32Raw(rest.data(), rest.size()) & 0xFFFFFF;
    return uint32_t(*best) << 24 | h;
}

std::vector<IndexEntry> readDbIndex(const fs::path& file) {
    std::vector<IndexEntry> out;
    for (auto& line : split(readText(file), '\n')) {
        std::string l = trim(line);
        if (l.empty() || l[0] == '#') continue;
        auto parts = split(l, ' ');
        IndexEntry e;
        e.name = parts[0];
        if (parts.size() > 1) e.timestamp = uint32_t(std::stoul(parts[1]));
        if (parts.size() > 2) e.version = parts[2];
        auto id = idFromName(e.name);
        if (!id) continue;
        e.id = *id;
        out.push_back(e);
    }
    return out;
}

void Names::add(std::string_view name) {
    std::string base = baseAssetName(name);
    if (auto id = idFromName(base)) byId_.emplace(*id, base);
}

std::string Names::lookup(uint32_t id) const {
    auto it = byId_.find(id);
    return it == byId_.end() ? std::string() : it->second;
}

fs::path pathForId(const fs::path& root, uint32_t id) {
    std::string h = hex32(id);
    uint8_t type = uint8_t(id >> 24);
    if (type == kTypeBundle || type == kTypeStreamBundle) return root / "Bundle" / h.substr(0, 2) / h.substr(2);
    return root / "Debug" / h.substr(0, 2) / h.substr(2, 2) / h.substr(4, 2) / h.substr(6, 2);
}

fs::path pathForEntry(const fs::path& root, const IndexEntry& e) {
    const std::string font = "aid_xuifontcachefont_banjox_default_";
    const std::string metaDefault = "aid_xuifontcachemeta_banjox_default_";
    const std::string metaPrecache = "aid_xuifontcachemeta_banjox_precache_";
    if (startsWith(e.name, font)) return root / "xuifontcachefont" / (e.name.substr(font.size()) + ".sbp");
    if (startsWith(e.name, metaDefault)) return root / "xuifontcachemeta" / ("metafont_" + e.name.substr(metaDefault.size()) + ".ini");
    if (startsWith(e.name, metaPrecache)) return root / "xuifontcachemeta" / ("precache_" + e.name.substr(metaPrecache.size()) + ".ini");
    return pathForId(root, e.id);
}

std::optional<uint32_t> idFromPath(const fs::path& p) {
    std::vector<std::string> parts;
    for (auto& c : p) parts.push_back(c.string());
    auto isHex = [](const std::string& s, size_t n) {
        if (s.size() != n) return false;
        for (char c : s) if (!std::isxdigit(uint8_t(c))) return false;
        return true;
    };
    size_t n = parts.size();
    if (n >= 1 && isHex(parts[n - 1], 8)) return parseHex(parts[n - 1]);
    if (n >= 2 && isHex(parts[n - 2], 2) && isHex(parts[n - 1], 6)) return parseHex(parts[n - 2] + parts[n - 1]);
    if (n >= 4 && isHex(parts[n - 4], 2) && isHex(parts[n - 3], 2) && isHex(parts[n - 2], 2) && isHex(parts[n - 1], 2))
        return parseHex(parts[n - 4] + parts[n - 3] + parts[n - 2] + parts[n - 1]);
    return std::nullopt;
}

} // namespace nut::assetdb
