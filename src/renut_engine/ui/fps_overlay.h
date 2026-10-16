#pragma once
#include <rex/cvar.h>
#include "imgui.h"
#include "renut_engine/perf/frame_stats.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

REXCVAR_DECLARE(bool, fps_overlay);
REXCVAR_DECLARE(double, fps_overlay_size);

// Game frame rate (frame_stats), top left: the rate in large type, frame
// times, the game thread's CPU time, the render thread's and the GPU's per
// frame (the largest highlighted), the update and render split of the game
// thread's, and a graph of recent frames. F1.
class FpsOverlay : public renut::gpu::overlay::Dialog {
public:
    void Draw() override {
        // The overlay's own input (gpu/overlay_ui.cpp feeds it the window's keys).
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
            REXCVAR_SET(fps_overlay, !REXCVAR_GET(fps_overlay));
        if (!REXCVAR_GET(fps_overlay))
            return;

        // The graph follows every frame; the numbers change four times a
        // second so they can be read.
        const renut::frame_stats::Snapshot now = renut::frame_stats::Get();
        const auto clock = std::chrono::steady_clock::now();
        if (clock - shown_at_ >= std::chrono::milliseconds(250)) {
            shown_ = now;
            shown_at_ = clock;
        }

        const float big = float(std::clamp(REXCVAR_GET(fps_overlay_size), 12.0, 200.0));
        // Not "small": rpcndr.h defines it.
        const float text = std::max(12.0f, big * 0.5f);

        ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.7f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, text * 0.4f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(text * 0.6f, text * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(text * 0.4f, text * 0.2f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoFocusOnAppearing;
        if (ImGui::Begin("##fps_overlay", nullptr, flags)) {
            const ImVec4 rate_color = RateColor(shown_.fps);
            const ImVec4 label = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);

            ImGui::PushFont(nullptr, big);
            ImGui::TextColored(rate_color, "%.0f", shown_.fps);
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::PushFont(nullptr, text);
            // Baseline of the small "FPS" near the big number's.
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (big - text) * 0.7f);
            ImGui::TextColored(label, "FPS");

            ImGui::TextColored(label, "%.1f ms", shown_.frame_ms);
            ImGui::SameLine();
            ImGui::TextColored(WorstColor(shown_), "worst %.1f", shown_.worst_ms);
            // The largest of the game thread, the render thread and the GPU
            // is what holds the frame rate back.
            const ImVec4 limit = ImVec4(1.0f, 0.85f, 0.2f, 1.0f);
            const double render_ms = shown_.render_thread_known ? shown_.render_thread_ms : 0.0;
            const double gpu_ms = shown_.gpu_known ? shown_.gpu_ms : 0.0;
            const double most = std::max({shown_.cpu_ms, render_ms, gpu_ms});
            ImGui::TextColored(shown_.cpu_ms >= most ? limit : label, "CPU %.1f ms", shown_.cpu_ms);
            if (shown_.render_thread_known) {
                ImGui::TextColored(render_ms >= most ? limit : label, "Render %.1f ms", render_ms);
            }
            if (shown_.gpu_known)
                ImGui::TextColored(gpu_ms >= most ? limit : label, "GPU %.1f ms", gpu_ms);
            else
                ImGui::TextColored(label, "GPU n/a");
            const ImVec4 detail = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
            ImGui::TextColored(detail, "update %.1f  render %.1f", shown_.update_ms,
                               shown_.render_ms);

            DrawGraph(now, text);
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
    }

private:
    static ImVec4 RateColor(double fps) {
        if (fps >= 55.0) return ImVec4(0.3f, 1.0f, 0.4f, 1.0f);
        if (fps >= 28.0) return ImVec4(1.0f, 0.85f, 0.2f, 1.0f);
        return ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    }

    // Red when the worst frame is more than twice the average (a hitch).
    static ImVec4 WorstColor(const renut::frame_stats::Snapshot& s) {
        if (s.frame_ms > 0.0 && s.worst_ms > s.frame_ms * 2.0)
            return ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    }

    // Recent frame lengths as bars, with lines at 60 and 30 fps.
    static void DrawGraph(const renut::frame_stats::Snapshot& s, float text_size) {
        const ImVec2 size(text_size * 9.0f, text_size * 2.2f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(size);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 end(origin.x + size.x, origin.y + size.y);
        draw->AddRectFilled(origin, end, IM_COL32(0, 0, 0, 110));

        float top_ms = 50.0f;
        for (int i = 0; i < s.history_count; ++i)
            top_ms = std::max(top_ms, s.history[i]);
        top_ms = std::min(top_ms, 250.0f);
        auto y_of = [&](float ms) {
            return end.y - size.y * std::min(ms, top_ms) / top_ms;
        };
        for (float ms : {1000.0f / 60.0f, 1000.0f / 30.0f}) {
            const float y = y_of(ms);
            draw->AddLine(ImVec2(origin.x, y), ImVec2(end.x, y), IM_COL32(255, 255, 255, 60));
        }

        const float bar = size.x / renut::frame_stats::Snapshot::kHistory;
        const int first = renut::frame_stats::Snapshot::kHistory - s.history_count;
        for (int i = 0; i < s.history_count; ++i) {
            const float ms = s.history[i];
            const ImU32 color = ms > 1000.0f / 28.0f   ? IM_COL32(255, 80, 80, 230)
                                : ms > 1000.0f / 55.0f ? IM_COL32(255, 215, 60, 230)
                                                       : IM_COL32(80, 255, 110, 230);
            const float x = origin.x + (first + i) * bar;
            draw->AddRectFilled(ImVec2(x, y_of(ms)), ImVec2(x + std::max(bar, 1.0f), end.y), color);
        }
    }

    renut::frame_stats::Snapshot shown_{};
    std::chrono::steady_clock::time_point shown_at_{};
};
