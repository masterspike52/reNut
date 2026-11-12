#pragma once

#include <cstdint>

#include <imgui.h>

namespace rex::gpu {
class IGraphicsSystem;
} // namespace rex::gpu
namespace rex::ui {
class IUISystem;
} // namespace rex::ui

namespace renut::gpu {

// OnInit, on the UI thread with the window open: creates the host device, starts
// the overlays and presents overlay-only frames whenever the guest isn't presenting.
bool StartRenderer(rex::ui::IUISystem &ui, rex::gpu::IGraphicsSystem &graphics);

// Any thread: the game's last shown frame as a texture of the SDK's ImGui, and its
// shown part (u0, v0, u1, v1); ImTextureID_Invalid before there's one.
ImTextureID GameFrameTexture(float uv[4]);
// Any thread: the same frame blurred by a gaussian of `sigma` window pixels, for glass and the Guide;
// ImTextureID_Invalid until there's one. The blur only runs while something keeps asking.
ImTextureID BlurredGameFrameTexture(float sigma, float uv[4]);
// Any thread: the screen as drawn so far, game and UI, refreshed as each draw using it is reached; blurred by
// `sigma` window pixels, or sharp for 0. For glass that shows what's under it (RexGlass's Config::screen).
ImTextureID ScreenTexture(float sigma, float uv[4]);

// Any thread: an RGBA8 picture for the SDK's ImGui (the Guide, RexFabric), or
// ImTextureID_Invalid for no pixels; drawn with linear filtering.
ImTextureID CreateOverlayTexture(uint32_t width, uint32_t height, const uint8_t *rgba);
// Any thread: frames already recorded keep it until they are replaced.
void DestroyOverlayTexture(ImTextureID texture);

// Before the title unloads (its guest memory goes): the render thread runs the game's queued work and
// ends, and later work runs on the thread that queues it.
void StopGuestRendering();

// OnShutdown.
void ShutdownRenderer();

} // namespace renut::gpu
