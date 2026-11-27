#include "image.hpp"

#include "deflate.hpp"

#include <climits>
#include <cstdlib>
#include <cstring>

#ifndef NUTTOOL_STB
#define NUTTOOL_STB 0
#endif

#if NUTTOOL_STB
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#include <stb_image.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
#endif

namespace nut::image {

namespace {

void pngChunk(Writer& w, const char* type, const Bytes& data) {
    w.u32(uint32_t(data.size()));
    size_t start = w.pos();
    w.bytes(type, 4);
    w.bytes(data);
    w.u32(crc32(&w.buf()[start], data.size() + 4));
}

uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

} // namespace

Bytes encodePng(const Rgba& img) {
    Writer w;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    w.bytes(sig, 8);
    Writer ihdr;
    ihdr.u32(img.width);
    ihdr.u32(img.height);
    ihdr.u8(8);
    ihdr.u8(6); // RGBA
    ihdr.u8(0);
    ihdr.u8(0);
    ihdr.u8(0);
    pngChunk(w, "IHDR", ihdr.buf());
    // Filter type "Sub" on every row: cheap and compresses well with our encoder.
    size_t stride = size_t(img.width) * 4;
    Bytes raw;
    raw.reserve((stride + 1) * img.height);
    for (uint32_t y = 0; y < img.height; ++y) {
        raw.push_back(1);
        const uint8_t* row = &img.pixels[y * stride];
        for (size_t x = 0; x < stride; ++x) raw.push_back(uint8_t(row[x] - (x >= 4 ? row[x - 4] : 0)));
    }
    pngChunk(w, "IDAT", deflate::zlibCompress(raw.data(), raw.size()));
    pngChunk(w, "IEND", {});
    return w.buf();
}

Rgba decodePng(const Bytes& file) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (file.size() < 8 || std::memcmp(file.data(), sig, 8) != 0) fail("not a PNG file");
    Reader r(file);
    r.skip(8);
    uint32_t w = 0, h = 0;
    int depth = 0, color = 0, interlace = 0;
    Bytes idat, palette, trns;
    while (r.remaining() >= 12) {
        uint32_t len = r.u32();
        char type[5] = {};
        r.bytes(type, 4);
        Bytes data = r.bytes(len);
        r.u32();
        std::string t(type);
        if (t == "IHDR") {
            w = be32(&data[0]);
            h = be32(&data[4]);
            depth = data[8];
            color = data[9];
            interlace = data[12];
        } else if (t == "PLTE") palette = data;
        else if (t == "tRNS") trns = data;
        else if (t == "IDAT") idat.insert(idat.end(), data.begin(), data.end());
        else if (t == "IEND") break;
    }
    if (interlace) fail("interlaced PNG files are not supported");
    if (depth != 8) fail("only 8-bit PNG files are supported");
    int channels = color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 : color == 4 ? 2 : color == 6 ? 4 : 0;
    if (!channels) fail("unsupported PNG color type");
    Bytes raw = deflate::zlibDecompress(idat.data(), idat.size());
    size_t stride = size_t(w) * size_t(channels);
    if (raw.size() < (stride + 1) * h) fail("PNG image data too short");
    Bytes cur(stride), prev(stride, 0);
    Rgba img;
    img.width = w;
    img.height = h;
    img.pixels.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = &raw[y * (stride + 1)];
        uint8_t f = src[0];
        for (size_t x = 0; x < stride; ++x) {
            int a = x >= size_t(channels) ? cur[x - size_t(channels)] : 0, b = prev[x], c = x >= size_t(channels) ? prev[x - size_t(channels)] : 0;
            uint8_t v = src[1 + x];
            switch (f) {
                case 0: break;
                case 1: v = uint8_t(v + a); break;
                case 2: v = uint8_t(v + b); break;
                case 3: v = uint8_t(v + (a + b) / 2); break;
                case 4: v = uint8_t(v + paeth(a, b, c)); break;
                default: fail("bad PNG filter");
            }
            cur[x] = v;
        }
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* o = &img.pixels[(size_t(y) * w + x) * 4];
            const uint8_t* s = &cur[size_t(x) * size_t(channels)];
            switch (color) {
                case 0: o[0] = o[1] = o[2] = s[0]; o[3] = 255; break;
                case 2: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
                case 3:
                    if (size_t(s[0]) * 3 + 2 >= palette.size()) fail("PNG palette index out of range");
                    o[0] = palette[s[0] * 3]; o[1] = palette[s[0] * 3 + 1]; o[2] = palette[s[0] * 3 + 2];
                    o[3] = s[0] < trns.size() ? trns[s[0]] : 255;
                    break;
                case 4: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
                case 6: std::memcpy(o, s, 4); break;
            }
        }
        std::swap(cur, prev);
    }
    return img;
}

