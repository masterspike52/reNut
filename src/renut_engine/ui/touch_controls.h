#pragma once
// Android: on-screen controls, a virtual Xbox 360 pad laid out like the
// Minecraft port's touch controls (mc-android/app/touch.h):
//
//   left:   a stick that centres where the thumb lands (bottom left), the
//           D-pad above it, LS beside it
//   right:  A B X Y under the right thumb, RS beside them; anywhere else on
//           the right half is a floating right stick (the camera)
//   top:    LT LB at the left corner, RB RT at the right, BACK and START in
//           the middle
//   layout: edit mode (from the side menu) shows every control; drag them
//           where you like, Done saves, Reset puts them back. Kept in
//           controls.txt next to renut.toml as offsets in screen heights.
//
// The pad is a synthetic input device, so it shares player 1 with a real
// controller. It hides (and disconnects) while a gamepad is connected.

#include "renut_engine/gpu/overlay/overlay_ui.h"

struct ImFont;

namespace rex::input {
class InputSystem;
}

namespace renut::touch {

// Before the game starts: adds the input driver and starts watching touches.
void Install(rex::input::InputSystem *input);

// The game is running (the manager's Play).
void SetStarted(bool started);
// While the side menu is open fingers belong to it.
void SetBlocked(bool blocked);
// The user's on/off switch (side menu), saved with the layout.
bool Enabled();
void SetEnabled(bool enabled);
// Shown at all: started, enabled, no gamepad connected.
bool Visible();
// A gamepad is connected.
bool GamepadConnected();

void BeginEdit();
bool Editing();

// The back gesture since the last call (Android's back, trapped by SDL).
bool TakeBackPress();

// The Andy font for the labels, once the manager has loaded it.
void SetFont(ImFont *font);

// Draws the controls (and the edit mode's buttons).
class Overlay : public renut::gpu::overlay::Dialog {
public:
    void Draw() override;
};

} // namespace renut::touch
