// LZX decoder for Xbox 360 XMemCompress "native" streams.
//
// A stream is a sequence of frames. Each frame is prefixed by either
//   u16be compressedSize                          (frame decodes to 32 KiB)
// or
//   0xFF, u16be uncompressedSize, u16be compressedSize
// The bitstream inside is standard LZX (16-bit little-endian words, read MSB first).
#pragma once

#include "common.hpp"

namespace nut::lzx {

// Decode one self-contained stream (decoder state starts fresh).
// `expectedSize` bounds the output; decoding stops when it is reached or input ends.
Bytes decompress(const uint8_t* src, size_t srcSize, size_t expectedSize, int windowBits = 17);

} // namespace nut::lzx
