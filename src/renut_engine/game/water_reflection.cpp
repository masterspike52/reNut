// Water reflections mirror at the highest water surface under each player (sub_821CECD0),
// which snaps between volumes and falls to the player's height off water: this eases it.
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace {

constexpr uint32_t kViews = 2;
constexpr uint32_t kFrameWaterPoints = 0xDA0;  // frame data: a (x, water y, z) per player
constexpr uint32_t kPassView = 0x34;
constexpr uint32_t kPassMirrorHeight = 0x54;

constexpr float kEaseSeconds = 0.08f;      // time constant of the ease toward a new height
constexpr float kSnapDistance = 10.0f;     // farther than this: a new place, snap
constexpr float kSnapGapSeconds = 0.5f;    // frames this far apart (loads, pauses): snap

using Clock = std::chrono::steady_clock;

struct ViewState {
  uint32_t world = 0;
  bool has_water = false;
  float water = 0.0f;  // the last height a water volume gave
  bool has_shown = false;
  float shown = 0.0f;
  Clock::time_point last;
};

ViewState g_views[kViews];
std::atomic<bool> g_override[kViews];
std::atomic<float> g_height[kViews];

uint32_t Load32(uint32_t va) {
  return rex::byte_swap(*REX_RUNTIME()->GetMembase().TranslateVirtual<const uint32_t *>(va));
}

float LoadFloat(uint32_t va) {
  return std::bit_cast<float>(Load32(va));
}

void StoreFloat(uint32_t va, float value) {
  *REX_RUNTIME()->GetMembase().TranslateVirtual<uint32_t *>(va) =rex::byte_swap(std::bit_cast<uint32_t>(value));
}

} // namespace

// sub_821CECD0 at 0x821CEF74, after each player's water query: r3 the query's result,
// r29 the player's slot, r30 its water point, r31 the frame data.
void WaterReflection_query(PPCRegister &r3, PPCRegister &r29, PPCRegister &r30, PPCRegister &r31) {
  const uint32_t view = (r30.u32 - (r31.u32 + kFrameWaterPoints)) / 16;
  if (view >= kViews)
    return;
  ViewState &s = g_views[view];
  const uint32_t world = Load32(r31.u32);
  const bool has_player = Load32(r29.u32) != 0;
  if (!has_player || world != s.world) {
    s = ViewState{};
    s.world = world;
  }
  if (!has_player) {
    g_override[view] = false;
    return;
  }
  if (r3.u32) {
    s.water = LoadFloat(r30.u32 + 4);
    s.has_water = true;
  }
  if (!s.has_water) {
    g_override[view] = false;
    return;
  }
  const Clock::time_point now = Clock::now();
  const float dt = std::chrono::duration<float>(now - s.last).count();
  s.last = now;
  if (!s.has_shown || dt > kSnapGapSeconds || std::fabs(s.water - s.shown) > kSnapDistance) {
    s.shown = s.water;
    s.has_shown = true;
  } else {
    s.shown += (s.water - s.shown) * (1.0f - std::exp(-dt / kEaseSeconds));
  }
  g_height[view] = s.shown;
  g_override[view] = true;
}

// sub_8237FA08 at 0x8237FB68, after it copied its player's water point to +0x50 and
// before it reads the mirror height at +0x54: r30 the reflection pass.
void WaterReflection_height(PPCRegister &r30) {
  const uint32_t view = Load32(r30.u32 + kPassView);
  if (view < kViews && g_override[view])
    StoreFloat(r30.u32 + kPassMirrorHeight, g_height[view]);
}
