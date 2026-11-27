// Self-contained DEFLATE / zlib codec (RFC 1950/1951).
// Used for SBP font glyphs (zlib streams) and PNG image I/O.
#pragma once

#include "common.hpp"

namespace nut::deflate {

// Raw DEFLATE. `consumed` (optional) receives the number of input bytes used.
Bytes inflate(const uint8_t* src, size_t size, size_t* consumed = nullptr, size_t sizeHint = 0);
Bytes compress(const uint8_t* src, size_t size);

// zlib wrapper (2-byte header + DEFLATE + Adler-32).
Bytes zlibDecompress(const uint8_t* src, size_t size, size_t* consumed = nullptr, size_t sizeHint = 0);
Bytes zlibCompress(const uint8_t* src, size_t size);

uint32_t adler32(const uint8_t* p, size_t n, uint32_t a = 1);

} // namespace nut::deflate
