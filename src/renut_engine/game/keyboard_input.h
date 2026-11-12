#pragma once

#include <cstdint>

namespace rex::ui {
class IUISystem;
}

namespace renut::game {

// Any thread, once the window exists: starts taking the window's keys, on the
// UI thread. Before AttachMouseCamera, whose listener must come after this one.
void AttachKeyboard(rex::ui::IUISystem &ui);
// Before the window goes.
void DetachKeyboard();

// Game thread: the next VK_PAD_* keystroke and XINPUT_KEYSTROKE flags the bound
// keys make for player 1, for the XUI menus.
bool NextKeystroke(uint16_t &virtual_key, uint16_t &flags);

// Any thread: the XINPUT_GAMEPAD wButtons the bound keys hold now, 0 while the
// keys are inactive.
uint32_t KeyboardButtons();

// Before rexfabric::Start: gives Lua mods' rex.input the window's keys, whether
// or not renut_keyboard is on. Keys count while no overlay window or the Guide is up.
void InstallModInput();

// Any thread: the cursor over the window (0..1 across and down) and the window's width / height;
// false before the cursor has been over it.
bool CursorPosition(float &x, float &y, float &aspect);

// Any thread: while a mod holds the game's input, port `port`'s last pad entry (40 bytes, as the game keeps it) that
// the game was kept from. False when it isn't held, or for another port.
bool HeldPadEntry(uint32_t port, uint8_t (&entry)[40]);

} // namespace renut::game
