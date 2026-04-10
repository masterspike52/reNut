#pragma once
#ifdef _WIN32
#include "renut_logging.h"
#include <chrono>
#include <timeapi.h> 


std::once_flag g_timer_init;

void EnableHighResTimer() {
    std::call_once(g_timer_init, [] {
        timeBeginPeriod(1);
        RNUT_INFO("[threading] high-res timer enabled");
        });
}

void DisableHighResTimer() {
    timeEndPeriod(1);
    RNUT_INFO("[threading] high-res timer disabled");
}

// Sleep CRT hook
ppc_u32_result_t Sleep_hook(ppc_u32_t ms) {
    EnableHighResTimer();

    if (ms == 0) {
        SwitchToThread();
        return 0;
    }

    auto target = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(uint32_t(ms));

    if (ms >= 2) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(uint32_t(ms)) - std::chrono::microseconds(1500));
    }
    else {
        // Must yield to OS so lower-priority threads get scheduled
        ::Sleep(1);
    }

    while (std::chrono::steady_clock::now() < target)
        _mm_pause();

    return 0;
}
PPC_HOOK(sub_82715B60, Sleep_hook);
#endif // _WIN32