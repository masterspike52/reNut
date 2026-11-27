#include "texture.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace nut::texture {
namespace {

constexpr uint32_t kMipInBase = 0xFFFFFFFFu;

uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
uint32_t log2Ceil(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) ++r; return r; }
uint32_t nextPow2(uint32_t v) { return 1u << log2Ceil(std::max(v, 1u)); }

// XGAddress2DTiledOffset: block index of (x, y) in a tiled surface `width` blocks wide.
uint32_t tiledOffset(uint32_t x, uint32_t y, uint32_t width, uint32_t bpb) {
    uint32_t alignedWidth = (width + 31) & ~31u;
    uint32_t logBpp = (bpb >> 2) + ((bpb >> 1) >> (bpb >> 2));
    uint32_t macro = ((x >> 5) + (y >> 5) * (alignedWidth >> 5)) << (logBpp + 7);
    uint32_t micro = ((x & 7) + ((y & 6) << 2)) << logBpp;
    uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + logBpp)) + ((y & 1) << 4);
    return (((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> logBpp;
}

void swapEndian(Bytes& d, uint32_t endian) {
    switch (endian) {
        case 1: for (size_t i = 0; i + 1 < d.size(); i += 2) std::swap(d[i], d[i + 1]); break;
        case 2: for (size_t i = 0; i + 3 < d.size(); i += 4) { std::swap(d[i], d[i + 3]); std::swap(d[i + 1], d[i + 2]); } break;
        case 3: for (size_t i = 0; i + 3 < d.size(); i += 4) { std::swap(d[i], d[i + 2]); std::swap(d[i + 1], d[i + 3]); } break;
        default: break;
    }
}

// Where a level lives inside the GPU blob.
struct Placement {
    uint32_t level = 0;
    uint32_t width = 0, height = 0;   // texels
    uint32_t wBlocks = 0, hBlocks = 0;
    uint32_t offset = 0;              // storage start, slice 0
    uint32_t pitch = 0;               // storage pitch in blocks
    uint32_t sliceStride = 0;         // bytes between slices
    uint32_t tx = 0, ty = 0;          // block offset inside packed storage
};

struct Storage {
    uint32_t offset, pitch, rows, stride;
};

Storage makeStorage(const Format& f, uint32_t wb, uint32_t hb, uint32_t at) {
    Storage s{};
    s.offset = at;
    if (f.tiled) {
        s.pitch = alignUp(wb, 32);
        s.rows = alignUp(hb, 32);
    } else {
        s.pitch = alignUp(wb * f.bytesPerBlock, 256) / f.bytesPerBlock;
        s.rows = hb;
    }
    s.stride = alignUp(s.pitch * s.rows * f.bytesPerBlock, 4096);
    return s;
}

// Offset (in blocks) of a packed level inside the shared mip-tail storage.
void packedOffset(const Format& f, uint32_t width, uint32_t height, uint32_t level, uint32_t& bx, uint32_t& by) {
    uint32_t lw = log2Ceil(width), lh = log2Ceil(height);
    uint32_t lsize = std::min(lw, lh);
    uint32_t base = lsize > 4 ? lsize - 4 : 0;
    uint32_t pm = level - base;
    uint32_t x = 0, y = 0;
    if (pm < 3) {
        if (lw > lh) y = 16u >> pm;
        else x = 16u >> pm;
    } else if (lw > lh) {
        x = (1u << (lw - base)) >> (pm - 2);
    } else {
        y = (1u << (lh - base)) >> (pm - 2);
    }
    bx = x / f.blockW;
    by = y / f.blockH;
}

std::vector<Placement> layout(const Header& h, const Contents& c, size_t* totalSize) {
    Format f = describeFormat(h.d3dFormat);
    if (!f.supported()) fail(std::string("unsupported texture format ") + hex32(h.d3dFormat));
    uint32_t lsize = std::min(log2Ceil(h.width), log2Ceil(h.height));
    uint32_t packedLevel = lsize > 4 ? lsize - 4 : 0;
    bool packs = f.tiled;
    uint32_t pw = nextPow2(h.width), ph = nextPow2(h.height);
    auto dims = [&](uint32_t level, uint32_t& w, uint32_t& hh) {
        w = level == 0 ? h.width : std::max(pw >> level, 1u);
        hh = level == 0 ? h.height : std::max(ph >> level, 1u);
    };
    auto blocks = [&](uint32_t w, uint32_t hh, uint32_t& wb, uint32_t& hb) {
        wb = (w + f.blockW - 1) / f.blockW;
        hb = (hh + f.blockH - 1) / f.blockH;
    };

    std::vector<Placement> out;
    size_t end = 0;
    uint32_t w0, h0, wb0, hb0;
    dims(0, w0, h0);
    blocks(w0, h0, wb0, hb0);
    Storage base = makeStorage(f, wb0, hb0, 0);
    uint32_t cursor = 0;
    if (c.firstLevel == 0) {
        cursor = base.stride * c.slices;
        end = cursor;
    }
    uint32_t mipCursor = c.firstLevel == 0 ? (h.mipOffset == kMipInBase || h.mipOffset == 0 ? cursor : h.mipOffset) : 0;
    Storage tail{};
    bool haveTail = false;
    uint32_t lastLevel = c.firstLevel + c.levelCount;
    for (uint32_t level = 0; level < lastLevel; ++level) {
        Placement p;
        p.level = level;
        dims(level, p.width, p.height);
        blocks(p.width, p.height, p.wBlocks, p.hBlocks);
        Storage st;
        if (level == 0) {
            st = base;
        } else if (packs && packedLevel == 0) {
            st = base; // whole chain lives in the base storage
        } else if (packs && level >= packedLevel) {
            if (!haveTail) {
                tail = makeStorage(f, p.wBlocks, p.hBlocks, mipCursor);
                mipCursor += tail.stride * c.slices;
                haveTail = true;
            }
            st = tail;
        } else {
            st = makeStorage(f, p.wBlocks, p.hBlocks, mipCursor);
            mipCursor += st.stride * c.slices;
        }
        if (packs && level >= packedLevel) packedOffset(f, h.width, h.height, level, p.tx, p.ty);
        p.offset = st.offset;
        p.pitch = st.pitch;
        p.sliceStride = st.stride;
        if (level >= c.firstLevel) {
            out.push_back(p);
            end = std::max<size_t>(end, size_t(st.offset) + size_t(st.stride) * c.slices);
        }
    }
    if (totalSize) *totalSize = end;
    return out;
}

uint32_t blockAddress(const Format& f, const Placement& p, uint32_t x, uint32_t y) {
    x += p.tx;
    y += p.ty;
    uint32_t idx = f.tiled ? tiledOffset(x, y, p.pitch, f.bytesPerBlock) : y * p.pitch + x;
    return idx * f.bytesPerBlock;
}

} // namespace

Format describeFormat(uint32_t d) {
    Format f;
    f.gpu = d & 0x3F;
    f.endian = (d >> 6) & 3;
    f.tiled = (d >> 8) & 1;
    switch (f.gpu) {
        case 0x12: f.blockW = f.blockH = 4; f.bytesPerBlock = 8; f.name = "DXT1"; break;
        case 0x13: f.blockW = f.blockH = 4; f.bytesPerBlock = 16; f.name = "DXT3"; break;
        case 0x14: f.blockW = f.blockH = 4; f.bytesPerBlock = 16; f.name = "DXT5"; break;
        case 0x31: f.blockW = f.blockH = 4; f.bytesPerBlock = 16; f.name = "DXN"; break;
        case 0x3C: f.blockW = f.blockH = 4; f.bytesPerBlock = 8; f.name = "DXT5A"; break;
        case 0x06: f.bytesPerBlock = 4; f.name = (d & 0xFFFFF000u) == 0x18280000u ? "A8R8G8B8" : "A8B8G8R8"; break;
        case 0x02: f.bytesPerBlock = 1; f.name = (d >> 12) == 0x04900 ? "A8" : "L8"; break;
        default: break;
    }
    return f;
}

bool isTextureHeader(const Bytes& d) { return d.size() >= 0x38 && std::memcmp(d.data(), "texture\0", 8) == 0; }

Header parseHeader(const Bytes& d) {
    if (!isTextureHeader(d)) fail("not a texture header");
    Header h;
    h.d3dFormat = be32(&d[0x18]);
    h.kind = Kind(be32(&d[0x1C]));
    h.width = be16(&d[0x24]);
    h.height = be16(&d[0x26]);
    h.baseOffset = be32(&d[0x28]);
    h.mipOffset = be32(&d[0x2C]);
    h.mipLevels = std::max<uint32_t>(1, d[0x30]);
    return h;
}

void writeHeaderDims(Bytes& d, uint32_t w, uint32_t h, uint32_t mips) {
    putBe16(&d[0x24], uint16_t(w));
    putBe16(&d[0x26], uint16_t(h));
    d[0x30] = uint8_t(mips);
}

namespace {

struct ArrayTable {
    uint32_t offsets, widths, heights, mips;
};

std::optional<ArrayTable> arrayTable(const Bytes& d, uint32_t slices) {
    if (d.size() < 0x54 || slices == 0 || be32(&d[0x38]) != slices) return std::nullopt;
    ArrayTable t{be32(&d[0x3C]), be32(&d[0x44]), be32(&d[0x48]), be32(&d[0x4C])};
    for (uint32_t p : {t.offsets, t.widths, t.heights, t.mips})
        if (p < 0x54 || p % 4 || size_t(p) + 4 * size_t(slices) > d.size()) return std::nullopt;
    for (uint32_t i = 0; i < slices; ++i)
        if (be32(&d[t.widths + 4 * i]) != be16(&d[0x24]) || be32(&d[t.heights + 4 * i]) != be16(&d[0x26])) return std::nullopt;
    return t;
}

} // namespace

bool hasArrayTable(const Bytes& header, uint32_t slices) { return arrayTable(header, slices).has_value(); }

void writeArrayTable(Bytes& d, const Header& h, uint32_t slices, uint32_t sliceBytes) {
    auto t = arrayTable(d, slices);
    if (!t) fail("this array texture's slice table is not the expected shape");
    for (uint32_t i = 0; i < slices; ++i) {
        putBe32(&d[t->widths + 4 * i], h.width);
        putBe32(&d[t->heights + 4 * i], h.height);
        putBe32(&d[t->mips + 4 * i], h.mipLevels);
        putBe32(&d[t->offsets + 4 * i], i * sliceBytes);
    }
}

Header resized(const Header& h, uint32_t w, uint32_t hh) {
    if (w == 0 || hh == 0 || w > 8192 || hh > 8192)
        fail("textures can be 1x1 to 8192x8192, not " + std::to_string(w) + "x" + std::to_string(hh));
    Header r = h;
    r.width = w;
    r.height = hh;
    if (h.mipLevels > 1) {
        int grow = int(log2Ceil(std::min(w, hh))) - int(log2Ceil(std::min(h.width, h.height)));
        int most = std::max(2, int(log2Ceil(std::max(w, hh))) + 1);
        r.mipLevels = uint32_t(std::clamp(int(h.mipLevels) + grow, 2, most));
    }
    return r;
}

Contents describeContents(const Header& h, size_t gpuBytes) {
    Contents c;
    bool split = h.mipLevels > 1 && h.baseOffset == 0 && h.mipOffset == 0;
    c.firstLevel = split ? 1 : 0;
    c.levelCount = h.mipLevels - c.firstLevel;
    if (h.kind == Kind::Cube) c.slices = 6;
    // Volume textures are stored like arrays: every slice is an ordinary 2D-tiled surface, and
    // all mip levels keep the full depth.
    if (h.kind == Kind::Array || h.kind == Kind::Volume) {
        Contents one = c;
        size_t per = 0;
        layout(h, one, &per);
        if (per == 0 || gpuBytes % per) fail("cannot determine slice count for array texture");
        c.slices = uint32_t(gpuBytes / per);
    }
    return c;
}

size_t gpuSize(const Header& h, const Contents& c) {
    size_t total = 0;
    layout(h, c, &total);
    return total;
}

std::vector<std::pair<uint32_t, uint32_t>> levelDims(const Header& h, const Contents& c) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (const Placement& p : layout(h, c, nullptr)) out.emplace_back(p.width, p.height);
    return out;
}

