// RexGlass, when the game has it. Built in when the game attaches RexGlass (REXFABRIC_WITH_GLASS), and used when
// the game installed it, so the menus and the node editor are glass in a game that wants glass and plain ImGui
// otherwise. Every function here falls back to plain ImGui.

#pragma once

#include <imgui.h>

#if REXFABRIC_WITH_GLASS
#include <rexglass/panel.h>
#include <rexglass/rexglass.h>
#include <rexglass/widgets.h>
#endif

namespace rexfabric::ui::glass {

// The colors buttons are lit in, by what they do. Alpha is how strongly glass takes the color.
inline constexpr ImVec4 kNeutral{0.62f, 0.70f, 0.88f, 0.42f};
inline constexpr ImVec4 kAccent{0.38f, 0.62f, 1.0f, 0.62f};
inline constexpr ImVec4 kCreate{0.32f, 0.86f, 0.55f, 0.58f};
inline constexpr ImVec4 kDanger{1.0f, 0.42f, 0.38f, 0.58f};
inline constexpr ImVec4 kFunction{0.45f, 0.62f, 1.0f, 0.58f};
inline constexpr ImVec4 kMacro{0.72f, 0.5f, 1.0f, 0.58f};

inline bool Enabled() {
#if REXFABRIC_WITH_GLASS
  return rexglass::Installed();
#else
  return false;
#endif
}

// ImGui::Begin, as a glass window when there's glass. Pair it with ImGui::End.
inline bool BeginWindow(const char* name, bool* open, ImGuiWindowFlags flags) {
#if REXFABRIC_WITH_GLASS
  if (Enabled()) return rexglass::BeginWindow(name, open, flags);
#endif
  return ImGui::Begin(name, open, flags);
}

// The glass theme while it's alive, when there's glass: around a window's Begin and End.
class Theme {
 public:
  Theme() : pushed_(Enabled()) {
#if REXFABRIC_WITH_GLASS
    if (pushed_) rexglass::PushTheme();
#endif
  }
  ~Theme() {
#if REXFABRIC_WITH_GLASS
    if (pushed_) rexglass::PopTheme();
#endif
  }
  Theme(const Theme&) = delete;
  Theme& operator=(const Theme&) = delete;

 private:
  bool pushed_;
};

namespace detail {

// A plain button in `color`, brighter as it's hovered and pressed.
template <typename Draw>
bool Colored(const ImVec4& color, Draw draw) {
  const auto shade = [&color](float alpha) { return ImVec4(color.x * 0.6f, color.y * 0.6f, color.z * 0.6f, alpha); };
  ImGui::PushStyleColor(ImGuiCol_Button, shade(0.55f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, shade(0.8f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x, color.y, color.z, 0.9f));
  const bool pressed = draw();
  ImGui::PopStyleColor(3);
  return pressed;
}

}  // namespace detail

// A button lit in `color`: a pill of tinted glass, or a plain button in the color.
inline bool Button(const char* label, const ImVec4& color = kNeutral, const ImVec2& size = ImVec2(0.0f, 0.0f)) {
#if REXFABRIC_WITH_GLASS
  if (Enabled()) return rexglass::Button(label, color, size);
#endif
  return detail::Colored(color, [&] { return ImGui::Button(label, size); });
}

inline bool SmallButton(const char* label, const ImVec4& color = kNeutral) {
#if REXFABRIC_WITH_GLASS
  if (Enabled()) return rexglass::SmallButton(label, color);
#endif
  return detail::Colored(color, [&] { return ImGui::SmallButton(label); });
}

// ImGui::BeginChild on a pane of glass, or a plain bordered child. Pair it with ImGui::EndChild.
inline bool BeginChild(const char* id, const ImVec2& size, ImGuiChildFlags child_flags, ImGuiWindowFlags flags, const ImVec4& tint) {
#if REXFABRIC_WITH_GLASS
  if (Enabled()) return rexglass::BeginChild(id, size, child_flags, flags, tint);
#endif
  (void)tint;
  return ImGui::BeginChild(id, size, child_flags | ImGuiChildFlags_Borders, flags);
}

}  // namespace rexfabric::ui::glass
