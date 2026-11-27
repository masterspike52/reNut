#include "font.hpp"

#include "deflate.hpp"
#include "image.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>

namespace nut::font {
namespace {

constexpr uint32_t kPageCells = 32; // cells per page row/column
constexpr int kUnitsPerPixel = 64;  // TrueType units per glyph pixel

Bytes unpackCell(const Bytes& raw, uint32_t cell) {
    Bytes px(size_t(cell) * cell);
    for (size_t i = 0; i < px.size(); ++i) px[i] = (raw[i / 2] >> ((i & 1) * 4)) & 15;
    return px;
}

Bytes packCell(const Bytes& px) {
    Bytes raw((px.size() + 1) / 2, 0);
    for (size_t i = 0; i < px.size(); ++i) raw[i / 2] |= uint8_t((px[i] & 15) << ((i & 1) * 4));
    return raw;
}

std::string pageName(size_t page) {
    char t[32];
    std::snprintf(t, sizeof t, "page_%03zu.png", page);
    return t;
}

// The game moves the pen by metrics[1] before drawing the cell and advances by metrics[1..3].
int advanceOf(const Glyph& g) { return std::max(0, g.metrics[1] + g.metrics[2] + g.metrics[3]); }

using Contour = std::vector<std::pair<int, int>>;

// Outlines of the pixels with coverage >= 8, clockwise with y up (rows counted from the cell bottom).
// Holes come out counter-clockwise, as TrueType expects.
std::vector<Contour> traceCell(const Bytes& px, uint32_t cell) {
    const int n = int(cell), stride = n + 1;
    static const int dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
    auto on = [&](int x, int row) { return x >= 0 && row >= 0 && x < n && row < n && px[size_t(row) * n + x] >= 8; };
    auto vid = [&](int x, int y) { return size_t(y) * stride + x; };
    std::vector<uint8_t> out(size_t(stride) * stride, 0); // bit d: unused edge leaving the vertex in direction d
    for (int row = 0; row < n; ++row)
        for (int x = 0; x < n; ++x) {
            if (!on(x, row)) continue;
            int y = n - row - 1;
            if (!on(x - 1, row)) out[vid(x, y)] |= uint8_t(1 << 1);
            if (!on(x, row - 1)) out[vid(x, y + 1)] |= uint8_t(1 << 0);
            if (!on(x + 1, row)) out[vid(x + 1, y + 1)] |= uint8_t(1 << 3);
            if (!on(x, row + 1)) out[vid(x + 1, y)] |= uint8_t(1 << 2);
        }
    std::vector<Contour> contours;
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x)
            while (out[vid(x, y)]) {
                int d = 0;
                while (!(out[vid(x, y)] >> d & 1)) ++d;
                Contour path;
                int cx = x, cy = y;
                for (;;) {
                    out[vid(cx, cy)] &= uint8_t(~(1 << d));
                    path.push_back({cx, cy});
                    cx += dx[d];
                    cy += dy[d];
                    if (cx == x && cy == y) break;
                    // Right turns first, so pixels touching at a corner become separate contours.
                    int nd = -1;
                    for (int t : {(d + 3) & 3, d, (d + 1) & 3})
                        if (out[vid(cx, cy)] >> t & 1) { nd = t; break; }
                    if (nd < 0) break;
                    d = nd;
                }
                Contour c;
                for (size_t i = 0; i < path.size(); ++i) {
                    auto& p = path[(i + path.size() - 1) % path.size()];
                    auto& q = path[i];
                    auto& r = path[(i + 1) % path.size()];
                    if ((q.first - p.first) * (r.second - q.second) != (q.second - p.second) * (r.first - q.first)) c.push_back(q);
                }
                if (c.size() >= 3) contours.push_back(std::move(c));
            }
    return contours;
}

struct TtGlyph {
    Bytes data;
    int xMin = 0, yMin = 0, xMax = 0, yMax = 0;
    size_t points = 0, contours = 0;
};

void putCoord(int delta, uint8_t shortBit, uint8_t sameBit, uint8_t& flag, Bytes& out) {
    if (delta == 0) {
        flag |= sameBit;
    } else if (std::abs(delta) < 256) {
        flag |= shortBit;
        if (delta > 0) flag |= sameBit;
        out.push_back(uint8_t(std::abs(delta)));
    } else {
        out.push_back(uint8_t(uint16_t(delta) >> 8));
        out.push_back(uint8_t(delta));
    }
}

TtGlyph buildGlyph(const Glyph& g, uint32_t cell, uint32_t base) {
    TtGlyph t;
    auto contours = traceCell(g.pixels, cell);
    if (contours.empty()) return t;
    const int ox = g.metrics[1], oy = int(base) - int(cell);
    t.xMin = t.yMin = INT_MAX;
    t.xMax = t.yMax = INT_MIN;
    for (auto& c : contours)
        for (auto& p : c) {
            p.first = (p.first + ox) * kUnitsPerPixel;
            p.second = (p.second + oy) * kUnitsPerPixel;
            t.xMin = std::min(t.xMin, p.first);
            t.xMax = std::max(t.xMax, p.first);
            t.yMin = std::min(t.yMin, p.second);
            t.yMax = std::max(t.yMax, p.second);
        }
    Writer w;
    w.u16(uint16_t(contours.size()));
    for (int v : {t.xMin, t.yMin, t.xMax, t.yMax}) w.u16(uint16_t(int16_t(v)));
    for (auto& c : contours) {
        t.points += c.size();
        w.u16(uint16_t(t.points - 1));
    }
    w.u16(0); // no instructions
    Bytes flags, xs, ys;
    int px = 0, py = 0;
    for (auto& c : contours)
        for (auto& p : c) {
            uint8_t flag = 1; // on-curve
            putCoord(p.first - px, 0x02, 0x10, flag, xs);
            putCoord(p.second - py, 0x04, 0x20, flag, ys);
            flags.push_back(flag);
            px = p.first;
            py = p.second;
        }
    w.bytes(flags);
    w.bytes(xs);
    w.bytes(ys);
    w.align(4);
    t.data = std::move(w.buf());
    t.contours = contours.size();
    return t;
}

uint32_t tableChecksum(const Bytes& b) {
    uint32_t sum = 0;
    for (size_t i = 0; i < b.size(); i += 4) {
        uint8_t t[4] = {};
        std::memcpy(t, &b[i], std::min<size_t>(4, b.size() - i));
        sum += be32(t);
    }
    return sum;
}

// Directory search fields for `count` entries of `unit` bytes: searchRange, entrySelector, rangeShift.
void putSearchFields(Writer& w, size_t count, size_t unit) {
    size_t p = 1, log = 0;
    while (p * 2 <= count) p *= 2, ++log;
    w.u16(uint16_t(p * unit));
    w.u16(uint16_t(log));
    w.u16(uint16_t(count * unit - p * unit));
}

fs::path leafName(const fs::path& dir) {
    fs::path p = fs::absolute(dir).lexically_normal();
    if (p.filename().empty()) p = p.parent_path();
    return p.filename().empty() ? fs::path("font") : p.filename();
}

} // namespace

