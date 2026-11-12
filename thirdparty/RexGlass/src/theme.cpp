// The glass theme, after the Toon engine's: rounded shapes, see-through window backgrounds so the glass shows, and
// translucent widgets that sit on it like frosted pills. Fonts are left to the game.

#include <rexglass/rexglass.h>

namespace rexglass {
namespace {

ImVec4 White(float alpha) { return ImVec4(1.0f, 1.0f, 1.0f, alpha); }

ImVec4 Tinted(const Accent& accent, float alpha) { return ImVec4(accent.r, accent.g, accent.b, alpha); }

void ApplySizes(ImGuiStyle& style) {
  style.WindowPadding = ImVec2(12.0f, 10.0f);
  style.FramePadding = ImVec2(9.0f, 5.0f);
  style.ItemSpacing = ImVec2(8.0f, 6.0f);
  style.ItemInnerSpacing = ImVec2(6.0f, 5.0f);
  style.CellPadding = ImVec2(6.0f, 4.0f);
  style.IndentSpacing = 18.0f;
  style.ScrollbarSize = 10.0f;
  style.GrabMinSize = 10.0f;

  // The glass draws window and popup edges; ImGui's would sit on top of the rim.
  style.WindowBorderSize = 0.0f;
  style.PopupBorderSize = 0.0f;
  style.ChildBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.TabBorderSize = 0.0f;

  style.WindowRounding = 14.0f;
  style.ChildRounding = 10.0f;
  style.PopupRounding = 12.0f;
  style.FrameRounding = 8.0f;
  style.GrabRounding = 8.0f;
  style.ScrollbarRounding = 8.0f;
  style.TabRounding = 9.0f;
  style.WindowMenuButtonPosition = ImGuiDir_None;
  style.SeparatorTextBorderSize = 1.0f;
}

void ApplyColors(ImGuiStyle& style, const Accent& accent) {
  ImVec4* colors = style.Colors;
  const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);

  colors[ImGuiCol_Text] = ImVec4(0.95f, 0.96f, 0.98f, 1.0f);
  colors[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.65f, 0.72f, 1.0f);
  colors[ImGuiCol_TextSelectedBg] = Tinted(accent, 0.35f);
  colors[ImGuiCol_TextLink] = Tinted(accent, 1.0f);

  // Glass replaces these.
  colors[ImGuiCol_WindowBg] = clear;
  colors[ImGuiCol_PopupBg] = clear;
  colors[ImGuiCol_TitleBg] = clear;
  colors[ImGuiCol_TitleBgActive] = White(0.04f);
  colors[ImGuiCol_TitleBgCollapsed] = clear;
  colors[ImGuiCol_MenuBarBg] = clear;

  colors[ImGuiCol_ChildBg] = White(0.025f);
  colors[ImGuiCol_Border] = White(0.08f);
  colors[ImGuiCol_BorderShadow] = clear;

  colors[ImGuiCol_FrameBg] = White(0.07f);
  colors[ImGuiCol_FrameBgHovered] = White(0.11f);
  colors[ImGuiCol_FrameBgActive] = White(0.15f);

  colors[ImGuiCol_Button] = White(0.09f);
  colors[ImGuiCol_ButtonHovered] = White(0.16f);
  colors[ImGuiCol_ButtonActive] = Tinted(accent, 0.45f);

  colors[ImGuiCol_Header] = Tinted(accent, 0.28f);
  colors[ImGuiCol_HeaderHovered] = White(0.10f);
  colors[ImGuiCol_HeaderActive] = Tinted(accent, 0.4f);

  colors[ImGuiCol_CheckMark] = Tinted(accent, 1.0f);
  colors[ImGuiCol_SliderGrab] = Tinted(accent, 0.85f);
  colors[ImGuiCol_SliderGrabActive] = Tinted(accent, 1.0f);

  colors[ImGuiCol_Separator] = White(0.08f);
  colors[ImGuiCol_SeparatorHovered] = Tinted(accent, 0.6f);
  colors[ImGuiCol_SeparatorActive] = Tinted(accent, 0.9f);

  colors[ImGuiCol_ResizeGrip] = clear;
  colors[ImGuiCol_ResizeGripHovered] = Tinted(accent, 0.5f);
  colors[ImGuiCol_ResizeGripActive] = Tinted(accent, 0.8f);

  colors[ImGuiCol_ScrollbarBg] = clear;
  colors[ImGuiCol_ScrollbarGrab] = White(0.12f);
  colors[ImGuiCol_ScrollbarGrabHovered] = White(0.2f);
  colors[ImGuiCol_ScrollbarGrabActive] = White(0.28f);

  // Tabs are pills on the tab bar: the shown one lit, the others faint.
  colors[ImGuiCol_Tab] = White(0.0f);
  colors[ImGuiCol_TabHovered] = White(0.12f);
  colors[ImGuiCol_TabSelected] = White(0.13f);
  colors[ImGuiCol_TabSelectedOverline] = Tinted(accent, 0.9f);
  colors[ImGuiCol_TabDimmed] = White(0.0f);
  colors[ImGuiCol_TabDimmedSelected] = White(0.07f);
  colors[ImGuiCol_TabDimmedSelectedOverline] = clear;

  colors[ImGuiCol_TableHeaderBg] = White(0.05f);
  colors[ImGuiCol_TableBorderStrong] = White(0.1f);
  colors[ImGuiCol_TableBorderLight] = White(0.05f);
  colors[ImGuiCol_TableRowBg] = clear;
  colors[ImGuiCol_TableRowBgAlt] = White(0.02f);

  colors[ImGuiCol_PlotLines] = Tinted(accent, 0.9f);
  colors[ImGuiCol_PlotHistogram] = Tinted(accent, 0.9f);
  colors[ImGuiCol_DragDropTarget] = Tinted(accent, 0.9f);
  colors[ImGuiCol_NavCursor] = Tinted(accent, 0.9f);
  colors[ImGuiCol_NavWindowingHighlight] = White(0.7f);
  colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.3f);
  colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.35f);
}

}  // namespace

void ApplyTheme(float scale, Accent accent) {
  ImGuiStyle& style = ImGui::GetStyle();
  // The game's font sizes stay as they were.
  const float font_size = style.FontSizeBase;
  const float font_scale = style.FontScaleMain;
  const float font_dpi = style.FontScaleDpi;
  style = ImGuiStyle();
  ApplySizes(style);
  ApplyColors(style, accent);
  style.ScaleAllSizes(scale);
  style.FontSizeBase = font_size;
  style.FontScaleMain = font_scale;
  style.FontScaleDpi = font_dpi;
}

}  // namespace rexglass