std::vector<Level> decode(const Header& h, const Bytes& gpu) {
    Format f = describeFormat(h.d3dFormat);
    Contents c = describeContents(h, gpu.size());
    std::vector<Level> levels;
    for (const Placement& p : layout(h, c, nullptr)) {
        Level lv;
        lv.width = p.width;
        lv.height = p.height;
        for (uint32_t s = 0; s < c.slices; ++s) {
            Bytes lin(size_t(p.wBlocks) * p.hBlocks * f.bytesPerBlock, 0);
            size_t sliceBase = size_t(p.offset) + size_t(s) * p.sliceStride;
            for (uint32_t y = 0; y < p.hBlocks; ++y)
                for (uint32_t x = 0; x < p.wBlocks; ++x) {
                    size_t src = sliceBase + blockAddress(f, p, x, y);
                    if (src + f.bytesPerBlock > gpu.size()) continue; // tolerate short blobs
                    std::memcpy(&lin[(size_t(y) * p.wBlocks + x) * f.bytesPerBlock], &gpu[src], f.bytesPerBlock);
                }
            swapEndian(lin, f.endian);
            lv.slices.push_back(std::move(lin));
        }
        levels.push_back(std::move(lv));
    }
    return levels;
}

Bytes encode(const Header& h, const Contents& c, const std::vector<Level>& levels) {
    Format f = describeFormat(h.d3dFormat);
    size_t total = 0;
    auto places = layout(h, c, &total);
    if (levels.size() != places.size()) fail("internal: level count mismatch");
    Bytes gpu(total, 0);
    for (size_t i = 0; i < places.size(); ++i) {
        const Placement& p = places[i];
        if (levels[i].slices.size() != c.slices) fail("internal: slice count mismatch");
        for (uint32_t s = 0; s < c.slices; ++s) {
            Bytes lin = levels[i].slices[s];
            if (lin.size() != size_t(p.wBlocks) * p.hBlocks * f.bytesPerBlock) fail("internal: level size mismatch");
            swapEndian(lin, f.endian);
            size_t sliceBase = size_t(p.offset) + size_t(s) * p.sliceStride;
            for (uint32_t y = 0; y < p.hBlocks; ++y)
                for (uint32_t x = 0; x < p.wBlocks; ++x) {
                    size_t dst = sliceBase + blockAddress(f, p, x, y);
                    std::memcpy(&gpu[dst], &lin[(size_t(y) * p.wBlocks + x) * f.bytesPerBlock], f.bytesPerBlock);
                }
        }
    }
    return gpu;
}

