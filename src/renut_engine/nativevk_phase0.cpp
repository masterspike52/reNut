#include "renut_engine/nativevk_phase0.h"
#include "renut_engine/shader_dump.h"
#include "renut_logging.h"

#include <atomic>
#include <mutex>
#include <unordered_set>

namespace renut::nativevk_phase0 {

namespace {

std::atomic<uint64_t> g_sessionFrameEnds{0};
std::atomic<uint64_t> g_sessionTotalDraws{0};

std::atomic<uint64_t> g_sessionSetVertexShaderCalls{0};
std::atomic<uint64_t> g_sessionUnresolvedCalls{0};

std::atomic<uint64_t> g_sessionCreateVertexShaderCalls{0};
std::atomic<uint64_t> g_sessionCreateVertexShaderParsed{0};

std::atomic<uint64_t> g_sessionUnresolvedResolvedViaBlobFallback{0};

constexpr uint32_t kMaxUnresolvedHandleLogs = 25;
std::atomic<uint32_t> g_unresolvedHandleLogs{0};

std::mutex g_handleMutex;
std::unordered_set<uint32_t> g_handlesSeen;
std::unordered_set<uint32_t> g_handlesUnresolved;

}  // namespace

// Called from d3d9_draw_stats.cpp / shader_dump.cpp's own hook thunks (see
// this file's closing comment for why) -- kept internal-linkage-free (not
// in the anonymous namespace above) so those other TUs can reach them.
void CountDraw() {
    g_sessionTotalDraws.fetch_add(1, std::memory_order_relaxed);
}

void RecordSetVertexShader(uint32_t handle) {
    g_sessionSetVertexShaderCalls.fetch_add(1, std::memory_order_relaxed);

    uint64_t hash = 0;
    const bool resolved = renut::shader_dump::TryResolveShaderUcodeHash(handle, hash);
    if (!resolved) {
        g_sessionUnresolvedCalls.fetch_add(1, std::memory_order_relaxed);
    }

    bool firstSeen;
    {
        std::lock_guard<std::mutex> lock(g_handleMutex);
        firstSeen = g_handlesSeen.insert(handle).second;
        if (firstSeen) {
            if (!resolved) g_handlesUnresolved.insert(handle);
        } else if (resolved) {
            g_handlesUnresolved.erase(handle);
        }
    }

    if (firstSeen && !resolved &&
        g_unresolvedHandleLogs.fetch_add(1, std::memory_order_relaxed) < kMaxUnresolvedHandleLogs) {
        RNUT_INFO("nativevk phase0: unresolved SetVertexShader handle {:#x}", handle);
    }
}

Snapshot GetLatest() {
    Snapshot snapshot;
    snapshot.session_frame_ends = g_sessionFrameEnds.load(std::memory_order_relaxed);
    snapshot.session_total_draws = g_sessionTotalDraws.load(std::memory_order_relaxed);
    snapshot.session_set_vertex_shader_calls = g_sessionSetVertexShaderCalls.load(std::memory_order_relaxed);
    snapshot.session_unresolved_set_vertex_shader_calls = g_sessionUnresolvedCalls.load(std::memory_order_relaxed);
    snapshot.session_create_vertex_shader_calls = g_sessionCreateVertexShaderCalls.load(std::memory_order_relaxed);
    snapshot.session_create_vertex_shader_parsed = g_sessionCreateVertexShaderParsed.load(std::memory_order_relaxed);
    snapshot.session_unresolved_resolved_via_blob_fallback =
        g_sessionUnresolvedResolvedViaBlobFallback.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_handleMutex);
    snapshot.session_distinct_handles_seen = g_handlesSeen.size();
    snapshot.session_distinct_handles_unresolved = g_handlesUnresolved.size();
    return snapshot;
}

void RecordCreateVertexShaderAttempt(bool parsed) {
    g_sessionCreateVertexShaderCalls.fetch_add(1, std::memory_order_relaxed);
    if (parsed) {
        g_sessionCreateVertexShaderParsed.fetch_add(1, std::memory_order_relaxed);
    }
}

void RecordUnresolvedHandleParsedAsBlob() {
    g_sessionUnresolvedResolvedViaBlobFallback.fetch_add(1, std::memory_order_relaxed);
}

// Wired from FPS.cpp's appMainDrawend (config/renut_hooks.toml, address
// 0x82222250, appMainDraw's real entry point per config/renut_funcs.toml).
//
// Real, confirmed bug (2026-08-19, playtest diagnostics): the sibling
// appMainDrawStart NEVER fires -- confirmed via
// `grep appMainDrawStart generated/*.cpp` finding zero matches. rexglue's
// codegen silently drops one of two midasm_hook entries sharing the same
// address (same pattern confirmed on the sibling appMainTickPreDrawStart/
// end pair -- only "end" survives there too), so there was no working
// "start" counterpart to pair this with -- removed rather than kept as
// dead weight (see git history for the FrameStart()/last_frame_draws this
// file used to have).
//
// Second confirmed issue: this hook itself only fired 8765 times against
// the SDK's own 14218 real frames in one session (~62%) -- NOT a reliable
// 1:1-with-real-frames marker (likely a guest tick-rate/frame-pacing
// mismatch, not another codegen bug). session_frame_ends is diagnostic
// only for that reason -- GetLatest() callers should compare
// session_total_draws against trace_stats' session_draws_total (both
// session-cumulative, no frame-alignment assumption needed) for the real
// Phase 0 coverage gate.
void FrameEnd() {
    g_sessionFrameEnds.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace renut::nativevk_phase0

// Real fix (2026-08-19): this file used to register its OWN midasm_hook
// entries (phase0Draw*_hook at the four D3D9 draw addresses,
// phase0SetVertexShader_hook at SetVertexShader) alongside other hooks
// already present at those exact same addresses. Confirmed via a full
// config/renut_hooks.toml sweep that rexglue's codegen only keeps the
// LAST-defined midasm_hook when two entries share the same
// (address, after_instruction) pair -- silently dropping the earlier one
// entirely (dumpVertexShaderSet_hook and the four original draw-count
// paths were all dead as a result, not just this one). Fixed by removing
// those duplicate entries and having the single surviving hook at each
// address call into both subsystems instead:
//   - SetVertexShader (0x8222A0A8): dumpVertexShaderSet_hook
//     (shader_dump.cpp) now also calls RecordSetVertexShader() directly.
//   - The four draw addresses: d3d9DrawVertices_hook and friends
//     (d3d9_draw_stats.cpp) now also call CountDraw() directly.
// CountDraw()/RecordSetVertexShader() stay part of this namespace's public
// API for that reason -- no longer called from a hook thunk in this file.