uint32_t baselineRow(const Font& f) {
    std::map<uint32_t, int> votes;
    for (auto& g : f.glyphs) {
        if (g.code == 0 || g.code >= 128 || std::string_view("HIELTZxzmnuv").find(char(g.code)) == std::string_view::npos) continue;
        int bottom = -1;
        for (size_t i = 0; i < g.pixels.size(); ++i)
            if (g.pixels[i] >= 8) bottom = int(i / f.cellSize);
        if (bottom >= 0) ++votes[uint32_t(bottom + 1)];
    }
    uint32_t best = 0;
    int most = 0;
    for (auto& [row, n] : votes)
        if (n > most) best = row, most = n;
    return most ? best : (uint32_t(f.header[2]) * 3 + 2) / 4;
}

Bytes toTtf(const Font& f, const std::string& family) {
    const int U = kUnitsPerPixel;
    const int lh = f.header[2] ? f.header[2] : int(f.cellSize);
    const int base = int(baselineRow(f));
    const int ascent = base * U, descent = (lh - base) * U, em = lh * U;

    // Glyph 0 (.notdef) is the cache's U+0001, which the game draws for missing characters.
    std::vector<const Glyph*> order{nullptr};
    for (auto& g : f.glyphs) {
        if (g.code == 1) order[0] = &g;
        else if (g.code != 0xFFFF) order.push_back(&g);
    }
    std::sort(order.begin() + 1, order.end(), [](const Glyph* a, const Glyph* b) { return a->code < b->code; });
    const size_t numGlyphs = order.size();
    if (numGlyphs > 0xFFFF) fail("font: too many glyphs for TrueType");

    Writer glyf, loca, hmtx;
    int xMin = INT_MAX, yMin = INT_MAX, xMax = INT_MIN, yMax = INT_MIN;
    int advMax = 0, minLsb = INT_MAX, minRsb = INT_MAX, maxExtent = INT_MIN, xHeight = 0, capHeight = 0;
    size_t maxPoints = 0, maxContours = 0, advCount = 0;
    int64_t advSum = 0;
    for (const Glyph* g : order) {
        TtGlyph t = g ? buildGlyph(*g, f.cellSize, uint32_t(base)) : TtGlyph{};
        int adv = (g ? advanceOf(*g) : lh / 2) * U;
        loca.u32(uint32_t(glyf.pos()));
        glyf.bytes(t.data);
        hmtx.u16(uint16_t(adv));
        hmtx.u16(uint16_t(int16_t(t.contours ? t.xMin : 0)));
        advMax = std::max(advMax, adv);
        if (adv) advSum += adv, ++advCount;
        if (!t.contours) continue;
        xMin = std::min(xMin, t.xMin);
        yMin = std::min(yMin, t.yMin);
        xMax = std::max(xMax, t.xMax);
        yMax = std::max(yMax, t.yMax);
        minLsb = std::min(minLsb, t.xMin);
        minRsb = std::min(minRsb, adv - t.xMax);
        maxExtent = std::max(maxExtent, t.xMax);
        maxPoints = std::max(maxPoints, t.points);
        maxContours = std::max(maxContours, t.contours);
        if (g && g->code == 'x') xHeight = t.yMax;
        if (g && g->code == 'H') capHeight = t.yMax;
    }
    loca.u32(uint32_t(glyf.pos()));
    if (xMin == INT_MAX) xMin = yMin = xMax = yMax = minLsb = minRsb = maxExtent = 0;

    // cmap: runs of consecutive codes map to consecutive glyph ids.
    struct Run { uint32_t start, end, gid; };
    std::vector<Run> runs;
    std::vector<bool> present(0x10000, false);
    for (size_t gid = 1; gid < numGlyphs; ++gid) {
        uint32_t code = order[gid]->code;
        present[code] = true;
        if (!runs.empty() && runs.back().end + 1 == code) runs.back().end = code;
        else runs.push_back({code, code, uint32_t(gid)});
    }
    Writer cmap;
    const size_t segs = runs.size() + 1;
    const bool fmt4 = 16 + segs * 8 <= 0xFFFF;
    cmap.u16(0);
    cmap.u16(2);
    for (uint16_t enc : {uint16_t(fmt4 ? 3 : 4), uint16_t(fmt4 ? 1 : 10)}) {
        cmap.u16(uint16_t(enc == 1 || enc == 10 ? 3 : 0));
        cmap.u16(enc);
        cmap.u32(20);
    }
    if (fmt4) {
        cmap.u16(4);
        cmap.u16(uint16_t(16 + segs * 8));
        cmap.u16(0);
        cmap.u16(uint16_t(segs * 2));
        putSearchFields(cmap, segs, 2);
        for (auto& r : runs) cmap.u16(uint16_t(r.end));
        cmap.u16(0xFFFF);
        cmap.u16(0);
        for (auto& r : runs) cmap.u16(uint16_t(r.start));
        cmap.u16(0xFFFF);
        for (auto& r : runs) cmap.u16(uint16_t(r.gid - r.start));
        cmap.u16(1);
        cmap.zeros(segs * 2);
    } else {
        cmap.u16(12);
        cmap.u16(0);
        cmap.u32(uint32_t(16 + runs.size() * 12));
        cmap.u32(0);
        cmap.u32(uint32_t(runs.size()));
        for (auto& r : runs) {
            cmap.u32(r.start);
            cmap.u32(r.end);
            cmap.u32(r.gid);
        }
    }

    // OS/2 script coverage: {first, last, unicode range bit, code page bit or -1}.
    static const struct { uint32_t lo, hi; int uni, cp; } kScripts[] = {
        {0x0020, 0x007E, 0, -1}, {0x00A0, 0x00FF, 1, 0}, {0x0100, 0x017F, 2, 1}, {0x0370, 0x03FF, 7, 3},
        {0x0400, 0x04FF, 9, 2}, {0x1100, 0x11FF, 28, -1}, {0x2000, 0x206F, 31, -1}, {0x3000, 0x303F, 48, -1},
        {0x3040, 0x309F, 49, 17}, {0x30A0, 0x30FF, 50, 17}, {0x4E00, 0x9FFF, 59, 18}, {0xAC00, 0xD7AF, 56, 19},
        {0xFF00, 0xFFEF, 68, -1},
    };
    uint32_t uniRange[4] = {}, cpRange = 0;
    for (auto& s : kScripts) {
        bool any = false;
        for (uint32_t c = s.lo; c <= s.hi && !any; ++c) any = present[c];
        if (!any) continue;
        uniRange[s.uni / 32] |= 1u << (s.uni % 32);
        if (s.cp >= 0) cpRange |= 1u << s.cp;
        if (s.cp == 18) cpRange |= 1u << 20; // CJK ideographs serve both Chinese code pages
    }
    const int winAscent = std::max(ascent, yMax), winDescent = std::max(descent, -yMin);
    Writer os2;
    os2.u16(4);
    os2.u16(uint16_t(advCount ? advSum / int64_t(advCount) : 0));
    os2.u16(400);
    os2.u16(5);
    os2.u16(0);
    for (int v : {em * 65 / 100, em * 60 / 100, 0, em * 7 / 100, em * 65 / 100, em * 60 / 100, 0, em * 35 / 100}) os2.u16(uint16_t(int16_t(v)));
    os2.u16(uint16_t(U));
    os2.u16(uint16_t(int16_t((xHeight ? xHeight : ascent / 2) / 2)));
    os2.u16(0);
    os2.zeros(10); // PANOSE
    for (uint32_t r : uniRange) os2.u32(r);
    os2.bytes("NUTT", 4);
    os2.u16(0xC0); // REGULAR | USE_TYPO_METRICS
    os2.u16(uint16_t(runs.empty() ? 0 : runs.front().start));
    os2.u16(uint16_t(runs.empty() ? 0 : runs.back().end));
    os2.u16(uint16_t(int16_t(ascent)));
    os2.u16(uint16_t(int16_t(-descent)));
    os2.u16(0);
    os2.u16(uint16_t(winAscent));
    os2.u16(uint16_t(winDescent));
    os2.u32(cpRange);
    os2.u32(0);
    os2.u16(uint16_t(int16_t(xHeight)));
    os2.u16(uint16_t(int16_t(capHeight)));
    os2.u16(0);
    os2.u16(' ');
    os2.u16(0);

    Writer head;
    const uint64_t now = uint64_t(std::time(nullptr)) + 2082844800ull; // seconds since 1904
    head.u32(0x00010000);
    head.u32(0x00010000);
    head.u32(0); // checkSumAdjustment, patched below
    head.u32(0x5F0F3CF5);
    head.u16(0x0009); // baseline at y=0, integer ppem
    head.u16(uint16_t(em));
    for (int k = 0; k < 2; ++k) {
        head.u32(uint32_t(now >> 32));
        head.u32(uint32_t(now));
    }
    for (int v : {xMin, yMin, xMax, yMax}) head.u16(uint16_t(int16_t(v)));
    head.u16(0);
    head.u16(uint16_t(lh));
    head.u16(2);
    head.u16(1); // long loca
    head.u16(0);

    Writer hhea;
    hhea.u32(0x00010000);
    hhea.u16(uint16_t(int16_t(ascent)));
    hhea.u16(uint16_t(int16_t(-descent)));
    hhea.u16(0);
    hhea.u16(uint16_t(advMax));
    for (int v : {minLsb, minRsb, maxExtent}) hhea.u16(uint16_t(int16_t(v)));
    hhea.u16(1);
    hhea.zeros(2 + 2 + 8 + 2);
    hhea.u16(uint16_t(numGlyphs));

    Writer maxp;
    maxp.u32(0x00010000);
    maxp.u16(uint16_t(numGlyphs));
    maxp.u16(uint16_t(maxPoints));
    maxp.u16(uint16_t(maxContours));
    maxp.zeros(4);
    maxp.u16(2);
    maxp.zeros(16);

    std::string ps;
    for (char c : family)
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ps += c;
    if (ps.empty()) ps = "NutFont";
    ps = ps.substr(0, 55) + "-Regular";
    const std::pair<uint16_t, std::string> names[] = {
        {1, family}, {2, "Regular"}, {3, "NutTool:" + family}, {4, family}, {5, "Version 1.000"}, {6, ps}};
    Writer name, strings;
    name.u16(0);
    name.u16(uint16_t(std::size(names)));
    name.u16(uint16_t(6 + 12 * std::size(names)));
    for (auto& [id, text] : names) {
        std::u16string s = utf8ToUtf16(text);
        name.u16(3);
        name.u16(1);
        name.u16(0x409);
        name.u16(id);
        name.u16(uint16_t(s.size() * 2));
        name.u16(uint16_t(strings.pos()));
        for (char16_t c : s) strings.u16(uint16_t(c));
    }
    name.bytes(strings.buf());

    Writer post;
    post.u32(0x00030000);
    post.u32(0);
    post.u16(uint16_t(int16_t(-2 * U)));
    post.u16(uint16_t(U));
    post.zeros(4 + 16);

    const std::pair<const char*, const Bytes*> tables[] = {
        {"OS/2", &os2.buf()}, {"cmap", &cmap.buf()}, {"glyf", &glyf.buf()}, {"head", &head.buf()}, {"hhea", &hhea.buf()},
        {"hmtx", &hmtx.buf()}, {"loca", &loca.buf()}, {"maxp", &maxp.buf()}, {"name", &name.buf()}, {"post", &post.buf()}};
    Writer out;
    out.u32(0x00010000);
    out.u16(uint16_t(std::size(tables)));
    putSearchFields(out, std::size(tables), 16);
    size_t off = 12 + 16 * std::size(tables), headOff = 0;
    for (auto& [tag, data] : tables) {
        out.bytes(tag, 4);
        out.u32(tableChecksum(*data));
        out.u32(uint32_t(off));
        out.u32(uint32_t(data->size()));
        if (std::string_view(tag) == "head") headOff = off;
        off += (data->size() + 3) & ~size_t(3);
    }
    for (auto& [tag, data] : tables) {
        out.bytes(*data);
        out.align(4);
    }
    out.patch32(headOff + 8, 0xB1B0AFBA - tableChecksum(out.buf()));
    return std::move(out.buf());
}

