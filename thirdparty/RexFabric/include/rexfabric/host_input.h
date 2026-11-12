// The keyboard and mouse behind Lua's rex.input.
//
// How a game samples input, whether it holds the mouse, and which of its own menus count as in front of the
// game are the game's business, so the game installs a sampler and rex.input reads through it. Until one is
// installed rex.input reports no input, so a mod's hotkeys stay quiet rather than erroring.
//
//   rexfabric::HostInput input;
//   input.resolve_key = [](std::string_view name) { return uint16_t(REX_INTERFACE(rex::hid::IInputSystem)->ParseVirtualKey(name)); };
//   input.key_down = [](uint16_t key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
//   input.accepts_input = [] { return WindowFocused() && !MenuOpen(); };
//   rexfabric::SetHostInput(std::move(input));
//
// key_down reports the raw key state; a mod that wants the usual behaviour checks rex.input.accepts first.
// Mods call these from guest threads and the frame thread, so they must be safe from any thread.

#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

namespace rexfabric {

// Any member may be left empty; the matching Lua call then reports no input.
struct HostInput {
  // A key name ("W", "F9", "Shift") as the code key_down takes, or 0 for a name this game doesn't know.
  // Asked once per name.
  std::function<uint16_t(std::string_view name)> resolve_key;
  std::function<bool(uint16_t key)> key_down;
  // 0 left, 1 right, 2 middle, 3 and 4 the side buttons.
  std::function<bool(int button)> mouse_button_down;
  // Motion and wheel since the last call, which resets them. The wheel is 120 per detent.
  std::function<std::pair<int32_t, int32_t>()> consume_mouse_delta;
  std::function<int32_t()> consume_wheel;
  // The window has focus and nothing in front of the game (a menu, an overlay) wants the input.
  std::function<bool()> accepts_input;
  // Holds the game's own input while a mod drives something itself, such as the camera, or lets it go.
  std::function<void(bool held)> set_guest_input_held;
  // Throws away motion piled up so far.
  std::function<void()> flush;
};

// Call once, from setup.
void SetHostInput(HostInput input);
const HostInput& GetHostInput();
bool HasHostInput();

}  // namespace rexfabric
