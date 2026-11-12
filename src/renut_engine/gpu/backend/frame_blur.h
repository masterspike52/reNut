#pragma once

#include <plume_render_interface.h>

#include "renut_engine/gpu/backend/video.h"

namespace renut::gpu {

// Any thread: the UI wants the game's frame blurred by a gaussian of `sigma` window pixels (BlurredGameFrameTexture).
void WantFrameBlur(float sigma);

// Records a blurred copy of the part of 'image' shown (scale, translate in its uv), `shown_width` window pixels wide,
// while the UI keeps asking for one. Caller holds s.mutex with the command list open, before the back buffer's pass.
void BlurFrameLocked(VideoState &s, const PresentImage &image, const float scale[2], const float translate[2],
                     float shown_width);

// Any thread: glass wants the screen as drawn so far (ScreenTexture), blurred by `sigma` window pixels (0: sharp).
void WantScreen(float sigma);
// Any thread: the last present drew its UI where the screen can be copied from, so ScreenTexture has something.
bool ScreenReady();

// Presenting thread, before the back buffer's pass: while glass keeps asking, the target the UI pass draws into
// instead of the back buffer, made the window's size; otherwise nullptr, and the pass draws on the back buffer.
plume::RenderFramebuffer *BeginScreenLocked(VideoState &s, uint32_t width, uint32_t height);
bool ScreenActiveLocked();
// The copies glass reads, sharp or blurred; kInvalidDescriptor when the UI isn't drawn where they can be made.
uint32_t ScreenSlotLocked(bool blurred);
// In the UI pass: copies and blurs `region` (pixels) of what's drawn so far, then draws on where it was.
// The pass's state (viewport, pipeline, buffers) has to be set again after.
bool CaptureScreenLocked(VideoState &s, plume::RenderCommandList *cmd, const plume::RenderRect &region);
// After the UI pass: puts what it drew on the back buffer.
void FinishScreenLocked(VideoState &s, plume::RenderFramebuffer *back_fb, uint32_t width, uint32_t height);

}  // namespace renut::gpu
