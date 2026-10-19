#include "renut_engine/mods/custom_parts.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>
#include <system_error>

#include "renut_engine/app/renut_logging.h"
#include "renut_engine/mods/json.h"
#include "renut_engine/mods/zip_reader.h"

namespace renut::mods {

namespace {

constexpr std::string_view kPartsFolder = "parts/";
constexpr std::string_view kObjparamsPrefix = "aid_objparams_";
constexpr std::string_view kVehicleBlockPrefix = "banjox_vehicleblock_";
constexpr std::string_view kModelPrefix = "aid_model_";
constexpr std::string_view kVehiclePartsPrefix = "banjox_vehicleparts_";
constexpr std::string_view kAttachPrefix = "aid_avatarhavokdata_";
// Records start with their u16 size; parts are far smaller than this.
constexpr size_t kMaxRecordSize = 0x10000;

std::vector<CustomPart> g_parts;
std::vector<KnownCustomPart> g_known;
std::vector<ModBundle> g_bundles;

constexpr std::string_view kKnownFile = ".renut_custom_parts.json";
constexpr uint8_t kTypeObjparams = 0x1F;
constexpr const char kPartAssetPrefix[] = "banjox_vehicleblock_renut_";

std::string JsonEscape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (uint8_t(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(uint8_t(c)));
            out += buf;
        } else {
            out += c;
        }
    }
    return out;
}

std::string Hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
}

std::string Lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

