#pragma once
#include <rex/cvar.h>
#include "imgui.h"
#include "renut_engine/app/app_paths.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"
#include "renut_engine/online/eos.h"
#include "renut_engine/online/online.h"

#include <cstdio>
#include <string>

REXCVAR_DECLARE(bool, renut_online);

// Online play (F5): signing in to Live (on Epic Online Services), its state,
// player 1's gamertag and online ID. The friends list is its own window (F6,
// friends_dialog.h).
class OnlineDialog : public renut::gpu::overlay::Dialog {
public:
    void Draw() override {
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
            visible_ = !visible_;
            if (visible_)
                Open();
        }
        if (!visible_)
            return;

        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.2f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
        if (ImGui::Begin("Online (F5)", &visible_, ImGuiWindowFlags_NoSavedSettings)) {
            DrawSignIn();
            ImGui::Separator();
            DrawName();
            if (REXCVAR_GET(renut_online)) {
                ImGui::Separator();
                DrawStatus();
            }
        }
        ImGui::End();
    }

private:
    void Open() {
        const std::string name = renut::online::PlayerName();
        std::snprintf(name_, sizeof(name_), "%s", name.c_str());
    }

    void DrawSignIn() {
        bool online = REXCVAR_GET(renut_online);
        if (ImGui::Checkbox("Signed in to Live", &online)) {
            rex::cvar::SetFlagByName("renut_online", online ? "true" : "false");
            rex::cvar::SaveConfig(renut::paths::WritableFolder() / "renut.toml");
        }
        if (!online)
            ImGui::TextDisabled("Sign in to open the game's online modes and the friends list.");
    }

    void DrawName() {
        ImGui::TextUnformatted("Gamertag");
        ImGui::SetNextItemWidth(220);
        const bool entered = ImGui::InputText("##gamertag", name_, sizeof(name_),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Save name") || entered) {
            renut::online::SetPlayerName(name_);
            const std::string saved = renut::online::PlayerName();
            std::snprintf(name_, sizeof(name_), "%s", saved.c_str());
        }
        ImGui::TextDisabled("Up to 15 characters, what friends see. The game picks it up at the "
                            "next profile check.");
    }

    void DrawStatus() {
        namespace eos = renut::online::eos;
        if (!eos::Start() || !eos::SignedIn()) {
            const std::string status = eos::Status();
            const bool failed = !eos::Start();
            ImGui::TextColored(failed ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "Epic Online Services: %s", status.empty() ? "starting" : status.c_str());
            return;
        }
        const std::string id = eos::OwnId();
        ImGui::TextUnformatted("Epic Online Services: signed in");
        ImGui::Text("Online ID: %s", id.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy"))
            ImGui::SetClipboardText(id.c_str());
        ImGui::TextDisabled("Friends add you by this ID in their Friends window (F6).");
    }

    bool visible_ = false;
    char name_[16] = {};
};