std::string toBmfont(const Font& f, const std::string& face) {
    const uint32_t page = kPageCells * f.cellSize, perPage = kPageCells * kPageCells;
    const size_t pages = (f.glyphs.size() + perPage - 1) / perPage;
    std::string quoted;
    for (char c : face) quoted += c == '"' ? '\'' : c;
    char t[256];
    std::string s = "info face=\"" + quoted + "\" size=" + std::to_string(f.header[2]) +
                    " bold=0 italic=0 charset=\"\" unicode=1 stretchH=100 smooth=1 aa=1 padding=0,0,0,0 spacing=0,0\n";
    std::snprintf(t, sizeof t, "common lineHeight=%u base=%u scaleW=%u scaleH=%u pages=%zu packed=0 alphaChnl=0 redChnl=4 greenChnl=4 blueChnl=4\n",
                  unsigned(f.header[2]), baselineRow(f), page, page, pages);
    s += t;
    for (size_t p = 0; p < pages; ++p) {
        std::snprintf(t, sizeof t, "page id=%zu file=\"%s\"\n", p, pageName(p).c_str());
        s += t;
    }
    std::snprintf(t, sizeof t, "chars count=%zu\n", f.glyphs.size());
    s += t;
    for (size_t i = 0; i < f.glyphs.size(); ++i) {
        const Glyph& g = f.glyphs[i];
        // Rectangle trimmed to the inked pixels of the cell.
        uint32_t x0 = f.cellSize, y0 = f.cellSize, x1 = 0, y1 = 0;
        for (uint32_t y = 0; y < f.cellSize; ++y)
            for (uint32_t x = 0; x < f.cellSize; ++x)
                if (g.pixels[size_t(y) * f.cellSize + x]) {
                    x0 = std::min(x0, x), y0 = std::min(y0, y);
                    x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
                }
        if (x1 == 0) x0 = y0 = 0;
        size_t c = i % perPage;
        uint32_t cx = uint32_t(c % kPageCells) * f.cellSize, cy = uint32_t(c / kPageCells) * f.cellSize;
        std::snprintf(t, sizeof t, "char id=%u x=%u y=%u width=%u height=%u xoffset=%d yoffset=%u xadvance=%d page=%zu chnl=15\n",
                      unsigned(g.code), cx + x0, cy + y0, x1 - x0, y1 - y0, int(g.metrics[1]) + int(x0), y0, advanceOf(g), i / perPage);
        s += t;
    }
    return s;
}

