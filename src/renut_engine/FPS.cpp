#include "Fps.h"
#include <renut_engine/hooks.h>
#include <renut_engine/Timer.h>
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

void appMainDrawStart() {
}

void appMainDrawend() {
}

void appMainTickPreDrawStart() {
}

void appMainTickPreDrawend() {
}