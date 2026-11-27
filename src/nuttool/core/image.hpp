// Image file helpers: PNG and DDS, plus JPG and others for reading.
#pragma once

#include "common.hpp"

namespace nut::image {

// 8-bit RGBA image.
struct Rgba {
    uint32_t width = 0, height = 0;
    Bytes pixels; // width * height * 4
};

Bytes encodePng(const Rgba& img);
Rgba decodePng(const Bytes& file);
// PNG, JPG, BMP, TGA, GIF (first frame), PSD (composite), told apart by their contents. Without
// stb_image in the build, only the PNGs decodePng reads.
Rgba decodeImage(const Bytes& file);
bool canDecodeImages(); // whether decodeImage reads more than PNG

// DDS container. `fourcc` is used for block-compressed data; otherwise the
// RGBA bit masks describe the pixel layout (little-endian, as on PC).
struct Dds {
    uint32_t width = 0, height = 0, mipCount = 1;
    uint32_t fourcc = 0;
    uint32_t bitCount = 0;
    uint32_t rMask = 0, gMask = 0, bMask = 0, aMask = 0;
    bool luminance = false;
    bool alphaOnly = false;
    std::vector<Bytes> mips; // linear data per level, PC byte order
};

constexpr uint32_t fourCC(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

Bytes encodeDds(const Dds& dds);
// On decode, mips[0] holds *all* pixel data after the header; the caller splits
// it into levels because only it knows the block geometry of the format.
Dds decodeDds(const Bytes& file);

} // namespace nut::image