bool IsPartId(const std::string &id) {
    // renutpart_<id> must fit the record's 64-byte text key field.
    if (id.empty() || id.size() > 40)
        return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

// 'name' without 'type_prefix' ("aid_<type>_"), starting with 'stem_prefix':
// "propulsion_engines_large", "banjox_vehicleblock_…" or "aid_objparams_…"
// -> "banjox_vehicleblock_propulsion_engines_large".
std::string AssetName(std::string name, std::string_view type_prefix, std::string_view stem_prefix) {
    name = Lower(std::move(name));
    if (name.starts_with(type_prefix))
        name.erase(0, type_prefix.size());
    if (!name.starts_with(stem_prefix))
        name.insert(0, stem_prefix);
    return name;
}

std::string BaseName(std::string base) {
    return AssetName(std::move(base), kObjparamsPrefix, kVehicleBlockPrefix);
}

void PutBigEndian(std::vector<uint8_t> &out, uint64_t value, size_t size) {
    for (size_t i = 0; i < size; ++i)
        out.push_back(uint8_t(value >> (8 * (size - 1 - i))));
}

// { "f32": 1.5 } and the like -> its big-endian bytes.
std::optional<std::vector<uint8_t>> FieldBytes(const JsonValue &value, std::string *error) {
    if (value.type != JsonValue::Type::Object || value.object.size() != 1) {
        *error = "a field value is { \"<type>\": <value> }";
        return std::nullopt;
    }
    const std::string &type = value.object[0].name;
    const JsonValue &v = value.object[0].value;
    std::vector<uint8_t> bytes;
    if (type == "str") {
        if (v.type != JsonValue::Type::String) {
            *error = "str needs a string";
            return std::nullopt;
        }
        bytes.assign(v.string.begin(), v.string.end());
        bytes.push_back(0);
        return bytes;
    }
    if (v.type != JsonValue::Type::Number) {
        *error = type + " needs a number";
        return std::nullopt;
    }
    if (type == "f32") {
        const float f = float(v.number);
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        PutBigEndian(bytes, bits, 4);
    } else if (type == "u32" || type == "i32") {
        PutBigEndian(bytes, uint32_t(int64_t(std::llround(v.number))), 4);
    } else if (type == "u16") {
        PutBigEndian(bytes, uint16_t(std::llround(v.number)), 2);
    } else if (type == "u8") {
        PutBigEndian(bytes, uint8_t(std::llround(v.number)), 1);
    } else {
        *error = "unknown field type '" + type + "' (f32, u32, i32, u16, u8 or str)";
        return std::nullopt;
    }
    return bytes;
}

std::optional<CustomPart> ReadPart(ZipReader &zip, const std::string &entry, std::string *error) {
    auto text = zip.Read(entry, error);
    if (!text)
        return std::nullopt;
    auto json = ParseJson(std::string_view(reinterpret_cast<const char *>(text->data()), text->size()), error);
    if (!json)
        return std::nullopt;
    if (json->type != JsonValue::Type::Object) {
        *error = "not a JSON object";
        return std::nullopt;
    }

    CustomPart part;
    part.id = Lower(json->String("id"));
    if (!IsPartId(part.id)) {
        *error = "\"id\" must be 1-40 characters of a-z, 0-9 and _";
        return std::nullopt;
    }
    part.name = json->String("name");
    const int64_t count = json->Int("count", 1);
    part.count = uint32_t(count < 1 ? 1 : count > 99 ? 99 : count);

    const std::string objparams = json->String("objparams");
    if (!objparams.empty()) {
        auto record = zip.Read(objparams, error);
        if (!record)
            return std::nullopt;
        if (record->size() < 2 || record->size() > kMaxRecordSize ||
            ((size_t((*record)[0]) << 8) | (*record)[1]) != record->size()) {
            *error = objparams + " is not an objparams record (its first u16 is its size)";
            return std::nullopt;
        }
        part.objparams = std::move(*record);
    }
    if (const JsonValue *params = json->Find("params")) {
        if (params->type != JsonValue::Type::Object) {
            *error = "\"params\" is an object";
            return std::nullopt;
        }
        part.params = *params;
    }
    part.script = json->String("script");
    if (!part.script.empty()) {
        auto source = zip.Read(part.script, error);
        if (!source)
            return std::nullopt;
        part.script_source.assign(source->begin(), source->end());
    }
    const std::string base = json->String("base");
    if (base.empty() && part.objparams.empty()) {
        *error = "needs \"base\" (a shipped part) or \"objparams\" (a whole record)";
        return std::nullopt;
    }
    if (!base.empty())
        part.base = BaseName(base);
    if (const std::string model = json->String("model"); !model.empty())
        part.model = AssetName(model, kModelPrefix, kVehiclePartsPrefix);
    if (const std::string attach = json->String("attach"); !attach.empty())
        part.attach = AssetName(attach, kAttachPrefix, kVehicleBlockPrefix);

    if (const JsonValue *fields = json->Find("fields")) {
        if (fields->type != JsonValue::Type::Object) {
            *error = "\"fields\" is an object of \"0x<offset>\": { \"<type>\": <value> }";
            return std::nullopt;
        }
        for (const JsonMember &member : fields->object) {
            CustomPartField field;
            char *end = nullptr;
            const unsigned long offset = std::strtoul(member.name.c_str(), &end, 0);
            if (member.name.empty() || *end || offset >= kMaxRecordSize) {
                *error = "field offset '" + member.name + "' is not a number like 0x3FC";
                return std::nullopt;
            }
            field.offset = uint32_t(offset);
            auto bytes = FieldBytes(member.value, error);
            if (!bytes) {
                *error = "field " + member.name + ": " + *error;
                return std::nullopt;
            }
            field.bytes = std::move(*bytes);
            part.fields.push_back(std::move(field));
        }
    }
    return part;
}

} // namespace

std::optional<std::vector<CustomPart>> ReadCustomParts(ZipReader &zip, std::string *error) {
    std::vector<CustomPart> parts;
    for (const ZipReader::EntryInfo &entry : zip.Entries()) {
        const std::string name = Lower(entry.name);
        if (!name.starts_with(kPartsFolder) || !name.ends_with(".json"))
            continue;
        auto part = ReadPart(zip, entry.name, error);
        if (!part) {
            *error = entry.name + ": " + *error;
            return std::nullopt;
        }
        parts.push_back(std::move(*part));
    }
    return parts;
}