bool looksLikeFont(const Bytes& d) {
    if (d.size() < 16 || d[0] != 1 || d[1] != 1) return false;
    uint32_t n = be16(&d[4]);
    if (n == 0 || 6 + size_t(n) * 10 > d.size()) return false;
    uint32_t first = be32(&d[8]);
    return first == 6 + n * 10 && d[first] == 0x78;
}

Font Font::parse(const Bytes& d) {
    if (!looksLikeFont(d)) fail("not an XUI font cache (.sbp)");
    Font f;
    std::memcpy(f.header, d.data(), 4);
    if (f.header[3] != 4) fail("only 4-bit font caches are supported");
    uint32_t n = be16(&d[4]);
    struct Rec { uint16_t code; uint32_t off; int8_t m[4]; };
    std::vector<Rec> recs(n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* p = &d[6 + i * 10];
        recs[i].code = be16(p);
        recs[i].off = be32(p + 2);
        for (int k = 0; k < 4; ++k) recs[i].m[k] = int8_t(p[6 + k]);
    }
    for (uint32_t i = 0; i < n; ++i) {
        size_t end = i + 1 < n ? recs[i + 1].off : d.size();
        if (recs[i].off > end || end > d.size()) fail("font: glyph data out of order");
        Glyph g;
        g.code = recs[i].code;
        std::memcpy(g.metrics, recs[i].m, 4);
        g.packed.assign(d.begin() + recs[i].off, d.begin() + end);
        Bytes raw = deflate::zlibDecompress(g.packed.data(), g.packed.size());
        if (f.cellSize == 0) f.cellSize = uint32_t(std::lround(std::sqrt(double(raw.size()) * 2)));
        if (raw.size() * 2 != size_t(f.cellSize) * f.cellSize) fail("font: inconsistent glyph cell size");
        g.pixels = unpackCell(raw, f.cellSize);
        f.glyphs.push_back(std::move(g));
    }
    return f;
}

