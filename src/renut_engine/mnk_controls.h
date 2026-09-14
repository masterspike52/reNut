#pragma once

// =============================================================================
// reNut keyboard + mouse controls
//
// This does NOT emulate an Xbox pad on the host side (rexglue's mnk_mode does
// that, and it feels the way it feels because everything has to survive a round
// trip through XInput: 16-bit stick quantization, the driver's own ramping, and
// then the game's deadzone/response curve on top).
//
// Instead we drive the player's movement input at the point the game itself
// consumes it: sub_8239FF20 builds one frame of normalized pad state (buttons +
// two -1..1 stick vectors + two 0..1 triggers) and hands it to sub_821FBD90,
// which commits it into the pad object every avatar/camera update reads from.
// We hook the handoff and write our keyboard/mouse values straight into that
// block, so movement and camera are exactly what the keys and mouse say -- no
// deadzone, no stick ramp, no XInput in the path at all.
//
// A physical pad still works: our values are merged in, not swapped for the
// pad's, so both can drive the game at once.
//
// Every bind and tuning value is a cvar in the "Nuts&Bolts/Controls" category,
// so they show up in the in-game reNut Settings section, in the F4 settings
// overlay, and persist to renut.toml like the rest of reNut's cvars.
// =============================================================================

#include <cstddef>
#include <string>

namespace rex::ui {
class Window;
}

namespace renut::mnk {

// Attach/detach the host-side keyboard+mouse listener. Called by RenutApp once
// the window exists.
void AttachWindow(rex::ui::Window* window);
void DetachWindow();

// True when the master toggle is on and the game currently owns input.
bool Active();

// Suspends keyboard/mouse control (and releases the cursor) while reNut's own
// controls overlay is up, so rebinding doesn't also drive the game.
void SetOverlayOpen(bool open);

// ---- Rebind capture, driven by the controls overlay -------------------------
// BeginCapture() arms the listener; the next key or mouse button pressed is
// recorded instead of being fed to the game. TakeCaptured() returns it once.
void BeginCapture();
void CancelCapture();
bool IsCapturing();
bool TakeCaptured(std::string& out_key_name);

// ---- Bind table, for the overlay --------------------------------------------
struct BindInfo {
  const char* cvar;     // cvar name, e.g. "mnk_bind_forward"
  const char* label;    // human label, e.g. "Move forward"
  const char* section;  // group header, e.g. "Movement"
};

const BindInfo* BindList(std::size_t& out_count);

}  // namespace renut::mnk