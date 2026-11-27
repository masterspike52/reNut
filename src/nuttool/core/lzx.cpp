#include "lzx.hpp"

#include <algorithm>
#include <array>

namespace nut::lzx {
namespace {

constexpr int kNumChars = 256;
constexpr int kPretreeSyms = 20;
constexpr int kAlignedSyms = 8;
constexpr int kLengthSyms = 249;
constexpr int kMaxMainSyms = kNumChars + 50 * 8;
constexpr size_t kFrameSize = 32768;

enum BlockType { kVerbatim = 1, kAligned = 2, kUncompressed = 3 };

struct Tables {
    std::array<uint8_t, 52> extraBits{};
    std::array<uint32_t, 52> positionBase{};
    Tables() {
        for (int i = 0, j = 0; i < 52; i += 2) {
            extraBits[i] = extraBits[i + 1] = uint8_t(j);
            if (i != 0 && j < 17) ++j;
        }
        for (int i = 0, j = 0; i < 52; ++i) {
            positionBase[i] = uint32_t(j);
            j += 1 << extraBits[i];
        }
    }
};
const Tables kTables;

int positionSlots(int windowBits) {
    switch (windowBits) {
        case 15: return 30;
        case 16: return 32;
        case 17: return 34;
        case 18: return 36;
        case 19: return 38;
        case 20: return 42;
        case 21: return 50;
        default: fail("unsupported LZX window size");
    }
}

// MSB-first reader over 16-bit little-endian words. Reads past the end yield zeros.
struct BitReader {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    uint32_t buf = 0;
    int left = 0;

    void reset(const uint8_t* b, const uint8_t* e) { p = b; end = e; buf = 0; left = 0; }
    void ensure(int n) {
        while (left < n) {
            uint32_t w = 0;
            if (end - p >= 2) { w = uint32_t(p[0]) | uint32_t(p[1]) << 8; p += 2; }
            else if (p < end) { w = p[0]; ++p; }
            buf |= w << (16 - left);
            left += 16;
        }
    }
    uint32_t peek(int n) const { return buf >> (32 - n); }
    void remove(int n) { buf <<= n; left -= n; }
    uint32_t read(int n) {
        if (n == 0) return 0;
        ensure(n);
        uint32_t v = peek(n);
        remove(n);
        return v;
    }
    // Discard to the next 16-bit boundary the way LZX uncompressed blocks expect.
    void alignForRaw() {
        ensure(16);
        if (left > 16) p -= 2;
        buf = 0;
        left = 0;
    }
    uint8_t rawByte() { if (p >= end) fail("LZX: unexpected end of input"); return *p++; }
};

class Huffman {
public:
    void build(const uint8_t* lens, int n) {
        maxLen_ = 0;
        for (int i = 0; i < n; ++i) maxLen_ = std::max<int>(maxLen_, lens[i]);
        if (maxLen_ == 0) { table_.clear(); return; }
        if (maxLen_ > 16) fail("LZX: code length too long");
        table_.assign(size_t(1) << maxLen_, 0xFFFFFFFFu);
        uint32_t code = 0;
        for (int len = 1; len <= maxLen_; ++len) {
            for (int sym = 0; sym < n; ++sym) {
                if (lens[sym] != len) continue;
                if (code >= (1u << len)) fail("LZX: over-subscribed Huffman table");
                uint32_t lo = code << (maxLen_ - len), hi = (code + 1) << (maxLen_ - len);
                for (uint32_t k = lo; k < hi; ++k) table_[k] = uint32_t(sym) | uint32_t(len) << 16;
                ++code;
            }
            code <<= 1;
        }
    }
    int decode(BitReader& br) const {
        if (table_.empty()) fail("LZX: use of empty Huffman table");
        br.ensure(maxLen_);
        uint32_t e = table_[br.peek(maxLen_)];
        if (e == 0xFFFFFFFFu) fail("LZX: invalid Huffman code");
        br.remove(int(e >> 16));
        return int(e & 0xFFFF);
    }

private:
    std::vector<uint32_t> table_;
    int maxLen_ = 0;
};

class Decoder {
public:
    Decoder(int windowBits, size_t expected) : windowSize_(size_t(1) << windowBits), mainSyms_(kNumChars + positionSlots(windowBits) * 8) {
        out_.reserve(expected);
    }

