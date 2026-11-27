// Small ImGui pieces every RexFabric window shares: text input into a std::string, and the modifier keys the SDK's
// drawer leaves out.

#pragma once

#include <string>

#include <imgui.h>

namespace rexfabric::ui::widgets {

// The colors of a status or a problem.
inline constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};
inline constexpr ImVec4 kWarningColor{1.0f, 0.75f, 0.35f, 1.0f};
inline constexpr ImVec4 kGoodColor{0.5f, 0.9f, 0.5f, 1.0f};
// How long a status message stays, in seconds.
inline constexpr double kStatusSeconds = 5.0;

// ImGui text input into a std::string, which grows as it's typed into.
bool InputString(const char* label, std::string& text, ImGuiInputTextFlags flags = 0, const char* hint = nullptr);
bool InputStringMultiline(const char* label, std::string& text, const ImVec2& size, ImGuiInputTextFlags flags = 0);

// The SDK's ImGui drawer reports Ctrl, Shift and Alt as keys but not as ImGui's modifiers, which ImGui rebuilds
// io.KeyCtrl and the rest from each frame, so its shortcuts never see them. Called each frame while a window that
// wants them is open, the modifiers follow their keys, a frame behind, so Ctrl+S and text fields' shortcuts work.
void SyncModifiers(ImGuiIO& io);

}  // namespace rexfabric::ui::widgets
