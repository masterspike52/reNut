#pragma once

#include "imgui.h"
#include "renut_engine/nativevk_phase0.h"
#include "renut_engine/trace_stats.h"

// Content-only: no window/Begin/End, no hotkey. Hosted as its own tab inside
// DebugHubOverlayDialog (see debug_hub_overlay.h) -- moved out of the
// Performance tab (render_stats_overlay.h) 2026-08-19 so general renderer
// stats and the SDK-independent native-renderer rewrite's own validation
// data (docs/ai/archive/native-renderer-rewrite-plan.md, Phase 0) aren't
// mixed together in one place.
namespace renut::overlays::nativevk_debug {

namespace detail {

inline bool Has(const renut::trace_stats::Snapshot& trace, const char* name) {
    return trace.values.find(name) != trace.values.end();
}

inline double Value(const renut::trace_stats::Snapshot& trace, const char* name) {
    const auto found = trace.values.find(name);
    return found == trace.values.end() ? 0.0 : found->second;
}

}  // namespace detail

inline void DrawContent() {
    using detail::Has;
    using detail::Value;

    const auto trace = renut::trace_stats::GetLatest();
    const auto phase0 = renut::nativevk_phase0::GetLatest();

    ImGui::TextUnformatted("D3D9-hook rewrite validation");
    ImGui::Text("%-28s %7llu", "D3D9-hook draws (session)",
        static_cast<unsigned long long>(phase0.session_total_draws));
    if (Has(trace, "session_draws_total")) {
        const double sdkDraws = Value(trace, "session_draws_total");
        const double ratio = sdkDraws > 0.0 ? double(phase0.session_total_draws) / sdkDraws * 100.0 : 0.0;
        ImGui::Text("%-28s %6.1f %%  (%.0f real)", "...as %% of SDK draws", ratio, sdkDraws);
    }
    ImGui::Text("%-28s %7llu", "Color draws (session, D3D9-hook)",
        static_cast<unsigned long long>(Value(trace, "color_draws")));
    ImGui::Text("%-28s %7llu", "SetVertexShader calls (session)",
        static_cast<unsigned long long>(phase0.session_set_vertex_shader_calls));
    const double unresolvedPct = phase0.session_set_vertex_shader_calls > 0
        ? double(phase0.session_unresolved_set_vertex_shader_calls) / double(phase0.session_set_vertex_shader_calls) * 100.0
        : 0.0;
    ImGui::Text("%-28s %7llu  (%.1f %%)", "...unresolved calls",
        static_cast<unsigned long long>(phase0.session_unresolved_set_vertex_shader_calls), unresolvedPct);
    ImGui::Text("%-28s %7llu", "Distinct handles (session)",
        static_cast<unsigned long long>(phase0.session_distinct_handles_seen));
    ImGui::Text("%-28s %7llu", "...distinct unresolved",
        static_cast<unsigned long long>(phase0.session_distinct_handles_unresolved));

    if (ImGui::CollapsingHeader("NativeVK Debug")) {
        ImGui::Text("%-28s %7llu", "Frame-end hook fires", static_cast<unsigned long long>(phase0.session_frame_ends));
        if (Has(trace, "frame")) {
            ImGui::Text("%-28s %7.0f", "SDK real frame count", Value(trace, "frame"));
        }
    }
}

}  // namespace renut::overlays::nativevk_debug