// ---- DDS ------------------------------------------------------------------

namespace {

void ddsPixelFormat(const Format& f, image::Dds& d) {
    switch (f.gpu) {
        case 0x12: d.fourcc = image::fourCC('D', 'X', 'T', '1'); break;
        case 0x13: d.fourcc = image::fourCC('D', 'X', 'T', '3'); break;
        case 0x14: d.fourcc = image::fourCC('D', 'X', 'T', '5'); break;
        case 0x31: d.fourcc = image::fourCC('A', 'T', 'I', '2'); break;
        case 0x3C: d.fourcc = image::fourCC('A', 'T', 'I', '1'); break;
        case 0x06:
            d.bitCount = 32;
            d.aMask = 0xFF000000;
            if (std::string(f.name) == "A8R8G8B8") { d.rMask = 0x00FF0000; d.gMask = 0x0000FF00; d.bMask = 0x000000FF; }
            else { d.rMask = 0x000000FF; d.gMask = 0x0000FF00; d.bMask = 0x00FF0000; }
            break;
        case 0x02:
            d.bitCount = 8;
            if (std::string(f.name) == "A8") { d.alphaOnly = true; d.aMask = 0xFF; }
            else { d.luminance = true; d.rMask = 0xFF; }
            break;
        default: fail("format has no DDS equivalent");
    }
}

bool isPow2(uint32_t v) { return v && !(v & (v - 1)); }

} // namespace

