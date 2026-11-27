// XDK "transparent decompression" container (magic 0x0FF512ED).
//
// The game opens files through XctdDecompression; files that do not start with the
// magic are read as-is, so repacked assets may be written either raw or wrapped.
//
// Layout (big-endian):
//   +0  u32 magic 0x0FF512ED
//   +4  u32 version (0x01000000)
//   +8  u32 hash/identifier (not validated by the reader)
//   +12 u32 flags: bits 4-5  blockSize = 0x8000 << n
//                  bits 6-21 blockCount (0 = data stored raw after the header)
//                  bits 22-23 table entry width selector (0 -> 20 bits, 1 -> 32 bits)
//   +16 bit-packed (MSB first) table of per-block *uncompressed* sizes, padded to u32
// The compressed file is split into blockSize-sized pieces counted from file offset 0
// (the header lives inside block 0); each piece is an independent LZX stream.
#pragma once

#include "common.hpp"

namespace nut::xctd {

constexpr uint32_t kMagic = 0x0FF512ED;

struct Info {
    uint32_t version = 0;
    uint32_t hash = 0;
    uint32_t flags = 0;
    uint32_t blockSize = 0;
    uint32_t headerSize = 0;
    std::vector<uint32_t> blockSizes; // uncompressed size of each block
    uint64_t uncompressedSize = 0;
    bool stored = false;
};

bool isXctd(const Bytes& data);
Info parse(const Bytes& data);
Bytes decompress(const Bytes& data);
// The first `want` bytes (or more, up to a block boundary) of the decompressed file, from data
// that may be only the start of the compressed file: see compressedBytesFor.
Bytes decompressPrefix(const Bytes& data, size_t want);
// How much of the compressed file decompressPrefix needs for `want` bytes of output.
size_t compressedBytesFor(const Info& info, size_t want, size_t fileSize);
// Wrap data without compression (block count 0); the game reads these transparently.
Bytes wrapStored(const Bytes& data);

} // namespace nut::xctd
