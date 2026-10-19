/**
 * @file    gpu/overlay/overlay_ui.h
 * @brief   reNut's own overlay: an ImGui context of its own, fed from the game
 *          window's messages, built on the window's thread and drawn by the
 *          renderer into each presented frame.
 *
 * The overlay has a Dear ImGui of its own, compiled into the executable
 * (gpu.cmake): the SDK's copy in rexruntime has one current context, which
 * the SDK switches to its own from any thread. The frame is built on the
 * SDK's UI thread (a call the overlay's pump queues there 60 times a second);
 * the window's input is queued as it arrives and applied by the next build.
 * What a build produces is copied out, and Present draws the latest copy, so
 * neither thread waits for the other.
 */
#pragma once

#include <cstdint>
#include <functional>

#include <plume_render_interface.h>

namespace renut::gpu::overlay {

// An overlay window. Draw builds its widgets, on the UI thread with the
// overlay's context current, once per overlay frame.
class Dialog {
public:
  virtual ~Dialog() = default;
  virtual void Draw() = 0;
};

// Any thread.
void AddDialog(Dialog *dialog);
void RemoveDialog(Dialog *dialog);

// Any thread: the last overlay frame showed a window that takes input (a
// settings or online window, not the FPS overlay), so the mouse should be a
// free cursor.
bool WantsCursor();

// Runs a function on the SDK's UI thread, later (WindowedAppContext::
// CallInUIThreadDeferred).
using UIThreadRunner = std::function<void(std::function<void()>)>;

// Once the window exists: takes the window's input and starts building frames
// through 'run_on_ui_thread'. Without a window it does nothing and returns false.
bool Start(UIThreadRunner run_on_ui_thread);
void Stop();

// Present, State().mutex held, the back buffer's framebuffer set: draws the
// latest overlay frame over it.
void DrawLocked(plume::RenderCommandList *cmd, uint32_t width, uint32_t height);

} // namespace renut::gpu::overlay
