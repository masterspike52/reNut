// Pictures read from files (PNG, JPEG and the other formats stb_image reads) as RGBA8.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rexfabric::ui {

// RGBA8, rows top first, tightly packed; empty when it can't be read.
std::vector<uint8_t> DecodeImageRGBA(const uint8_t* data, size_t size, int& width, int& height);

}  // namespace rexfabric::ui
