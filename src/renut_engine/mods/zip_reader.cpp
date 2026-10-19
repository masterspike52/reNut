#include "renut_engine/mods/zip_reader.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace renut::mods {

namespace {

uint16_t Le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// ---------------------------------------------------------------- inflate

constexpr int kMaxBits = 15;
constexpr int kFastBits = 9;

// Canonical Huffman code: counts per length, symbols in code order, and a
// table for codes of up to kFastBits bits (entry = symbol << 4 | length).
struct Huffman {
    uint16_t count[kMaxBits + 1];
    uint16_t symbol[320];
    uint16_t fast[1 << kFastBits];

    // False when the lengths are over-subscribed; incomplete codes are fine
    // (a distance code may have a single symbol).
    bool Build(const uint8_t *lengths, int n) {
        std::memset(count, 0, sizeof(count));
        for (int s = 0; s < n; ++s)
            ++count[lengths[s]];
        count[0] = 0;
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left = (left << 1) - count[len];
            if (left < 0)
                return false;
        }
        uint16_t offs[kMaxBits + 2] = {};
        for (int len = 1; len < kMaxBits; ++len)
            offs[len + 1] = uint16_t(offs[len] + count[len]);
        for (int s = 0; s < n; ++s)
            if (lengths[s])
                symbol[offs[lengths[s]]++] = uint16_t(s);

        std::memset(fast, 0, sizeof(fast));
        int code = 0, index = 0;
        for (int len = 1; len <= kFastBits; ++len) {
            for (int k = 0; k < count[len]; ++k, ++code, ++index) {
                // Deflate sends codes most significant bit first into an
                // LSB-first stream: the table is indexed by the reversed code.
                int reversed = 0;
                for (int b = 0; b < len; ++b)
                    reversed |= ((code >> b) & 1) << (len - 1 - b);
                for (int t = reversed; t < (1 << kFastBits); t += 1 << len)
                    fast[t] = uint16_t(symbol[index] << 4 | len);
            }
            code <<= 1;
        }
        return true;
    }
};

class Inflater {
public:
    Inflater(const uint8_t *in, size_t size, std::vector<uint8_t> &out) : in_(in), end_(in + size), out_(out) {}

    bool Run(size_t expected_size) {
        out_.clear();
        out_.reserve(expected_size);
        limit_ = expected_size;
        bool last = false;
        while (!last) {
            uint32_t header;
            if (!Bits(3, header))
                return false;
            last = header & 1;
            bool ok = false;
            switch (header >> 1) {
            case 0: ok = Stored(); break;
            case 1: ok = Fixed(); break;
            case 2: ok = Dynamic(); break;
            default: return false;
            }
            if (!ok || out_.size() > expected_size)
                return false;
        }
        return out_.size() == expected_size;
    }

private:
    void Fill() {
        while (bit_count_ <= 56 && in_ < end_) {
            bit_buf_ |= uint64_t(*in_++) << bit_count_;
            bit_count_ += 8;
        }
    }

    bool Bits(int n, uint32_t &value) {
        if (bit_count_ < n)
            Fill();
        if (bit_count_ < n)
            return false;
        value = uint32_t(bit_buf_ & ((uint64_t(1) << n) - 1));
        bit_buf_ >>= n;
        bit_count_ -= n;
        return true;
    }