image::Dds toDds(const Header& h, const std::vector<Level>& levels, uint32_t slice) {
    Format f = describeFormat(h.d3dFormat);
    image::Dds d;
    ddsPixelFormat(f, d);
    d.width = levels.at(0).width;
    d.height = levels.at(0).height;
    // DDS readers derive mip sizes as max(1, w >> i); the GPU rounds mips up to powers of two,
    // so non-power-of-two textures are exported without their mip chain.
    bool chainOk = isPow2(d.width) && isPow2(d.height);
    for (size_t i = 0; i < levels.size(); ++i) {
        if (i > 0 && !chainOk) break;
        d.mips.push_back(levels[i].slices.at(slice));
    }
    d.mipCount = uint32_t(d.mips.size());
    return d;
}

std::vector<Level> fromDds(const Header& h, const Contents& c, const Bytes& file) {
    if (c.slices != 1) fail("importing cube/array textures is not supported");
    Format f = describeFormat(h.d3dFormat);
    image::Dds want;
    ddsPixelFormat(f, want);
    image::Dds d = image::decodeDds(file);
    if (d.fourcc != want.fourcc || (!want.fourcc && d.bitCount != want.bitCount))
        fail(std::string("DDS format does not match the texture (expected ") + f.name + ")");
    if (!want.fourcc && (d.rMask != want.rMask || d.gMask != want.gMask || d.bMask != want.bMask))
        fail(std::string("DDS channel layout does not match the texture (expected ") + f.name + ")");
    auto places = layout(h, c, nullptr);
    const auto& first = places.front();
    if (d.width != first.width || d.height != first.height)
        fail("DDS is " + std::to_string(d.width) + "x" + std::to_string(d.height) + " but the texture needs " +
             std::to_string(first.width) + "x" + std::to_string(first.height));
    const Bytes& all = d.mips[0];
    std::vector<Level> levels;
    size_t pos = 0;
    image::Rgba prev;
    for (size_t i = 0; i < places.size(); ++i) {
        const auto& p = places[i];
        size_t bytes = size_t(p.wBlocks) * p.hBlocks * f.bytesPerBlock;
        Level lv;
        lv.width = p.width;
        lv.height = p.height;
        if (i < d.mipCount && pos + bytes <= all.size()) {
            lv.slices.emplace_back(all.begin() + pos, all.begin() + pos + bytes);
            pos += bytes;
        } else {
            // Missing levels are generated from the previous one.
            Level& last = levels.back();
            image::Rgba src = toRgba(h, last);
            lv.slices.push_back(fromRgba(f, downscale(src, p.width, p.height)));
        }
        levels.push_back(std::move(lv));
    }
    return levels;
}

