#pragma once
// Pictures to RGBA: PNG, JPEG, BMP, GIF (first frame) and TGA. The Guide's own
// decoder, as rexglue's reads only PNG.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rexglue_guide::image {

// RGBA pixels, or empty (with *width and *height 0) if it can't be read.
std::vector<uint8_t> Decode(const void* data, size_t size, int* width, int* height);

}  // namespace rexglue_guide::image
