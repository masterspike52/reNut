/**
 * @file    game/jiggy_wall_hold.cpp
 * @brief   The jiggy wall stays shut, still animating, until the renderer has
 *          built the pipelines of the world behind it.
 *
 * N&B's transition into and out of a world (IDA, TU3): sub_82558DC0 begins
 * one (dword_82E19EE0, the requests, + 1) and its end takes the request away.
 * sub_82559010, once a frame from sub_821CA5B8, closes the wall while there
 * are requests, notes them (dword_82E19EE4, 'shown'), plays the wall's loop
 * while they last, and begins opening it (dword_82E1A0A0 set, 'shown' back to
 * 0) the first frame there are none.
 *
 * The world's shaders come with its load, behind the wall, and the renderer's
 * workers build their pipelines then; while the wall is shut a draw whose
 * pipeline is not built yet is left out (renut_async_pipelines,
 * gpu/render/pipeline.cpp). Here the tick sees one more request than the game
 * has while the workers are still at it (PipelineBuildsPending), so the wall
 * keeps its loop instead of opening onto a world that is not all there yet;
 * the request is gone again when the tick returns. At most kMaxHold.
 */
#include <rex/hook.h>
#include <rex/types.h>

#include <chrono>
#include <cstdint>

#include "renut_engine/app/renut_logging.h"
#include "renut_engine/game/guest_call.h"
#include "renut_engine/gpu/render/pipeline.h"

REX_EXTERN(__imp__sub_82559010);

namespace {

using renut::guest::Load32;
using renut::guest::Store32;

constexpr uint32_t kRequests = 0x82E19EE0;  // dword_82E19EE0
constexpr uint32_t kShown = 0x82E19EE4;     // dword_82E19EE4
constexpr uint32_t kOpening = 0x82E1A0A0;   // dword_82E1A0A0
// A hold that long means something else is wrong: the wall opens anyway.
constexpr auto kMaxHold = std::chrono::seconds(30);

using Clock = std::chrono::steady_clock;
Clock::time_point g_hold_since{};
bool g_holding = false;

// The wall shut with no request left: the tick would open it now.
bool WallWouldOpen(const uint8_t *base) {
  return Load32(base, kRequests) == 0 && int32_t(Load32(base, kShown)) > 0 && Load32(base, kOpening) == 0;
}

} // namespace

// void sub_82559010(double seconds, ...): the jiggy wall's tick.
REX_HOOK_RAW(sub_82559010) {
  bool hold = false;
  if (WallWouldOpen(base) && renut::gpu::PipelineBuildsPending()) {
    const Clock::time_point now = Clock::now();
    if (!g_holding) {
      g_holding = true;
      g_hold_since = now;
      renut::gpu::BoostPipelineBuilds(true);
    }
    hold = now - g_hold_since < kMaxHold;
  } else if (g_holding) {
    g_holding = false;
    renut::gpu::BoostPipelineBuilds(false);
    RNUT_INFO("Jiggy wall: held shut {} ms for the world's pipelines",
              std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_hold_since).count());
  }
  if (hold)
    Store32(base, kRequests, 1);
  __imp__sub_82559010(ctx, base);
  if (hold)
    Store32(base, kRequests, Load32(base, kRequests) - 1);
}
