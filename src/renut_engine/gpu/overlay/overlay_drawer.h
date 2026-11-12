#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <rex/overlay_renderer.h>

namespace plume {
struct RenderCommandList;
struct RenderFramebuffer;
struct RenderPipeline;
struct RenderShader;
struct RenderTexture;
struct RenderTextureView;
} // namespace plume

namespace renut::gpu {

// A texture the SDK's ImGui draws with: its pixels, and the host texture the
// presenting thread makes from them on first draw; recorded frames share it.
struct OverlayTextureData {
  OverlayTextureData(uint32_t width, uint32_t height, uint32_t sampler_slot, const uint8_t *rgba);
  // Any thread: the host texture goes to the presenting thread for release.
  ~OverlayTextureData();

  uint32_t width;
  uint32_t height;
  uint32_t sampler_slot;
  std::vector<uint8_t> rgba;  // until uploaded
  // Presenting thread, State().mutex held:
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  uint32_t slot;
  bool failed = false;
  bool game_frame = false;     // stands for the frame just scanned out (GameFrameTexture)
  bool blurred_frame = false;  // stands for its blurred copy (BlurredGameFrameTexture)
  uint8_t screen = 0;          // stands for the screen as drawn so far (ScreenTexture): 1 sharp, 2 blurred
};

// Presenting thread: the frame just scanned out, its slot (kInvalidDescriptor:
// none) and shown part (u0, v0, u1, v1), for GameFrameTexture.
void SetGameFrameLocked(uint32_t slot, const float uv[4]);
// Presenting thread: the blurred copy of its shown part (frame_blur.h), or kInvalidDescriptor.
void SetBlurredFrameLocked(uint32_t slot);

// Records the SDK's overlay frames (IUISystem::DrawOverlays) for the presenting
// thread to draw, so Present never waits for the UI thread.
class OverlayDrawer final : public rex::IOverlayRenderer {
public:
  OverlayDrawer();
  ~OverlayDrawer() override;

  // Recording, on the UI thread: copies the frame and honours its texture requests.
  void RenderOverlays(ImDrawData &draw_data) override;

  // Presenting thread, State().mutex held and a command list open: draws the
  // latest recorded frame over 'framebuffer' (width x height).
  static void DrawLatestLocked(plume::RenderCommandList *cmd, plume::RenderFramebuffer *framebuffer,
                               uint32_t width, uint32_t height);

  // UI thread: frames recorded so far, and dropping the latest (the SDK
  // drew nothing this time, as when its last dialog closed).
  static uint64_t FramesRecorded();
  static void ClearLatest();

  struct Frame;
};

} // namespace renut::gpu
