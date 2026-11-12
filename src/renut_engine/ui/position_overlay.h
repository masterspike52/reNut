#pragma once
#include "imgui.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

#include <algorithm>
#include <atomic>

namespace renut::debug {

// The setting renut.debug_player_position (frameHooks.cpp).
bool PlayerPositionEnabled();

// The player's Banjo, sampled on the game thread each frame by frameHooks.cpp's hook on the player's
// camera update (sub_821CD438), on foot or driving.
struct PlayerPosition {
    std::atomic<bool> known{false};
    std::atomic<float> x{0.0f}, y{0.0f}, z{0.0f};
};
inline PlayerPosition g_player_position;

}  // namespace renut::debug

// The position overlay (debug_player_position), top right: Banjo's coordinates, as the world's
// markers, doors and renut.doors.add take them.
class PositionOverlay : public renut::gpu::overlay::Dialog {
public:
    void Draw() override {
        if (!renut::debug::PlayerPositionEnabled())
            return;
        const renut::debug::PlayerPosition &p = renut::debug::g_player_position;
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float size = std::max(14.0f, display.y * 0.022f);
        ImGui::SetNextWindowPos(ImVec2(display.x - 12.0f, 12.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.7f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, size * 0.4f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(size * 0.6f, size * 0.4f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoFocusOnAppearing;
        if (ImGui::Begin("##renut_position", nullptr, flags)) {
            ImGui::PushFont(nullptr, size);
            if (p.known.load(std::memory_order_acquire))
                ImGui::Text("Banjo  %.1f, %.1f, %.1f", p.x.load(std::memory_order_relaxed),
                            p.y.load(std::memory_order_relaxed), p.z.load(std::memory_order_relaxed));
            else
                ImGui::TextUnformatted("Banjo  (not in a level)");
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
};
