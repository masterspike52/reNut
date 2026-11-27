// Rare texture assets (".rtx", asset type 0x01).
//
// A texture is two symbols with the same name inside a CAFF:
//   .data        header (0x70 bytes, more for array textures)
//   .texturegpu  Xbox 360 GPU data (tiled, big-endian)
//
// Header (big-endian):
//   +0x00 "texture\0"      +0x08 "04.05.05.0032\0"
//   +0x18 D3DFORMAT         +0x1C kind (0 = 2D, 2 = cube, 4 = array, 5 = volume)
//   +0x24 u16 width         +0x26 u16 height
//   +0x28 base level offset +0x2C mip chain offset (0xFFFFFFFF = mips follow the base level)
//   +0x30 u8 mip levels     +0x34 u32 header extent (0x3C normally, 0x54 with extra data)
//
// Large textures are split for streaming: "<name>_0x<hash>top" (stream bundle) holds level 0
// and "<name>_0x<hash>mip" (bundle) holds levels 1..n with base offset == mip offset == 0.
#pragma once

#include "common.hpp"
#include "image.hpp"

namespace nut::texture {

enum class Kind { Tex2D = 0, Cube = 2, Array = 4, Volume = 5 };

struct Format {
    uint32_t gpu = 0;    // GPUTEXTUREFORMAT (low 6 bits of D3DFORMAT)
    uint32_t endian = 0; // 0 none, 1 8in16, 2 8in32, 3 16in32
    bool tiled = false;
    uint32_t blockW = 1, blockH = 1, bytesPerBlock = 0;
    const char* name = "unknown";
    bool supported() const { return bytesPerBlock != 0; }
    bool compressed() const { return blockW > 1; }
};

Format describeFormat(uint32_t d3dFormat);

struct Header {
    uint32_t d3dFormat = 0;
    Kind kind = Kind::Tex2D;
    uint32_t width = 0, height = 0;
    uint32_t baseOffset = 0, mipOffset = 0;
    uint32_t mipLevels = 1;
};

bool isTextureHeader(const Bytes& data);
Header parseHeader(const Bytes& data);
void writeHeaderDims(Bytes& data, uint32_t width, uint32_t height, uint32_t mipLevels);
// Array textures end their header with a table of each slice's width, height, mip count and data
// offset (pointers at 0x3C/0x44/0x48/0x4C, relative to the header). True when it matches the header.
bool hasArrayTable(const Bytes& header, uint32_t slices);
// Rewrites that table for a texture now shaped like `h`, with `sliceBytes` of GPU data per slice.
void writeArrayTable(Bytes& header, const Header& h, uint32_t slices, uint32_t sliceBytes);
// The same texture at another size (2D, up to 8192x8192). A mip chain keeps the same number of
// levels below the smaller side's size, so it ends where the original did.
Header resized(const Header& h, uint32_t width, uint32_t height);

// Which levels the GPU blob holds.
struct Contents {
    uint32_t firstLevel = 0; // 1 for "mip" halves of split textures
    uint32_t levelCount = 1; // number of levels stored, starting at firstLevel
    uint32_t slices = 1;     // 6 for cubes, N for arrays
};
Contents describeContents(const Header& h, size_t gpuSize);

struct Level {
    uint32_t width = 0, height = 0;
    std::vector<Bytes> slices; // linear, PC byte order
};

// Untile all stored levels (every slice of cube, array and volume textures). Throws for unsupported formats.
std::vector<Level> decode(const Header& h, const Bytes& gpu);
// Re-tile levels into a new GPU blob laid out like the game expects.
Bytes encode(const Header& h, const Contents& c, const std::vector<Level>& levels);
size_t gpuSize(const Header& h, const Contents& c);
// Texel dimensions of each stored level (firstLevel onwards).
std::vector<std::pair<uint32_t, uint32_t>> levelDims(const Header& h, const Contents& c);

image::Dds toDds(const Header& h, const std::vector<Level>& levels, uint32_t slice = 0);
std::vector<Level> fromDds(const Header& h, const Contents& c, const Bytes& ddsFile);

// Block (de)compression for PNG conversion.
image::Rgba toRgba(const Header& h, const Level& level, uint32_t slice = 0);
Bytes fromRgba(const Format& f, const image::Rgba& img); // linear blocks for one level
image::Rgba downscale(const image::Rgba& img, uint32_t w, uint32_t h);

} // namespace nut::texture
