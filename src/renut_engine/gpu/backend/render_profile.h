/**
 * @file    gpu/backend/render_profile.h
 * @brief   Where the render thread's CPU time goes (renut_render_profile).
 *
 * The overlay's "Render" is the render thread's busy time per frame (its waits
 * for the GPU and the display left out): the CPU work of turning the game's
 * D3D calls into the host API's. With renut_render_profile above 0 that time
 * is split into the stages of a draw (shaders and targets, textures,
 * geometry, vertex fetch, constants, pipelines, render passes, vertex
 * streams, recording) and the work between draws (clears, resolves), and
 * every renut_render_profile seconds the log lists them by milliseconds per
 * frame, with the draws per frame. Off, a scope costs one flag test. Render
 * thread only.
 */
#pragma once

#include <chrono>
#include <cstdint>

namespace renut::gpu::render_profile {

enum class Stage : uint8_t {
  kShaders,      // the draw's shaders, declaration and targets found
  kExportWait,   // waiting for earlier memexport results
  kTextures,     // textures bound (and uploaded or converted when they changed)
  kGeometry,     // indices read, expanded or cached
  kVertexFetch,  // memexport streams converted, vertex fetch data uploaded
  kConstants,    // float, bool and shared constants uploaded
  kPipeline,     // pipeline looked up (and built when new)
  kTargets,      // render targets bound (render pass changes)
  kStreams,      // vertex streams converted and bound
  kRecord,       // the draw's state and draw recorded
  kClear,        // clears between draws
  kResolve,      // resolves between draws
  kCount
};

bool Enabled();

// Times its scope into 'stage' (when the profile is on).
class Scope {
 public:
  explicit Scope(Stage stage);
  ~Scope();
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;

 private:
  Stage stage_;
  bool active_;
  std::chrono::steady_clock::time_point start_;
};

// Times the consecutive stages of one piece of work (a draw): each Enter ends
// the stage before it, and the last ends with the object.
class Stages {
 public:
  Stages();
  ~Stages();
  Stages(const Stages &) = delete;
  Stages &operator=(const Stages &) = delete;
  void Enter(Stage stage);

 private:
  void End(std::chrono::steady_clock::time_point now);
  bool active_;
  bool timing_ = false;
  Stage stage_ = Stage::kShaders;
  std::chrono::steady_clock::time_point start_;
};

// A draw was recorded.
void CountDraw();

// The render thread finished a frame, busy for 'busy_ms' (its waits left out):
// adds it, and logs the report when it is due.
void FrameDone(double busy_ms);

}  // namespace renut::gpu::render_profile
