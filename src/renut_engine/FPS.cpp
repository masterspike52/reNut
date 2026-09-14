#include <rex/logging.h>
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
void renutFrameLimit();             
void renutApplyShaderCompileMode(); 

REX_EXTERN(__imp__appMainDraw);
REX_HOOK_RAW(appMainDraw){
    static uint32_t drawTicks = 0;
    if (++drawTicks <= 6 || (drawTicks % 300) == 0) {
        REXLOG_INFO("game: appMainDraw tick {}", drawTicks);
    }
    renutApplyShaderCompileMode();

    Timer timer;
    timer.start();
    __imp__appMainDraw(ctx, base);
    timer.stop();
    gpuMS = timer.elapsedMilliseconds();
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