Bytes Font::serialize() const {
    Writer w;
    w.bytes(header, 4);
    w.u16(uint16_t(glyphs.size()));
    uint32_t off = uint32_t(6 + glyphs.size() * 10);
    std::vector<Bytes> streams;
    for (auto& g : glyphs) {
        if (!g.packed.empty()) {
            streams.push_back(g.packed);
            continue;
        }
        Bytes raw = packCell(g.pixels);
        streams.push_back(deflate::zlibCompress(raw.data(), raw.size()));
    }
    for (size_t i = 0; i < glyphs.size(); ++i) {
        w.u16(glyphs[i].code);
        w.u32(off);
        for (int k = 0; k < 4; ++k) w.u8(uint8_t(glyphs[i].metrics[k]));
        off += uint32_t(streams[i].size());
    }
    for (auto& s : streams) w.bytes(s);
    return w.buf();
}

void exportTo(const Font& f, const fs::path& dir) {
    fs::create_directories(longPath(dir));
    Json j = Json::object();
    j["lineHeight"] = unsigned(f.header[2]);
    j["header"] = Json::array();
    for (int k = 0; k < 4; ++k) j["header"].push(unsigned(f.header[k]));
    j["cellSize"] = f.cellSize;
    j["cellsPerRow"] = kPageCells;
    j["note"] = "Glyph pixels are white with alpha = coverage (16 levels). metrics = {unused, A, B, C}: the cell is drawn at pen + A and the pen advances A + B + C. baseline = row from the cell top.";
    Json glyphs = Json::array();
    size_t perPage = kPageCells * kPageCells;
    size_t pages = (f.glyphs.size() + perPage - 1) / perPage;
    for (size_t p = 0; p < pages; ++p) {
        image::Rgba img;
        img.width = img.height = kPageCells * f.cellSize;
        img.pixels.assign(size_t(img.width) * img.height * 4, 0);
        for (size_t c = 0; c < perPage && p * perPage + c < f.glyphs.size(); ++c) {
            const Glyph& g = f.glyphs[p * perPage + c];
            uint32_t ox = uint32_t(c % kPageCells) * f.cellSize, oy = uint32_t(c / kPageCells) * f.cellSize;
            for (uint32_t y = 0; y < f.cellSize; ++y)
                for (uint32_t x = 0; x < f.cellSize; ++x) {
                    uint8_t* o = &img.pixels[((size_t(oy) + y) * img.width + ox + x) * 4];
                    o[0] = o[1] = o[2] = 255;
                    o[3] = uint8_t(g.pixels[size_t(y) * f.cellSize + x] * 17);
                }
            Json e = Json::object();
            char cp[16];
            std::snprintf(cp, sizeof cp, "U+%04X", g.code);
            e["code"] = cp;
            e["page"] = unsigned(p);
            e["cell"] = unsigned(c);
            Json m = Json::array();
            for (int k = 0; k < 4; ++k) m.push(int(g.metrics[k]));
            e["metrics"] = m;
            glyphs.push(e);
        }
        writeFile(dir / pageName(p), image::encodePng(img));
    }
    j["glyphs"] = glyphs;
    j["baseline"] = baselineRow(f);
    writeText(dir / "font.json", j.dump());
    const fs::path leaf = leafName(dir);
    const auto u8 = leaf.u8string();
    const std::string family(u8.begin(), u8.end());
    fs::path ttf = dir / leaf;
    ttf += ".ttf";
    writeText(dir / "font.fnt", toBmfont(f, family));
    writeFile(ttf, toTtf(f, family));
}

