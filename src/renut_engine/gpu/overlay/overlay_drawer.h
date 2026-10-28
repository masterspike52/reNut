/**
 * @file    gpu/overlay/overlay_drawer.h
 * @brief   The SDK's ImmediateDrawer (ImGui overlay) implemented on plume, for
 *          detached mode (config.graphics == nullptr).
 *
 * The SDK draws its menus (ImGuiDrawer::Draw) on the UI thread, which owns
 * their ImGui. The drawer records what it is asked to draw there, into a frame
 * of its own, and the presenting thread draws the latest frame into each
 * back buffer (DrawLatestLocked), so neither waits for the other. Textures the
 * SDK makes keep their pixels until the presenting thread first draws them.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>

namespace plume {
struct RenderCommandList;
struct RenderFramebuffer;
struct RenderPipeline;
struct RenderShader;
struct RenderTexture;
struct RenderTextureView;
} // namespace plume

namespace renut::gpu {

// A texture the SDK made: its pixels, and the host texture the presenting
// thread makes from them the first time it draws it. Recorded frames share
// it, so it outlives the SDK's texture while a frame still draws it.
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
  bool game_frame = false;  // stands for the frame just scanned out (GameFrameTexture)
};

// Presenting thread: the frame just scanned out, its slot (kInvalidDescriptor:
// none) and shown part (u0, v0, u1, v1), for GameFrameTexture.
void SetGameFrameLocked(uint32_t slot, const float uv[4]);

class OverlayTexture final : public rex::ui::ImmediateTexture {
public:
  explicit OverlayTexture(std::shared_ptr<OverlayTextureData> data);

  const std::shared_ptr<OverlayTextureData> &data() const { return data_; }

private:
  std::shared_ptr<OverlayTextureData> data_;
};

class OverlayDrawer final : public rex::ui::ImmediateDrawer {
public:
  OverlayDrawer();
  ~OverlayDrawer() override;

  // Recording, on the UI thread (ImGuiDrawer::Draw).
  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                           rex::ui::ImmediateTextureFilter filter,
                                                           bool is_repeated,
                                                           const uint8_t *data) override;
  void Begin(rex::ui::UIDrawContext &ctx, float coord_width, float coord_height) override;
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch &batch) override;
  void Draw(const rex::ui::ImmediateDraw &draw) override;
  void EndDrawBatch() override;
  void End() override;

  // Presenting thread, State().mutex held and a command list open: draws the
  // latest recorded frame over 'framebuffer' (width x height).
  static void DrawLatestLocked(plume::RenderCommandList *cmd, plume::RenderFramebuffer *framebuffer,
                               uint32_t width, uint32_t height);

  // UI thread: frames recorded so far, and dropping the latest (the SDK
  // drew nothing this time, as when its last dialog closed).
  static uint64_t FramesRecorded();
  static void ClearLatest();

  struct Frame;

private:
  std::unique_ptr<Frame> recording_;  // between Begin and End
};

} // namespace renut::gpu
