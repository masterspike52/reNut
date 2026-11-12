#pragma once

namespace rex::ui {
class IUISystem;
}

namespace renut::game {

// Any thread, once the window exists: the mouse camera takes the window's
// mouse (a click captures it) on the UI thread.
void AttachMouseCamera(rex::ui::IUISystem &ui);
// Before the window goes: gives the mouse back and stops listening.
void DetachMouseCamera();

// Any thread: the game window holds the mouse (a click captured it).
bool MouseCaptured();

} // namespace renut::game
