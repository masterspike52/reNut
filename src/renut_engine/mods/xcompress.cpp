#include "renut_engine/mods/xcompress.h"

#include <algorithm>
#include <cstring>

namespace renut::mods {

namespace {

uint16_t Be16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

constexpr uint32_t kXCompressMagic = 0x0FF512ED;
constexpr uint32_t kArchiveMagic = 0x438CB47C;

// LZX as Xbox 360 XMemCompress writes it (the structure of libmspack's lzxd):
// 16-bit little-endian words read MSB first, 32 KiB output frames, verbatim /
// aligned-offset / uncompressed blocks, delta-coded tree lengths and E8
// translation. Each frame is fed on its own, byte-aligned.
class LzxDecoder {
public:
    static constexpr int kFrameSize = 32768;

    explicit LzxDecoder(int window_bits)
        : window_size_(1 << window_bits),
          window_(size_t(1) << window_bits),
          main_elements_(kNumChars + (window_bits == 20 ? 42 : window_bits == 21 ? 50 : window_bits * 2) * 8),
          main_len_(size_t(main_elements_) + 16),
          main_(main_elements_, 12),
          length_(kNumSecondaryLengths + 16, 12),
          aligned_(kAlignedSyms, 7),
          pre_(kPretreeSyms, 6) {
        Reset();
    }

    void Reset() {
        r0_ = r1_ = r2_ = 1;
        header_read_ = false;
        block_remaining_ = block_type_ = block_length_ = 0;
        intel_started_ = false;
        intel_cur_pos_ = intel_file_size_ = 0;
        window_posn_ = frame_posn_ = 0;
        std::fill(main_len_.begin(), main_len_.end(), uint8_t(0));
        std::memset(length_len_, 0, sizeof(length_len_));
    }

    // Decodes one frame whose compressed bytes are exactly [in, in + in_len).
    bool DecodeFrame(const uint8_t *in, size_t in_len, uint8_t *out, int frame_size) {
        in_ = in;
        ip_ = 0;
        iend_ = in_len;
        bit_buf_ = 0;
        bits_left_ = 0;
        return DecodeNextFrame(out, frame_size);
    }

private:
    static constexpr int kNumChars = 256, kNumPrimaryLengths = 7, kMinMatch = 2, kNumSecondaryLengths = 249;
    static constexpr int kPretreeSyms = 20, kAlignedSyms = 8, kMaxBits = 16;
    static constexpr int kBlockVerbatim = 1, kBlockAligned = 2, kBlockUncompressed = 3;

    struct Tables {
        uint8_t extra_bits[51];
        uint32_t position_base[51];
        Tables() {
            int j = 0;
            for (int i = 0; i < 50; i += 2) {
                extra_bits[i] = extra_bits[i + 1] = uint8_t(j);
                if (i != 0 && j < 17)
                    ++j;
            }
            extra_bits[50] = 17;
            uint32_t b = 0;
            for (int i = 0; i < 51; ++i) {
                position_base[i] = b;
                b += 1u << extra_bits[i];
            }
        }
    };
    static const Tables &T() {
        static const Tables tables;
        return tables;
    }

    // Canonical Huffman decoder: a table for short codes, a canonical walk
    // for long ones.
    class Huffman {
    public:
        Huffman(int max_syms, int table_bits)
            : table_bits_(table_bits), table_(size_t(1) << table_bits), symbols_(size_t(max_syms)) {}

        // False when every length is zero (empty tree) or the lengths do not
        // form a code.
        bool Build(const uint8_t *lens, int n, bool *empty) {
            std::memset(count_, 0, sizeof(count_));
            for (int s = 0; s < n; ++s)
                ++count_[lens[s]];
            count_[0] = 0;
            uint16_t offs[kMaxBits + 2] = {};
            int total = 0;
            for (int l = 1; l <= kMaxBits; ++l) {
                offs[l] = uint16_t(total);
                total += count_[l];
            }
            std::fill(table_.begin(), table_.end(), uint16_t(0xFFFF));
            *empty = total == 0;
            if (total == 0)
                return false;
            total_ = total;
            for (int s = 0; s < n; ++s)
                if (lens[s] != 0)
                    symbols_[offs[lens[s]]++] = uint16_t(s);
            int code = 0, idx = 0;
            for (int l = 1; l <= kMaxBits; ++l) {
                for (int k = 0; k < count_[l]; ++k, ++code, ++idx) {
                    if (l <= table_bits_) {
                        size_t start = size_t(code) << (table_bits_ - l), cnt = size_t(1) << (table_bits_ - l);
                        if (start + cnt > table_.size())
                            return false;
                        uint16_t e = uint16_t(symbols_[idx] << 5 | l);
                        std::fill_n(table_.begin() + ptrdiff_t(start), cnt, e);
                    }
                }
                code <<= 1;
            }
            return true;
        }

