// XUI font glyph caches (xuifontcachefont\*.sbp).
//
// Layout (big-endian):
//   u8 1, u8 1, u8 lineHeight, u8 bitsPerPixel (4), u16 glyphCount
//   glyphCount x { u16 codepoint, u32 dataOffset, i8 metrics[4] }   sorted by codepoint
//   glyph data: one zlib stream per glyph, decompressing to a square cell
//   (cellSize x cellSize, 4 bpp, low nibble = left pixel)
#pragma once

#include "common.hpp"
#include "json.hpp"

namespace nut::font {

struct Glyph {
    uint16_t code = 0;
    int8_t metrics[4] = {};
    Bytes packed;     // zlib stream for `pixels`; clear it after changing pixels so it is re-encoded
    Bytes pixels;     // cellSize * cellSize, one byte per pixel (0..15)
};

struct Font {
    uint8_t header[4] = {1, 1, 0, 4};
    uint32_t cellSize = 0;
    std::vector<Glyph> glyphs;

    static Font parse(const Bytes& file);
    Bytes serialize() const;
};

bool looksLikeFont(const Bytes& data);

// Export to <dir>/font.json + <dir>/page_NNN.png (32x32 cells per page), plus the font itself:
// <dir>/font.fnt (BMFont over the pages) and <dir>/<dir name>.ttf.
void exportTo(const Font& f, const fs::path& dir);

// Baseline as a row from the cell top; the cache stores none, so it is read off flat-bottomed letters.
uint32_t baselineRow(const Font& f);
// TrueType font with one square per pixel of coverage >= 8/15; 1 em = the cache's line height.
Bytes toTtf(const Font& f, const std::string& family);
// BMFont text descriptor for the pages written by exportTo.
std::string toBmfont(const Font& f, const std::string& face);
// Rebuild from an exported directory. When `base` is given, glyphs whose pixels are unchanged
// keep their original compressed data so the output stays close to the original size.
Font importFrom(const fs::path& dir, const Font* base = nullptr);

} // namespace nut::font