Font importFrom(const fs::path& dir, const Font* base) {
    Json j = Json::parse(readText(dir / "font.json"));
    std::map<uint16_t, const Glyph*> original;
    if (base) for (auto& g : base->glyphs) original[g.code] = &g;
    Font f;
    for (int k = 0; k < 4; ++k) f.header[k] = uint8_t(j["header"][size_t(k)].asU32());
    f.header[2] = uint8_t(j["lineHeight"].asU32());
    f.cellSize = j["cellSize"].asU32();
    uint32_t perRow = j["cellsPerRow"].asU32();
    std::vector<image::Rgba> pages;
    for (auto& e : j["glyphs"].items()) {
        Glyph g;
        std::string code = e["code"].asString();
        g.code = uint16_t(parseHex(startsWith(code, "U+") ? code.substr(2) : code));
        for (int k = 0; k < 4; ++k) g.metrics[k] = int8_t(e["metrics"][size_t(k)].asNumber());
        size_t page = e["page"].asU32(), cell = e["cell"].asU32();
        while (pages.size() <= page) {
            fs::path pp = dir / pageName(pages.size());
            pages.push_back(fs::exists(pp) ? image::decodePng(readFile(pp)) : image::Rgba{});
        }
        const image::Rgba& img = pages[page];
        if (img.pixels.empty()) fail("missing " + pageName(page));
        g.pixels.resize(size_t(f.cellSize) * f.cellSize);
        uint32_t ox = uint32_t(cell % perRow) * f.cellSize, oy = uint32_t(cell / perRow) * f.cellSize;
        if (ox + f.cellSize > img.width || oy + f.cellSize > img.height) fail("glyph cell outside " + pageName(page));
        for (uint32_t y = 0; y < f.cellSize; ++y)
            for (uint32_t x = 0; x < f.cellSize; ++x) {
                const uint8_t* p = &img.pixels[((size_t(oy) + y) * img.width + ox + x) * 4];
                // Coverage comes from alpha; fully opaque pages (no alpha) fall back to luminance.
                uint8_t a = p[3] == 255 ? uint8_t((p[0] + p[1] + p[2]) / 3) : p[3];
                g.pixels[size_t(y) * f.cellSize + x] = uint8_t((a + 8) / 17);
            }
        auto it = original.find(g.code);
        if (it != original.end() && it->second->pixels == g.pixels) g.packed = it->second->packed;
        f.glyphs.push_back(std::move(g));
    }
    std::sort(f.glyphs.begin(), f.glyphs.end(), [](const Glyph& a, const Glyph& b) { return a.code < b.code; });
    for (size_t i = 1; i < f.glyphs.size(); ++i)
        if (f.glyphs[i].code == f.glyphs[i - 1].code) fail("duplicate glyph code in font.json");
    return f;
}

} // namespace nut::font