bool canDecodeImages() { return NUTTOOL_STB != 0; }

Rgba decodeImage(const Bytes& file) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    bool png = file.size() >= 8 && std::memcmp(file.data(), sig, 8) == 0;
#if NUTTOOL_STB
    // Our own decoder covers the PNGs we write; stb takes 16-bit and interlaced ones, and the rest.
    if (png) {
        try {
            return decodePng(file);
        } catch (std::exception&) {
        }
    }
    if (file.size() > size_t(INT_MAX)) fail("the image file is too large");
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &n, 4);
    if (!px) fail(std::string("cannot read the image (") + stbi_failure_reason() + ")");
    Rgba img;
    img.width = uint32_t(w);
    img.height = uint32_t(h);
    img.pixels.assign(px, px + size_t(w) * size_t(h) * 4);
    stbi_image_free(px);
    return img;
#else
    if (!png) fail("this build reads only PNG images (it was built without stb_image)");
    return decodePng(file);
#endif
}

// ---- DDS ----------------------------------------------------------------

namespace {
constexpr uint32_t DDSD_CAPS = 0x1, DDSD_HEIGHT = 0x2, DDSD_WIDTH = 0x4, DDSD_PITCH = 0x8, DDSD_PIXELFORMAT = 0x1000,
                   DDSD_MIPMAPCOUNT = 0x20000, DDSD_LINEARSIZE = 0x80000;
constexpr uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_ALPHA = 0x2, DDPF_FOURCC = 0x4, DDPF_RGB = 0x40, DDPF_LUMINANCE = 0x20000;
} // namespace

Bytes encodeDds(const Dds& d) {
    Writer w(false);
    w.u32(fourCC('D', 'D', 'S', ' '));
    w.u32(124);
    uint32_t flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
    if (d.mips.size() > 1) flags |= DDSD_MIPMAPCOUNT;
    flags |= d.fourcc ? DDSD_LINEARSIZE : DDSD_PITCH;
    w.u32(flags);
    w.u32(d.height);
    w.u32(d.width);
    w.u32(d.fourcc ? uint32_t(d.mips.empty() ? 0 : d.mips[0].size()) : d.width * d.bitCount / 8);
    w.u32(0);
    w.u32(uint32_t(d.mips.size()));
    w.zeros(44);
    w.u32(32);
    uint32_t pf = 0;
    if (d.fourcc) pf = DDPF_FOURCC;
    else if (d.alphaOnly) pf = DDPF_ALPHA;
    else if (d.luminance) pf = DDPF_LUMINANCE | (d.aMask ? DDPF_ALPHAPIXELS : 0);
    else pf = DDPF_RGB | (d.aMask ? DDPF_ALPHAPIXELS : 0);
    w.u32(pf);
    w.u32(d.fourcc);
    w.u32(d.bitCount);
    w.u32(d.rMask);
    w.u32(d.gMask);
    w.u32(d.bMask);
    w.u32(d.aMask);
    w.u32(0x1000 | (d.mips.size() > 1 ? 0x400008 : 0)); // TEXTURE | MIPMAP | COMPLEX
    w.zeros(16);
    for (auto& m : d.mips) w.bytes(m);
    return w.buf();
}

Dds decodeDds(const Bytes& file) {
    Reader r(file, false);
    if (file.size() < 128 || r.u32() != fourCC('D', 'D', 'S', ' ')) fail("not a DDS file");
    r.u32();
    r.u32();
    Dds d;
    d.height = r.u32();
    d.width = r.u32();
    r.u32();
    r.u32();
    d.mipCount = std::max<uint32_t>(1, r.u32());
    r.skip(44);
    r.u32();
    uint32_t pf = r.u32();
    d.fourcc = r.u32();
    d.bitCount = r.u32();
    d.rMask = r.u32();
    d.gMask = r.u32();
    d.bMask = r.u32();
    d.aMask = r.u32();
    r.skip(20);
    if (!(pf & DDPF_FOURCC)) d.fourcc = 0;
    d.luminance = (pf & DDPF_LUMINANCE) != 0;
    d.alphaOnly = (pf & DDPF_ALPHA) != 0 && !(pf & DDPF_RGB);
    if (d.fourcc == fourCC('D', 'X', '1', '0')) fail("DX10-header DDS files are not supported; save as legacy DXT/RGB DDS");
    // The caller slices r.cur() into levels since it knows the block geometry.
    d.mips.emplace_back(r.cur(), r.cur() + r.remaining());
    return d;
}

} // namespace nut::image