// ---- block codecs ----------------------------------------------------------

namespace {

struct Color { int r, g, b, a; };

Color from565(uint16_t c) {
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255};
}
uint16_t to565(int r, int g, int b) {
    return uint16_t(((std::clamp(r, 0, 255) * 31 + 127) / 255) << 11 | ((std::clamp(g, 0, 255) * 63 + 127) / 255) << 5 |
                    ((std::clamp(b, 0, 255) * 31 + 127) / 255));
}

void decodeColorBlock(const uint8_t* b, Color out[16], bool forceFour) {
    uint16_t c0 = le16(b), c1 = le16(b + 2);
    Color p[4] = {from565(c0), from565(c1), {}, {}};
    if (c0 > c1 || forceFour) {
        p[2] = {(2 * p[0].r + p[1].r) / 3, (2 * p[0].g + p[1].g) / 3, (2 * p[0].b + p[1].b) / 3, 255};
        p[3] = {(p[0].r + 2 * p[1].r) / 3, (p[0].g + 2 * p[1].g) / 3, (p[0].b + 2 * p[1].b) / 3, 255};
    } else {
        p[2] = {(p[0].r + p[1].r) / 2, (p[0].g + p[1].g) / 2, (p[0].b + p[1].b) / 2, 255};
        p[3] = {0, 0, 0, 0};
    }
    uint32_t idx = le32(b + 4);
    for (int i = 0; i < 16; ++i) out[i] = p[(idx >> (2 * i)) & 3];
}

