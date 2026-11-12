#pragma once
#include <rex/console_command.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include "imgui.h"
#include "renut_engine/app/renut_settings.h"
#include "renut_engine/game/graphics_menu.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"
#include "renut_engine/online/online.h"
#include <rexfabric/rexfabric.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// reNut's own settings (F4): the "Nuts&Bolts/" settings, a tab per category, applied
// live and saved with Save to config (the SDK's settings window moves to F8).
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
    // A setting or command shown: its full name, its old name (labels) and its category.
    struct Entry {
        std::string full;
        std::string name;
        std::string category;
    };

    // reNut's own ("renut.<old name>"), then those Lua mods added ("mods.<name>", "Nuts&Bolts/Mod: <mod>").
    static std::vector<Entry> Entries() {
        std::vector<Entry> entries;
        for (const auto& entry : renut::graphics_menu::CategorizedSettings())
            entries.push_back({std::string(renut::settings::kScope) + "." + entry.name, entry.name, entry.category});
        for (const rexfabric::ModSetting& setting : rexfabric::ModSettings()) {
            const size_t dot = setting.name.find('.');
            entries.push_back({setting.name, dot == std::string::npos ? setting.name : setting.name.substr(dot + 1),
                               setting.category});
        }
        return entries;
    }

    static rex::SettingBase* FindSetting(const std::string& full) {
        auto found = REX_SETTINGS().Find(full);
        return found ? *found : nullptr;
    }

    static rex::ConsoleCommand* FindCommand(const std::string& full) {
        const std::string scope = full.substr(0, full.rfind('.'));
        for (rex::ConsoleCommand* command : REX_SETTINGS().EnumerateCommands(scope))
            if (command->GetName() == full)
                return command;
        return nullptr;
    }

    static bool Registered(const Entry& entry) {
        return FindSetting(entry.full) || FindCommand(entry.full);
    }

    static bool IsRenut(const Entry& entry) {
        return entry.category.rfind(kCategoryPrefix, 0) == 0;
    }

    // reNut's categories with a registered setting, without the prefix.
    static std::vector<std::string> Categories(const std::vector<Entry>& entries) {
        std::vector<std::string> categories;
        for (const Entry& entry : entries) {
            if (!IsRenut(entry) || !Registered(entry))
                continue;
            std::string category = entry.category.substr(std::char_traits<char>::length(kCategoryPrefix));
            if (std::find(categories.begin(), categories.end(), category) == categories.end())
                categories.push_back(std::move(category));
        }
        std::sort(categories.begin(), categories.end());
        return categories;
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
        const std::vector<Entry> entries = Entries();
        for (const std::string& category : Categories(entries)) {
            if (!ImGui::BeginTabItem(category.c_str()))
                continue;
            ImGui::BeginChild("##flags", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
            const std::string full = kCategoryPrefix + category;
            for (const Entry& entry : entries) {
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
        for (const Entry& entry : Entries()) {
            if (!IsRenut(entry))
                continue;
            const rex::SettingBase* setting = FindSetting(entry.full);
            const rex::ConsoleCommand* command = setting ? nullptr : FindCommand(entry.full);
            const std::string help = setting ? setting->GetHelp() : command ? command->GetHelp() : std::string();
            if ((setting || command) && (Lower(entry.name).find(search) != std::string::npos ||
                                         Lower(help).find(search) != std::string::npos))
                DrawFlag(entry);
        }
        ImGui::EndChild();
    }

    void Set(const std::string& full, const std::string& value) {
        if (REX_SETTINGS().SetText(full, value)) {
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
            (void)REX_SETTINGS().Save(config_path_);
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

    void DrawFlag(const Entry& entry) {
        using rex::SettingType;
        const std::string& full = entry.full;
        const std::string& name = entry.name;
        rex::SettingBase* setting = FindSetting(full);
        rex::ConsoleCommand* command = setting ? nullptr : FindCommand(full);
        if (!setting && !command)
            return;
        ImGui::PushID(full.c_str());
        // Allowing cheats in a session is the host's: greyed out in a session
        // someone else hosts.
        const bool not_host = name == "renut_host_cheats" && !renut::online::CanAllowSessionCheats();
        const bool read_only = (setting && setting->IsFrozen()) || not_host;
        if (read_only)
            ImGui::BeginDisabled();

        const std::string label = Label(name);
        const std::string help = setting ? setting->GetHelp() : command->GetHelp();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s\n\n%s%s", full.c_str(), help.c_str(),
                              not_host ? "\n\nOnly the session's host can change this." : "");
        if (setting && setting->GetConstraints().life == rex::SettingLife::kRestart) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "(restart)");
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
        ImGui::SetNextItemWidth(-1.0f);

        if (command) {
            if (ImGui::Button("Run"))
                (void)command->Run("");
            if (read_only)
                ImGui::EndDisabled();
            ImGui::PopID();
            return;
        }

        const std::string value = setting->GetText();
        const rex::SettingConstraints& constraints = setting->GetConstraints();
        const auto& allowed = constraints.allowed;
        switch (setting->GetType()) {
        case SettingType::kBool: {
            bool v = value == "true" || value == "1";
            if (ImGui::Checkbox("##v", &v))
                Set(full, v ? "true" : "false");
            break;
        }
        case SettingType::kI32:
        case SettingType::kI64:
        case SettingType::kU32:
        case SettingType::kU64: {
            int v = std::atoi(value.c_str());
            if (ImGui::InputInt("##v", &v, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (constraints.min)
                    v = std::max(v, int(*constraints.min));
                if (constraints.max)
                    v = std::min(v, int(*constraints.max));
                Set(full, std::to_string(v));
            }
            break;
        }
        case SettingType::kF64: {
            double v = std::atof(value.c_str());
            if (ImGui::InputDouble("##v", &v, 0.0, 0.0, "%.3f",
                                   ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (constraints.min)
                    v = std::max(v, *constraints.min);
                if (constraints.max)
                    v = std::min(v, *constraints.max);
                Set(full, std::to_string(v));
            }
            break;
        }
        case SettingType::kString:
        case SettingType::kPath:
        default:
            if (!allowed.empty()) {
                if (ImGui::BeginCombo("##v", value.c_str())) {
                    for (const std::string& option : allowed) {
                        const bool selected = option == value;
                        if (ImGui::Selectable(option.c_str(), selected) && !selected)
                            Set(full, option);
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
                    Set(full, buffer);
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