        int Decode(uint32_t bit_buf, int *len) const {
            uint16_t e = table_[bit_buf >> (32 - table_bits_)];
            if (e != 0xFFFF) {
                *len = e & 31;
                return e >> 5;
            }
            int code = 0, first = 0, index = 0;
            for (int l = 1; l <= kMaxBits; ++l) {
                code |= int((bit_buf >> (32 - l)) & 1);
                int count = count_[l];
                if (code - count < first) {
                    int at = index + (code - first);
                    if (at < 0 || at >= total_)
                        return -1;
                    *len = l;
                    return symbols_[size_t(at)];
                }
                index += count;
                first += count;
                first <<= 1;
                code <<= 1;
            }
            return -1;
        }

    private:
        int table_bits_;
        int total_ = 0;
        std::vector<uint16_t> table_;
        uint16_t count_[kMaxBits + 1] = {};
        std::vector<uint16_t> symbols_;
    };

    // ---- bit reading (past the frame's end reads zeros, as XMem expects)
    void Ensure(int n) {
        while (bits_left_ < n) {
            uint32_t b0 = ip_ < iend_ ? in_[ip_] : 0;
            uint32_t b1 = ip_ + 1 < iend_ ? in_[ip_ + 1] : 0;
            ip_ += 2;
            bit_buf_ |= ((b1 << 8) | b0) << (16 - bits_left_);
            bits_left_ += 16;
        }
    }
    uint32_t Peek(int n) const { return bit_buf_ >> (32 - n); }
    void Remove(int n) {
        bit_buf_ <<= n;
        bits_left_ -= n;
    }
    uint32_t Bits(int n) {
        if (n == 0)
            return 0;
        if (n > 16) {
            uint32_t hi = Bits(n - 16);
            return (hi << 16) | Bits(16);
        }
        Ensure(n);
        uint32_t v = Peek(n);
        Remove(n);
        return v;
    }
    int Sym(const Huffman &h) {
        Ensure(kMaxBits);
        int len = 0;
        int s = h.Decode(bit_buf_, &len);
        if (s >= 0)
            Remove(len);
        return s;
    }

    bool ReadLens(uint8_t *lens, int first, int last) {
        for (int x = 0; x < kPretreeSyms; ++x)
            pre_len_[x] = uint8_t(Bits(4));
        bool empty;
        if (!pre_.Build(pre_len_, kPretreeSyms, &empty))
            return false;
        for (int x = first; x < last;) {
            int z = Sym(pre_);
            if (z < 0)
                return false;
            if (z == 17) {
                int y = int(Bits(4)) + 4;
                while (y-- > 0 && x < last)
                    lens[x++] = 0;
            } else if (z == 18) {
                int y = int(Bits(5)) + 20;
                while (y-- > 0 && x < last)
                    lens[x++] = 0;
            } else if (z == 19) {
                int y = int(Bits(1)) + 4;
                z = Sym(pre_);
                if (z < 0)
                    return false;
                z = lens[x] - z;
                if (z < 0)
                    z += 17;
                while (y-- > 0 && x < last)
                    lens[x++] = uint8_t(z);
            } else {
                z = lens[x] - z;
                if (z < 0)
                    z += 17;
                lens[x++] = uint8_t(z);
            }
        }
        return true;
    }

    uint32_t ReadLe32() {
        uint32_t v = uint32_t(in_[ip_]) | uint32_t(in_[ip_ + 1]) << 8 | uint32_t(in_[ip_ + 2]) << 16 | uint32_t(in_[ip_ + 3]) << 24;
        ip_ += 4;
        return v;
    }

