#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <plume_render_interface.h>

struct ImFont;

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

// Any thread: the last overlay frame showed a window that takes input, so the
// mouse should be a free cursor.
bool WantsCursor();

// Any thread: the game window's mouse wheel since the last call (120 a detent, up positive).
int32_t ConsumeMouseWheel();

// Any thread: an RGBA picture for the overlay, uploaded with the next drawn
// frame; its id is an ImTextureID for ImDrawList::AddImage.
uint64_t AddImage(uint32_t width, uint32_t height, std::vector<uint8_t> rgba);
void RemoveImage(uint64_t id);

// In a Dialog's Draw: the face for large menu text (Selawik Semibold), or the
// default font if that could not load. ImGui scales it to any size.
ImFont *MenuFont();

// The launcher's face: the game's own font (launcher_font_data.inc, made by
// tools/make_launcher_font.py), or MenuFont if that could not load.
ImFont *LauncherFont();

// Runs a function on the SDK's UI thread, later (IUISystem::PostTask).
using UIThreadRunner = std::function<void(std::function<void()>)>;

// Once the window exists: takes the window's input and starts building frames
// through 'run_on_ui_thread'. Without a window it does nothing and returns false.
bool Start(UIThreadRunner run_on_ui_thread);
void Stop();

// Present, State().mutex held, the back buffer's framebuffer set: draws the
// latest overlay frame over it.
void DrawLocked(plume::RenderCommandList *cmd, uint32_t width, uint32_t height);

} // namespace renut::gpu::overlay
