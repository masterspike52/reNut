// Text mods print on the screen while they're being made, as Unreal's Print String does: each line in the screen's
// top right corner for its own few seconds, the newest at the top. Lua prints with rex.screen_text, which the node
// editor's Print String node writes.
//
// Printing is thread safe and cheap. The lines are drawn by an overlay on the app's ImGui, made with the menus, which
// opens a window only while there's something to show.

#pragma once

#include <cstdint>
#include <string>

namespace rex::ui {
class IUISystem;
}

namespace rexfabric::ui {

// Shows `text` for `seconds` in `rgb` (0xRRGGBB), `size` pixels tall (0: the default). A line with a `key` replaces
// the shown line with the same key, where it is. Before SetScreenTextUI, or without a UI, it's dropped.
void PrintToScreen(std::string text, double seconds, uint32_t rgb, float size = 0.0f, std::string key = {});
// Where the lines are drawn. Set once, from CreateMenus.
void SetScreenTextUI(rex::ui::IUISystem& ui);

// A shape drawn anywhere on the screen, under the lines (rex.draw). Positions and sizes are fractions of the
// screen's height and width, so it looks the same at any resolution.
struct ScreenShape {
  enum class Kind : uint8_t { kText, kRect, kLine, kCircle };
  Kind kind = Kind::kText;
  std::string key;           // a shape with a key replaces the one with the same key; empty: none
  std::string text;          // kText
  float x = 0.0f, y = 0.0f;  // its position: text's anchor, a rect's top left, a line's start, a circle's centre
  float x2 = 0.0f, y2 = 0.0f;  // a rect's width and height, a line's end
  float size = 0.03f;        // text's height and a circle's radius, as fractions of the screen's height
  uint32_t rgb = 0xFFFFFF;
  float alpha = 1.0f;
  int align = 0;             // text: -1 left of x, 0 centred, 1 right of x
  bool filled = true;        // rects and circles
  bool background = false;   // text: a dark box behind it
  float thickness = 2.0f;    // pixels: lines and outlines
  float rounding = 0.0f;     // a rect's corners, as a fraction of the screen's height
  double seconds = 0.0;      // how long it stays: 0 until removed with a key, else a tenth of a second
};

// Thread safe: draws `shape` from the next frame. Before SetScreenTextUI, or without a UI, it's dropped.
void DrawOnScreen(ScreenShape shape);
// Removes the shape with `key`; ClearScreen removes those whose keys start with `prefix` (every one for "").
void RemoveFromScreen(const std::string& key);
void ClearScreen(const std::string& prefix);

}  // namespace rexfabric::ui
