#pragma once

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "imgui.h"
#include "renut_engine/game_activity_stats.h"
#include "renut_engine/overlays/ab_benchmark_overlay.h"
#include "renut_engine/overlays/dl_compat.h"
#include "renut_engine/overlays/render_stats_overlay.h"

// One F5 window hosting the three renderer debug panels (Renderer
// performance, A/B benchmark, Native shader debug) as tabs, instead of three
// separately-toggled/always-on windows scattered around the screen. The
// native-shader tab's content lives in the GPU plugin (only rexgpu-nativevk
// has one) and is pulled in with dl_compat.h's dlsym-equivalent, the same
// cross-module pattern ab_benchmark_overlay.h already used for renut_ab_*.
// Available on Windows too (see dl_compat.h) as of 2026-08-19 -- previously
// this whole dialog, including the Performance tab (which needs no plugin
// symbol lookup at all), was compiled out of Windows builds entirely because
// only the Native Shaders/A-B Benchmark tabs actually needed dlopen/dlsym.
class DebugHubOverlayDialog : public rex::ui::ImGuiDialog {
public:
    explicit DebugHubOverlayDialog(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {
        rex::ui::RegisterBind("bind_renut_debug_hub", "F5",
            "Toggle renderer debug panels (performance / A-B benchmark / native shaders)", [this] {
                visible_ = !visible_;
                renut::game_activity_stats::SetEnabled(visible_);
            });
    }

    ~DebugHubOverlayDialog() {
        rex::ui::UnregisterBind("bind_renut_debug_hub");
    }

    void OnDraw(ImGuiIO&) override {
        if (!visible_) {
            return;
        }
        ImGui::SetNextWindowPos(ImVec2(10.0f, 130.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.72f);
        ImGui::SetNextWindowSize(ImVec2(560.0f, 680.0f), ImGuiCond_FirstUseEver);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
        if (!ImGui::Begin("Renderer debug##RenutDebugHub", &visible_, flags)) {
            ImGui::End();
            return;
        }
        if (ImGui::BeginTabBar("RenutDebugHubTabs")) {
            if (ImGui::BeginTabItem("Performance")) {
                renut::overlays::render_stats::DrawContent();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("A/B Benchmark")) {
                renut::overlays::ab_benchmark::DrawContent();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Native Shaders")) {
                DrawNativeShaderTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

private:
    bool visible_ = false;
    void (*native_shader_draw_)() = nullptr;
    bool native_shader_resolved_ = false;

    void DrawNativeShaderTab() {
        if (!native_shader_resolved_) {
            native_shader_resolved_ = true;
            // The plugin is already resident (the graphics system came from
            // it), so this only ever takes a handle to an already-loaded
            // module, never a real load -- see dl_compat.h's own comment.
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
                native_shader_draw_ = reinterpret_cast<void (*)()>(
                    RenutDlSym(self, "RenutNativeShaderDebugPanelDrawContent"));
                RenutDlClose(self);
            }
        }
        if (!native_shader_draw_) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "Native shader debug panel not available in this GPU plugin.");
            ImGui::TextWrapped("Run with gpu_plugin = \"nativevk\" in renut.toml.");
            return;
        }
        native_shader_draw_();
    }
};
