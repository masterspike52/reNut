#include <rexglass/rexglass.h>

#include <algorithm>

#include <imgui_internal.h>

#include "state.h"

namespace rexglass {

State& GetState() {
  static State state;
  return state;
}

void Install(ImGuiContext* context, Config config) {
  State& state = GetState();
  if (state.installed || !context) return;
  state.config = std::move(config);
  state.installed = true;

  // The theme below restyles the current context, so it's this one until Install returns.
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(context);
  ImGuiContextHook hook;
  hook.Type = ImGuiContextHookType_RenderPost;
  hook.Owner = 0;
  hook.Callback = [](ImGuiContext* hooked, ImGuiContextHook*) { DecorateWindows(*hooked); };
  ImGui::AddContextHook(context, &hook);

  if (state.config.theme) {
    ApplyTheme(state.config.scale, state.config.accent);
  } else if (state.config.all_windows) {
    // The game's own look otherwise, but window backgrounds have to be see-through for the glass to show.
    ImVec4* colors = ImGui::GetStyle().Colors;
    for (ImGuiCol color : {ImGuiCol_WindowBg, ImGuiCol_PopupBg, ImGuiCol_TitleBg, ImGuiCol_TitleBgCollapsed, ImGuiCol_MenuBarBg}) colors[color].w = 0.0f;
    colors[ImGuiCol_TitleBgActive].w = std::min(colors[ImGuiCol_TitleBgActive].w, 0.1f);
  }
  ImGui::SetCurrentContext(previous);
}

bool Installed() { return GetState().installed; }

const Config& GetConfig() { return GetState().config; }

void SetStyle(const WindowStyle& style) { GetState().config.style = style; }

ImTextureID Backdrop(ImVec4* uv) {
  State& state = GetState();
  if (!state.installed || !state.config.backdrop) return ImTextureID_Invalid;
  // Asked once a frame: the game's renderer may blur on demand, and the image lasts the frame.
  const int frame = ImGui::GetFrameCount();
  if (state.backdrop_frame != frame) {
    state.backdrop_frame = frame;
    float corners[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    state.backdrop = state.config.backdrop(state.config.blur * state.config.scale, corners);
    state.backdrop_uv = ImVec4(corners[0], corners[1], corners[2], corners[3]);
  }
  if (uv) *uv = state.backdrop_uv;
  return state.backdrop;
}

ImTextureID Screen(bool blurred, ImVec4* uv) {
  State& state = GetState();
  if (!state.installed || !state.config.screen) return ImTextureID_Invalid;
  // Both asked once a frame. The images are the same objects frame to frame; the game's drawer refreshes them as it
  // comes to the draws that use them.
  const int frame = ImGui::GetFrameCount();
  if (state.screen_frame != frame) {
    state.screen_frame = frame;
    const float sigmas[2] = {0.0f, state.config.blur * state.config.scale};
    for (int i = 0; i < 2; ++i) {
      float corners[4] = {0.0f, 0.0f, 1.0f, 1.0f};
      state.screen[i] = state.config.screen(sigmas[i], corners);
      state.screen_uv[i] = ImVec4(corners[0], corners[1], corners[2], corners[3]);
    }
  }
  const int index = blurred ? 1 : 0;
  if (uv) *uv = state.screen_uv[index];
  return state.screen[index];
}

bool BeginWindow(const char* name, bool* open, ImGuiWindowFlags flags) {
  // See-through, so the glass behind shows; the glass draws the edge and the title band.
  const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, clear);
  ImGui::PushStyleColor(ImGuiCol_TitleBg, clear);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(1.0f, 1.0f, 1.0f, 0.04f));
  ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, clear);
  ImGui::PushStyleColor(ImGuiCol_MenuBarBg, clear);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const bool shown = ImGui::Begin(name, open, flags);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(5);
  MarkWindow();
  return shown;
}

void MarkWindow() {
  if (const ImGuiWindow* window = ImGui::GetCurrentWindowRead()) GetState().marked.insert(window->ID);
}

}  // namespace rexglass
