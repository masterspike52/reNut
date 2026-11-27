#include "loctext.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace nut::loctext {
namespace {

constexpr char kMagic[] = "LSBL";

int findStringSymbol(const caff::Caff& c) {
    for (size_t i = 0; i < c.symbols.size(); ++i) {
        const auto& s = c.symbols[i];
        if (s.section == 0 || s.section > c.sections.size()) continue;
        const auto& sec = c.sections[s.section - 1];
        if (sec.name != ".data" || s.size < 4) continue;
        auto b = sec.data.begin() + s.offset;
        if (std::search(b, b + s.size, kMagic, kMagic + 4) != b + s.size) return int(i);
    }
    return -1;
}

} // namespace

uint16_t keyHash(std::string_view name) {
    uint32_t h = 0;
    for (char ch : name) h = h * 33 + uint8_t(ch);
    return uint16_t(h);
}

bool isLoctext(const caff::Caff& c) { return findStringSymbol(c) >= 0; }

Table read(const caff::Caff& c) {
    int si = findStringSymbol(c);
    if (si < 0) fail("no LSBL string table found");
    Bytes d = c.symbolData(size_t(si));
    size_t at = size_t(std::search(d.begin(), d.end(), kMagic, kMagic + 4) - d.begin());
    Table t;
    t.prefix.assign(d.begin(), d.begin() + at);
    Reader r(d.data() + at, d.size() - at, false);
    r.skip(4);
    r.u32();
    uint32_t version = r.u32();
    if (version != 4) fail("unsupported LSBL version " + std::to_string(version));
    uint32_t offStrings = r.u32(), offNames = r.u32(), offNotes = r.u32(), offOrder = r.u32();
    if (!offStrings || !offNames) fail("LSBL table without strings/names block is not supported");
    auto blockAt = [&](uint32_t off) {
        Reader b(d.data() + at, d.size() - at, false);
        b.seek(off);
        uint32_t size = b.u32();
        b.seek(off);
        return b.bytes(size);
    };
    if (offNotes) t.notes = blockAt(offNotes);
    if (offOrder) {
        t.hasOrder = true;
        Reader o(d.data() + at, d.size() - at, false);
        o.seek(offOrder);
        o.u32();
        uint32_t n = o.u32();
        for (uint32_t k = 0; k < n; ++k) t.order.push_back(o.u16());
    }

    // Strings.
    r.seek(offStrings);
    r.u32();
    uint32_t count = r.u32();
    std::vector<std::pair<uint16_t, uint32_t>> idx(count + 1);
    for (auto& e : idx) { e.first = r.u16(); e.second = r.u32(); }
    size_t textBase = r.pos();
    std::map<uint16_t, std::string> textByKey;
    for (uint32_t i = 0; i < count; ++i) {
        Reader tr(d.data() + at, d.size() - at, false);
        tr.seek(textBase + size_t(idx[i].second) * 2);
        std::u16string s;
        for (char16_t ch; (ch = char16_t(tr.u16())) != 0;) s += ch;
        textByKey[idx[i].first] = utf16ToUtf8(s);
    }

    // Names, emitted in nameIndex order.
    r.seek(offNames);
    r.u32();
    uint32_t nNames = r.u32();
    std::vector<std::tuple<uint16_t, uint16_t, uint32_t>> names(nNames);
    for (auto& [k, ni, off] : names) { k = r.u16(); ni = r.u16(); off = r.u32(); }
    size_t namesBase = r.pos();
    std::sort(names.begin(), names.end(), [](auto& a, auto& b) { return std::get<1>(a) < std::get<1>(b); });
    std::set<uint16_t> seen;
    for (auto& [k, ni, off] : names) {
        Reader nr(d.data() + at, d.size() - at, false);
        nr.seek(namesBase + off);
        Entry e;
        e.key = k;
        e.name = nr.cstr();
        e.text = textByKey.count(k) ? textByKey[k] : std::string();
        t.entries.push_back(std::move(e));
        seen.insert(k);
    }
    // Strings without a name (not expected in shipped data, but keep them).
    for (auto& [k, text] : textByKey)
        if (!seen.count(k)) t.entries.push_back({k, "", text});
    return t;
}

namespace {
// strcmp ordering (unsigned bytes), which is how the shipped tables sort their names.
bool nameLess(const Entry& a, const Entry& b) {
    return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
                                        [](char x, char y) { return uint8_t(x) < uint8_t(y); });
}
} // namespace

