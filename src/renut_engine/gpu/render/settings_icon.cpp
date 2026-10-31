/**
 * @file    gpu/render/settings_icon.cpp
 */
#include "renut_engine/gpu/render/settings_icon.h"

#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

#include <xxhash.h>
#include <zstd.h>

#include "renut_engine/gpu/log.h"

namespace renut::gpu {

namespace {

#include "renut_engine/gpu/render/settings_icon_data.inc"

constexpr uint32_t kBlocksWide = 96 / 4;
constexpr uint32_t kBlocksHigh = 96 / 4;
constexpr uint32_t kBlockBytes = 16;
static_assert(kSettingsIconBytes == size_t(kBlocksWide) * kBlocksHigh * kBlockBytes);
// XXH3 of the map tab icon's blocks, row after row, each row's seeded with the
// last's (tools/make_menu_icon.py --hash-original).
constexpr uint64_t kMapIconHash = 0x0437825419BBDB1FULL;

const std::vector<uint8_t> &Icon() {
  static std::vector<uint8_t> icon;
  static std::once_flag once;
  std::call_once(once, [] {
    icon.resize(kSettingsIconBytes);
    const size_t n = ZSTD_decompress(icon.data(), icon.size(), kSettingsIconZstd, sizeof(kSettingsIconZstd));
    if (ZSTD_isError(n) || n != icon.size()) {
      RGPU_ERROR("Settings icon: decompressing failed");
      icon.clear();
    }
  });
  return icon;
}

} // namespace

bool ReplaceSettingsIcon(uint8_t *blocks, uint32_t blocks_w, uint32_t blocks_h, uint32_t row_pitch) {
  if (blocks_w != kBlocksWide || blocks_h != kBlocksHigh)
    return false;
  const size_t row_bytes = size_t(kBlocksWide) * kBlockBytes;
  uint64_t hash = 0;
  for (uint32_t y = 0; y < blocks_h; ++y)
    hash = XXH3_64bits_withSeed(blocks + size_t(y) * row_pitch, row_bytes, hash);
  if (hash != kMapIconHash)
    return false;
  const std::vector<uint8_t> &icon = Icon();
  if (icon.empty())
    return false;
  for (uint32_t y = 0; y < blocks_h; ++y)
    std::memcpy(blocks + size_t(y) * row_pitch, icon.data() + y * row_bytes, row_bytes);
  return true;
}

} // namespace renut::gpu
