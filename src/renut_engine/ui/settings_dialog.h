#pragma once
#include <rex/cvar.h>
#include "imgui.h"
#include "renut_engine/app/config_file.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// reNut's own settings (F4): the cvars under "Nuts&Bolts/", one tab per
// category. Changes apply as they are made; Save to config writes them to the
// config file. The SDK's settings window (every cvar) moves from F4 to F8
// (RenutApp::OnCreateDialogs).
class RenutSettingsDialog : public renut::gpu::overlay::Dialog {
public:
    static constexpr const char* kCategoryPrefix = "Nuts&Bolts/";

    explicit RenutSettingsDialog(std::filesystem::path config_path)
        : config_path_(std::move(config_path)) {}

    void Draw() override {
        if (ImGui::IsKeyPressed(ImGuiKey_F4, false))
            visible_ = !visible_;
        if (!visible_)
            return;

        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(ImVec2(680, 520), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.9f);
        if (ImGui::Begin("reNut Settings (F4)", &visible_,
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##search", "Search", search_, sizeof(search_));
            if (search_[0])
                DrawSearch();
            else
                DrawTabs();
            ImGui::Separator();
            DrawSaveBar();
        }
        ImGui::End();
    }

private:
    // reNut's categories, in registry order, without the prefix.
    static std::vector<std::string> Categories() {
        std::vector<std::string> categories;
        for (const auto& entry : rex::cvar::GetRegistry()) {
            if (!IsRenut(entry))
                continue;
            std::string category = entry.category.substr(std::char_traits<char>::length(kCategoryPrefix));
            if (std::find(categories.begin(), categories.end(), category) == categories.end())
                categories.push_back(std::move(category));
        }
        std::sort(categories.begin(), categories.end());
        return categories;
    }

    static bool IsRenut(const rex::cvar::FlagEntry& entry) {
        return entry.category.rfind(kCategoryPrefix, 0) == 0;
    }

    static std::string Lower(std::string s) {
        for (char& c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // "renut_depth_of_field" as "Depth of field"; abbreviations in capitals
    // ("renut_msaa" as "MSAA").
    static std::string Label(const std::string& name) {
        static const char* const kUpper[] = {"msaa", "fxaa", "smaa", "ssao", "hud", "fov", "fps", "ui"};
        std::string label = name.rfind("renut_", 0) == 0 ? name.substr(6) : name;
        std::string out;
        size_t start = 0;
        while (start <= label.size()) {
            size_t end = label.find('_', start);
            if (end == std::string::npos)
                end = label.size();
            std::string word = label.substr(start, end - start);
            if (std::find_if(std::begin(kUpper), std::end(kUpper), [&](const char* u) { return word == u; }) !=
                std::end(kUpper))
                for (char& c : word)
                    c = char(std::toupper(static_cast<unsigned char>(c)));
            out += (out.empty() ? "" : " ") + word;
            start = end + 1;
        }
        if (!out.empty())
            out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
        return out;
    }

    void DrawTabs() {
        if (!ImGui::BeginTabBar("##categories"))
            return;
        for (const std::string& category : Categories()) {
            if (!ImGui::BeginTabItem(category.c_str()))
                continue;
            ImGui::BeginChild("##flags", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
            const std::string full = kCategoryPrefix + category;
            for (auto& entry : rex::cvar::GetRegistry()) {
                if (entry.category == full)
                    DrawFlag(entry);
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    void DrawSearch() {
        const std::string search = Lower(search_);
        ImGui::BeginChild("##flags", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
        for (auto& entry : rex::cvar::GetRegistry()) {
            if (IsRenut(entry) && (Lower(entry.name).find(search) != std::string::npos ||
                                   Lower(entry.description).find(search) != std::string::npos))
                DrawFlag(entry);
        }
        ImGui::EndChild();
    }

    void Set(const rex::cvar::FlagEntry& entry, const std::string& value) {
        if (rex::cvar::SetFlagByName(entry.name, value)) {
            unsaved_ = true;
            saved_ = false;
        }
    }

    void DrawSaveBar() {
        if (unsaved_) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.6f, 0.3f, 1.0f));
        }
        const bool save = ImGui::Button("Save to config");
        if (unsaved_)
            ImGui::PopStyleColor(2);
        if (save) {
            renut::config::Save(config_path_);
            unsaved_ = false;
            saved_ = true;
        }
        ImGui::SameLine();
        const std::string file = config_path_.filename().string();
        if (unsaved_)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Unsaved changes (%s)", file.c_str());
        else if (saved_)
            ImGui::TextDisabled("Saved to %s", file.c_str());
        else
            ImGui::TextDisabled("(%s)", file.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("  Every setting, the SDK's too: F8");
    }

    void DrawFlag(rex::cvar::FlagEntry& entry) {
        using rex::cvar::FlagType;
        using rex::cvar::Lifecycle;
        ImGui::PushID(entry.name.c_str());
        const bool read_only = entry.lifecycle == Lifecycle::kInitOnly;
        if (read_only)
            ImGui::BeginDisabled();

        const std::string label = Label(entry.name);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s\n\n%s", entry.name.c_str(), entry.description.c_str());
        if (entry.lifecycle == Lifecycle::kRequiresRestart) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "(restart)");
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
        ImGui::SetNextItemWidth(-1.0f);

        const std::string value = entry.getter ? entry.getter() : std::string();
        const auto& allowed = entry.constraints.allowed_values;
        switch (entry.type) {
        case FlagType::Boolean: {
            bool v = value == "true" || value == "1";
            if (ImGui::Checkbox("##v", &v))
                Set(entry, v ? "true" : "false");
            break;
        }
        case FlagType::Int32:
        case FlagType::Int64:
        case FlagType::Uint32:
        case FlagType::Uint64: {
            int v = std::atoi(value.c_str());
            if (ImGui::InputInt("##v", &v, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (entry.constraints.min)
                    v = std::max(v, int(*entry.constraints.min));
                if (entry.constraints.max)
                    v = std::min(v, int(*entry.constraints.max));
                Set(entry, std::to_string(v));
            }
            break;
        }
        case FlagType::Double: {
            double v = std::atof(value.c_str());
            if (ImGui::InputDouble("##v", &v, 0.0, 0.0, "%.3f",
                                   ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (entry.constraints.min)
                    v = std::max(v, *entry.constraints.min);
                if (entry.constraints.max)
                    v = std::min(v, *entry.constraints.max);
                Set(entry, std::to_string(v));
            }
            break;
        }
        case FlagType::Command:
            if (ImGui::Button("Run") && entry.command_callback)
                entry.command_callback("");
            break;
        case FlagType::String:
        default:
            if (!allowed.empty()) {
                if (ImGui::BeginCombo("##v", value.c_str())) {
                    for (const std::string& option : allowed) {
                        const bool selected = option == value;
                        if (ImGui::Selectable(option.c_str(), selected) && !selected)
                            Set(entry, option);
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            } else {
                char buffer[512];
                std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
                if (ImGui::InputText("##v", buffer, sizeof(buffer),
                                     ImGuiInputTextFlags_EnterReturnsTrue))
                    Set(entry, buffer);
            }
            break;
        }

        if (read_only)
            ImGui::EndDisabled();
        ImGui::PopID();
    }

    std::filesystem::path config_path_;
    bool visible_ = false;
    bool unsaved_ = false;  // changed since the last save
    bool saved_ = false;    // saved, nothing changed since
    char search_[128] = {};
};
