#pragma once

#include "imgui.h"
#include "renut_engine/Fps.h"
#include "renut_engine/game_activity_stats.h"
#include "renut_engine/trace_stats.h"

// Content-only: no window/Begin/End, no hotkey. Hosted as a tab inside
// DebugHubOverlayDialog (see debug_hub_overlay.h), which owns the single F5
// toggle shared by all the renderer debug panels.
//
// Only fields with a real, confirmed data source are shown (2026-08-19: audited
// every field against cmake/patches/rexglue_sdk_native_renderer.patch --
// removed everything that was never set by either the SDK trace or
// d3d9_draw_stats.cpp's fallback, rather than showing permanent "not
// measured" placeholders for fields nothing will ever populate).
namespace renut::overlays::render_stats {

namespace detail {

inline bool Has(const renut::trace_stats::Snapshot& trace, const char* name) {
    return trace.values.find(name) != trace.values.end();
}

inline double Value(const renut::trace_stats::Snapshot& trace, const char* name) {
    const auto found = trace.values.find(name);
    return found == trace.values.end() ? 0.0 : found->second;
}

inline void Time(const renut::trace_stats::Snapshot& trace, const char* label, const char* value) {
    if (!Has(trace, value)) return;
    ImGui::Text("%-28s %7.2f ms", label, Value(trace, value));
}

inline void Count(const renut::trace_stats::Snapshot& trace, const char* label, const char* value) {
    if (!Has(trace, value)) return;
    ImGui::Text("%-28s %7.0f", label, Value(trace, value));
}

inline void Rate(const renut::trace_stats::Snapshot& trace, const char* label, const char* value) {
    if (!Has(trace, value)) return;
    ImGui::Text("%-28s %7.1f ns", label, Value(trace, value));
}

inline void DrawTrace(const renut::trace_stats::Snapshot& trace) {
    ImGui::Separator();
    ImGui::TextUnformatted("GPU trace: latest frame");
    ImGui::TextDisabled("%s", trace.path.c_str());
    Time(trace, "Frame", "frame_ms");
    Rate(trace, "Draw submission per draw", "ns_per_draw");
    Time(trace, "CPU blocked on GPU", "gpuwait_ms");
    Time(trace, "Draw submission", "issuedraw_ms");
    Time(trace, "Texture requests", "tex_ms");
    Time(trace, "Descriptor bindings", "bind_ms");
    Time(trace, "Vertex residency", "vfetch_ms");
    Time(trace, "Samplers", "samp_ms");
    Time(trace, "Render targets", "rt_ms");
    Time(trace, "Pipeline setup", "pipe_ms");
    Time(trace, "Present", "swap_ms");
    Time(trace, "EDRAM resolve", "issuecopy_ms");
    Time(trace, "Primitive processing", "prim_ms");
    Time(trace, "Shader translation", "shadertrans_ms");
    Time(trace, "Memexport range sync", "memexport_ms");
    Time(trace, "Render pass enter/barriers", "renderpass_enter_ms");
    Time(trace, "Draw command submission", "drawcmd_ms");
    Time(trace, "System constants", "sysconst_ms");
    Time(trace, "Dynamic state", "dyn_ms");

    ImGui::Separator();
    ImGui::TextUnformatted("GPU submission visibility");
    Count(trace, "Draws submitted", "draws");
    Count(trace, "Depth-only submissions", "depthonly");
    Count(trace, "No-pixel-shader submissions", "nopixelshader");
    Count(trace, "GPU fence waits", "gpuwait_n");
    Count(trace, "EDRAM resolves", "issuecopy_n");
}

}  // namespace detail

inline void DrawContent() {
    const auto trace = renut::trace_stats::GetLatest();
    const auto activity = renut::game_activity_stats::GetSnapshot();
    ImGui::Text("Guest tick (game logic)      %6.2f ms", cpuMS);
    ImGui::Text("Guest draw submission (appMainDraw) %6.2f ms", gpuMS);
    ImGui::Separator();
    ImGui::TextUnformatted("Game activity: frame / session");
    ImGui::Text("Animation streams    %5llu / %-8llu %6.3f ms", static_cast<unsigned long long>(activity.animation_stream_frame), static_cast<unsigned long long>(activity.animation_stream_session), activity.animation_stream_frame_ns / 1000000.0);
    ImGui::Text("Body blends          %5llu / %-8llu %6.3f ms", static_cast<unsigned long long>(activity.body_blend_frame), static_cast<unsigned long long>(activity.body_blend_session), activity.body_blend_frame_ns / 1000000.0);
    ImGui::Text("Actor scripts        %5llu / %-8llu %6.3f ms", static_cast<unsigned long long>(activity.actor_script_frame), static_cast<unsigned long long>(activity.actor_script_session), activity.actor_script_frame_ns / 1000000.0);
    ImGui::Text("Actor generation     %5llu / %-8llu %6.3f ms", static_cast<unsigned long long>(activity.actor_generation_frame), static_cast<unsigned long long>(activity.actor_generation_session), activity.actor_generation_frame_ns / 1000000.0);
    if (trace.available) {
        detail::DrawTrace(trace);
    } else {
        ImGui::TextDisabled("GPU trace unavailable: waiting for the first frame from the GPU plugin.");
    }
}

}  // namespace renut::overlays::render_stats