    int Decode(const Huffman &h) {
        if (bit_count_ < kMaxBits)
            Fill();
        uint16_t e = h.fast[bit_buf_ & ((1u << kFastBits) - 1)];
        if (e && (e & 15) <= bit_count_) {
            bit_buf_ >>= (e & 15);
            bit_count_ -= (e & 15);
            return e >> 4;
        }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits && len <= bit_count_; ++len) {
            code |= int((bit_buf_ >> (len - 1)) & 1);
            int count = h.count[len];
            if (code - count < first) {
                bit_buf_ >>= len;
                bit_count_ -= len;
                return h.symbol[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    bool Stored() {
        uint32_t skip, len, nlen;
        if (!Bits(bit_count_ & 7, skip) || !Bits(16, len) || !Bits(16, nlen) || len != (~nlen & 0xFFFF))
            return false;
        // Whole bytes still in the bit buffer come first.
        while (len && bit_count_ >= 8) {
            out_.push_back(uint8_t(bit_buf_));
            bit_buf_ >>= 8;
            bit_count_ -= 8;
            --len;
        }
        if (size_t(end_ - in_) < len || out_.size() + len > limit_)
            return false;
        out_.insert(out_.end(), in_, in_ + len);
        in_ += len;
        return true;
    }

    bool Codes(const Huffman &lengths, const Huffman &distances) {
        static constexpr uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                                  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                                  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                                   193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                                   6145, 8193, 12289, 16385, 24577};
        static constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                                   7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = Decode(lengths);
            if (sym < 0)
                return false;
            if (sym < 256) {
                if (out_.size() >= limit_)
                    return false;
                out_.push_back(uint8_t(sym));
                continue;
            }
            if (sym == 256)
                return true;
            sym -= 257;
            if (sym >= 29)
                return false;
            uint32_t extra;
            if (!Bits(kLenExtra[sym], extra))
                return false;
            size_t len = kLenBase[sym] + extra;
            int dsym = Decode(distances);
            if (dsym < 0 || dsym >= 30 || !Bits(kDistExtra[dsym], extra))
                return false;
            size_t dist = kDistBase[dsym] + extra;
            if (dist > out_.size())
                return false;
            if (out_.size() + len > limit_)
                return false;
            size_t from = out_.size() - dist;
            for (size_t i = 0; i < len; ++i)
                out_.push_back(out_[from + i]);
        }
    }

    bool Fixed() {
        static const std::array<Huffman, 2> kTrees = [] {
            std::array<Huffman, 2> trees;
            uint8_t lengths[288];
            int s = 0;
            for (; s < 144; ++s) lengths[s] = 8;
            for (; s < 256; ++s) lengths[s] = 9;
            for (; s < 280; ++s) lengths[s] = 7;
            for (; s < 288; ++s) lengths[s] = 8;
            trees[0].Build(lengths, 288);
            std::memset(lengths, 5, 30);
            trees[1].Build(lengths, 30);
            return trees;
        }();
        return Codes(kTrees[0], kTrees[1]);
    }

    bool Dynamic() {
        static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        uint32_t nlen, ndist, ncode;
        if (!Bits(5, nlen) || !Bits(5, ndist) || !Bits(4, ncode))
            return false;
        nlen += 257;
        ndist += 1;
        ncode += 4;
        if (nlen > 286 || ndist > 30)
            return false;
        uint8_t lengths[320] = {};
        for (uint32_t i = 0; i < ncode; ++i) {
            uint32_t v;
            if (!Bits(3, v))
                return false;
            lengths[kOrder[i]] = uint8_t(v);
        }
        Huffman code_tree;
        if (!code_tree.Build(lengths, 19))
            return false;
        std::memset(lengths, 0, sizeof(lengths));
        for (uint32_t i = 0; i < nlen + ndist;) {
            int sym = Decode(code_tree);
            if (sym < 0)
                return false;
            if (sym < 16) {
                lengths[i++] = uint8_t(sym);
                continue;
            }
            uint8_t value = 0;
            uint32_t repeat;
            if (sym == 16) {
                if (i == 0 || !Bits(2, repeat))
                    return false;
                value = lengths[i - 1];
                repeat += 3;
            } else if (sym == 17) {
                if (!Bits(3, repeat))
                    return false;
                repeat += 3;
            } else {
                if (!Bits(7, repeat))
                    return false;
                repeat += 11;
            }
            if (i + repeat > nlen + ndist)
                return false;
            while (repeat--)
                lengths[i++] = value;
        }
        if (lengths[256] == 0)
            return false;
        Huffman length_tree, distance_tree;
        if (!length_tree.Build(lengths, int(nlen)) || !distance_tree.Build(lengths + nlen, int(ndist)))
            return false;
        return Codes(length_tree, distance_tree);
    }

    const uint8_t *in_;
    const uint8_t *end_;
    std::vector<uint8_t> &out_;
    size_t limit_ = 0;
    uint64_t bit_buf_ = 0;
    int bit_count_ = 0;
};

} // namespace

bool Inflate(const uint8_t *in, size_t in_size, std::vector<uint8_t> &out, size_t expected_size) {
    return Inflater(in, in_size, out).Run(expected_size);
}

