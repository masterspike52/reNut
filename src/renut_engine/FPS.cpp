#include <rex/logging.h>
#include "Fps.h"
#include <renut_engine/hooks.h>
#include <renut_engine/Timer.h>
#include <renut_engine/game_activity_stats.h>
#include <rex/hook.h>

//CPU Time
REX_EXTERN(__imp__appMainTickPreDraw);
REX_HOOK_RAW(appMainTickPreDraw){
    Timer timer;
    timer.start();
    __imp__appMainTickPreDraw(ctx, base);
    timer.stop();
    cpuMS = timer.elapsedMilliseconds();
    auto fpshook = fpsManager.GetCreateCounter("Tick");
    fpshook->Tick();
}

//GPU Time
void renutFrameLimit();             // frameHooks.cpp: our low-overhead frame cap ("vsync")
void renutApplyShaderCompileMode(); // frameHooks.cpp: sync-compile toggle (black-flash fix)

REX_EXTERN(__imp__appMainDraw);
REX_HOOK_RAW(appMainDraw){
    static uint32_t drawTicks = 0;
    if (++drawTicks <= 6 || (drawTicks % 300) == 0) {
        REXLOG_INFO("game: appMainDraw tick {}", drawTicks);
    }
    // Keep the engine's shader-compile mode in sync with our toggle (cheap; only
    // touches the engine cvar when the toggle actually changes).
    renutApplyShaderCompileMode();

    Timer timer;
    timer.start();
    __imp__appMainDraw(ctx, base);
    timer.stop();
    gpuMS = timer.elapsedMilliseconds();

    // Pace the frame after the draw is submitted so the GPU renders fewer frames
    // instead of running uncapped. No-op while frame_cap is "Off".
    renutFrameLimit();
}

// Hooked from config/renut_hooks.toml, address 0x82222250 (appMainDraw's own
// entry point per config/renut_funcs.toml) with after_instruction = true --
// fires once per real call into appMainDraw. Real fix (2026-08-19):
// game_activity_stats::EndFrame() (the only thing that copies the per-event
// Record*/Finish* counters -- see render_hooks_stub.cpp -- into the
// snapshot the Performance tab's "Game activity" section reads) needs a real
// hook to call it from, or the displayed snapshot never advances past its
// initial all-zero state even though the counters underneath are
// incrementing correctly. This address only fires for ~62% of real frames
// in practice (not confirmed why -- possibly a guest tick-rate/frame-pacing
// mismatch), so the snapshot updates in bursts rather than every frame, but
// that's real, working data instead of permanently dead.
void appMainDrawend() {
    renut::game_activity_stats::EndFrame();
}

void FPSCounter::Tick(){
    auto Time = std::chrono::steady_clock::now();
    std::chrono::duration<double, std::milli> delta = Time - lastTick;
    lastTick = Time;
    float ms = static_cast<float>(delta.count());
    frameTimes.push_back(ms);
    if (frameTimes.size() > AverageCount) {
        frameTimes.erase(frameTimes.begin());
    }
    float total = 0.0f;
    for (float f : frameTimes) {
        total += f;
    }
    averageMs = total / frameTimes.size();
    averageFps = 1000.0f / averageMs;
}

// Hooked from config/renut_hooks.toml, same guest address as appMainDrawend
// above (0x82222250, appMainDraw's entry point) but never actually fires --
// rexglue's codegen only keeps the last-defined midasm_hook when two entries
// share an (address, after_instruction) pair; this one differs from
// appMainDrawend only by after_instruction, so both coexist and this stays a
// documented no-op.
void appMainDrawStart() {
}

void appMainTickPreDrawStart() {
}

void appMainTickPreDrawend() {
}

