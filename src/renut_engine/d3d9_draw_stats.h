#pragma once

#include <cstdint>
#include <unordered_map>
#include <string>

// Session-cumulative draw-submission stats sourced purely from D3D9 guest
// hooks (config/renut_hooks.toml) -- works identically regardless of GPU
// plugin or whether rexglue-sdk has the native-renderer patch applied.
// Exists to "connect the values" trace_stats' RenutEmitTraceRow path can
// only populate when a patched SDK is loaded (see docs/ai/nativevk.md):
// "draws", "depthonly", "nopixelshader", "color_draws" mean the same thing
// either way, just measured from the guest side instead of inside the SDK's
// Vulkan command processor. trace_stats::GetLatest() merges these in only
// for keys the SDK trace didn't already provide, so real SDK-measured data
// (when available) always wins over this fallback.
namespace renut::d3d9_draw_stats {

std::unordered_map<std::string, double> GetLatest();

}
