#pragma once
#include <rex/cvar.h>
#include "imgui.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"
#include "renut_engine/gpu/render/pipeline.h"

#include <chrono>
#include <cmath>
#include <cstdio>

REXCVAR_DECLARE(bool, shader_compile_overlay);

// Bottom right, while pipelines compile (a draw waiting on one, or the
// prebuild working through the pipeline list): a spinner and how many are
// left. Stays a second after the last one so a burst reads as one.
class ShaderCompileOverlay : public renut::gpu::overlay::Dialog {
public:
    void Draw() override {
        if (!REXCVAR_GET(shader_compile_overlay))
            return;
        const auto status = renut::gpu::GetPipelineCompileStatus();
        const auto now = std::chrono::steady_clock::now();
        const uint32_t left = status.compiling + status.queued;
        if (left) {
            last_busy_ = now;
            if (!showing_)
                session_start_ = status.compiled;
            showing_ = true;
        } else if (!showing_ || now - last_busy_ > std::chrono::seconds(1)) {
            showing_ = false;
            return;
        }

        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        const float px = std::max(14.0f, screen.y * 0.028f);
        char text[96];
        const uint32_t done = status.compiled - session_start_;
        if (left)
            std::snprintf(text, sizeof(text), "COMPILING SHADERS  %u LEFT  (%u DONE)", left, done);
        else
            std::snprintf(text, sizeof(text), "SHADERS READY  (%u COMPILED)", done);

        ImDrawList *dl = ImGui::GetForegroundDrawList();
        ImFont *font = ImGui::GetFont();
        const ImVec2 ext = font->CalcTextSizeA(px, FLT_MAX, 0, text);
        const float pad = px * 0.6f, spin = px * 0.5f;
        const ImVec2 hi(screen.x - px, screen.y - px);
        const ImVec2 lo(hi.x - ext.x - pad * 3 - spin * 2, hi.y - ext.y - pad * 2);
        // NutStaller's slab: navy fading out to the left.
        const ImU32 dark = IM_COL32(4, 10, 34, 220), clear = IM_COL32(4, 10, 34, 90);
        dl->AddRectFilledMultiColor(lo, hi, clear, dark, dark, clear);
        dl->AddLine(ImVec2(lo.x, hi.y), hi, IM_COL32(255, 255, 255, 110));
        const ImVec2 c(lo.x + pad + spin, (lo.y + hi.y) * 0.5f);
        if (left) {
            const float t = std::chrono::duration<float>(now.time_since_epoch()).count() * 6.0f;
            dl->PathArcTo(c, spin, t, t + 4.4f, 20);
            dl->PathStroke(IM_COL32(255, 255, 255, 230), 0, px * 0.14f);
        } else {
            dl->AddCircleFilled(c, spin * 0.5f, IM_COL32(110, 220, 90, 255));
        }
        dl->AddText(font, px, ImVec2(c.x + spin + pad, lo.y + pad), IM_COL32_WHITE, text);
    }

private:
    bool showing_ = false;
    uint32_t session_start_ = 0;
    std::chrono::steady_clock::time_point last_busy_;
};
