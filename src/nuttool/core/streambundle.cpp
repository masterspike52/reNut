#include "streambundle.hpp"

#include <cmath>
#include <fstream>

namespace nut::streambundle {

bool looksLikeStreamBundle(const Bytes& d) {
    if (d.size() < 20) return false;
    return std::fabs(bef32(d.data()) - kVersion) < 0.01f && be32(&d[4]) == 12;
}

StreamBundle StreamBundle::parse(const Bytes& file) {
    if (!looksLikeStreamBundle(file)) fail("not a stream bundle");
    Reader r(file);
    StreamBundle sb;
    sb.version = r.f32();
    r.u32();
    uint32_t count = r.u32();
    sb.timestamp = r.u32();
    uint32_t deps = r.u32();
    for (uint32_t i = 0; i < deps; ++i) sb.dependencies.push_back(r.u32());
    struct Raw { uint32_t id, off, size; };
    std::vector<Raw> raw(count);
    for (auto& e : raw) { e.id = r.u32(); e.off = r.u32(); e.size = r.u32(); }
    size_t expect = r.pos();
    for (auto& e : raw) {
        if (e.size == 0) { sb.entries.push_back({e.id, {}}); continue; } // placeholder slots are {0,0,0}
        if (e.off != expect) fail("stream bundle: entry " + hex32(e.id) + " is not contiguous");
        if (uint64_t(e.off) + e.size > file.size()) fail("stream bundle: entry " + hex32(e.id) + " exceeds file");
        sb.entries.push_back({e.id, Bytes(file.begin() + e.off, file.begin() + e.off + e.size)});
        expect += e.size;
    }
    if (expect != file.size()) fail("stream bundle: " + std::to_string(file.size() - expect) + " trailing bytes");
    return sb;
}

bool readIndex(const fs::path& file, std::vector<IndexEntry>& out) {
    std::ifstream f(longPath(readPath(file)), std::ios::binary);
    if (!f) return false;
    uint8_t head[20];
    if (!f.read(reinterpret_cast<char*>(head), sizeof head)) return false;
    if (!looksLikeStreamBundle(Bytes(head, head + sizeof head))) return false;
    uint32_t count = be32(head + 8), deps = be32(head + 16);
    if (count > 1000000 || deps > 100000) return false;
    Bytes table((size_t(deps) + size_t(count) * 3) * 4);
    if (!f.read(reinterpret_cast<char*>(table.data()), std::streamsize(table.size()))) return false;
    out.clear();
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = &table[(size_t(deps) + size_t(i) * 3) * 4];
        out.push_back({be32(e), be32(e + 4), be32(e + 8)});
    }
    return true;
}

Bytes readEntry(const fs::path& file, const IndexEntry& e) {
    std::ifstream f(longPath(readPath(file)), std::ios::binary);
    if (!f) fail("cannot open " + file.string());
    Bytes data(e.size);
    f.seekg(std::streamoff(e.offset));
    if (e.size && !f.read(reinterpret_cast<char*>(data.data()), std::streamsize(e.size))) fail("stream bundle entry exceeds file");
    return data;
}

Bytes StreamBundle::serialize() const {
    Writer w;
    w.f32(version);
    w.u32(12);
    w.u32(uint32_t(entries.size()));
    w.u32(timestamp);
    w.u32(uint32_t(dependencies.size()));
    for (uint32_t d : dependencies) w.u32(d);
    uint32_t off = uint32_t(w.pos() + entries.size() * 12);
    for (auto& e : entries) {
        w.u32(e.id);
        w.u32(e.data.empty() ? 0 : off);
        w.u32(uint32_t(e.data.size()));
        off += uint32_t(e.data.size());
    }
    for (auto& e : entries) w.bytes(e.data);
    return w.buf();
}

} // namespace nut::streambundle
