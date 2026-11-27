#include "deflate.hpp"

#include <algorithm>
#include <array>

namespace nut::deflate {
namespace {

constexpr uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// LSB-first bit reader (DEFLATE bit order).
struct InBits {
    const uint8_t* p;
    const uint8_t* end;
    const uint8_t* begin;
    uint64_t buf = 0;
    int left = 0;

    void fill() {
        while (left <= 56 && p < end) { buf |= uint64_t(*p++) << left; left += 8; }
    }
    uint32_t bits(int n) {
        if (n == 0) return 0;
        if (left < n) fill();
        if (left < n) fail("inflate: unexpected end of data");
        uint32_t v = uint32_t(buf & ((1ull << n) - 1));
        buf >>= n;
        left -= n;
        return v;
    }
    void alignByte() { int r = left & 7; buf >>= r; left -= r; }
    size_t consumed() const { return size_t(p - begin) - size_t(left / 8); }
};

// Canonical Huffman decoder using a count/symbol table (puff-style).
struct Huff {
    std::array<uint16_t, 16> count{};
    std::vector<uint16_t> symbol;

    void build(const uint8_t* lens, int n) {
        count.fill(0);
        for (int i = 0; i < n; ++i) count[lens[i]]++;
        count[0] = 0;
        std::array<uint16_t, 16> offs{};
        for (int i = 1; i < 16; ++i) offs[i] = uint16_t(offs[i - 1] + count[i - 1]);
        symbol.assign(size_t(n), 0);
        for (int i = 0; i < n; ++i) if (lens[i]) symbol[offs[lens[i]]++] = uint16_t(i);
    }
    int decode(InBits& in) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= int(in.bits(1));
            int c = count[len];
            if (code - c < first) return symbol[size_t(index + (code - first))];
            index += c;
            first = (first + c) << 1;
            code <<= 1;
        }
        fail("inflate: invalid Huffman code");
    }
};

void inflateBlock(InBits& in, Bytes& out, const Huff& lit, const Huff& dist) {
    for (;;) {
        int sym = lit.decode(in);
        if (sym < 256) { out.push_back(uint8_t(sym)); continue; }
        if (sym == 256) return;
        sym -= 257;
        if (sym >= 29) fail("inflate: bad length symbol");
        size_t len = kLenBase[sym] + in.bits(kLenExtra[sym]);
        int ds = dist.decode(in);
        if (ds >= 30) fail("inflate: bad distance symbol");
        size_t d = kDistBase[ds] + in.bits(kDistExtra[ds]);
        if (d > out.size()) fail("inflate: distance too far back");
        size_t from = out.size() - d;
        for (size_t k = 0; k < len; ++k) out.push_back(out[from + k]);
    }
}

} // namespace

Bytes inflate(const uint8_t* src, size_t size, size_t* consumed, size_t sizeHint) {
    InBits in{src, src + size, src};
    Bytes out;
    out.reserve(sizeHint ? sizeHint : size * 4);
    static Huff fixedLit, fixedDist;
    static bool fixedInit = false;
    if (!fixedInit) {
        uint8_t l[288];
        for (int i = 0; i < 144; ++i) l[i] = 8;
        for (int i = 144; i < 256; ++i) l[i] = 9;
        for (int i = 256; i < 280; ++i) l[i] = 7;
        for (int i = 280; i < 288; ++i) l[i] = 8;
        fixedLit.build(l, 288);
        uint8_t d[30];
        std::fill(d, d + 30, uint8_t(5));
        fixedDist.build(d, 30);
        fixedInit = true;
    }
    int last;
    do {
        last = int(in.bits(1));
        int type = int(in.bits(2));
        if (type == 0) {
            in.alignByte();
            uint32_t len = in.bits(16), nlen = in.bits(16);
            if ((len ^ 0xFFFF) != nlen) fail("inflate: stored block length mismatch");
            for (uint32_t i = 0; i < len; ++i) out.push_back(uint8_t(in.bits(8)));
        } else if (type == 1) {
            inflateBlock(in, out, fixedLit, fixedDist);
        } else if (type == 2) {
            int hlit = int(in.bits(5)) + 257, hdist = int(in.bits(5)) + 1, hclen = int(in.bits(4)) + 4;
            uint8_t cl[19] = {};
            for (int i = 0; i < hclen; ++i) cl[kClOrder[i]] = uint8_t(in.bits(3));
            Huff clh;
            clh.build(cl, 19);
            uint8_t lens[320] = {};
            for (int i = 0; i < hlit + hdist;) {
                int sym = clh.decode(in);
                if (sym < 16) { lens[i++] = uint8_t(sym); continue; }
                int rep = 0;
                uint8_t val = 0;
                if (sym == 16) { if (i == 0) fail("inflate: repeat with no previous length"); val = lens[i - 1]; rep = 3 + int(in.bits(2)); }
                else if (sym == 17) rep = 3 + int(in.bits(3));
                else rep = 11 + int(in.bits(7));
                if (i + rep > hlit + hdist) fail("inflate: too many code lengths");
                while (rep--) lens[i++] = val;
            }
            Huff lit, dist;
            lit.build(lens, hlit);
            dist.build(lens + hlit, hdist);
            inflateBlock(in, out, lit, dist);
        } else {
            fail("inflate: invalid block type");
        }
    } while (!last);
    if (consumed) *consumed = in.consumed();
    return out;
}