void decodeAlphaBlock(const uint8_t* b, int out[16]) {
    int a0 = b[0], a1 = b[1];
    int p[8] = {a0, a1};
    if (a0 > a1) for (int i = 1; i < 7; ++i) p[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    else { for (int i = 1; i < 5; ++i) p[i + 1] = ((5 - i) * a0 + i * a1) / 5; p[6] = 0; p[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) out[i] = p[(bits >> (3 * i)) & 7];
}

void encodeAlphaBlock(const int in[16], uint8_t* out) {
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; ++i) { lo = std::min(lo, in[i]); hi = std::max(hi, in[i]); }
    out[0] = uint8_t(hi);
    out[1] = uint8_t(lo);
    int p[8] = {hi, lo};
    for (int i = 1; i < 7; ++i) p[i + 1] = ((7 - i) * hi + i * lo) / 7;
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0, bestErr = 1 << 30;
        if (hi != lo)
            for (int k = 0; k < 8; ++k) { int e = std::abs(p[k] - in[i]); if (e < bestErr) { bestErr = e; best = k; } }
        bits |= uint64_t(best) << (3 * i);
    }
    for (int i = 0; i < 6; ++i) out[2 + i] = uint8_t(bits >> (8 * i));
}

void encodeColorBlock(const Color in[16], uint8_t* out, bool allowTransparent) {
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    bool anyTransparent = false;
    for (int i = 0; i < 16; ++i) {
        if (allowTransparent && in[i].a < 128) { anyTransparent = true; continue; }
        int c[3] = {in[i].r, in[i].g, in[i].b};
        for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], c[k]); mx[k] = std::max(mx[k], c[k]); }
    }
    if (mn[0] > mx[0]) { mn[0] = mn[1] = mn[2] = mx[0] = mx[1] = mx[2] = 0; }
    // Inset the bounding box slightly to reduce quantisation error at the extremes.
    for (int k = 0; k < 3; ++k) { int inset = (mx[k] - mn[k]) / 16; mn[k] += inset; mx[k] -= inset; }
    uint16_t c0 = to565(mx[0], mx[1], mx[2]), c1 = to565(mn[0], mn[1], mn[2]);
    if (anyTransparent) { if (c0 > c1) std::swap(c0, c1); }   // 3-colour mode needs c0 <= c1
    else if (c0 < c1) std::swap(c0, c1);
    putLe16(out, c0);
    putLe16(out + 2, c1);
    Color pal[4] = {from565(c0), from565(c1), {}, {}};
    bool four = c0 > c1;
    if (four) {
        pal[2] = {(2 * pal[0].r + pal[1].r) / 3, (2 * pal[0].g + pal[1].g) / 3, (2 * pal[0].b + pal[1].b) / 3, 255};
        pal[3] = {(pal[0].r + 2 * pal[1].r) / 3, (pal[0].g + 2 * pal[1].g) / 3, (pal[0].b + 2 * pal[1].b) / 3, 255};
    } else {
        pal[2] = {(pal[0].r + pal[1].r) / 2, (pal[0].g + pal[1].g) / 2, (pal[0].b + pal[1].b) / 2, 255};
    }
    uint32_t idx = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0;
        if (!four && anyTransparent && in[i].a < 128) best = 3;
        else {
            int bestErr = 1 << 30;
            for (int k = 0; k < (four ? 4 : 3); ++k) {
                int dr = pal[k].r - in[i].r, dg = pal[k].g - in[i].g, db = pal[k].b - in[i].b;
                int e = dr * dr + dg * dg + db * db;
                if (e < bestErr) { bestErr = e; best = k; }
            }
        }
        idx |= uint32_t(best) << (2 * i);
    }
    putLe32(out + 4, idx);
}

} // namespace

