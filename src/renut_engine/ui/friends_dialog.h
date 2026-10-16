#pragma once
#include <rex/cvar.h>
#include "imgui.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"
#include "renut_engine/online/eos.h"
#include "renut_engine/online/friends.h"
#include "renut_engine/online/online.h"

#include <cstdint>
#include <cstdio>
#include <string>

REXCVAR_DECLARE(bool, renut_online);

// The friends list (F6): friends (kept by online ID) and whether they are
// online or in a game to join, friend requests received and sent, player 1's
// online ID (with copy), and sending a request by online ID (with paste).
class FriendsDialog : public renut::gpu::overlay::Dialog {
public:
    void Draw() override {
        if (ImGui::IsKeyPressed(ImGuiKey_F6, false))
            visible_ = !visible_;
        if (!visible_)
            return;

        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.15f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
        if (ImGui::Begin("Friends (F6)", &visible_, ImGuiWindowFlags_NoSavedSettings)) {
            if (!REXCVAR_GET(renut_online)) {
                ImGui::TextDisabled("Sign in to Live in the Online window (F5) first.");
            } else if (!renut::online::friends::EnsureStarted()) {
                const std::string status = renut::online::eos::Status();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Epic Online Services: %s",
                                   status.empty() ? "starting" : status.c_str());
            } else {
                DrawOwnId();
                DrawFriends();
                DrawRequests();
                DrawSent();
                DrawAdd();
            }
        }
        ImGui::End();
    }

private:
    static constexpr ImVec4 kGreen{0.3f, 1.0f, 0.4f, 1.0f};

    // The lists are copied anew every frame, so a row's widgets are told apart
    // by the player's online ID: a click (pressed one frame, released another)
    // is not lost.
    static void PushPlayerId(const std::string& id) { ImGui::PushID(id.c_str()); }

    void DrawOwnId() {
        const std::string id = renut::online::eos::OwnId();
        ImGui::Text("Your online ID: %s", id.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy##own_id")) {
            ImGui::SetClipboardText(id.c_str());
            copied_ = true;
        }
        if (copied_) {
            ImGui::SameLine();
            ImGui::TextDisabled("copied");
        }
    }

    void DrawFriends() {
        namespace friends = renut::online::friends;
        const auto list = friends::Friends();
        ImGui::SeparatorText("Friends");
        if (list.empty()) {
            ImGui::TextDisabled("No friends yet: send a request by online ID below.");
            return;
        }
        std::string remove;
        if (ImGui::BeginTable("##friends", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Gamertag");
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            for (const friends::Friend& f : list) {
                PushPlayerId(f.id);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(f.gamertag.empty() ? "?" : f.gamertag.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Online ID %s", f.id.c_str());
                ImGui::TableNextColumn();
                if (!f.online)
                    ImGui::TextDisabled("offline");
                else if (f.state & friends::kStateJoinable)
                    ImGui::TextColored(kGreen, "in a game you can join (%u/%u)",
                                       unsigned(f.session.filled_slots), unsigned(f.session.public_slots));
                else
                    ImGui::TextColored(kGreen, "online");
                ImGui::TableNextColumn();
                if (confirm_remove_ == f.id) {
                    if (ImGui::SmallButton("Really remove"))
                        remove = f.id;
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Keep"))
                        confirm_remove_.clear();
                } else if (ImGui::SmallButton("Remove")) {
                    confirm_remove_ = f.id;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!remove.empty()) {
            friends::Remove(remove);
            confirm_remove_.clear();
        }
        ImGui::TextDisabled("Join friends from the game's own friends list.");
    }

    void DrawRequests() {
        namespace friends = renut::online::friends;
        const auto list = friends::Requests();
        if (list.empty())
            return;
        ImGui::SeparatorText("Friend requests");
        for (const friends::Request& r : list) {
            PushPlayerId(r.id);
            ImGui::Text("%s wants to be friends", r.gamertag.empty() ? r.id.c_str() : r.gamertag.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Accept"))
                friends::Accept(r.id);
            ImGui::SameLine();
            if (ImGui::SmallButton("Decline"))
                friends::Decline(r.id);
            ImGui::PopID();
        }
    }

    void DrawSent() {
        namespace friends = renut::online::friends;
        const auto list = friends::SentRequests();
        if (list.empty())
            return;
        ImGui::SeparatorText("Requests sent");
        for (const friends::Request& s : list) {
            PushPlayerId(s.id);
            ImGui::TextUnformatted(s.gamertag.empty() ? s.id.c_str() : s.gamertag.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("waiting for an answer");
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel"))
                friends::CancelRequest(s.id);
            ImGui::PopID();
        }
    }

    void DrawAdd() {
        namespace eos = renut::online::eos;
        ImGui::SeparatorText("Add a friend by online ID");
        ImGui::SetNextItemWidth(300);
        const bool entered = ImGui::InputTextWithHint("##online_id", "their online ID", id_, sizeof(id_),
                                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Paste")) {
            if (const char* clipboard = ImGui::GetClipboardText())
                std::snprintf(id_, sizeof(id_), "%s", clipboard);
        }
        ImGui::SameLine();
        if ((ImGui::Button("Send friend request") || entered) && id_[0]) {
            const std::string id = eos::ParseId(id_);
            if (!id.empty() && id != eos::OwnId()) {
                renut::online::friends::SendRequest(id);
                id_[0] = 0;
                invalid_ = false;
            } else {
                invalid_ = true;
            }
        }
        if (invalid_)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                               "That is not someone else's online ID (32 letters and digits).");
        ImGui::TextDisabled("Ctrl+V pastes too. They accept the request in their Friends window; "
                            "a request to someone offline is sent again until they are on.");
    }

    bool visible_ = false;
    char id_[80] = {};
    bool invalid_ = false;
    bool copied_ = false;
    std::string confirm_remove_;
};
