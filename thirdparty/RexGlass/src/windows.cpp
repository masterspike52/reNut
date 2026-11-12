// Glass behind windows. ImGui has drawn every window by the time Render finishes, so the panels are built then, in
// a scratch draw list, and spliced into each window's draw list ahead of its own commands: behind the window and its
// children, and in front of the windows under it. The renderer then draws them as any other geometry.

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include <imgui_internal.h>

#include <rexglass/panel.h>

#include "state.h"

namespace rexglass {
namespace {

// A modal draws the rectangle that dims everything behind it at the front of its draw list. The glass goes after
// it, or the dimming would cover the modal's own glass.
int FirstCommandAfterDimming(const ImGuiWindow& window) {
  const ImDrawList& list = *window.DrawList;
  if ((window.Flags & ImGuiWindowFlags_Modal) == 0 || list.CmdBuffer.Size < 2) return 0;
  const ImDrawCmd& first = list.CmdBuffer[0];
  const ImVec2 viewport = window.Viewport->Pos;
  const bool covers_viewport = first.ClipRect.x < viewport.x + 1.0f && first.ClipRect.y < viewport.y + 1.0f;
  return first.UserCallback == nullptr && first.ElemCount == 6 && covers_viewport ? 1 : 0;
}

bool Floating(const ImGuiWindow& window) { return (window.Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) != 0; }

bool IsGlass(const ImGuiWindow& window, const State& state) {
  if (state.marked.contains(window.ID)) return true;
  if (!state.config.all_windows) return false;
  const ImGuiWindowFlags flags = window.Flags;
  // Child windows sit on their parent's glass.
  if ((flags & ImGuiWindowFlags_ChildWindow) != 0 && !Floating(window)) return false;
  // Windows without a background, and overlays that take no input at all, aren't panels. NoInputs is three flags and
  // menus carry one of them, so an overlay has all of them.
  return (flags & ImGuiWindowFlags_NoBackground) == 0 && (flags & ImGuiWindowFlags_NoInputs) != ImGuiWindowFlags_NoInputs;
}

Panel WindowPanel(const ImGuiWindow& window, const Config& config) {
  // Panel sizes are at scale 1; the window's are already scaled.
  const float scale = std::max(config.scale, 0.01f);
  const bool floating = Floating(window);
  Panel panel;
  panel.min = window.Pos;
  panel.max = ImVec2(window.Pos.x + window.Size.x, window.Pos.y + window.Size.y);
  panel.rounding = window.WindowRounding / scale;
  panel.scale = scale;
  panel.tint = floating ? config.style.popup_tint : config.style.tint;
  panel.header_tint = config.style.header_tint;
  panel.header_height = (window.Flags & ImGuiWindowFlags_NoTitleBar) != 0 ? 0.0f : window.TitleBarHeight / scale;
  panel.frost = config.style.frost;
  panel.refraction = config.style.refraction;
  // Popups float over other windows; the game frame would show through them as a hole.
  panel.see_game = !floating;
  return panel;
}

// Makes the run of vertices from `first` see-through when it's one fill of `rect`: vertices of one color (its
// anti-aliased fringe shares the color) whose bounds are the rectangle. Returns where the run ended, or `first` when
// it isn't that fill, leaving it alone.
int ClearFill(ImVector<ImDrawVert>& vertices, int first, const ImRect& rect) {
  if (first >= vertices.Size || rect.GetWidth() <= 0.0f || rect.GetHeight() <= 0.0f) return first;
  const ImU32 color = vertices[first].col & ~IM_COL32_A_MASK;
  ImRect bounds(vertices[first].pos, vertices[first].pos);
  int end = first;
  while (end < vertices.Size && (vertices[end].col & ~IM_COL32_A_MASK) == color) bounds.Add(vertices[end++].pos);
  // Rounding and the fringe leave the bounds within a pixel or two of the rectangle.
  constexpr float kSlack = 2.0f;
  const bool fills = std::abs(bounds.Min.x - rect.Min.x) <= kSlack && std::abs(bounds.Min.y - rect.Min.y) <= kSlack && std::abs(bounds.Max.x - rect.Max.x) <= kSlack && std::abs(bounds.Max.y - rect.Max.y) <= kSlack;
  if (!fills) return first;
  for (int i = first; i < end; ++i) vertices[i].col &= ~IM_COL32_A_MASK;
  return end;
}

// A window's own background and title bar would cover its glass, and games set them per window
// (SetNextWindowBgAlpha), which a style can't undo. ImGui draws them first in the window's draw list, background then
// title bar, so they're found there by their rectangles and made see-through; the glass's tint and header band take
// their place. A fill that isn't there (an already see-through background isn't drawn at all) is skipped.
void ClearBackground(ImGuiWindow& window) {
  ImVector<ImDrawVert>& vertices = window.DrawList->VtxBuffer;
  const ImVec2 max(window.Pos.x + window.Size.x, window.Pos.y + window.Size.y);
  const float title = (window.Flags & ImGuiWindowFlags_NoTitleBar) != 0 ? 0.0f : window.TitleBarHeight;
  int next = 0;
  if (!window.Collapsed && (window.Flags & ImGuiWindowFlags_NoBackground) == 0) {
    const int after = ClearFill(vertices, next, ImRect(ImVec2(window.Pos.x, window.Pos.y + title), max));
    // A title bar the background's own color merges with it into one fill of the whole window.
    next = after != next ? after : ClearFill(vertices, next, ImRect(window.Pos, max));
  }
  if (title > 0.0f) next = ClearFill(vertices, next, window.TitleBarRect());
  if ((window.Flags & ImGuiWindowFlags_MenuBar) != 0) ClearFill(vertices, next, window.MenuBarRect());
}

// Moves the scratch list's geometry into `list`, its commands going in at `position`.
void Splice(ImDrawList& list, const ImDrawList& scratch, int position, ImDrawData& data) {
  const int vertex_base = list.VtxBuffer.Size;
  const int index_base = list.IdxBuffer.Size;
  // The SDK's renderer reads indices as they are, without vertex offsets, so a list stays within 16 bit indices.
  if (sizeof(ImDrawIdx) == 2 && vertex_base + scratch.VtxBuffer.Size > 0xFFFF) return;

  list.VtxBuffer.reserve(vertex_base + scratch.VtxBuffer.Size);
  for (const ImDrawVert& vertex : scratch.VtxBuffer) list.VtxBuffer.push_back(vertex);
  list.IdxBuffer.reserve(index_base + scratch.IdxBuffer.Size);
  for (const ImDrawIdx index : scratch.IdxBuffer) list.IdxBuffer.push_back(static_cast<ImDrawIdx>(index + vertex_base));

  position = std::clamp(position, 0, list.CmdBuffer.Size);
  for (const ImDrawCmd& source : scratch.CmdBuffer) {
    if (source.ElemCount == 0) continue;
    ImDrawCmd command = source;
    command.IdxOffset += static_cast<unsigned int>(index_base);
    command.VtxOffset = 0;
    list.CmdBuffer.insert(list.CmdBuffer.Data + position++, command);
  }
  data.TotalVtxCount += scratch.VtxBuffer.Size;
  data.TotalIdxCount += scratch.IdxBuffer.Size;
}

}  // namespace

void DecorateWindows(ImGuiContext& context) {
  State& state = GetState();
  ImDrawData* data = ImGui::GetDrawData();
  if (data && data->Valid && (state.config.all_windows || !state.marked.empty())) {
    std::unordered_set<const ImDrawList*> drawn(data->CmdLists.begin(), data->CmdLists.end());
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    static ImDrawList scratch(ImGui::GetDrawListSharedData());
    for (ImGuiWindow* window : context.Windows) {
      if (!window->Active || window->Hidden || !drawn.contains(window->DrawList) || !IsGlass(*window, state)) continue;
      ClearBackground(*window);
      scratch._ResetForNewFrame();
      scratch.PushTexture(context.IO.Fonts->TexRef);
      scratch.PushClipRect(viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y));
      AddPanel(&scratch, WindowPanel(*window, state.config));
      Splice(*window->DrawList, scratch, FirstCommandAfterDimming(*window), *data);
    }
  }
  state.marked.clear();
}

}  // namespace rexglass