image::Rgba toRgba(const Header& h, const Level& lv, uint32_t slice) {
    Format f = describeFormat(h.d3dFormat);
    const Bytes& d = lv.slices.at(slice);
    image::Rgba img;
    img.width = lv.width;
    img.height = lv.height;
    img.pixels.assign(size_t(img.width) * img.height * 4, 255);
    auto put = [&](uint32_t x, uint32_t y, int r, int g, int b, int a) {
        if (x >= img.width || y >= img.height) return;
        uint8_t* o = &img.pixels[(size_t(y) * img.width + x) * 4];
        o[0] = uint8_t(r); o[1] = uint8_t(g); o[2] = uint8_t(b); o[3] = uint8_t(a);
    };
    uint32_t wb = (lv.width + f.blockW - 1) / f.blockW, hb = (lv.height + f.blockH - 1) / f.blockH;
    for (uint32_t by = 0; by < hb; ++by)
        for (uint32_t bx = 0; bx < wb; ++bx) {
            const uint8_t* b = &d[(size_t(by) * wb + bx) * f.bytesPerBlock];
            Color c[16];
            int a[16], g2[16];
            switch (f.gpu) {
                case 0x12:
                    decodeColorBlock(b, c, false);
                    for (int i = 0; i < 16; ++i) put(bx * 4 + i % 4, by * 4 + i / 4, c[i].r, c[i].g, c[i].b, c[i].a);
                    break;
                case 0x13:
                    decodeColorBlock(b + 8, c, true);
                    for (int i = 0; i < 16; ++i) {
                        int av = (b[i / 2] >> ((i & 1) * 4)) & 15;
                        put(bx * 4 + i % 4, by * 4 + i / 4, c[i].r, c[i].g, c[i].b, av * 17);
                    }
                    break;
                case 0x14:
                    decodeAlphaBlock(b, a);
                    decodeColorBlock(b + 8, c, true);
                    for (int i = 0; i < 16; ++i) put(bx * 4 + i % 4, by * 4 + i / 4, c[i].r, c[i].g, c[i].b, a[i]);
                    break;
                case 0x3C:
                    decodeAlphaBlock(b, a);
                    for (int i = 0; i < 16; ++i) put(bx * 4 + i % 4, by * 4 + i / 4, a[i], a[i], a[i], 255);
                    break;
                case 0x31:
                    decodeAlphaBlock(b, a);
                    decodeAlphaBlock(b + 8, g2);
                    for (int i = 0; i < 16; ++i) {
                        double nx = a[i] / 127.5 - 1, ny = g2[i] / 127.5 - 1;
                        double nz = std::sqrt(std::max(0.0, 1 - nx * nx - ny * ny));
                        put(bx * 4 + i % 4, by * 4 + i / 4, a[i], g2[i], int(std::lround((nz + 1) * 127.5)), 255);
                    }
                    break;
                case 0x06:
                    if (std::string(f.name) == "A8R8G8B8") put(bx, by, b[2], b[1], b[0], b[3]);
                    else put(bx, by, b[0], b[1], b[2], b[3]);
                    break;
                case 0x02:
                    if (std::string(f.name) == "A8") put(bx, by, 255, 255, 255, b[0]);
                    else put(bx, by, b[0], b[0], b[0], 255);
                    break;
                default: fail("cannot convert this format to RGBA");
            }
        }
    return img;
}

