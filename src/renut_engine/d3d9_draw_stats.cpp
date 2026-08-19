#include "renut_engine/d3d9_draw_stats.h"

#include <atomic>

#include <rex/ppc.h>

namespace renut::d3d9_draw_stats {

namespace {

std::atomic<uint64_t> g_totalDraws{0};
std::atomic<uint64_t> g_depthOnlyDraws{0};
std::atomic<bool> g_pixelShaderBound{false};

// Internal-linkage helper, called only from this file's own hook thunks
// below (same TU, so that's fine regardless of anonymous-namespace linkage).
void RecordDraw() {
    g_totalDraws.fetch_add(1, std::memory_order_relaxed);
    if (!g_pixelShaderBound.load(std::memory_order_relaxed)) {
        g_depthOnlyDraws.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

void RecordSetPixelShader(uint32_t handle) {
    g_pixelShaderBound.store(handle != 0, std::memory_order_relaxed);
}

std::unordered_map<std::string, double> GetLatest() {
    const uint64_t total = g_totalDraws.load(std::memory_order_relaxed);
    const uint64_t depthOnly = g_depthOnlyDraws.load(std::memory_order_relaxed);
    return {
        {"draws", double(total)},
        {"depthonly", double(depthOnly)},
        // Same underlying condition the SDK-patch trace uses (see
        // cmake/patches/rexglue_sdk_native_renderer.patch: both increment
        // together whenever !pixel_shader at draw time) -- kept as two keys
        // to match trace_stats' existing field names, not because they can
        // differ here.
        {"nopixelshader", double(depthOnly)},
        {"color_draws", double(total - depthOnly)},
    };
}

}  // namespace renut::d3d9_draw_stats

// Hook thunks: global-scope, matching every other midasm_hook target in this
// codebase (see config/renut_hooks.toml's d3d9SetPixelShader_hook/
// d3d9Draw*_hook entries).

void d3d9SetPixelShader_hook(PPCRegister&, PPCRegister& r4) {
    renut::d3d9_draw_stats::RecordSetPixelShader(r4.u32);
}

void d3d9DrawVertices_hook() { renut::d3d9_draw_stats::RecordDraw(); }
void d3d9DrawIndexedVertices_hook() { renut::d3d9_draw_stats::RecordDraw(); }
void d3d9DrawVerticesUP_hook() { renut::d3d9_draw_stats::RecordDraw(); }
void d3d9DrawIndexedVerticesUP_hook() { renut::d3d9_draw_stats::RecordDraw(); }
