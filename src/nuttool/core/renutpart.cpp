#include "renutpart.hpp"

#include "objparams.hpp"

#include <algorithm>
#include <cstdio>

namespace nut::renutpart {
namespace {

constexpr std::string_view kPartPrefix = "aid_objparams_banjox_vehicleblock_";
// Fields reNut manages itself: the class can't change, and the name is the garage name.
constexpr uint32_t kObjTag = 0x002, kObjDefId = 0x042, kName = 0x0A0;

const char* const kCallbacks[] = {
    "on_spawn = function(part) end,",
    "on_attach = function(part) end,",
    "on_detach = function(part) end,",
    "on_tick = function(part, dt) end,",
    "on_physics = function(part, dt) end,                 -- forces: part:vehicle():push(force, dt)",
    "on_throttle = function(part, throttle, dt) end,      -- throttle: -1 (reverse) to 1",
    "on_use = function(part) end,                         -- its button",
    "on_release = function(part) end,",
    "on_driver_in = function(part) end,",
    "on_driver_out = function(part) end,",
    "on_damage = function(part, amount) end,              -- renut.set_amount(x) changes it",
    "on_break = function(part) end,",
    "on_destroy = function(part) end,",
};

std::string luaString(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out + "\"";
}

std::string cString(const Bytes& d, uint32_t offset, uint32_t size) {
    uint32_t n = 0;
    while (n < size && d[offset + n]) ++n;
    return std::string(reinterpret_cast<const char*>(&d[offset]), n);
}

std::string format(const char* fmt, auto... args) {
    char buf[64];
    std::snprintf(buf, sizeof buf, fmt, args...);
    return buf;
}

// One `[0x190] = { f32 = 80 },  -- mass` line per value (a raw run gives one per byte).
void fieldLines(const objparams::Change& c, const Bytes& d, const gamescript::NameLookup& names, std::vector<std::string>& out) {
    std::string comment = c.name;
    auto line = [&](uint32_t offset, const std::string& value) {
        std::string s = format("[0x%03X] = { ", offset) + value + " },";
        if (!comment.empty()) {
            s.resize(std::max<size_t>(s.size(), 36), ' ');
            s += "  -- " + comment;
        }
        out.push_back(s);
    };
    switch (c.type) {
    case 'f': line(c.offset, format("f32 = %.9g", double(bef32(&d[c.offset])))); break;
    case 'i': line(c.offset, format("i32 = %d", int32_t(be32(&d[c.offset])))); break;
    case 'a':
    case 'x': {
        uint32_t v = be32(&d[c.offset]);
        if (names && c.type == 'a' && !names(v).empty()) comment += (comment.empty() ? "" : ": ") + names(v);
        line(c.offset, format("u32 = 0x%08X", v));
        break;
    }
    case 's': line(c.offset, "str = " + luaString(cString(d, c.offset, c.size))); break;
    default:
        for (uint32_t i = 0; i < c.size; ++i) line(c.offset + i, format("u8 = %u", unsigned(d[c.offset + i])));
        break;
    }
}

} // namespace

std::string baseName(const std::string& part) {
    std::string b = lower(trim(part));
    if (startsWith(b, kPartPrefix)) b.erase(0, kPartPrefix.size());
    return b;
}

bool validId(const std::string& id) {
    if (id.empty() || id.size() > 40) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

std::string lua(const Part& p, const gamescript::NameLookup& names) {
    if (!validId(p.id)) fail("a part id is 1-40 characters of a-z, 0-9 and _ (got '" + p.id + "')");
    std::string name = p.name;
    std::vector<std::string> fields;
    if (!p.original.empty() && !p.edited.empty()) {
        for (const auto& c : objparams::changes(p.original, p.edited)) {
            if (c.offset == kObjTag || c.offset == kObjDefId) continue;
            // The record's name is a text key; an edited one is taken as the garage name.
            if (c.offset == kName) {
                if (name.empty()) name = cString(p.edited, c.offset, c.size);
                continue;
            }
            fieldLines(c, p.edited, names, fields);
        }
    }

    std::string out = "-- " + (name.empty() ? p.id : name) + ": a reNut vehicle part, made with NutTool from " + p.base + ".\n";
    out += "-- reNut's docs/tutorials/parts-and-vehicles.md describes renut.parts.add and the callbacks.\n\n";
    out += "renut.parts.add{\n";
    out += "  id = " + luaString(p.id) + ",\n";
    out += "  base = " + luaString(p.base) + ",\n";
    if (!name.empty()) out += "  name = " + luaString(name) + ",\n";
    out += "  count = " + std::to_string(std::clamp(p.count, 1, 99)) + ",\n";
    if (!p.model.empty()) out += "  model = " + luaString(p.model) + ",                -- in the mod's bundles folder\n";
    if (!p.attach.empty()) out += "  attach = " + luaString(p.attach) + ",               -- its footprint, in the mod's bundles folder\n";
    if (!fields.empty()) {
        out += "  fields = {                              -- what differs from " + p.base + "\n";
        for (auto& f : fields) out += "    " + f + "\n";
        out += "  },\n";
    }
    if (p.behaviour) {
        out += "  params = {},                            -- your settings, as part.params\n\n";
        out += "  -- A new kind of part: fill in what it does. Callbacks it doesn't need can go.\n";
        for (const char* c : kCallbacks) out += std::string("  ") + c + "\n";
    }
    out += "}\n";
    return out;
}

fs::path write(const fs::path& mod, const Part& p, const std::string& modName, const std::string& author, bool overwrite,
               const gamescript::NameLookup& names) {
    std::string script = lua(p, names);
    fs::path file = mod / "scripts" / (p.id + ".lua");
    if (fs::exists(longPath(file)) && !overwrite) fail(file.string() + " exists already");
    fs::create_directories(longPath(file.parent_path()));
    writeText(file, script);
    fs::path toml = mod / "mod.toml";
    if (!fs::exists(longPath(toml))) {
        std::string title = modName.empty() ? mod.filename().string() : modName;
        std::string text = "name = " + luaString(title) + "\ndescription = \"Vehicle parts made with NutTool.\"\nversion = \"1\"\n";
        if (!author.empty()) text += "author = " + luaString(author) + "\n";
        writeText(toml, text);
    }
    return file;
}

} // namespace nut::renutpart
