/**
 * renut_field_of_view: a wider (or narrower) view than the game's.
 *
 * N&B keeps the vertical field of view in degrees in flt_82E1B6B4, which
 * meCalculateView (0x822BB8F0) sets every time the camera changes from the
 * active camera's lens (camera +0x60 the focal length, +0x6C the lens table
 * entry): sub_821E4540(entry, out, focal) writes 2 * atan(film / (2 * focal))
 * in degrees (55 for entry 3). Everything that frames the view reads that
 * value: meCalculateProjection (0x822BC0E0) builds the projection and, through
 * sub_8221DE68, the culling frustum's six planes from it; sub_822DB248 the
 * LOD screen metric; sub_82386BF8 the shader constants that rebuild view rays
 * from depth (fog, SSAO). So the value is scaled where it is made, and every
 * reader agrees with the picture. sub_821E0BE8 builds a camera snapshot with
 * the same call, and is scaled alike. The value is made afresh each time, so
 * scaling it does not compound.
 */
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

REXCVAR_DEFINE_STRING(renut_field_of_view, "Game", "Nuts&Bolts/Graphics",
                      "Field of view, as a share of the game's own: Game, or 110% to 150% "
                      "(wider: more of the world on screen). Cutscenes widen too.")
    .allowed({"Game", "110%", "120%", "130%", "140%", "150%"});

REX_EXTERN(__imp__sub_821E4540);

namespace {

// The widest vertical field of view the projection gets, in degrees.
constexpr float kMaxFieldOfView = 150.0f;

float FieldOfViewScale() {
  const std::string &setting = REXCVAR_GET(renut_field_of_view);
  if (setting.empty() || setting == "Game")
    return 1.0f;
  const int percent = std::atoi(setting.c_str());
  return percent > 0 ? float(percent) / 100.0f : 1.0f;
}

} // namespace

// void sub_821E4540(int lens, float *fov_degrees, double focal)
REX_HOOK_RAW(sub_821E4540) {
  const uint32_t out = ctx.r4.u32;
  __imp__sub_821E4540(ctx, base);
  const float scale = FieldOfViewScale();
  if (scale == 1.0f || !out)
    return;
  uint32_t bits = rex::byte_swap(*reinterpret_cast<const uint32_t *>(base + out));
  float fov;
  std::memcpy(&fov, &bits, sizeof(fov));
  if (!(fov > 0.0f))
    return;
  fov = std::min(fov * scale, std::max(fov, kMaxFieldOfView));
  std::memcpy(&bits, &fov, sizeof(bits));
  *reinterpret_cast<uint32_t *>(base + out) = rex::byte_swap(bits);
}
