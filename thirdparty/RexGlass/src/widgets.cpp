// Glass widgets: the theme for a stretch of UI, glass buttons, glass child windows.

#include <rexglass/widgets.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include <imgui_internal.h>

#include <rexglass/panel.h>
#include <rexglass/rexglass.h>

namespace rexglass {
namespace {

// How quickly a button lights up and fades, per second.
constexpr float kHoverRate = 14.0f;
// How long a button's light is kept after it was last drawn, in frames, before it's forgotten.
constexpr int kForgetFrames = 120;

// The styles PushTheme replaced, innermost last.
std::vector<ImGuiStyle>& SavedStyles() {
  static std::vector<ImGuiStyle> saved;
  return saved;
}

struct Light {
  float hover = 0.0f;
  int frame = 0;
};

// Each button's hover light, eased, by id.
float EaseHover(ImGuiID id, bool hovered) {
  static std::unordered_map<ImGuiID, Light> lights;
  const int frame = ImGui::GetFrameCount();
  Light& light = lights[id];
  const float target = hovered ? 1.0f : 0.0f;
  light.hover += (target - light.hover) * (1.0f - std::exp(-kHoverRate * ImGui::GetIO().DeltaTime));
  light.frame = frame;
  if (frame % kForgetFrames == 0) std::erase_if(lights, [frame](const auto& entry) { return frame - entry.second.frame > kForgetFrames; });
  return light.hover;
}

float Scale() { return std::max(GetConfig().scale, 0.01f); }

bool GlassButton(const char* label, const ImVec4& color, const ImVec2& size_arg, const ImVec2& padding) {
  const ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems) return false;
  const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
  const ImVec2 size = ImGui::CalcItemSize(size_arg, text.x + padding.x * 2.0f, text.y + padding.y * 2.0f);
  const bool pressed = ImGui::InvisibleButton(label, size);
  const bool held = ImGui::IsItemActive();
  const float hover = EaseHover(ImGui::GetItemID(), ImGui::IsItemHovered());
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();

  // Tinted glass only: a button sits on glass already, and sampling what's under each one would cost a capture each.
  Panel panel;
  panel.min = min;
  panel.max = max;
  panel.scale = Scale();
  panel.rounding = size.y * 0.5f / panel.scale;
  const float press = held ? 1.0f : 0.0f;
  panel.tint = ImVec4(color.x, color.y, color.z, std::min(color.w * (0.55f + 0.3f * hover + 0.25f * press), 1.0f));
  panel.glow = ImVec4(color.x, color.y, color.z, 0.12f + 0.4f * hover + 0.35f * press);
  panel.highlight = hover;
  panel.refraction = 0.0f;
  panel.frost = 0.5f;
  panel.shadow = false;
  panel.see_through = false;
  // Within BeginDisabled, ImGui fades everything by the style's alpha.
  panel.opacity = ImGui::GetStyle().Alpha;
  AddPanel(window->DrawList, panel);

  const ImRect bounds(min, max);
  ImGui::RenderTextClipped(ImVec2(min.x + padding.x, min.y + padding.y), ImVec2(max.x - padding.x, max.y - padding.y), label, nullptr, &text, ImVec2(0.5f, 0.5f), &bounds);
  return pressed;
}

}  // namespace

void PushTheme() {
  ImGuiStyle& style = ImGui::GetStyle();
  SavedStyles().push_back(style);
  const Config& config = GetConfig();
  ApplyTheme(config.scale, config.accent);
  // Without glass on every window, popups opened here aren't glass, and the theme's see-through popup background
  // would leave them unreadable over busy content.
  if (!config.all_windows) {
    const ImVec4 tint = config.style.popup_tint;
    style.Colors[ImGuiCol_PopupBg] = ImVec4(tint.x, tint.y, tint.z, 0.96f);
  }
}

void PopTheme() {
  std::vector<ImGuiStyle>& saved = SavedStyles();
  if (saved.empty()) return;
  ImGui::GetStyle() = saved.back();
  saved.pop_back();
}

bool Button(const char* label, const ImVec4& color, const ImVec2& size) { return GlassButton(label, color, size, ImGui::GetStyle().FramePadding); }

bool SmallButton(const char* label, const ImVec4& color) { return GlassButton(label, color, ImVec2(0.0f, 0.0f), ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f)); }

bool BeginChild(const char* id, const ImVec2& size, ImGuiChildFlags child_flags, ImGuiWindowFlags flags, const ImVec4& tint) {
  // The glass is the child's background and border.
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  const bool shown = ImGui::BeginChild(id, size, child_flags, flags);
  ImGui::PopStyleColor(2);

  const ImGuiWindow* window = ImGui::GetCurrentWindow();
  const Config& config = GetConfig();
  Panel panel;
  panel.min = window->Pos;
  panel.max = ImVec2(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y);
  panel.scale = Scale();
  panel.rounding = ImGui::GetStyle().ChildRounding / panel.scale;
  panel.tint = tint;
  panel.frost = config.style.frost;
  panel.refraction = config.style.refraction;
  // On the window's glass: the game shows through the window already, not through holes in it.
  panel.see_game = false;

  // In the parent's draw list, which is drawn before the child's, so behind everything of the child's own, its
  // scrollbar included; the shadow falls on the parent around it.
  ImGuiWindow* parent = window->ParentWindow;
  if (!parent) return shown;
  parent->DrawList->PushClipRect(parent->ClipRect.Min, parent->ClipRect.Max, false);
  AddPanel(parent->DrawList, panel);
  parent->DrawList->PopClipRect();
  return shown;
}

}  // namespace rexglass