void AddCustomParts(std::vector<CustomPart> parts) {
    for (CustomPart &part : parts) {
        bool taken = false;
        for (const CustomPart &other : g_parts)
            taken |= other.id == part.id;
        if (taken) {
            RNUT_ERROR("mods: {}: part '{}' skipped: an earlier mod has a part with that id", part.mod, part.id);
            continue;
        }
        RNUT_INFO("mods: {}: part '{}' ({}){}{}{}", part.mod, part.id, part.name.empty() ? "no name" : part.name,
                  part.base.empty() ? "" : ", from " + part.base,
                  part.script.empty() ? "" : ", scripted by " + part.script,
                  part.model.empty() ? "" : ", model " + part.model);
        g_parts.push_back(std::move(part));
    }
}

const std::vector<CustomPart> &CustomParts() {
    return g_parts;
}

void AddModBundle(ModBundle bundle) {
    for (const ModBundle &other : g_bundles) {
        if (other.id == bundle.id) {
            RNUT_ERROR("mods: {}: bundle {:08X} skipped: '{}' already adds it", bundle.mod, bundle.id, other.mod);
            return;
        }
    }
    g_bundles.push_back(std::move(bundle));
}

const std::vector<ModBundle> &ModBundles() {
    return g_bundles;
}

void RememberCustomParts(const std::filesystem::path &mods_folder) {
    namespace fs = std::filesystem;
    const fs::path file = mods_folder / kKnownFile;
    std::error_code ec;
    if (fs::is_regular_file(file, ec)) {
        std::ifstream in(file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string error;
        auto json = ParseJson(text, &error);
        if (!json) {
            RNUT_WARN("mods: {} is damaged ({}); vehicles with parts of removed mods may not load", kKnownFile, error);
        } else {
            for (const JsonValue &p : json->Array("parts")) {
                KnownCustomPart known;
                known.aid = uint32_t(std::strtoul(p.String("aid").c_str(), nullptr, 16));
                known.base_aid = uint32_t(std::strtoul(p.String("base").c_str(), nullptr, 16));
                known.id = p.String("id");
                known.mod = p.String("mod");
                if (known.aid)
                    g_known.push_back(std::move(known));
            }
        }
    }

    bool changed = false;
    for (const CustomPart &part : g_parts) {
        KnownCustomPart now;
        now.aid = AssetId(kTypeObjparams, kPartAssetPrefix + part.id);
        now.base_aid = part.base.empty() ? 0 : AssetId(kTypeObjparams, part.base);
        now.id = part.id;
        now.mod = part.mod;
        auto it = std::find_if(g_known.begin(), g_known.end(), [&](const KnownCustomPart &k) { return k.aid == now.aid; });
        if (it == g_known.end()) {
            g_known.push_back(std::move(now));
            changed = true;
        } else if (it->base_aid != now.base_aid || it->id != now.id || it->mod != now.mod) {
            *it = std::move(now);
            changed = true;
        }
    }
    if (!changed)
        return;
    std::string text = "{\n  \"parts\": [\n";
    for (size_t i = 0; i < g_known.size(); ++i) {
        const KnownCustomPart &k = g_known[i];
        text += "    { \"aid\": \"" + Hex(k.aid) + "\", \"base\": \"" + Hex(k.base_aid) + "\", \"id\": \"" +
                JsonEscape(k.id) + "\", \"mod\": \"" + JsonEscape(k.mod) + "\" }" + (i + 1 < g_known.size() ? "," : "") +
                "\n";
    }
    text += "  ]\n}\n";
    fs::create_directories(mods_folder, ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), std::streamsize(text.size()));
    if (!out)
        RNUT_WARN("mods: cannot write {}; vehicles with this run's custom parts may not load once their mods are removed",
                  kKnownFile);
}

const KnownCustomPart *FindKnownCustomPart(uint32_t aid) {
    for (const KnownCustomPart &k : g_known)
        if (k.aid == aid)
            return &k;
    return nullptr;
}

bool AnyKnownCustomParts() {
    return !g_known.empty();
}

const std::vector<KnownCustomPart> &KnownCustomParts() {
    return g_known;
}

uint32_t AssetId(uint8_t type, const std::string &name) {
    const uint32_t hash = ~Crc32(reinterpret_cast<const uint8_t *>(name.data()), name.size()) & 0xFFFFFFu;
    return uint32_t(type) << 24 | hash;
}

} // namespace renut::mods