    void frame(const uint8_t* src, size_t csize, size_t frameEnd) {
        br_.reset(src, src + csize);
        padPending_ = false; // each frame chunk starts a fresh, aligned bitstream
        if (!headerRead_) {
            if (br_.read(1)) {
                uint32_t hi = br_.read(16), lo = br_.read(16);
                intelSize_ = int32_t(hi << 16 | lo);
            }
            headerRead_ = true;
        }
        while (out_.size() < frameEnd) {
            if (blockRemaining_ == 0) readBlockHeader();
            size_t run = std::min<size_t>(blockRemaining_, frameEnd - out_.size());
            size_t produced = 0;
            if (blockType_ == kUncompressed) {
                while (produced < run) out_.push_back(br_.rawByte()), ++produced;
            } else {
                produced = decodeSymbols(run);
            }
            if (produced > blockRemaining_) fail("LZX: match overruns block");
            blockRemaining_ -= uint32_t(produced);
            // An odd-length uncompressed block is followed by a pad byte. With XMem framing
            // that byte belongs to the current frame's chunk, so consume it now.
            if (blockType_ == kUncompressed && blockRemaining_ == 0 && (blockLength_ & 1) && br_.p < br_.end) {
                br_.rawByte();
                padPending_ = false;
            }
        }
    }

    Bytes finish(size_t expected) {
        if (out_.size() > expected) out_.resize(expected);
        if (intelStarted_ && intelSize_) e8Translate();
        return std::move(out_);
    }
    size_t produced() const { return out_.size(); }

private:
    void readLengths(uint8_t* lens, int first, int last) {
        uint8_t preLens[kPretreeSyms];
        for (auto& l : preLens) l = uint8_t(br_.read(4));
        Huffman pre;
        pre.build(preLens, kPretreeSyms);
        for (int x = first; x < last;) {
            int z = pre.decode(br_);
            if (z == 17) {
                int y = int(br_.read(4)) + 4;
                while (y-- && x < last) lens[x++] = 0;
            } else if (z == 18) {
                int y = int(br_.read(5)) + 20;
                while (y-- && x < last) lens[x++] = 0;
            } else if (z == 19) {
                int y = int(br_.read(1)) + 4;
                z = pre.decode(br_);
                if (z > 16) fail("LZX: bad pretree run");
                int v = lens[x] - z;
                if (v < 0) v += 17;
                while (y-- && x < last) lens[x++] = uint8_t(v);
            } else {
                int v = lens[x] - z;
                if (v < 0) v += 17;
                lens[x++] = uint8_t(v);
            }
        }
    }

    void readBlockHeader() {
        if (blockType_ == kUncompressed && (blockLength_ & 1) && padPending_) br_.rawByte();
        blockType_ = int(br_.read(3));
        uint32_t hi = br_.read(16), lo = br_.read(8);
        blockLength_ = blockRemaining_ = hi << 8 | lo;
        switch (blockType_) {
            case kAligned:
                for (int i = 0; i < kAlignedSyms; ++i) alignedLens_[i] = uint8_t(br_.read(3));
                aligned_.build(alignedLens_.data(), kAlignedSyms);
                [[fallthrough]];
            case kVerbatim:
                readLengths(mainLens_.data(), 0, kNumChars);
                readLengths(mainLens_.data(), kNumChars, mainSyms_);
                main_.build(mainLens_.data(), mainSyms_);
                if (mainLens_[0xE8]) intelStarted_ = true;
                readLengths(lengthLens_.data(), 0, kLengthSyms);
                length_.build(lengthLens_.data(), kLengthSyms);
                break;
            case kUncompressed:
                intelStarted_ = true;
                padPending_ = true;
                br_.alignForRaw();
                for (auto* r : {&r0_, &r1_, &r2_}) {
                    uint32_t v = 0;
                    for (int k = 0; k < 4; ++k) v |= uint32_t(br_.rawByte()) << (8 * k);
                    *r = v;
                }
                break;
            default:
                fail("LZX: invalid block type " + std::to_string(blockType_));
        }
        if (blockLength_ == 0) fail("LZX: zero-length block");
    }

