#pragma once

#include "imgui.h"
#include "renut_engine/overlays/dl_compat.h"

// Content-only: no window/Begin/End, no hotkey. Hosted as a tab inside
// DebugHubOverlayDialog (see debug_hub_overlay.h), which owns the single F5
// toggle shared by all the renderer debug panels.
//
// Drives the GPU plugin's alternating A/B benchmark and shows its progress.
// The plugin is loaded with dlopen, so its symbols are resolved at runtime
// rather than linked. When they are missing - a stock xenos build, or a
// plugin without the benchmark - this reports that instead of failing.
namespace renut::overlays::ab_benchmark {

namespace detail {

struct Api {
    void (*start)() = nullptr;
    void (*stop)() = nullptr;
    int (*is_active)() = nullptr;
    const char* (*status)() = nullptr;

    bool Available() const { return start && stop && is_active && status; }
};

inline const Api& Get() {
    static const Api api = [] {
        Api result;
        // The plugin is already resident (the graphics system came from it), so
        // this only ever takes a handle to an already-loaded module, never a
        // real load -- see dl_compat.h's own comment for why that matters.
#ifdef _WIN32
        void* self = RenutDlOpenNoLoad("rexgpu-nativevkrd.dll");
        if (!self) {
            self = RenutDlOpenNoLoad("rexgpu-nativevk.dll");
        }
#elif defined(__APPLE__)
        void* self = RenutDlOpenNoLoad("librexgpu-nativevkrd.dylib");
        if (!self) {
            self = RenutDlOpenNoLoad("librexgpu-nativevk.dylib");
        }
#else
        void* self = RenutDlOpenNoLoad("librexgpu-nativevkrd.so");
        if (!self) {
            self = RenutDlOpenNoLoad("librexgpu-nativevk.so");
        }
#endif
        if (!self) {
            // Fall back to a global lookup for builds that link it directly.
            self = RenutDlOpenSelf();
        }
        if (self) {
            result.start = reinterpret_cast<void (*)()>(RenutDlSym(self, "renut_ab_start"));
            result.stop = reinterpret_cast<void (*)()>(RenutDlSym(self, "renut_ab_stop"));
            result.is_active = reinterpret_cast<int (*)()>(RenutDlSym(self, "renut_ab_is_active"));
            result.status = reinterpret_cast<const char* (*)()>(RenutDlSym(self, "renut_ab_status"));
            RenutDlClose(self);
        }
        return result;
    }();
    return api;
}

inline void Toggle(const Api& api) {
    if (!api.Available()) {
        return;
    }
    if (api.is_active() != 0) {
        api.stop();
    } else {
        api.start();
    }
}

}  // namespace detail

inline void DrawContent() {
    const detail::Api& api = detail::Get();
    if (!api.Available()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Benchmark not available in this GPU plugin.");
        ImGui::TextWrapped(
            "This benchmark harness was part of the old renut plugin's diagnostic "
            "instrumentation, removed while shrinking the rexglue-sdk patch to a "
            "minimal, principled diff (see docs/ai/history.md). Not currently wired "
            "up in rexgpu-nativevk.");
        return;
    }

    const bool running = api.is_active() != 0;
    if (ImGui::Button(running ? "Stop" : "Start", ImVec2(120.0f, 0.0f))) {
        detail::Toggle(api);
    }
    ImGui::SameLine();
    if (running) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "running");
    } else {
        ImGui::TextDisabled("idle");
    }

    ImGui::Separator();
    ImGui::TextUnformatted(api.status());
    ImGui::Separator();
    ImGui::TextDisabled("A = stock path, B = optimisation under test.");
    ImGui::TextDisabled("Stand somewhere busy and keep the camera still.");
}

}  // namespace renut::overlays::ab_benchmark
