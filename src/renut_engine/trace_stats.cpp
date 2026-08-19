#include "renut_engine/trace_stats.h"

#include <mutex>

#include "renut_engine/d3d9_draw_stats.h"
#include "renut_engine/renut_trace_hook.h"

namespace renut::trace_stats {

namespace {
std::mutex g_mutex;
Snapshot g_latest;
// Session-cumulative sum of row->draws across every RenutEmitTraceRow call
// (2026-08-19, see docs/ai/archive/native-renderer-rewrite-plan.md Phase 0):
// a single frame's "draws" value isn't safely comparable against
// nativevk_phase0's session-cumulative D3D9-hook draw count -- the two hook
// mechanisms don't share a common, reliably-firing per-frame boundary (the
// appMainDrawStart/appMainDrawend pair Phase 0 originally used to scope a
// "last frame" snapshot turned out not to fire once per real frame; see that
// file's own comment). Comparing session totals sidesteps needing any frame
// alignment between the two measurement paths at all.
uint64_t g_sessionDrawsTotal = 0;
}

Snapshot GetLatest() {
    Snapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        snapshot = g_latest;
    }

    // "Connect the values" without a patched SDK (2026-08-19): draws/
    // depthonly/nopixelshader/color_draws mean the same thing whether
    // they're measured inside the SDK's Vulkan command processor (only
    // available with the native-renderer patch, see docs/ai/nativevk.md) or
    // from pure D3D9 guest hooks (d3d9_draw_stats.cpp, always available).
    // Only fill in keys the SDK trace didn't already provide -- real
    // SDK-measured data always wins when it exists. Also flips `available`
    // on so the panel shows this fallback data instead of "GPU trace
    // unavailable" when that's genuinely all there is.
    for (const auto& [key, value] : renut::d3d9_draw_stats::GetLatest()) {
        if (snapshot.values.find(key) == snapshot.values.end()) {
            snapshot.values[key] = value;
        }
    }
    if (!snapshot.values.empty()) {
        snapshot.available = true;
        if (snapshot.path.empty()) {
            snapshot.path = "(D3D9 guest hooks -- no native-renderer SDK patch loaded)";
        }
    }
    return snapshot;
}

// Strong definition of the weak hook declared in renut_trace_hook.h: the
// nativevk/xenos plugin (loaded into this same process) calls this once per
// frame with everything it measured. Must be exported to the plugin's
// dynamic symbol table (see CMakeLists.txt's --export-dynamic-symbol) so its
// PLT entry resolves here instead of staying null. extern "C" keeps the name
// unmangled to match renut_trace_hook.h's declaration exactly.
extern "C" void RenutEmitTraceRow(const RenutTraceRow* row) {
    if (!row) {
        return;
    }
    Snapshot snapshot;
    snapshot.available = true;
    snapshot.path = "(live)";
    auto& values = snapshot.values;
    values.reserve(32);
    values["frame"] = double(row->frame);
    values["draws"] = double(row->draws);
    values["frame_ms"] = row->frame_ms;
    values["issuedraw_ms"] = row->issuedraw_ms;
    values["ns_per_draw"] = row->ns_per_draw;
    values["prim_ms"] = row->prim_ms;
    values["samp_ms"] = row->samp_ms;
    values["tex_ms"] = row->tex_ms;
    values["rt_ms"] = row->rt_ms;
    values["pipe_ms"] = row->pipe_ms;
    values["shadertrans_ms"] = row->shadertrans_ms;
    values["sysconst_ms"] = row->sysconst_ms;
    values["vfetch_ms"] = row->vfetch_ms;
    values["dyn_ms"] = row->dyn_ms;
    values["bind_ms"] = row->bind_ms;
    values["memexport_ms"] = row->memexport_ms;
    values["renderpass_enter_ms"] = row->renderpass_enter_ms;
    values["drawcmd_ms"] = row->drawcmd_ms;
    values["gpuwait_ms"] = row->gpuwait_ms;
    values["gpuwait_n"] = double(row->gpuwait_n);
    values["swap_ms"] = row->swap_ms;
    values["issuecopy_ms"] = row->issuecopy_ms;
    values["issuecopy_n"] = double(row->issuecopy_n);
    values["depthonly"] = double(row->depthonly);
    values["nopixelshader"] = double(row->nopixelshader);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_sessionDrawsTotal += row->draws;
    values["session_draws_total"] = double(g_sessionDrawsTotal);
    g_latest = std::move(snapshot);
}

}