    size_t decodeSymbols(size_t run) {
        size_t produced = 0;
        const bool aligned = blockType_ == kAligned;
        while (produced < run) {
            int sym = main_.decode(br_);
            if (sym < kNumChars) {
                out_.push_back(uint8_t(sym));
                ++produced;
                continue;
            }
            sym -= kNumChars;
            uint32_t matchLen = uint32_t(sym & 7);
            if (matchLen == 7) matchLen += uint32_t(length_.decode(br_));
            matchLen += 2;
            uint32_t slot = uint32_t(sym >> 3);
            uint32_t offset;
            if (slot > 2) {
                int extra = kTables.extraBits[slot];
                offset = kTables.positionBase[slot] - 2;
                if (aligned) {
                    if (extra > 3) {
                        offset += br_.read(extra - 3) << 3;
                        offset += uint32_t(aligned_.decode(br_));
                    } else if (extra == 3) {
                        offset += uint32_t(aligned_.decode(br_));
                    } else if (extra > 0) {
                        offset += br_.read(extra);
                    }
                } else {
                    offset += br_.read(extra);
                }
                r2_ = r1_; r1_ = r0_; r0_ = offset;
            } else if (slot == 0) {
                offset = r0_;
            } else if (slot == 1) {
                offset = r1_; r1_ = r0_; r0_ = offset;
            } else {
                offset = r2_; r2_ = r0_; r0_ = offset;
            }
            if (offset == 0 || offset > out_.size() || offset > windowSize_) fail("LZX: match offset out of range");
            size_t from = out_.size() - offset;
            for (uint32_t k = 0; k < matchLen; ++k) out_.push_back(out_[from + k]);
            produced += matchLen;
        }
        return produced;
    }

    // Undo the x86 CALL translation applied per 32 KiB frame by the compressor.
    void e8Translate() {
        for (size_t fs = 0, idx = 0; fs < out_.size() && idx < 32768; fs += kFrameSize, ++idx) {
            size_t n = std::min(kFrameSize, out_.size() - fs);
            if (n <= 10) continue;
            uint8_t* d = out_.data() + fs;
            int32_t curPos = int32_t(fs);
            for (size_t i = 0; i + 10 <= n;) {
                if (d[i] != 0xE8) { ++i; ++curPos; continue; }
                int32_t abs = int32_t(le32(d + i + 1));
                if (abs >= -curPos && abs < intelSize_) {
                    int32_t rel = abs >= 0 ? abs - curPos : abs + intelSize_;
                    putLe32(d + i + 1, uint32_t(rel));
                }
                i += 5;
                curPos += 5;
            }
        }
    }

    size_t windowSize_;
    int mainSyms_;
    Bytes out_;
    BitReader br_;
    bool headerRead_ = false;
    bool intelStarted_ = false;
    bool padPending_ = false;
    int32_t intelSize_ = 0;
    int blockType_ = 0;
    uint32_t blockLength_ = 0;
    uint32_t blockRemaining_ = 0;
    uint32_t r0_ = 1, r1_ = 1, r2_ = 1;
    std::array<uint8_t, kMaxMainSyms> mainLens_{};
    std::array<uint8_t, kLengthSyms> lengthLens_{};
    std::array<uint8_t, kAlignedSyms> alignedLens_{};
    Huffman main_, length_, aligned_;
};

} // namespace

Bytes decompress(const uint8_t* src, size_t srcSize, size_t expectedSize, int windowBits) {
    Decoder dec(windowBits, expectedSize);
    size_t p = 0, frameStart = 0;
    while (p < srcSize && dec.produced() < expectedSize) {
        size_t usize = kFrameSize, csize;
        if (src[p] == 0xFF) {
            if (srcSize - p < 5) break;
            usize = be16(src + p + 1);
            csize = be16(src + p + 3);
            p += 5;
        } else {
            if (srcSize - p < 2) break;
            csize = be16(src + p);
            p += 2;
        }
        if (csize == 0) break;
        if (csize > srcSize - p) fail("LZX: frame exceeds input");
        size_t frameEnd = std::min(frameStart + usize, expectedSize);
        dec.frame(src + p, csize, frameEnd);
        p += csize;
        frameStart += usize;
    }
    if (dec.produced() < expectedSize)
        fail("LZX: stream ended early (" + std::to_string(dec.produced()) + " of " + std::to_string(expectedSize) + " bytes)");
    return dec.finish(expectedSize);
}

} // namespace nut::lzx
