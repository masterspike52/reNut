// Glass widgets for a module's own windows: the glass theme for a stretch of UI, buttons that are pills of tinted
// glass, and child windows on panes of glass.
//
//   rexglass::PushTheme();
//   if (rexglass::BeginWindow("Tools")) {
//     rexglass::BeginChild("##list", ImVec2(200.0f, 0.0f), ImGuiChildFlags_None, 0, ImVec4(0.1f, 0.12f, 0.2f, 0.4f));
//     if (rexglass::Button("Add", ImVec4(0.3f, 0.85f, 0.5f, 0.6f))) Add();
//     ImGui::EndChild();
//   }
//   ImGui::End();
//   rexglass::PopTheme();

#pragma once

#include <imgui.h>

namespace rexglass {

// The glass theme (ApplyTheme) from here to PopTheme, whatever Config::theme says, so a module's glass window looks
// glass in a game that keeps its own look everywhere else. Pairs nest. Around Begin and End, so the window's own
// rounding and padding follow it too.
void PushTheme();
void PopTheme();

// A pill of tinted glass that's a button, lit in `color` as it's hovered and pressed. color's alpha is how strongly
// the glass takes the color. Sizes as ImGui::Button's: 0 fits the label, negative leaves that much room to the right.
bool Button(const char* label, const ImVec4& color, const ImVec2& size = ImVec2(0.0f, 0.0f));
// The same without padding above and below, for rows of tools, as ImGui::SmallButton.
bool SmallButton(const char* label, const ImVec4& color);

// ImGui::BeginChild on a pane of glass tinted `tint`, which shows what's under it: the window's own glass or
// whatever the window draws behind its children. Pair it with ImGui::EndChild, as with BeginChild.
bool BeginChild(const char* id, const ImVec2& size, ImGuiChildFlags child_flags, ImGuiWindowFlags flags, const ImVec4& tint);

}  // namespace rexglass
