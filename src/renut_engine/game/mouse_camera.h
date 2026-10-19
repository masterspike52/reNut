/**
 * @file    game/mouse_camera.h
 * @brief   The mouse turns the game camera (mouse_camera.cpp).
 */
#pragma once

namespace rex::ui {
class Window;
}

namespace renut::game {

// Any thread, once the window exists: the mouse camera takes the window's
// mouse (a click captures it) on the UI thread.
void AttachMouseCamera(rex::ui::Window *window);
// Before the window goes: gives the mouse back and stops listening.
void DetachMouseCamera();

// Any thread: the game window holds the mouse (a click captured it).
bool MouseCaptured();

} // namespace renut::game