    bool DecodeNextFrame(uint8_t *output, int frame_size) {
        if (frame_size > kFrameSize || frame_size <= 0)
            return false;

        if (!header_read_) {
            uint32_t i = Bits(1), j = 0;
            if (i != 0) {
                i = Bits(16);
                j = Bits(16);
            }
            intel_file_size_ = int32_t((i << 16) | j);
            header_read_ = true;
        }

        const Tables &tables = T();
        uint8_t *window = window_.data();
        int wpos = window_posn_;
        int bytes_todo = frame_posn_ + frame_size - wpos;
        while (bytes_todo > 0) {
            if (block_remaining_ == 0) {
                block_type_ = int(Bits(3));
                uint32_t hi = Bits(16), lo = Bits(8);
                block_remaining_ = block_length_ = int((hi << 8) | lo);
                bool empty;
                switch (block_type_) {
                case kBlockAligned:
                    for (int k = 0; k < 8; ++k)
                        aligned_len_[k] = uint8_t(Bits(3));
                    if (!aligned_.Build(aligned_len_, kAlignedSyms, &empty))
                        return false;
                    [[fallthrough]];
                case kBlockVerbatim:
                    if (!ReadLens(main_len_.data(), 0, 256) || !ReadLens(main_len_.data(), 256, main_elements_))
                        return false;
                    if (!main_.Build(main_len_.data(), main_elements_, &empty))
                        return false;
                    if (main_len_[0xE8] != 0)
                        intel_started_ = true;
                    if (!ReadLens(length_len_, 0, kNumSecondaryLengths))
                        return false;
                    length_empty_ = !length_.Build(length_len_, kNumSecondaryLengths, &empty);
                    if (length_empty_ && !empty)
                        return false;
                    break;
                case kBlockUncompressed:
                    intel_started_ = true;
                    if (bits_left_ == 0)
                        Ensure(16);
                    // Bits still buffered belong to words already consumed.
                    if (bits_left_ > 16)
                        ip_ -= 2;
                    bits_left_ = 0;
                    bit_buf_ = 0;
                    if (ip_ + 12 > iend_)
                        return false;
                    r0_ = ReadLe32();
                    r1_ = ReadLe32();
                    r2_ = ReadLe32();
                    break;
                default:
                    return false;
                }
            }

            int this_run = std::min(block_remaining_, bytes_todo);
            bytes_todo -= this_run;
            block_remaining_ -= this_run;

            if (block_type_ == kBlockUncompressed) {
                if (ip_ + size_t(this_run) > iend_ || wpos + this_run > window_size_)
                    return false;
                std::memcpy(window + wpos, in_ + ip_, size_t(this_run));
                ip_ += size_t(this_run);
                wpos += this_run;
                this_run = 0;
                // Odd-length stored blocks are padded to 16 bits; with XMem
                // framing the pad byte belongs to the frame the block ends in.
                if (block_remaining_ == 0 && (block_length_ & 1) != 0)
                    ++ip_;
            } else {
                const bool aligned = block_type_ == kBlockAligned;
                while (this_run > 0) {
                    int main_el = Sym(main_);
                    if (main_el < 0)
                        return false;
                    if (main_el < kNumChars) {
                        if (wpos >= window_size_)
                            return false;
                        window[wpos++] = uint8_t(main_el);
                        --this_run;
                        continue;
                    }
                    main_el -= kNumChars;
                    int match_len = main_el & kNumPrimaryLengths;
                    if (match_len == kNumPrimaryLengths) {
                        if (length_empty_)
                            return false;
                        int extra_len = Sym(length_);
                        if (extra_len < 0)
                            return false;
                        match_len += extra_len;
                    }
                    match_len += kMinMatch;
                    uint32_t match_off;
                    int slot = main_el >> 3;
                    switch (slot) {
                    case 0:
                        match_off = r0_;
                        break;
                    case 1:
                        match_off = r1_;
                        r1_ = r0_;
                        r0_ = match_off;
                        break;
                    case 2:
                        match_off = r2_;
                        r2_ = r0_;
                        r0_ = match_off;
                        break;
                    default: {
                        int extra = slot >= 36 ? 17 : tables.extra_bits[slot];
                        if (!aligned) {
                            match_off = tables.position_base[slot] - 2 + Bits(extra);
                        } else {
                            match_off = tables.position_base[slot] - 2;
                            if (extra > 3) {
                                match_off += Bits(extra - 3) << 3;
                                int a = Sym(aligned_);
                                if (a < 0)
                                    return false;
                                match_off += uint32_t(a);
                            } else if (extra == 3) {
                                int a = Sym(aligned_);
                                if (a < 0)
                                    return false;
                                match_off += uint32_t(a);
                            } else if (extra > 0) {
                                match_off += Bits(extra);
                            } else {
                                match_off = 1;
                            }
                        }
                        r2_ = r1_;
                        r1_ = r0_;
                        r0_ = match_off;
                        break;
                    }
                    }
                    if (wpos + match_len > window_size_ || match_off > uint32_t(window_size_))
                        return false;
                    int off = int(match_off);
                    if (off > wpos) {
                        int j = off - wpos;
                        int src = window_size_ - j, n = match_len;
                        if (j < n) {
                            n -= j;
                            while (j-- > 0)
                                window[wpos++] = window[src++];
                            src = 0;
                        }
                        while (n-- > 0)
                            window[wpos++] = window[src++];
                    } else {
                        int src = wpos - off;
                        for (int n = 0; n < match_len; ++n)
                            window[wpos + n] = window[src + n];
                        wpos += match_len;
                    }
                    this_run -= match_len;
                }
            }
            if (this_run < 0) {
                if (-this_run > block_remaining_)
                    return false;
                block_remaining_ -= -this_run;
            }
        }
        if (wpos - frame_posn_ != frame_size)
            return false;

        std::memcpy(output, window + frame_posn_, size_t(frame_size));
        if (intel_started_ && intel_file_size_ != 0 && frame_size > 10)
            E8Translate(output, frame_size);
        intel_cur_pos_ += frame_size;

        frame_posn_ += frame_size;
        if (wpos == window_size_)
            wpos = 0;
        if (frame_posn_ == window_size_)
            frame_posn_ = 0;
        window_posn_ = wpos;
        return true;
    }