Bytes fromRgba(const Format& f, const image::Rgba& img) {
    uint32_t wb = (img.width + f.blockW - 1) / f.blockW, hb = (img.height + f.blockH - 1) / f.blockH;
    Bytes out(size_t(wb) * hb * f.bytesPerBlock, 0);
    auto px = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        x = std::min(x, img.width - 1);
        y = std::min(y, img.height - 1);
        return &img.pixels[(size_t(y) * img.width + x) * 4];
    };
    for (uint32_t by = 0; by < hb; ++by)
        for (uint32_t bx = 0; bx < wb; ++bx) {
            uint8_t* o = &out[(size_t(by) * wb + bx) * f.bytesPerBlock];
            Color c[16];
            int a[16], g[16];
            for (int i = 0; i < 16 && f.compressed(); ++i) {
                const uint8_t* p = px(bx * 4 + i % 4, by * 4 + i / 4);
                c[i] = {p[0], p[1], p[2], p[3]};
                a[i] = p[3];
                g[i] = p[1];
            }
            switch (f.gpu) {
                case 0x12: encodeColorBlock(c, o, true); break;
                case 0x13:
                    for (int i = 0; i < 16; ++i) o[i / 2] |= uint8_t(((a[i] * 15 + 127) / 255) << ((i & 1) * 4));
                    for (auto& cc : c) cc.a = 255;
                    encodeColorBlock(c, o + 8, false);
                    break;
                case 0x14:
                    encodeAlphaBlock(a, o);
                    for (auto& cc : c) cc.a = 255;
                    encodeColorBlock(c, o + 8, false);
                    break;
                case 0x3C: {
                    int r[16];
                    for (int i = 0; i < 16; ++i) r[i] = c[i].r;
                    encodeAlphaBlock(r, o);
                    break;
                }
                case 0x31: {
                    int r[16];
                    for (int i = 0; i < 16; ++i) r[i] = c[i].r;
                    encodeAlphaBlock(r, o);
                    encodeAlphaBlock(g, o + 8);
                    break;
                }
                case 0x06: {
                    const uint8_t* p = px(bx, by);
                    if (std::string(f.name) == "A8R8G8B8") { o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3]; }
                    else std::memcpy(o, p, 4);
                    break;
                }
                case 0x02: {
                    const uint8_t* p = px(bx, by);
                    o[0] = std::string(f.name) == "A8" ? p[3] : p[0];
                    break;
                }
                default: fail("cannot encode this format");
            }
        }
    return out;
}

image::Rgba downscale(const image::Rgba& src, uint32_t w, uint32_t h) {
    image::Rgba out;
    out.width = w;
    out.height = h;
    out.pixels.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t x0 = x * src.width / w, x1 = std::max(x0 + 1, (x + 1) * src.width / w);
            uint32_t y0 = y * src.height / h, y1 = std::max(y0 + 1, (y + 1) * src.height / h);
            uint32_t acc[4] = {}, n = 0;
            for (uint32_t yy = y0; yy < y1 && yy < src.height; ++yy)
                for (uint32_t xx = x0; xx < x1 && xx < src.width; ++xx, ++n)
                    for (int k = 0; k < 4; ++k) acc[k] += src.pixels[(size_t(yy) * src.width + xx) * 4 + k];
            for (int k = 0; k < 4; ++k) out.pixels[(size_t(y) * w + x) * 4 + k] = uint8_t(n ? acc[k] / n : 0);
        }
    return out;
}

} // namespace nut::texture
