#pragma once

// Minimal, additive extension point: VulkanCommandProcessor times a handful
// of its own existing phases (already-real call sites -- primitive
// processing, texture/sampler/render-target/pipeline setup, draw submission,
// GPU fence waits) and hands the numbers off once per frame via this single
// weak hook. All aggregation, storage, and display (the reNut "Renderer
// performance" overlay) lives in reNut's own code, which defines the strong
// symbol and exports it to the plugin -- same pattern as
// src/renut_engine/linuxfixes/*.cpp's REX_HOOK overrides, just in the other
// direction (SDK calling out to the host exe instead of the host exe
// overriding an SDK symbol). rexgpu-xenos links and runs identically whether
// or not a host defines this symbol -- the call site checks it is non-null
// first.
//
// Lives in reNut's own tree (not rex/graphics/vulkan/renut_trace_hook.h
// inside the SDK checkout) so trace_stats.cpp compiles regardless of SDK
// patch state -- the identically-named SDK-side header (added by
// cmake/patches/rexglue_sdk_native_renderer.patch, only present when the SDK
// is built via the REXSDK_DIR/from-source flow in docs/ai/nativevk.md, NOT
// the default find_package(rexglue) prebuilt path) previously made this a
// hard compile failure for anyone on a plain SDK. This copy MUST stay
// byte-identical (struct layout, call signature) to the SDK-side one -- it's
// a real ABI boundary crossed at runtime via a weak dynamic symbol, not just
// a compile-time convenience. If either side changes, update both.

#include <cstdint>

extern "C" {

struct RenutTraceRow {
  uint64_t frame;
  uint64_t draws;
  double frame_ms;
  double issuedraw_ms;
  double ns_per_draw;
  double prim_ms;
  double samp_ms;
  double tex_ms;
  double rt_ms;
  double pipe_ms;
  double shadertrans_ms;
  double sysconst_ms;
  double vfetch_ms;
  double dyn_ms;
  double bind_ms;
  double memexport_ms;
  double renderpass_enter_ms;
  double drawcmd_ms;
  double gpuwait_ms;
  uint64_t gpuwait_n;
  double swap_ms;
  double issuecopy_ms;
  uint64_t issuecopy_n;
  uint64_t depthonly;
  uint64_t nopixelshader;
};

// Defined by reNut's own code (src/renut_engine/trace_stats.cpp), exported
// from the renut executable's dynamic symbol table so the plugin's PLT entry
// resolves to it. Declared weak so rexgpu-xenos and any standalone build of
// rexgpu-nativevk (without a host defining it) still link and run -- the call
// site always null-checks before calling.
void RenutEmitTraceRow(const RenutTraceRow* row) __attribute__((weak));

}
