#include "caff.hpp"

#include "lzx.hpp"

#include <algorithm>
#include <optional>

namespace nut::caff {
namespace {

constexpr size_t kHeaderSize = 0x78;
constexpr const char* kVersion = "07.08.06.0036";

// Header offsets of the swap/reloc count pairs, indexed [set][0=entries,1=list length].
constexpr size_t kSwapCount[2][2] = {{0x24, 0x28}, {0x34, 0x38}};
constexpr size_t kRelocCount[2][2] = {{0x2C, 0x30}, {0x3C, 0x40}};

Bytes maybeDecompress(const uint8_t* p, uint32_t stored, uint32_t size, const char* what) {
    if (stored == size) return Bytes(p, p + size);
    if (stored > size) fail(std::string("CAFF: ") + what + " stored size exceeds size");
    // Compressed CAFF blocks are not used by the shipped assets; the runtime hands them to
    // the same LZX codec as XCTD, so attempt that.
    return lzx::decompress(p, stored, size);
}

} // namespace

bool isCaff(const Bytes& d) { return d.size() >= kHeaderSize && std::memcmp(d.data(), "CAFF", 4) == 0; }

uint32_t headerChecksum(const uint8_t* header) {
    uint8_t tmp[kHeaderSize];
    std::memcpy(tmp, header, kHeaderSize);
    std::memset(tmp + 0x18, 0, 4);
    uint32_t h = 0;
    for (size_t i = 0; i < kHeaderSize; ++i) {
        h = (h << 4) + uint32_t(int32_t(int8_t(tmp[i]))); // the game sign-extends each byte
        uint32_t g = h & 0xF0000000u;
        if (g) h ^= g >> 24 | g;
    }
    return h;
}

Caff Caff::parse(const Bytes& file) {
    std::optional<Caff> c = parseImpl(file, nullptr, nullptr);
    return std::move(*c);
}

std::optional<Caff> Caff::parsePartial(const Bytes& prefix, const std::vector<std::string>& keep, size_t& need) {
    need = 0;
    return parseImpl(prefix, &keep, &need);
}

std::optional<Caff> Caff::parseImpl(const Bytes& file, const std::vector<std::string>* keep, size_t* need) {
    if (keep && file.size() < kHeaderSize) {
        *need = kHeaderSize;
        return std::nullopt;
    }
    if (!isCaff(file)) fail("not a CAFF file");
    if (std::memcmp(&file[4], kVersion, 13) != 0) fail("unsupported CAFF version");
    Caff c;
    c.header.assign(file.begin(), file.begin() + kHeaderSize);
    const uint8_t* h = file.data();
    if (!(h[0x48] & 1)) fail("little-endian CAFF files are not supported");
    uint32_t hdrSize = be32(h + 0x14);
    if (hdrSize != kHeaderSize) fail("unexpected CAFF header size");
    uint32_t nNames = be32(h + 0x1C), nSyms = be32(h + 0x20);
    uint8_t nSecs = h[0x49], nPools = h[0x4B];
    uint32_t secNameBytes = be32(h + 0x4C);
    uint32_t g0Size = be32(h + 0x50), g0Stored = be32(h + 0x60);
    uint32_t g1Size = be32(h + 0x64), g1Stored = be32(h + 0x74);
    size_t pos = kHeaderSize;
    if (keep && pos + g0Stored + g1Stored > file.size()) {
        *need = pos + g0Stored + g1Stored;
        return std::nullopt;
    }
    if (pos + g0Stored + g1Stored > file.size()) fail("CAFF: info blocks exceed file");
    Bytes g0 = maybeDecompress(&file[pos], g0Stored, g0Size, "info block");
    pos += g0Stored;
    Bytes g1 = maybeDecompress(&file[pos], g1Stored, g1Size, "fixup block");
    pos += g1Stored;

    // ---- info block
    Reader r(g0);
    struct RawSec { uint32_t nameOff; uint8_t align; uint32_t first, size, stored; };
    std::vector<RawSec> raw(nSecs);
    for (auto& s : raw) {
        s.nameOff = r.u32();
        s.align = r.u8();
        s.first = r.u32();
        s.size = r.u32();
        r.skip(16);
        s.stored = r.u32();
    }
    size_t namesAt = r.pos();
    r.skip(secNameBytes);
    for (auto& s : raw) {
        Section sec;
        Reader nr(g0.data() + namesAt, secNameBytes);
        nr.seek(s.nameOff);
        sec.name = nr.cstr();
        sec.alignLog2 = s.align;
        sec.firstSymbol = s.first;
        c.sections.push_back(std::move(sec));
    }
    uint32_t strBytes = r.u32();
    if (strBytes) {
        std::vector<uint32_t> offs(nNames);
        for (auto& o : offs) o = r.u32();
        const uint8_t* strs = r.cur();
        r.skip(strBytes);
        for (uint32_t o : offs) {
            Reader sr(strs, strBytes);
            sr.seek(o);
            c.symbolNames.push_back(sr.cstr());
        }
    }
    uint32_t extraBytes = r.u32();
    c.extra = r.bytes(extraBytes);
    for (uint32_t i = 0; i < nSyms; ++i) {
        Symbol s;
        s.nameIndex = r.u32();
        s.offset = r.u32();
        s.size = r.u32();
        s.section = r.u8();
        s.kind = r.u8();
        c.symbols.push_back(s);
    }

    // ---- fixup block: swapA, swapA list, swapB, swapB list, relocA, relocA list, relocB, relocB list, pools
    Reader f(g1);
    auto readSwaps = [&](int set) {
        uint32_t n = be32(h + kSwapCount[set][0]), listLen = be32(h + kSwapCount[set][1]);
        std::vector<uint32_t> counts;
        for (uint32_t i = 0; i < n; ++i) {
            SwapEntry e;
            e.symbol = f.u32();
            counts.push_back(f.u32());
            e.elementCount = f.u32();
            e.elementSize = f.u32();
            e.stride = f.u32();
            c.swaps[set].push_back(e);
        }
        uint32_t used = 0;
        for (size_t i = 0; i < c.swaps[set].size(); ++i) {
            for (uint32_t k = 0; k < counts[i]; ++k) c.swaps[set][i].offsets.push_back(f.u32());
            used += counts[i];
        }
        if (used != listLen) fail("CAFF: swap list length mismatch");
    };
    auto readRelocs = [&](int set) {
        uint32_t n = be32(h + kRelocCount[set][0]), listLen = be32(h + kRelocCount[set][1]);
        std::vector<uint32_t> counts;
        for (uint32_t i = 0; i < n; ++i) {
            Relocation e;
            e.from = f.u32();
            e.to = f.u32();
            counts.push_back(f.u32());
            c.relocs[set].push_back(e);
        }
        uint32_t used = 0;
        for (size_t i = 0; i < c.relocs[set].size(); ++i) {
            for (uint32_t k = 0; k < counts[i]; ++k) c.relocs[set][i].offsets.push_back(f.u32());
            used += counts[i];
        }
        if (used != listLen) fail("CAFF: relocation list length mismatch");
    };
    readSwaps(0);
    readSwaps(1);
    readRelocs(0);
    readRelocs(1);
    std::vector<uint32_t> poolCounts;
    for (uint8_t i = 0; i < nPools; ++i) {
        Pool p;
        p.symbol = f.u32();
        poolCounts.push_back(f.u32());
        c.pools.push_back(p);
    }
    for (size_t i = 0; i < c.pools.size(); ++i) {
        for (uint32_t k = 0; k < poolCounts[i]; ++k) {
            PoolString s;
            s.length = f.u32();
            s.offset = f.u32();
            s.hash = f.u32();
            s.flags = f.u32();
            c.pools[i].strings.push_back(s);
        }
    }
    if (f.remaining()) fail("CAFF: " + std::to_string(f.remaining()) + " unparsed bytes in fixup block");

    // ---- section data (a partial parse loads only the kept sections and leaves the rest empty)
    for (size_t i = 0; i < raw.size(); ++i) {
        bool load = !keep || std::find(keep->begin(), keep->end(), c.sections[i].name) != keep->end();
        if (load && keep && pos + raw[i].stored > file.size()) *need = std::max(*need, pos + raw[i].stored);
        else if (load) {
            if (pos + raw[i].stored > file.size()) fail("CAFF: section '" + c.sections[i].name + "' exceeds file");
            c.sections[i].data = maybeDecompress(&file[pos], raw[i].stored, raw[i].size, "section");
        }
        pos += raw[i].stored;
    }
    if (keep) {
        if (*need) return std::nullopt;
        return c;
    }
    if (pos != file.size()) fail("CAFF: " + std::to_string(file.size() - pos) + " trailing bytes");
    return c;
}

Bytes Caff::serialize() const {
    if (sections.size() > 255) fail("CAFF: too many sections");
    if (pools.size() > 255) fail("CAFF: too many string pools");

    // ---- info block
    Writer g0;
    std::string secNames;
    std::vector<uint32_t> nameOffs;
    for (auto& s : sections) {
        nameOffs.push_back(uint32_t(secNames.size()));
        secNames += s.name;
        secNames += '\0';
    }
    for (size_t i = 0; i < sections.size(); ++i) {
        g0.u32(nameOffs[i]);
        g0.u8(sections[i].alignLog2);
        g0.u32(sections[i].firstSymbol);
        g0.u32(uint32_t(sections[i].data.size()));
        g0.zeros(16);
        g0.u32(uint32_t(sections[i].data.size()));
    }
    g0.bytes(secNames.data(), secNames.size());
    std::string strs;
    std::vector<uint32_t> strOffs;
    for (auto& n : symbolNames) {
        strOffs.push_back(uint32_t(strs.size()));
        strs += n;
        strs += '\0';
    }
    g0.u32(uint32_t(strs.size()));
    if (!strs.empty()) {
        for (uint32_t o : strOffs) g0.u32(o);
        g0.bytes(strs.data(), strs.size());
    }
    g0.u32(uint32_t(extra.size()));
    g0.bytes(extra);
    for (auto& s : symbols) {
        g0.u32(s.nameIndex);
        g0.u32(s.offset);
        g0.u32(s.size);
        g0.u8(s.section);
        g0.u8(s.kind);
    }
    g0.align(4);

    // ---- fixup block
    Writer g1;
    uint32_t swapLen[2] = {}, relocLen[2] = {};
    for (int set = 0; set < 2; ++set) {
        for (auto& e : swaps[set]) {
            g1.u32(e.symbol);
            g1.u32(uint32_t(e.offsets.size()));
            g1.u32(e.elementCount);
            g1.u32(e.elementSize);
            g1.u32(e.stride);
        }
        for (auto& e : swaps[set]) { for (uint32_t o : e.offsets) g1.u32(o); swapLen[set] += uint32_t(e.offsets.size()); }
    }
    for (int set = 0; set < 2; ++set) {
        for (auto& e : relocs[set]) {
            g1.u32(e.from);
            g1.u32(e.to);
            g1.u32(uint32_t(e.offsets.size()));
        }
        for (auto& e : relocs[set]) { for (uint32_t o : e.offsets) g1.u32(o); relocLen[set] += uint32_t(e.offsets.size()); }
    }
    for (auto& p : pools) { g1.u32(p.symbol); g1.u32(uint32_t(p.strings.size())); }
    for (auto& p : pools)
        for (auto& s : p.strings) { g1.u32(s.length); g1.u32(s.offset); g1.u32(s.hash); g1.u32(s.flags); }

    // ---- header
    Bytes hdr = header;
    if (hdr.size() != kHeaderSize) {
        hdr.assign(kHeaderSize, 0);
        std::memcpy(hdr.data(), "CAFF", 4);
        std::memcpy(hdr.data() + 4, kVersion, 13);
        hdr[0x48] = 1;
    }
    uint8_t* h = hdr.data();
    putBe32(h + 0x14, uint32_t(kHeaderSize));
    putBe32(h + 0x1C, uint32_t(symbolNames.size()));
    putBe32(h + 0x20, uint32_t(symbols.size()));
    for (int set = 0; set < 2; ++set) {
        putBe32(h + kSwapCount[set][0], uint32_t(swaps[set].size()));
        putBe32(h + kSwapCount[set][1], swapLen[set]);
        putBe32(h + kRelocCount[set][0], uint32_t(relocs[set].size()));
        putBe32(h + kRelocCount[set][1], relocLen[set]);
    }
    h[0x49] = uint8_t(sections.size());
    h[0x4A] = 0; // everything is written uncompressed
    h[0x4B] = uint8_t(pools.size());
    putBe32(h + 0x4C, uint32_t(secNames.size()));
    std::memset(h + 0x50, 0, 40);
    putBe32(h + 0x50, uint32_t(g0.pos()));
    putBe32(h + 0x60, uint32_t(g0.pos()));
    putBe32(h + 0x64, uint32_t(g1.pos()));
    putBe32(h + 0x74, uint32_t(g1.pos()));
    putBe32(h + 0x18, headerChecksum(h));

    Bytes out = hdr;
    out.insert(out.end(), g0.buf().begin(), g0.buf().end());
    out.insert(out.end(), g1.buf().begin(), g1.buf().end());
    for (auto& s : sections) out.insert(out.end(), s.data.begin(), s.data.end());
    return out;
}

std::string Caff::symbolName(size_t i) const {
    const Symbol& s = symbols.at(i);
    if (s.nameIndex == 0 || s.nameIndex > symbolNames.size()) return {};
    return symbolNames[s.nameIndex - 1];
}

const Section& Caff::sectionOf(const Symbol& s) const {
    if (s.section == 0 || s.section > sections.size()) fail("CAFF: symbol has invalid section index");
    return sections[s.section - 1];
}

Bytes Caff::symbolData(size_t i) const {
    const Symbol& s = symbols.at(i);
    const Section& sec = sectionOf(s);
    if (uint64_t(s.offset) + s.size > sec.data.size()) fail("CAFF: symbol exceeds its section");
    return Bytes(sec.data.begin() + s.offset, sec.data.begin() + s.offset + s.size);
}

int Caff::findSection(std::string_view name) const {
    for (size_t i = 0; i < sections.size(); ++i) if (sections[i].name == name) return int(i);
    return -1;
}

void Caff::replaceSymbolData(size_t idx, const Bytes& data) {
    Symbol& s = symbols.at(idx);
    Section& sec = sections.at(s.section - 1);
    uint32_t oldEnd = s.offset + s.size;
    // Refuse when another symbol overlaps this one's byte range; shifting would corrupt it.
    for (size_t i = 0; i < symbols.size(); ++i) {
        const Symbol& o = symbols[i];
        if (i == idx || o.section != s.section || o.size == 0 || s.size == 0) continue;
        if (o.offset < oldEnd && s.offset < o.offset + o.size)
            fail("symbol " + std::to_string(idx + 1) + " overlaps symbol " + std::to_string(i + 1) + "; cannot resize it");
    }
    int64_t delta = int64_t(data.size()) - int64_t(s.size);
    uint32_t align = 1u << sec.alignLog2;
    int64_t shift = delta;
    if (shift > 0) shift = (shift + align - 1) / align * align;
    else if (shift < 0) shift = -((-shift) / align * align); // shrink only by whole alignment units
    // The symbol's slot is old size + shift; pad new data with zeros to fill it.
    size_t slot = size_t(int64_t(s.size) + shift);
    if (slot < data.size()) fail("internal: slot too small");
    Bytes nd(sec.data.begin(), sec.data.begin() + s.offset);
    nd.insert(nd.end(), data.begin(), data.end());
    nd.resize(nd.size() + (slot - data.size()), 0);
    nd.insert(nd.end(), sec.data.begin() + oldEnd, sec.data.end());
    sec.data = std::move(nd);
    for (size_t i = 0; i < symbols.size(); ++i) {
        Symbol& o = symbols[i];
        if (i != idx && o.section == s.section && o.offset >= oldEnd) o.offset = uint32_t(int64_t(o.offset) + shift);
    }
    s.size = uint32_t(data.size());
}

// A bundle's last symbol is its manifest, and the game reads it from there: new symbols, their
// bytes and their names go in front of it.
static bool isManifest(const Caff& c, size_t s) { return c.symbolName(s) == "manifest"; }

size_t Caff::appendSymbol(uint32_t nameIndex, uint8_t section, uint8_t kind, const Bytes& data) {
    Section& sec = sections.at(size_t(section) - 1);
    uint32_t align = 1u << sec.alignLog2;
    size_t man = !symbols.empty() && isManifest(*this, symbols.size() - 1) ? symbols.size() - 1 : symbols.size();
    // When the manifest is the last thing in this section, its bytes move behind the new ones.
    Bytes manData;
    if (man < symbols.size() && symbols[man].section == section) {
        bool last = std::none_of(symbols.begin(), symbols.end(), [&](const Symbol& o) { return o.section == section && o.offset > symbols[man].offset; });
        if (last) {
            manData = symbolData(man);
            sec.data.resize(symbols[man].offset);
        }
    }
    size_t at = (sec.data.size() + align - 1) / align * align;
    if (at + data.size() + manData.size() + align > 0xFFFFFFFFull) fail("CAFF: section " + sec.name + " over 4 GB");
    sec.data.resize(at, 0);
    sec.data.insert(sec.data.end(), data.begin(), data.end());
    if (!manData.empty()) {
        size_t mo = (sec.data.size() + align - 1) / align * align;
        sec.data.resize(mo, 0);
        sec.data.insert(sec.data.end(), manData.begin(), manData.end());
        symbols[man].offset = uint32_t(mo);
    }
    Symbol s;
    s.nameIndex = nameIndex;
    s.offset = uint32_t(at);
    s.size = uint32_t(data.size());
    s.section = section;
    s.kind = kind;
    if (man == symbols.size()) {
        symbols.push_back(s);
        return symbols.size() - 1;
    }
    // Insert before the manifest; whatever referred to it (1-based man + 1) moves up one.
    symbols.insert(symbols.begin() + std::ptrdiff_t(man), s);
    uint32_t old = uint32_t(man + 1);
    for (int set = 0; set < 2; ++set) {
        for (auto& r : relocs[set]) {
            if (r.from >= old) ++r.from;
            if (r.to >= old) ++r.to;
        }
        for (auto& w : swaps[set])
            if (w.symbol >= old) ++w.symbol;
    }
    for (auto& p : pools)
        if (p.symbol >= old) ++p.symbol;
    return man;
}

uint32_t Caff::symbolNameIndex(const std::string& name) {
    for (size_t i = 0; i < symbolNames.size(); ++i)
        if (symbolNames[i] == name) return uint32_t(i + 1);
    // Like the manifest symbol, its name stays last.
    if (!symbolNames.empty() && symbolNames.back() == "manifest") {
        uint32_t manName = uint32_t(symbolNames.size());
        symbolNames.insert(symbolNames.end() - 1, name);
        for (auto& s : symbols)
            if (s.nameIndex == manName) ++s.nameIndex;
        return manName;
    }
    symbolNames.push_back(name);
    return uint32_t(symbolNames.size());
}

// ---- JSON manifest -------------------------------------------------------

Json Caff::toJson() const {
    Json j = Json::object();
    std::string hdrHex;
    for (uint8_t b : header) hdrHex += hex8(b);
    j["header"] = hdrHex;
    Json secs = Json::array();
    for (size_t i = 0; i < sections.size(); ++i) {
        Json s = Json::object();
        s["name"] = sections[i].name;
        s["alignLog2"] = unsigned(sections[i].alignLog2);
        s["firstSymbol"] = sections[i].firstSymbol;
        s["size"] = unsigned(sections[i].data.size());
        secs.push(s);
    }
    j["sections"] = secs;
    Json names = Json::array();
    for (auto& n : symbolNames) names.push(n);
    j["symbolNames"] = names;
    if (!extra.empty()) {
        std::string x;
        for (uint8_t b : extra) x += hex8(b);
        j["extra"] = x;
    }
    Json syms = Json::array();
    for (auto& s : symbols) {
        Json o = Json::object();
        o["name"] = s.nameIndex;
        o["section"] = unsigned(s.section);
        o["offset"] = s.offset;
        o["size"] = s.size;
        o["kind"] = unsigned(s.kind);
        syms.push(o);
    }
    j["symbols"] = syms;
    for (int set = 0; set < 2; ++set) {
        Json arr = Json::array();
        for (auto& e : swaps[set]) {
            Json o = Json::object();
            o["symbol"] = e.symbol;
            o["elementCount"] = e.elementCount;
            o["elementSize"] = e.elementSize;
            o["stride"] = e.stride;
            Json offs = Json::array();
            for (auto v : e.offsets) offs.push(v);
            o["offsets"] = offs;
            arr.push(o);
        }
        j[set == 0 ? "swapsA" : "swapsB"] = arr;
    }
    for (int set = 0; set < 2; ++set) {
        Json arr = Json::array();
        for (auto& e : relocs[set]) {
            Json o = Json::object();
            o["from"] = e.from;
            o["to"] = e.to;
            Json offs = Json::array();
            for (auto v : e.offsets) offs.push(v);
            o["offsets"] = offs;
            arr.push(o);
        }
        j[set == 0 ? "relocsA" : "relocsB"] = arr;
    }
    Json pl = Json::array();
    for (auto& p : pools) {
        Json o = Json::object();
        o["symbol"] = p.symbol;
        Json strsJ = Json::array();
        for (auto& s : p.strings) {
            Json e = Json::array();
            e.push(s.length);
            e.push(s.offset);
            e.push(s.hash);
            e.push(s.flags);
            strsJ.push(e);
        }
        o["strings"] = strsJ;
        pl.push(o);
    }
    j["pools"] = pl;
    return j;
}

static Bytes fromHex(const std::string& s) {
    if (s.size() % 2) fail("odd-length hex string");
    Bytes b;
    for (size_t i = 0; i < s.size(); i += 2) b.push_back(uint8_t(parseHex(s.substr(i, 2))));
    return b;
}

Caff Caff::fromJson(const Json& j) {
    Caff c;
    c.header = fromHex(j["header"].asString());
    for (auto& s : j["sections"].items()) {
        Section sec;
        sec.name = s["name"].asString();
        sec.alignLog2 = uint8_t(s["alignLog2"].asU32());
        sec.firstSymbol = s["firstSymbol"].asU32();
        c.sections.push_back(std::move(sec));
    }
    for (auto& n : j["symbolNames"].items()) c.symbolNames.push_back(n.asString());
    if (j.has("extra")) c.extra = fromHex(j["extra"].asString());
    for (auto& o : j["symbols"].items()) {
        Symbol s;
        s.nameIndex = o["name"].asU32();
        s.section = uint8_t(o["section"].asU32());
        s.offset = o["offset"].asU32();
        s.size = o["size"].asU32();
        s.kind = uint8_t(o["kind"].asU32());
        c.symbols.push_back(s);
    }
    for (int set = 0; set < 2; ++set) {
        const Json& arr = j[set == 0 ? "swapsA" : "swapsB"];
        if (arr.isNull()) continue;
        for (auto& o : arr.items()) {
            SwapEntry e;
            e.symbol = o["symbol"].asU32();
            e.elementCount = o["elementCount"].asU32();
            e.elementSize = o["elementSize"].asU32();
            e.stride = o["stride"].asU32();
            for (auto& v : o["offsets"].items()) e.offsets.push_back(v.asU32());
            c.swaps[set].push_back(e);
        }
    }
    for (int set = 0; set < 2; ++set) {
        const Json& arr = j[set == 0 ? "relocsA" : "relocsB"];
        if (arr.isNull()) continue;
        for (auto& o : arr.items()) {
            Relocation e;
            e.from = o["from"].asU32();
            e.to = o["to"].asU32();
            for (auto& v : o["offsets"].items()) e.offsets.push_back(v.asU32());
            c.relocs[set].push_back(e);
        }
    }
    if (!j["pools"].isNull()) {
        for (auto& o : j["pools"].items()) {
            Pool p;
            p.symbol = o["symbol"].asU32();
            for (auto& e : o["strings"].items())
                p.strings.push_back({e[0].asU32(), e[1].asU32(), e[2].asU32(), e[3].asU32()});
            c.pools.push_back(p);
        }
    }
    return c;
}

} // namespace nut::caff