void write(caff::Caff& c, const Table& t) {
    int si = findStringSymbol(c);
    if (si < 0) fail("no LSBL string table found");
    std::map<uint16_t, const Entry*> byKey;
    for (auto& e : t.entries) {
        if (e.key == 0xFFFF) fail("key 0xFFFF is reserved (entry '" + e.name + "')");
        if (!byKey.emplace(e.key, &e).second) fail("duplicate key " + hex8(uint8_t(e.key >> 8)) + hex8(uint8_t(e.key)) + " for '" + e.name + "'");
    }

    // Strings block.
    Writer sb(false);
    sb.u32(0);
    sb.u32(uint32_t(byKey.size()));
    std::u16string chars;
    std::vector<uint32_t> offsets;
    for (auto& [k, e] : byKey) {
        offsets.push_back(uint32_t(chars.size()));
        chars += utf8ToUtf16(e->text);
        chars += u'\0';
    }
    size_t i = 0;
    for (auto& [k, e] : byKey) { sb.u16(k); sb.u32(offsets[i++]); }
    sb.u16(0xFFFF);
    sb.u32(uint32_t(chars.size()));
    for (char16_t ch : chars) sb.u16(uint16_t(ch));
    sb.u16(0); // the shipped tables always end the text with one extra terminator
    sb.patch32(0, uint32_t(sb.pos()));

    // Names block: records sorted by key, name strings in strcmp order.
    std::vector<const Entry*> named;
    for (auto& e : t.entries) if (!e.name.empty()) named.push_back(&e);
    std::stable_sort(named.begin(), named.end(), [](const Entry* a, const Entry* b) { return nameLess(*a, *b); });
    Writer nb(false);
    nb.u32(0);
    nb.u32(uint32_t(named.size()));
    std::string nameData;
    std::map<uint16_t, std::pair<uint16_t, uint32_t>> nameInfo;
    for (size_t n = 0; n < named.size(); ++n) {
        nameInfo[named[n]->key] = {uint16_t(n + 1), uint32_t(nameData.size())};
        nameData += named[n]->name;
        nameData += '\0';
    }
    for (auto& [k, info] : nameInfo) { nb.u16(k); nb.u16(info.first); nb.u32(info.second); }
    nb.bytes(nameData.data(), nameData.size());
    nb.patch32(0, uint32_t(nb.pos()));

    // Order block: the stored authoring order, minus removed keys, plus new keys at the end.
    Writer ob(false);
    if (t.hasOrder) {
        std::vector<uint16_t> order;
        std::set<uint16_t> placed;
        for (uint16_t k : t.order)
            if (nameInfo.count(k) && placed.insert(k).second) order.push_back(k);
        for (auto* e : named)
            if (placed.insert(e->key).second) order.push_back(e->key);
        ob.u32(uint32_t(8 + 2 * order.size()));
        ob.u32(uint32_t(order.size()));
        for (uint16_t k : order) ob.u16(k);
    }

    Writer w(false);
    w.bytes("LSBL", 4);
    w.u32(0x1C);
    w.u32(4);
    uint32_t offS = 0x1C, offN = offS + uint32_t(sb.pos());
    uint32_t offX = t.notes.empty() ? 0 : offN + uint32_t(nb.pos());
    uint32_t offO = t.hasOrder ? offN + uint32_t(nb.pos() + t.notes.size()) : 0;
    w.u32(offS);
    w.u32(offN);
    w.u32(offX);
    w.u32(offO);
    w.bytes(sb.buf());
    w.bytes(nb.buf());
    w.bytes(t.notes);
    w.bytes(ob.buf());

    Bytes data = t.prefix;
    data.insert(data.end(), w.buf().begin(), w.buf().end());
    while (data.size() % 16) data.push_back(0);
    c.replaceSymbolData(size_t(si), data);
}

Json toJson(const Table& t) {
    std::vector<const Entry*> listed;
    std::set<const Entry*> done;
    if (t.hasOrder) {
        std::map<uint16_t, const Entry*> byKey;
        for (auto& e : t.entries) byKey[e.key] = &e;
        for (uint16_t k : t.order) {
            auto it = byKey.find(k);
            if (it != byKey.end() && done.insert(it->second).second) listed.push_back(it->second);
        }
    }
    for (auto& e : t.entries) if (done.insert(&e).second) listed.push_back(&e);
    Json arr = Json::array();
    for (const Entry* ep : listed) {
        const Entry& e = *ep;
        Json o = Json::object();
        o["name"] = e.name;
        o["key"] = "0x" + hex8(uint8_t(e.key >> 8)) + hex8(uint8_t(e.key));
        o["text"] = e.text;
        arr.push(o);
    }
    Json j = Json::object();
    j["strings"] = arr;
    return j;
}

void applyJson(Table& t, const Json& j, bool prune) {
    std::map<std::string, size_t> byName;
    for (size_t i = 0; i < t.entries.size(); ++i) byName[t.entries[i].name] = i;
    std::set<size_t> touched;
    for (auto& o : j["strings"].items()) {
        std::string name = o["name"].asString();
        std::string text = o["text"].asString();
        auto it = byName.find(name);
        if (it != byName.end() && !name.empty()) {
            t.entries[it->second].text = text;
            touched.insert(it->second);
            continue;
        }
        Entry e;
        e.name = name;
        e.key = o.has("key") && name.empty() ? uint16_t(o["key"].asU32()) : keyHash(name);
        e.text = text;
        touched.insert(t.entries.size());
        byName[name] = t.entries.size();
        t.entries.push_back(std::move(e));
    }
    if (prune) {
        std::vector<Entry> kept;
        for (size_t i = 0; i < t.entries.size(); ++i) if (touched.count(i)) kept.push_back(t.entries[i]);
        t.entries = std::move(kept);
    }
}

} // namespace nut::loctext
