#pragma once
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include "imgui.h"
#include "renut_engine/globals.h"

class FpsOverlayDialog : public rex::ui::ImGuiDialog {
public:
    explicit FpsOverlayDialog(rex::ui::ImGuiDrawer* drawer)
        : rex::ui::ImGuiDialog(drawer) {
        rex::ui::RegisterBind("bind_fps_overlay", "F1", "Toggle FPS overlay", [this] {
            visible_ = !visible_;
            });
    }

    ~FpsOverlayDialog() {
        rex::ui::UnregisterBind("bind_fps_overlay");
    }

    void OnDraw(ImGuiIO& io) override {
        if (!visible_) return;

        const double fps = fpsCount;
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::SetNextWindowSize(ImVec2(120.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##fps_overlay", nullptr, flags)) {
            ImVec4 color;
            if (fps >= 59.0)       color = ImVec4(0.2f, 1.0f, 1.0f, 1.0f);
            else if (fps >= 30.0)  color = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
            else if (fps <= 20.0)  color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
            else                   color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);

            char buf[32];
            snprintf(buf, sizeof(buf), "FPS: %.1f", fps);
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 shadow = IM_COL32(0, 0, 0, 200);
            dl->AddText(ImVec2(pos.x - 1, pos.y - 1), shadow, buf);
            dl->AddText(ImVec2(pos.x + 1, pos.y - 1), shadow, buf);
            dl->AddText(ImVec2(pos.x - 1, pos.y + 1), shadow, buf);
            dl->AddText(ImVec2(pos.x + 1, pos.y + 1), shadow, buf);
            ImGui::TextColored(color, "%s", buf);
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }

private:
    bool visible_ = false;
};