/**
 * @file    gpu/backend/render_profile.cpp
 */
#include "renut_engine/gpu/backend/render_profile.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <rex/cvar.h>

#include "renut_engine/gpu/backend/command_recorder.h"
#include "renut_engine/gpu/log.h"

REXCVAR_DEFINE_DOUBLE(renut_render_profile, 0.0, "Nuts&Bolts/Debug",
                      "Every this many seconds, log the render thread's CPU time per frame by "
                      "stage of the work (what the overlay's Render is spent on). 0: off.");

namespace renut::gpu::render_profile {

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char *kStageNames[] = {
    "shaders, declaration and targets found",
    "waiting for memexport results",
    "textures (bound, uploaded or converted)",
    "geometry (indices read, expanded, cached)",
    "vertex fetch (memexport streams, fetch data)",
    "constants uploaded",
    "pipelines (looked up, built when new)",
    "render targets bound (render passes)",
    "vertex streams (converted, bound)",
    "draw state and draw recorded",
    "clears",
    "resolves",
};
static_assert(std::size(kStageNames) == size_t(Stage::kCount));

// Since the last report.
std::array<Clock::duration, size_t(Stage::kCount)> g_stages{};
uint64_t g_draws = 0;
uint64_t g_frames = 0;
double g_busy_ms = 0.0;
double g_recorder_ms = 0.0;
Clock::time_point g_since = Clock::now();

void Report() {
  const double frames = double(std::max<uint64_t>(g_frames, 1));
  std::array<std::pair<double, size_t>, size_t(Stage::kCount)> stages{};
  double counted = 0.0;
  for (size_t i = 0; i < stages.size(); ++i) {
    const double ms = std::chrono::duration<double, std::milli>(g_stages[i]).count() / frames;
    stages[i] = {ms, i};
    counted += ms;
  }
  std::sort(stages.begin(), stages.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
  const double busy = g_busy_ms / frames;
  std::string lines;
  for (const auto &[ms, i] : stages) {
    if (ms >= 0.005)
      lines += fmt::format("\n  {:7.3f} ms  {}", ms, kStageNames[i]);
  }
  lines += fmt::format("\n  {:7.3f} ms  the rest (present, uploads, other work)", std::max(0.0, busy - counted));
  // Beside it, not part of it (renut_threaded_recording).
  if (g_recorder_ms > 0.0)
    lines += fmt::format("\n  recording thread: {:.3f} ms per frame of host API calls", g_recorder_ms / frames);
  RGPU_INFO("Render thread profile: {:.3f} ms busy per frame over {} frames, {:.0f} draws per frame{}",
            busy, g_frames, double(g_draws) / frames, lines);
  g_stages = {};
  g_draws = 0;
  g_frames = 0;
  g_busy_ms = 0.0;
  g_recorder_ms = 0.0;
}

}  // namespace

bool Enabled() {
  return REXCVAR_GET(renut_render_profile) > 0.0;
}

Scope::Scope(Stage stage) : stage_(stage), active_(Enabled()) {
  if (active_)
    start_ = Clock::now();
}

Scope::~Scope() {
  if (active_)
    g_stages[size_t(stage_)] += Clock::now() - start_;
}

Stages::Stages() : active_(Enabled()) {}

Stages::~Stages() {
  if (timing_)
    End(Clock::now());
}

void Stages::End(Clock::time_point now) {
  g_stages[size_t(stage_)] += now - start_;
  timing_ = false;
}

void Stages::Enter(Stage stage) {
  if (!active_)
    return;
  const Clock::time_point now = Clock::now();
  if (timing_)
    End(now);
  stage_ = stage;
  start_ = now;
  timing_ = true;
}

void CountDraw() {
  if (Enabled())
    ++g_draws;
}

void FrameDone(double busy_ms) {
  const double interval = REXCVAR_GET(renut_render_profile);
  const double recorder_ms = CommandRecorder::TakeBusyMs();
  if (interval <= 0.0) {
    g_since = Clock::now();
    return;
  }
  ++g_frames;
  g_busy_ms += busy_ms;
  g_recorder_ms += recorder_ms;
  if (Clock::now() - g_since >= std::chrono::duration<double>(interval)) {
    Report();
    g_since = Clock::now();
  }
}

}  // namespace renut::gpu::render_profile
