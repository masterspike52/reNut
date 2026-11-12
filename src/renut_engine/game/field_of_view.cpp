// renut_field_of_view: scales the vertical FOV (degrees) that sub_821E4540
// computes from the camera lens, so projection, culling and LOD all agree.
#include <rex/ppc/hook.h>
#include <rex/setting.h>
#include <rex/types.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/game/field_of_view.h"

REX_EXTERN(__imp__sub_821E4540);

namespace {

struct FieldOfViewSettings final : rex::SettingOwner {
  FieldOfViewSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
  REX_SETTING(std::string, renut_field_of_view, "Game",
              "Field of view, as a share of the game's own: Game, or 110% to 150% "
              "(wider: more of the world on screen). Cutscenes widen too.",
              rex::Allowed("Game", "110%", "120%", "130%", "140%", "150%"));
};
FieldOfViewSettings &g_fov = *new FieldOfViewSettings();  // never freed, registered by RenutApp::OnConfigure

// The widest vertical field of view the projection gets, in degrees.
constexpr float kMaxFieldOfView = 150.0f;

// A mod's field of view (renut.camera.set_fov), in degrees; 0 for none.
std::atomic<float> g_override{0.0f};

float FieldOfViewScale() {
  const std::string setting = g_fov.renut_field_of_view.Get();
  if (setting.empty() || setting == "Game")
    return 1.0f;
  const int percent = std::atoi(setting.c_str());
  return percent > 0 ? float(percent) / 100.0f : 1.0f;
}

} // namespace

void renut::game::SetFieldOfView(float degrees) {
  g_override.store(degrees > 0.0f ? std::clamp(degrees, 5.0f, kMaxFieldOfView) : 0.0f, std::memory_order_relaxed);
}

// void sub_821E4540(int lens, float *fov_degrees, double focal)
REX_HOOK_RAW(sub_821E4540) {
  const uint32_t out = ctx.r4.u32;
  __imp__sub_821E4540(ctx, base);
  if (!out)
    return;
  // A mod's takes the place of the game's, scaled or not.
  if (const float fixed = g_override.load(std::memory_order_relaxed); fixed > 0.0f) {
    uint32_t bits;
    std::memcpy(&bits, &fixed, sizeof(bits));
    *reinterpret_cast<uint32_t *>(base + out) = rex::byte_swap(bits);
    return;
  }
  const float scale = FieldOfViewScale();
  if (scale == 1.0f)
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
