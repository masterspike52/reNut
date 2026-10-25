/**
 * @file    gpu/render/title_logo.cpp
 */
#include "renut_engine/gpu/render/title_logo.h"

#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <xxhash.h>
#include <zstd.h>

#include "renut_engine/gpu/log.h"

REXCVAR_DEFINE_BOOL(renut_title_logo, true, "Nuts&Bolts/Menus",
                    "Show reNut's logo on the title screen in place of the game's");

namespace renut::gpu {

namespace {

#include "renut_engine/gpu/render/title_logo_data.inc"

constexpr uint32_t kBlocksWide = 1280 / 4;
constexpr uint32_t kBlocksHigh = 720 / 4;
constexpr uint32_t kBlockBytes = 16;
static_assert(kTitleLogoBytes == size_t(kBlocksWide) * kBlocksHigh * kBlockBytes);
// XXH3 of the game's logo blocks, row after row, each row's seeded with the
// last's (tools/make_title_logo.py --hash-original).
constexpr uint64_t kGameLogoHash = 0x4C8034C0E53B8B66ULL;

const std::vector<uint8_t> &Logo() {
  static std::vector<uint8_t> logo;
  static std::once_flag once;
  std::call_once(once, [] {
    logo.resize(kTitleLogoBytes);
    const size_t n = ZSTD_decompress(logo.data(), logo.size(), kTitleLogoZstd, sizeof(kTitleLogoZstd));
    if (ZSTD_isError(n) || n != logo.size()) {
      RGPU_ERROR("Title logo: decompressing failed");
      logo.clear();
    }
  });
  return logo;
}

} // namespace

bool ReplaceTitleLogo(uint8_t *blocks, uint32_t blocks_w, uint32_t blocks_h, uint32_t row_pitch) {
  if (blocks_w != kBlocksWide || blocks_h != kBlocksHigh || !REXCVAR_GET(renut_title_logo))
    return false;
  const size_t row_bytes = size_t(kBlocksWide) * kBlockBytes;
  uint64_t hash = 0;
  for (uint32_t y = 0; y < blocks_h; ++y)
    hash = XXH3_64bits_withSeed(blocks + size_t(y) * row_pitch, row_bytes, hash);
  if (hash != kGameLogoHash)
    return false;
  const std::vector<uint8_t> &logo = Logo();
  if (logo.empty())
    return false;
  for (uint32_t y = 0; y < blocks_h; ++y)
    std::memcpy(blocks + size_t(y) * row_pitch, logo.data() + y * row_bytes, row_bytes);
  return true;
}

} // namespace renut::gpu
