#pragma once

#include <cstdint>

// Phase 0 of the SDK-independent native renderer rewrite (see
// docs/ai/archive/native-renderer-rewrite-plan.md): validates, read-only,
// whether hooking D3D9 draw/state calls directly at their guest addresses
// (the new architecture's whole foundation) actually sees close to 100% of
// real draws, and whether shader identity is resolvable for draws whose
// vertex shader bypassed CreateVertexShader via IM_LOAD. Counts/logs only --
// no rendering behavior changes.
namespace renut::nativevk_phase0 {

struct Snapshot {
    // Raw diagnostic counter (2026-08-19): confirmed via
    // `grep appMainDrawStart generated/*.cpp` (zero matches) that rexglue's
    // codegen silently drops one of two midasm_hook entries sharing an
    // address -- appMainDrawStart never fires, only appMainDrawend does, and
    // even that one only fired 8765 times against the SDK's own 14218 real
    // frames in one session (~62%). Neither is a reliable per-frame boundary,
    // so this is diagnostic only -- session_total_draws is the reliable
    // session-cumulative D3D9-hook draw count, compared against trace_stats'
    // own session-cumulative "session_draws_total" for the real coverage gate.
    uint64_t session_frame_ends = 0;
    uint64_t session_total_draws = 0;

    // Session-lifetime SetVertexShader (0x8222A0A8) call count, and how many
    // of those calls carried a handle TryResolveShaderUcodeHash could NOT
    // resolve (i.e. the handle's shader never went through CreateVertexShader
    // -- the known IM_LOAD-bypass risk this phase must measure, not assume).
    uint64_t session_set_vertex_shader_calls = 0;
    uint64_t session_unresolved_set_vertex_shader_calls = 0;

    // Distinct (deduplicated) handle counts for the same measurement, since
    // a single frequently-redrawn shader would otherwise dominate the raw
    // call counts above and hide how many DISTINCT shaders are affected.
    uint64_t session_distinct_handles_seen = 0;
    uint64_t session_distinct_handles_unresolved = 0;
};

Snapshot GetLatest();

// Called from FPS.cpp's appMainDrawend (config/renut_hooks.toml address
// 0x82222250, after_instruction=true) -- see session_frame_ends' own
// comment for what this actually measures. No FrameStart() counterpart:
// appMainDrawStart (same address, after_instruction=false) never fires at
// all (a real, confirmed codegen limitation, not something to work around
// here), so there was nothing for it to usefully do.
void FrameEnd();

// Called from d3d9_draw_stats.cpp / shader_dump.cpp's own hook thunks, not
// from any hook in this file directly (see nativevk_phase0.cpp's closing
// comment for why) -- declared here so their purpose is documented next to
// the rest of this module's API.
void CountDraw();
void RecordSetVertexShader(uint32_t handle);

}
