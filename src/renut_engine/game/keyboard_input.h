/**
 * @file    game/keyboard_input.h
 * @brief   Bound keys and mouse buttons play player 1's pad (keyboard_input.cpp).
 */
#pragma once

#include <cstdint>

namespace rex::ui {
class Window;
}

namespace renut::game {

// Any thread, once the window exists: starts taking the window's keys, on the
// UI thread.
void AttachKeyboard(rex::ui::Window *window);
// Before the window goes.
void DetachKeyboard();

// Game thread (XInputGetKeystroke, controllers.cpp): the next pad keystroke
// the bound keys make for player 1, as the XUI menus read them: a VK_PAD_*
// code and XINPUT_KEYSTROKE flags (down, up, or a held key's repeat).
bool NextKeystroke(uint16_t &virtual_key, uint16_t &flags);

} // namespace renut::game
