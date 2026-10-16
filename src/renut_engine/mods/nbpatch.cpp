#include "renut_engine/mods/nbpatch.h"

#include <cctype>
#include <cstring>

#include "renut_engine/mods/json.h"
#include "renut_engine/mods/zip_reader.h"

namespace renut::mods {

namespace {

// PatchPackage.Slug: lower-case letters and digits, other runs become '-'.
std::string Slug(const std::string &name) {
    std::string out;
    for (unsigned char c : name) {
        c = static_cast<unsigned char>(std::tolower(c));
        if (c < 0x80 && std::isalnum(c))
            out += char(c);
        else if (!out.empty() && out.back() != '-')
            out += '-';
    }
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    return out;
}

std::vector<std::string> Strings(const JsonValue &root, std::string_view key) {
    std::vector<std::string> out;
    for (const JsonValue &v : root.Array(key))
        if (v.type == JsonValue::Type::String)
            out.push_back(v.string);
    return out;
}

} // namespace

std::optional<PatchManifest> ReadManifest(ZipReader &zip, std::string *error) {
    auto bytes = zip.Read("patch.json", error);
    if (!bytes)
        return std::nullopt;
    auto root = ParseJson({reinterpret_cast<const char *>(bytes->data()), bytes->size()}, error);
    if (!root)
        return std::nullopt;
    if (root->type != JsonValue::Type::Object) {
        *error = "patch.json is not an object";
        return std::nullopt;
    }

    PatchManifest m;
    m.format = int(root->Int("Format", 1));
    m.name = root->String("Name");
    m.id = root->String("Id");
    if (m.id.empty())
        m.id = Slug(m.name);
    m.version = root->String("Version", "1");
    m.author = root->String("Author");
    m.category = root->String("Category");
    m.requires_ids = Strings(*root, "Requires");
    m.conflicts = Strings(*root, "Conflicts");
    m.ops = root->Array("Ops").size();
    for (const JsonValue &f : root->Array("Files")) {
        PatchFile pf;
        pf.path = f.String("Path");
        pf.kind = f.String("Kind", "delta");
        pf.source_sha256 = f.String("SourceSha256");
        pf.source_size = f.Int("SourceSize");
        pf.target_sha256 = f.String("TargetSha256");
        pf.target_size = f.Int("TargetSize");
        pf.entry = f.String("Entry");
        m.files.push_back(std::move(pf));
    }
    for (const JsonValue &e : root->Array("ExeMods"))
        m.exe_mods.push_back({e.String("Id"), e.String("Name"), e.Array("Words").size()});
    return m;
}

// "NBDL", u32 version 1, u64 target length (little-endian), then ops to the end:
//   0x01 COPY varint source offset, varint length
//   0x02 ADD  varint length, bytes
std::optional<std::vector<uint8_t>> ApplyDelta(std::span<const uint8_t> source, std::span<const uint8_t> delta, std::string *error) {
    auto fail = [&](const char *what) {
        *error = what;
        return std::nullopt;
    };
    if (delta.size() < 16 || std::memcmp(delta.data(), "NBDL", 4) != 0)
        return fail("not an NB delta");
    uint64_t length = 0;
    for (int i = 0; i < 8; ++i)
        length |= uint64_t(delta[8 + size_t(i)]) << (i * 8);
    if (length > 0x7FFFFFFFull)
        return fail("delta target too large");

    size_t p = 16;
    auto varint = [&](uint64_t &value) {
        value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p >= delta.size())
                return false;
            uint8_t b = delta[p++];
            value |= uint64_t(b & 0x7F) << shift;
            if (b < 0x80)
                return true;
        }
        return false;
    };

    std::vector<uint8_t> out(length);
    uint64_t o = 0;
    while (p < delta.size()) {
        uint8_t op = delta[p++];
        uint64_t a, n;
        if (op == 1) {
            if (!varint(a) || !varint(n))
                return fail("truncated delta");
            if (a + n > source.size() || o + n > length)
                return fail("delta copy out of range (wrong source file?)");
            std::memcpy(out.data() + o, source.data() + a, size_t(n));
            o += n;
        } else if (op == 2) {
            if (!varint(n))
                return fail("truncated delta");
            if (p + n > delta.size() || o + n > length)
                return fail("delta literal out of range");
            std::memcpy(out.data() + o, delta.data() + p, size_t(n));
            p += size_t(n);
            o += n;
        } else {
            return fail("bad delta op");
        }
    }
    if (o != length)
        return fail("delta produced a short file");
    return out;
}

} // namespace renut::mods