namespace {

// LSB-first bit writer.
struct OutBits {
    Bytes out;
    uint64_t buf = 0;
    int n = 0;
    void put(uint32_t v, int bits) {
        buf |= uint64_t(v) << n;
        n += bits;
        while (n >= 8) { out.push_back(uint8_t(buf)); buf >>= 8; n -= 8; }
    }
    // Huffman codes are emitted MSB-first, so reverse before writing.
    void putCode(uint32_t code, int len) {
        uint32_t r = 0;
        for (int i = 0; i < len; ++i) r |= ((code >> i) & 1) << (len - 1 - i);
        put(r, len);
    }
    void flush() { if (n) { out.push_back(uint8_t(buf)); buf = 0; n = 0; } }
};

void putFixedLit(OutBits& ob, int sym) {
    if (sym < 144) ob.putCode(0x30 + uint32_t(sym), 8);
    else if (sym < 256) ob.putCode(0x190 + uint32_t(sym - 144), 9);
    else if (sym < 280) ob.putCode(uint32_t(sym - 256), 7);
    else ob.putCode(0xC0 + uint32_t(sym - 280), 8);
}

} // namespace

Bytes compress(const uint8_t* src, size_t size) {
    // Greedy LZ77 with hash chains, emitted as a single fixed-Huffman block.
    constexpr int kWindow = 32768, kHashBits = 15, kMaxChain = 64;
    std::vector<int32_t> head(size_t(1) << kHashBits, -1), prev(size, -1);
    auto hash = [&](size_t i) { return ((uint32_t(src[i]) << 10) ^ (uint32_t(src[i + 1]) << 5) ^ src[i + 2]) & ((1u << kHashBits) - 1); };
    OutBits ob;
    ob.put(1, 1); // final block
    ob.put(1, 2); // fixed Huffman
    size_t i = 0;
    while (i < size) {
        int bestLen = 0;
        size_t bestDist = 0;
        if (i + 3 <= size) {
            uint32_t h = hash(i);
            int32_t cand = head[h];
            for (int chain = 0; cand >= 0 && chain < kMaxChain; ++chain, cand = prev[size_t(cand)]) {
                size_t d = i - size_t(cand);
                if (d > kWindow) break;
                int l = 0;
                size_t maxL = std::min<size_t>(258, size - i);
                while (size_t(l) < maxL && src[size_t(cand) + size_t(l)] == src[i + size_t(l)]) ++l;
                if (l > bestLen) { bestLen = l; bestDist = d; if (l == 258) break; }
            }
        }
        size_t adv = bestLen >= 3 ? size_t(bestLen) : 1;
        for (size_t k = 0; k < adv; ++k) {
            if (i + k + 3 <= size) {
                uint32_t h = hash(i + k);
                prev[i + k] = head[h];
                head[h] = int32_t(i + k);
            }
        }
        if (bestLen >= 3) {
            int ls = 0;
            while (ls < 28 && kLenBase[ls + 1] <= bestLen) ++ls;
            putFixedLit(ob, 257 + ls);
            ob.put(uint32_t(bestLen - kLenBase[ls]), kLenExtra[ls]);
            int ds = 0;
            while (ds < 29 && kDistBase[ds + 1] <= bestDist) ++ds;
            ob.putCode(uint32_t(ds), 5);
            ob.put(uint32_t(bestDist - kDistBase[ds]), kDistExtra[ds]);
        } else {
            putFixedLit(ob, src[i]);
        }
        i += adv;
    }
    putFixedLit(ob, 256);
    ob.flush();
    return std::move(ob.out);
}

uint32_t adler32(const uint8_t* p, size_t n, uint32_t a) {
    uint32_t s1 = a & 0xFFFF, s2 = a >> 16;
    while (n) {
        size_t k = std::min<size_t>(n, 5552);
        n -= k;
        while (k--) { s1 += *p++; s2 += s1; }
        s1 %= 65521;
        s2 %= 65521;
    }
    return s2 << 16 | s1;
}

Bytes zlibDecompress(const uint8_t* src, size_t size, size_t* consumed, size_t sizeHint) {
    if (size < 6) fail("zlib: stream too short");
    if ((src[0] & 0x0F) != 8 || (uint32_t(src[0]) << 8 | src[1]) % 31) fail("zlib: bad header");
    if (src[1] & 0x20) fail("zlib: preset dictionary not supported");
    size_t used = 0;
    Bytes out = inflate(src + 2, size - 2, &used, sizeHint);
    if (2 + used + 4 > size) fail("zlib: missing checksum");
    if (be32(src + 2 + used) != adler32(out.data(), out.size())) fail("zlib: checksum mismatch");
    if (consumed) *consumed = 2 + used + 4;
    return out;
}

Bytes zlibCompress(const uint8_t* src, size_t size) {
    Bytes out{0x78, 0xDA};
    Bytes body = compress(src, size);
    out.insert(out.end(), body.begin(), body.end());
    uint8_t t[4];
    putBe32(t, adler32(src, size));
    out.insert(out.end(), t, t + 4);
    return out;
}

} // namespace nut::deflate