    void E8Translate(uint8_t *d, int size) const {
        int32_t curpos = intel_cur_pos_;
        int end = size - 10, i = 0;
        while (i < end) {
            if (d[i++] != 0xE8) {
                ++curpos;
                continue;
            }
            int32_t abs = int32_t(uint32_t(d[i]) | uint32_t(d[i + 1]) << 8 | uint32_t(d[i + 2]) << 16 | uint32_t(d[i + 3]) << 24);
            if (abs >= -curpos && abs < intel_file_size_) {
                int32_t rel = abs >= 0 ? abs - curpos : abs + intel_file_size_;
                d[i] = uint8_t(rel);
                d[i + 1] = uint8_t(rel >> 8);
                d[i + 2] = uint8_t(rel >> 16);
                d[i + 3] = uint8_t(rel >> 24);
            }
            i += 4;
            curpos += 5;
        }
    }

    int window_size_;
    std::vector<uint8_t> window_;
    int main_elements_;
    int window_posn_ = 0, frame_posn_ = 0;
    uint32_t r0_ = 1, r1_ = 1, r2_ = 1;
    int block_type_ = 0, block_length_ = 0, block_remaining_ = 0;
    bool header_read_ = false, intel_started_ = false;
    int32_t intel_file_size_ = 0, intel_cur_pos_ = 0;

    std::vector<uint8_t> main_len_;
    uint8_t length_len_[kNumSecondaryLengths + 16] = {};
    uint8_t aligned_len_[kAlignedSyms] = {};
    uint8_t pre_len_[kPretreeSyms] = {};
    Huffman main_, length_, aligned_, pre_;
    bool length_empty_ = false;

