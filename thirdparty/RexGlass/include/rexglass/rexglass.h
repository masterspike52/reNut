// RexGlass: glass for ImGui in rexglue projects, after the Liquid Glass UI of the Toon engine.
//
//   rex::Result<void> OnInit() override {
//     rexglass::Install(GetUI().GetImGuiIO().Ctx, {.all_windows = true, .theme = true});
//     return rex::Ok();
//   }
//
// Windows become glass panels (rexglass/panel.h): every ImGui window with all_windows, or the ones a module opens
// with rexglass::BeginWindow. Glass is drawn as ordinary ImGui geometry, so it works on any renderer. A game whose
// renderer can blur its own frame passes it in as Config::backdrop, and the glass then shows the game through it,
// frosted and bent at the rim.
//
// Other modules use RexGlass when the game attaches it: they build their glass look when the target has the
// REXGLASS_ATTACHED property, and use it when Installed() says the game set it up.

#pragma once

#include <functional>

#include <imgui.h>

namespace rexglass {

// How windows look as glass. Colors are straight alpha; alpha is how strongly the glass takes the color.
struct WindowStyle {
  ImVec4 tint{0.05f, 0.06f, 0.09f, 0.52f};         // windows
  ImVec4 header_tint{0.08f, 0.09f, 0.13f, 0.55f};  // their title bars
  ImVec4 popup_tint{0.07f, 0.08f, 0.11f, 0.66f};   // menus, popups and tooltips, which float over busy content
  float frost = 0.92f;
  float refraction = 0.55f;
};

// The theme's accent: selection, check marks, sliders.
struct Accent {
  float r = 0.42f;
  float g = 0.66f;
  float b = 1.0f;
};

struct Config {
  // Every ImGui window is glass. Off, only the windows modules open with BeginWindow are.
  bool all_windows = false;
  // Restyles ImGui for glass: rounded shapes, see-through window backgrounds, translucent widgets. A game that has
  // a look of its own leaves it off; glass windows are still see-through.
  bool theme = false;
  Accent accent;
  WindowStyle style;
  // The UI scale, so rims, outlines and the theme's sizes grow with it.
  float scale = 1.0f;

  // The game's frame, blurred by a Gaussian of `sigma` window pixels, as an image the UI can draw, or
  // ImTextureID_Invalid when there's none this frame. uv is the part of the image that covers the window (u0, v0, u1, v1). Asked once a
  // frame, while the UI draws. From the game's own renderer; unset, glass has nothing to show through and is
  // tinted only. The same as RexLiveless's Config::blurred_game_frame, so a game can pass one function to both.
  std::function<ImTextureID(float sigma, float uv[4])> backdrop;
  // The screen as drawn so far, blurred by a Gaussian of `sigma` window pixels (0 for sharp), as an image the UI can
  // draw, or ImTextureID_Invalid. From a game whose renderer draws the UI itself: when its drawer comes to a draw using the image,
  // it first refreshes it from what it has drawn, if something was drawn there since. So every panel shows what's
  // really under it, the game and the UI alike: other windows, a node editor's wires and nodes. Asked once a frame
  // for each sigma; uv as for backdrop. Set, it takes the place of backdrop.
  std::function<ImTextureID(float sigma, float uv[4])> screen;
  // How blurred the backdrop is, in window pixels at scale 1.
  float blur = 12.0f;
};

// Sets RexGlass up on an ImGui context: the UI system's, GetUI().GetImGuiIO().Ctx. Call once, from OnInit on the UI
// thread.
void Install(ImGuiContext* context, Config config);
bool Installed();
const Config& GetConfig();
// Changes the look after Install, as a settings menu might.
void SetStyle(const WindowStyle& style);

// The game's blurred frame for this frame, and the part of it covering the screen, or ImTextureID_Invalid.
ImTextureID Backdrop(ImVec4* uv);
// The screen as drawn so far (Config::screen), blurred by the config's blur or sharp, or ImTextureID_Invalid.
ImTextureID Screen(bool blurred, ImVec4* uv);

// A window of glass whatever all_windows says: ImGui::Begin with a see-through background and border, marked as
// glass. Always pair it with ImGui::End, as with Begin.
bool BeginWindow(const char* name, bool* open = nullptr, ImGuiWindowFlags flags = 0);
// Makes the current window glass this frame, for a window begun some other way. Its background should be
// see-through, or it hides the glass.
void MarkWindow();

// Restyles ImGui for glass, as Config::theme does at Install.
void ApplyTheme(float scale = 1.0f, Accent accent = {});

// A slowly drifting backdrop of colored light, for a canvas under glass panels (a node editor). Draws over
// [min, max] in the draw list. `seconds` drives the drift. With `rounding`, the corners `corners` names
// (ImDrawFlags_RoundCorners*, 0 for all) are rounded, so the aurora can fill a window.
void DrawAurora(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max, float seconds, float opacity = 1.0f, float rounding = 0.0f, ImDrawFlags corners = 0);
// The aurora's color at a point, as DrawAurora draws it over [min, max], for glass over it to show it through itself
// (Panel::canvas).
ImU32 AuroraColor(const ImVec2& min, const ImVec2& max, float seconds, const ImVec2& point);

}  // namespace rexglass