uint32_t Crc32(const uint8_t *data, size_t size) {
    static const std::array<uint32_t, 256> kTable = [] {
        std::array<uint32_t, 256> table;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return table;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
        crc = kTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------- zip

bool ZipReader::Open(const std::filesystem::path &path, std::string *error) {
    entries_.clear();
    file_.open(path, std::ios::binary);
    if (!file_) {
        *error = "cannot open the file";
        return false;
    }
    file_.seekg(0, std::ios::end);
    const uint64_t file_size = uint64_t(file_.tellg());
    // The end-of-central-directory record is in the last 22 + 65535 bytes.
    const uint64_t tail_size = std::min<uint64_t>(file_size, 22 + 0xFFFF);
    std::vector<uint8_t> tail(tail_size);
    file_.seekg(std::streamoff(file_size - tail_size));
    file_.read(reinterpret_cast<char *>(tail.data()), std::streamsize(tail_size));
    if (!file_ || tail_size < 22) {
        *error = "not a zip file";
        return false;
    }
    const uint8_t *eocd = nullptr;
    for (size_t i = tail_size - 22 + 1; i-- > 0;) {
        if (Le32(&tail[i]) == 0x06054b50) {
            eocd = &tail[i];
            break;
        }
    }
    if (!eocd) {
        *error = "not a zip file (no end of central directory)";
        return false;
    }
    const uint16_t count = Le16(eocd + 10);
    const uint32_t cd_size = Le32(eocd + 12);
    const uint32_t cd_offset = Le32(eocd + 16);
    if (count == 0xFFFF || cd_offset == 0xFFFFFFFFu || uint64_t(cd_offset) + cd_size > file_size) {
        *error = "zip64 or damaged central directory";
        return false;
    }
    std::vector<uint8_t> cd(cd_size);
    file_.seekg(cd_offset);
    file_.read(reinterpret_cast<char *>(cd.data()), std::streamsize(cd_size));
    if (!file_) {
        *error = "cannot read the central directory";
        return false;
    }
    size_t p = 0;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > cd.size() || Le32(&cd[p]) != 0x02014b50) {
            *error = "damaged central directory";
            return false;
        }
        const uint16_t name_len = Le16(&cd[p + 28]);
        const uint16_t extra_len = Le16(&cd[p + 30]);
        const uint16_t comment_len = Le16(&cd[p + 32]);
        if (p + 46 + name_len > cd.size()) {
            *error = "damaged central directory";
            return false;
        }
        EntryInfo e;
        e.method = Le16(&cd[p + 10]);
        e.crc32 = Le32(&cd[p + 16]);
        e.compressed_size = Le32(&cd[p + 20]);
        e.size = Le32(&cd[p + 24]);
        e.local_header_offset = Le32(&cd[p + 42]);
        e.name.assign(reinterpret_cast<const char *>(&cd[p + 46]), name_len);
        // Some Windows zip tools write '\' between folders; names are '/' here.
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        entries_.push_back(std::move(e));
        p += 46 + size_t(name_len) + extra_len + comment_len;
    }
    return true;
}

const ZipReader::EntryInfo *ZipReader::Find(std::string_view name) const {
    std::string wanted(name);
    std::replace(wanted.begin(), wanted.end(), '\\', '/');
    for (const EntryInfo &e : entries_)
        if (e.name == wanted)
            return &e;
    return nullptr;
}

std::optional<std::vector<uint8_t>> ZipReader::Read(std::string_view name, std::string *error) {
    const EntryInfo *e = Find(name);
    if (!e) {
        *error = std::string(name) + " is missing from the package";
        return std::nullopt;
    }
    uint8_t local[30];
    file_.clear();
    file_.seekg(std::streamoff(e->local_header_offset));
    file_.read(reinterpret_cast<char *>(local), sizeof(local));
    if (!file_ || Le32(local) != 0x04034b50) {
        *error = std::string(name) + ": damaged local header";
        return std::nullopt;
    }
    file_.seekg(std::streamoff(e->local_header_offset + 30 + Le16(local + 26) + Le16(local + 28)));
    std::vector<uint8_t> packed(e->compressed_size);
    file_.read(reinterpret_cast<char *>(packed.data()), std::streamsize(packed.size()));
    if (!file_) {
        *error = std::string(name) + ": truncated";
        return std::nullopt;
    }
    std::vector<uint8_t> data;
    if (e->method == 0) {
        data = std::move(packed);
    } else if (e->method == 8) {
        if (!Inflate(packed.data(), packed.size(), data, e->size)) {
            *error = std::string(name) + ": bad deflate data";
            return std::nullopt;
        }
    } else {
        *error = std::string(name) + ": unsupported compression method " + std::to_string(e->method);
        return std::nullopt;
    }
    if (data.size() != e->size || Crc32(data.data(), data.size()) != e->crc32) {
        *error = std::string(name) + ": CRC mismatch";
        return std::nullopt;
    }
    return data;
}

} // namespace renut::mods