    const uint8_t *in_ = nullptr;
    size_t ip_ = 0, iend_ = 0;
    uint32_t bit_buf_ = 0;
    int bits_left_ = 0;
};

} // namespace

bool IsXCompressed(std::span<const uint8_t> data) {
    return data.size() >= 16 && Be32(data.data()) == kXCompressMagic;
}

// Layout (big-endian):
//   0x00 u32 identifier 0x0FF512ED, 0x04 u16 version, 0x06 u16 reserved,
//   0x08 u32 hash, 0x0C u32 flags: bits 0-3 window = 1 << (n + 15),
//   bits 4-5 segment size = 0x8000 << n, bits 6-21 segment count,
//   bits 22-23 size-table entries 0 = 20-bit, 1 = 32-bit.
//   0x10 bit-packed (MSB first) per-segment uncompressed sizes, padded to u32.
// Segment i starts at i * segment size (segment 0 after the table). Each is
// an independent LZX stream of XMem frames: u16 compressed size (32 KiB of
// output), or 0xFF, u16 uncompressed size, u16 compressed size. A segment
// that starts with a zero u16 is stored.
std::optional<std::vector<uint8_t>> XDecompress(std::span<const uint8_t> data, std::string *error) {
    auto fail = [&](const char *what) {
        *error = std::string("xcompress: ") + what;
        return std::nullopt;
    };
    if (!IsXCompressed(data))
        return fail("not an xcompress file");
    const uint8_t *file = data.data();
    const size_t file_size = data.size();
    const uint32_t flags = Be32(file + 12);
    const int segments = int((flags >> 6) & 0xFFFF);
    const uint32_t width = (flags >> 22) & 3;
    if (width > 1)
        return fail("unknown size-table width");
    const int size_bits = width == 0 ? 20 : 32;
    const size_t segment_size = size_t(0x8000) << ((flags >> 4) & 3);
    const int window_bits = int(flags & 0xF) + 15;
    if (window_bits > 21)
        return fail("unsupported window size");
    const size_t table_end = 16 + size_t((size_bits * segments + 31) >> 5) * 4;
    if (table_end > file_size)
        return fail("truncated size table");

    std::vector<uint64_t> sizes(static_cast<size_t>(segments));
    uint64_t total = 0;
    size_t bit_pos = 16 * 8;
    for (int i = 0; i < segments; ++i) {
        uint64_t v = 0;
        for (int b = 0; b < size_bits; ++b, ++bit_pos)
            v = (v << 1) | ((file[bit_pos >> 3] >> (7 - (bit_pos & 7))) & 1);
        sizes[size_t(i)] = v;
        total += v;
    }
    if (total > 0x7FFFFFFFull - 64)
        return fail("output too large");

    std::vector<uint8_t> output(total);
    LzxDecoder lzx(window_bits);
    size_t out_pos = 0;
    for (int s = 0; s < segments; ++s) {
        size_t p = s == 0 ? table_end : size_t(s) * segment_size;
        const size_t seg_end = std::min(file_size, size_t(s + 1) * segment_size);
        lzx.Reset();
        uint64_t remaining = sizes[size_t(s)];
        if (p + 2 > file_size)
            return fail("segment truncated");
        if (Be16(file + p) == 0) {
            p += 2;
            if (p + remaining > seg_end)
                return fail("stored segment truncated");
            std::memcpy(output.data() + out_pos, file + p, size_t(remaining));
            out_pos += size_t(remaining);
            continue;
        }
        while (remaining > 0) {
            if (p >= seg_end)
                return fail("segment truncated");
            size_t usize = LzxDecoder::kFrameSize, csize;
            if (file[p] == 0xFF) {
                if (p + 5 > seg_end)
                    return fail("frame header truncated");
                usize = Be16(file + p + 1);
                csize = Be16(file + p + 3);
                p += 5;
            } else {
                if (p + 2 > seg_end)
                    return fail("frame header truncated");
                csize = Be16(file + p);
                p += 2;
            }
            if (usize > remaining)
                usize = size_t(remaining);
            if (p + csize > seg_end)
                return fail("frame overruns its segment");
            if (!lzx.DecodeFrame(file + p, csize, output.data() + out_pos, int(usize))) {
                *error = "xcompress: bad LZX data in segment " + std::to_string(s) + " at file offset " + std::to_string(p);
                return std::nullopt;
            }
            p += csize;
            out_pos += usize;
            remaining -= usize;
        }
    }
    return output;
}

// Streaming archive (Bundle/50, magic 0x438CB47C): u32 magic, u32 entry size
// (12), u32 entry count, u32 timestamp, u32 dependency count, u32
// dependencies[], then entries {u32 asset id, u32 offset, u32 size}.
std::optional<std::vector<uint8_t>> ExpandGameFile(std::vector<uint8_t> raw, std::string *error) {
    if (IsXCompressed(raw))
        return XDecompress(raw, error);
    if (raw.size() < 20 || Be32(raw.data()) != kArchiveMagic || Be32(raw.data() + 4) != 12)
        return raw;

    const int32_t count = int32_t(Be32(raw.data() + 8));
    const int32_t deps = int32_t(Be32(raw.data() + 16));
    if (count < 0 || deps < 0 || 20 + (uint64_t(deps) + uint64_t(count) * 3) * 4 > raw.size()) {
        *error = "streaming archive: damaged entry table";
        return std::nullopt;
    }
    // Every entry is checked before any is decompressed, as NB Studio reads
    // the whole table first.
    std::vector<std::span<const uint8_t>> entries;
    size_t o = 20 + size_t(deps) * 4;
    for (int32_t i = 0; i < count; ++i, o += 12) {
        const int32_t off = int32_t(Be32(raw.data() + o + 4));
        const int32_t size = int32_t(Be32(raw.data() + o + 8));
        if (size <= 0 && off <= 0)
            continue;
        if (off < 0 || size < 0 || uint64_t(off) + uint64_t(size) > raw.size()) {
            *error = "streaming archive: entry out of range";
            return std::nullopt;
        }
        entries.emplace_back(raw.data() + off, size_t(size));
    }
    std::vector<std::vector<uint8_t>> extra;
    for (std::span<const uint8_t> entry : entries) {
        if (!IsXCompressed(entry))
            continue;
        // NB Studio stops collecting at the first entry it cannot decompress
        // and keeps the ones before it; the delta source must match that.
        std::string ignored;
        auto data = XDecompress(entry, &ignored);
        if (!data)
            break;
        extra.push_back(std::move(*data));
    }
    if (extra.empty())
        return raw;
    for (const auto &x : extra)
        raw.insert(raw.end(), x.begin(), x.end());
    return raw;
}

} // namespace renut::mods
