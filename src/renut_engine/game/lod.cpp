/**
 * @file    game/lod.cpp
 * @brief   Models keep their detailed meshes, and the grass its patches,
 *          farther away.
 *
 * N&B picks a model's level of detail in sub_82BC3F98 (its two callers,
 * sub_822E82C0 and sub_82377D48, pass f1 = 1.0, f2 = 0.0): for each LOD group
 * it works out |view-space z| * f1 / the model's screen size + f2, and steps
 * through the group's thresholds to the last one that metric reaches. Tuned
 * for 720p on the console, the switches are close and plain at higher
 * resolutions: the pop-in. renut_lod_distance scales f1 down, so each switch
 * happens that many times farther away; "Max" keeps the most detailed mesh
 * (as the disable_lod cheat does).
 *
 * The level's grass has distance bands of its own, which the model LOD does
 * not move. sub_8238BE48 draws it by tile (256 bytes each): every patch
 * instance (112 bytes, list at +0xFC, count at +0xAC) has a band per view,
 * and band b draws with the patch mesh's level b forced; the last band
 * (count - 1) is not drawn. sub_8238D650 gives each frustum-visible patch the
 * last band whose start (floats at +0xB0, count at +0xD0, at most 8) its
 * distance from the camera reaches; the tile as a whole drops to the last
 * band, and sub_8238D8C8 stops updating its patches' visibility, once its
 * distance less its radius (+0x80) passes start[count - 2]. The shaders have
 * no fade, so whole patches pop in at that distance. renut_grass_distance
 * scales the band starts for those two calls, so the patches and their
 * density switches come that many times farther away.
 */
#include "renut_engine/app/platform_defaults.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include <cstdint>
#include <cstring>
#include <string>

REXCVAR_DEFINE_STRING(renut_lod_distance, RENUT_DEFAULT("2x", "1x"), "Nuts&Bolts/Graphics",
                      "How far models keep their detailed meshes: 1x is the game's, 2x and 4x "
                      "switch to the simpler ones that many times farther away (less pop-in), Max "
                      "always draws the most detailed.")
    .allowed({"1x", "2x", "4x", "Max"});

REXCVAR_DEFINE_STRING(renut_grass_distance, RENUT_DEFAULT("2x", "1x"), "Nuts&Bolts/Graphics",
                      "How far the level's grass is drawn, and its density kept: 1x is the "
                      "game's, 2x, 4x and 8x draw it that many times farther away (less pop-in, "
                      "more grass to draw).")
    .allowed({"1x", "2x", "4x", "8x"});

REX_EXTERN(__imp__sub_82BC3F98);
REX_EXTERN(__imp__sub_8238D650);
REX_EXTERN(__imp__sub_8238D8C8);

namespace {

double LodDistanceScale() {
  const std::string setting = REXCVAR_GET(renut_lod_distance);
  if (setting == "2x")
    return 0.5;
  if (setting == "4x")
    return 0.25;
  if (setting == "Max")
    return 0.0;
  return 1.0;
}

float GrassDistanceScale() {
  const std::string setting = REXCVAR_GET(renut_grass_distance);
  if (setting == "2x")
    return 2.0f;
  if (setting == "4x")
    return 4.0f;
  if (setting == "8x")
    return 8.0f;
  return 1.0f;
}

// Grass tile (see the file comment).
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
