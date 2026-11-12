#include <rex/ppc/hook.h>
#include <rex/setting.h>
#include <rex/types.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "renut_engine/app/renut_settings.h"

REX_EXTERN(__imp__sub_82BC3F98);
REX_EXTERN(__imp__sub_8238D650);
REX_EXTERN(__imp__sub_8238D8C8);

namespace {

struct LodSettings final : rex::SettingOwner {
  LodSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
  REX_SETTING(std::string, renut_lod_distance, "2x",
              "How far models keep their detailed meshes: 1x is the game's, 2x and 4x "
              "switch to the simpler ones that many times farther away (less pop-in), Max "
              "always draws the most detailed.",
              rex::Allowed("1x", "2x", "4x", "Max"));
  REX_SETTING(std::string, renut_grass_distance, "2x",
              "How far the level's grass is drawn, and its density kept: 1x is the "
              "game's, 2x, 4x and 8x draw it that many times farther away (less pop-in, "
              "more grass to draw).",
              rex::Allowed("1x", "2x", "4x", "8x"));
};
LodSettings &g_lod = *new LodSettings();  // never freed, registered by RenutApp::OnConfigure

double LodDistanceScale() {
  const std::string setting = g_lod.renut_lod_distance.Get();
  if (setting == "2x")
    return 0.5;
  if (setting == "4x")
    return 0.25;
  if (setting == "Max")
    return 0.0;
  return 1.0;
}

float GrassDistanceScale() {
  const std::string setting = g_lod.renut_grass_distance.Get();
  if (setting == "2x")
    return 2.0f;
  if (setting == "4x")
    return 4.0f;
  if (setting == "8x")
    return 8.0f;
  return 1.0f;
}

// Grass tile: distance band start floats at +0xB0, their count at +0xD0.
constexpr uint32_t kTileBandStarts = 0xB0;
constexpr uint32_t kTileBandCount = 0xD0;
constexpr uint32_t kMaxBands = (kTileBandCount - kTileBandStarts) / 4;

uint32_t LoadU32(const uint8_t *base, uint32_t va) {
  return rex::byte_swap(*reinterpret_cast<const uint32_t *>(base + va));
}

void StoreU32(uint8_t *base, uint32_t va, uint32_t value) {
  *reinterpret_cast<uint32_t *>(base + va) = rex::byte_swap(value);
}

// Scales a grass tile's band starts while it lives, then puts the game's back.
class ScaledGrassBands {
 public:
  ScaledGrassBands(uint8_t *base, uint32_t tile) : base_(base), tile_(tile) {
    const float scale = GrassDistanceScale();
    count_ = LoadU32(base, tile + kTileBandCount);
    if (scale == 1.0f || count_ < 2 || count_ > kMaxBands) {
      count_ = 0;
      return;
    }
    for (uint32_t i = 0; i < count_; ++i) {
      const uint32_t va = tile + kTileBandStarts + 4 * i;
      saved_[i] = LoadU32(base, va);
      float start;
      std::memcpy(&start, &saved_[i], sizeof(start));
      start *= scale;
      uint32_t bits;
      std::memcpy(&bits, &start, sizeof(bits));
      StoreU32(base, va, bits);
    }
  }

  ~ScaledGrassBands() {
    for (uint32_t i = 0; i < count_; ++i)
      StoreU32(base_, tile_ + kTileBandStarts + 4 * i, saved_[i]);
  }

 private:
  uint8_t *base_;
  uint32_t tile_;
  uint32_t count_ = 0;
  uint32_t saved_[kMaxBands] = {};
};

} // namespace

// BOOL sub_82BC3F98(model, instance, view, ..., double distance_scale, double bias)
REX_HOOK_RAW(sub_82BC3F98) {
  ctx.f1.f64 *= LodDistanceScale();
  __imp__sub_82BC3F98(ctx, base);
}

// void sub_8238D650(tile, view, ...; v1 = camera position): the patch bands
REX_HOOK_RAW(sub_8238D650) {
  ScaledGrassBands scaled(base, ctx.r3.u32);
  __imp__sub_8238D650(ctx, base);
}

// void sub_8238D8C8(tile, frustum; v1 = camera position): patch visibility
REX_HOOK_RAW(sub_8238D8C8) {
  ScaledGrassBands scaled(base, ctx.r3.u32);
  __imp__sub_8238D8C8(ctx, base);
}
