// A glass panel: a rounded shape that shows what's behind it, frosted, bent at its rim like thick glass, tinted,
// with a bright outline, an iridescent edge and a soft shadow. After the Liquid Glass panels of the Toon engine.
//
// Panels are plain ImGui geometry, so they draw on any renderer. What shows through, best first: the screen as drawn
// so far, when the game's renderer hands it over (Config::screen), so a panel refracts the UI under it as well as
// the game; the game's blurred frame (Config::backdrop) or an image the panel brings; a canvas the panel describes;
// otherwise the panel is tinted glass over whatever is under it.
//
//   rexglass::Panel panel;
//   panel.min = ImGui::GetItemRectMin();
//   panel.max = ImGui::GetItemRectMax();
//   panel.tint = ImVec4(0.2f, 0.3f, 0.5f, 0.4f);
//   rexglass::AddPanel(ImGui::GetWindowDrawList(), panel);

#pragma once

#include <functional>

#include <imgui.h>

namespace rexglass {

// What a panel over a canvas shows through itself, when there's no image of what's behind it: the canvas described
// rather than captured, its color at each point and its grid. The panel draws it itself, bent at its rim and split
// into color there, so glass on a node editor's canvas refracts the canvas. In the draw list's coordinates.
struct Canvas {
  std::function<ImU32(const ImVec2& point)> color;
  // Lines at every multiple of `grid_spacing` across and down; 0 for none.
  float grid_spacing = 0.0f;
  ImU32 grid_color = 0;
  float grid_thickness = 1.0f;
};

struct Panel {
  // Where, in the draw list's coordinates.
  ImVec2 min;
  ImVec2 max;
  // Where a point of the draw list is on screen, for a draw list that's transformed before it's drawn (a node
  // editor's canvas), so the panel finds what's under it. Unset: the draw list is in screen coordinates.
  std::function<ImVec2(const ImVec2& point)> to_screen;
  // Sizes are at scale 1; `scale` multiplies them, so rims and shadows follow the UI's scale or a canvas's zoom.
  float rounding = 14.0f;
  float scale = 1.0f;
  // Which corners are rounded, as ImDrawFlags_RoundCorners*; 0 is all of them, as in ImGui.
  ImDrawFlags corners = 0;

  // rgb, and in a how strongly the glass takes the color over what's behind it.
  ImVec4 tint{0.10f, 0.12f, 0.16f, 0.45f};
  // The same for a band across the top, `header_height` tall: a title bar, a node's title. 0 for none.
  ImVec4 header_tint{0.10f, 0.12f, 0.16f, 0.45f};
  float header_height = 0.0f;

  // A glow along the rim: rgb, and in a its strength. For hover and selection.
  ImVec4 glow{0.0f, 0.0f, 0.0f, 0.0f};
  // How hard the rim bends what's behind it; 0 is flat glass.
  float refraction = 1.0f;
  // 0 clear, 1 fully frosted. Without anything to show through, frost makes the tint more solid instead.
  float frost = 0.55f;
  // Brightens the outline, 0 to 1, for hover.
  float highlight = 0.0f;
  // Fades the whole panel, for appearing.
  float opacity = 1.0f;
  bool shadow = true;

  // Shows the game's frame behind it (Config::backdrop). Off for a panel over other UI, such as a node on a canvas,
  // which would show the game through the UI as a hole. The screen as drawn so far (Config::screen) shows the UI
  // under it too, so with it every panel shows what's under it whatever this says.
  bool see_game = true;
  // Off, nothing behind is sampled at all, the screen included: tinted glass with its rim and light only. Cheap, for
  // small widgets sitting on glass already, such as buttons, which would otherwise each ask the renderer for the
  // screen again.
  bool see_through = true;
  // An image of what's behind, covering the whole screen, used instead of the game's frame: a module that blurs the
  // game itself passes its own. uv is the part of the image that covers the screen: u0, v0, u1, v1.
  ImTextureID backdrop = ImTextureID_Invalid;
  ImVec4 backdrop_uv{0.0f, 0.0f, 1.0f, 1.0f};
  // The canvas under the panel, shown through it when there's no image behind it. Kept by the caller until the
  // panel is drawn.
  const Canvas* canvas = nullptr;
};

// Draws the panel into the draw list now: things drawn after it in the same list are on top of it.
void AddPanel(ImDrawList* draw_list, const Panel& panel);

}  // namespace rexglass
