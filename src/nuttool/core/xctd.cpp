#include "xctd.hpp"

#include "lzx.hpp"

namespace nut::xctd {

bool isXctd(const Bytes& data) { return data.size() >= 16 && be32(data.data()) == kMagic; }

Info parse(const Bytes& data) {
    if (!isXctd(data)) fail("not an XCTD compressed file");
    Info in;
    in.version = be32(&data[4]);
    in.hash = be32(&data[8]);
    in.flags = be32(&data[12]);
    in.blockSize = 0x8000u << ((in.flags >> 4) & 3);
    uint32_t count = (in.flags >> 6) & 0xFFFF;
    uint32_t sel = (in.flags >> 22) & 3;
    if (sel > 1) fail("XCTD: unknown table width selector");
    uint32_t width = sel ? 32 : 20;
    uint32_t tableWords = (width * count + 31) / 32;
    in.headerSize = 16 + tableWords * 4;
    if (data.size() < in.headerSize) fail("XCTD: truncated header");
    if (count == 0) {
        in.stored = true;
        in.uncompressedSize = data.size() - 16;
        return in;
    }
    const uint8_t* t = &data[16];
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t bit = uint64_t(width) * i;
        uint64_t word = uint64_t(be32(t + (bit >> 5) * 4)) << 32;
        if ((bit >> 5) + 1 < tableWords) word |= be32(t + ((bit >> 5) + 1) * 4);
        uint32_t v = uint32_t((word << (bit & 31)) >> (64 - width));
        in.blockSizes.push_back(v);
        in.uncompressedSize += v;
    }
    return in;
}

Bytes decompress(const Bytes& data) { return decompressPrefix(data, SIZE_MAX); }

size_t compressedBytesFor(const Info& in, size_t want, size_t fileSize) {
    if (in.stored) return std::min(fileSize, want + 16);
    size_t produced = 0;
    for (size_t i = 0; i < in.blockSizes.size(); ++i) {
        produced += in.blockSizes[i];
        if (produced >= want) return std::min(fileSize, (i + 1) * size_t(in.blockSize));
    }
    return fileSize;
}

Bytes decompressPrefix(const Bytes& data, size_t want) {
    Info in = parse(data);
    if (in.stored) return Bytes(data.begin() + 16, data.begin() + 16 + std::min(want, data.size() - 16));
    Bytes out;
    out.reserve(size_t(std::min<uint64_t>(in.uncompressedSize, want)));
    for (size_t i = 0; i < in.blockSizes.size() && out.size() < want; ++i) {
        size_t begin = i == 0 ? in.headerSize : i * size_t(in.blockSize);
        size_t end = std::min(data.size(), (i + 1) * size_t(in.blockSize));
        if (begin >= end) fail("XCTD: block " + std::to_string(i) + " outside file");
        if (end - begin >= 2 && be16(&data[begin]) == 0) {
            // Incompressible block: u16 0 marker followed by the raw bytes.
            if (end - begin - 2 < in.blockSizes[i]) fail("XCTD: stored block " + std::to_string(i) + " too short");
            out.insert(out.end(), data.begin() + begin + 2, data.begin() + begin + 2 + in.blockSizes[i]);
            continue;
        }
        Bytes block = lzx::decompress(&data[begin], end - begin, in.blockSizes[i]);
        out.insert(out.end(), block.begin(), block.end());
    }
    return out;
}

Bytes wrapStored(const Bytes& data) {
    Writer w;
    w.u32(kMagic);
    w.u32(0x01000000);
    w.u32(crc32(data.data(), data.size()));
    w.u32(2u << 4); // 128 KiB blocks, zero table entries => raw payload
    w.bytes(data);
    return w.buf();
}

} // namespace nut::xctd
