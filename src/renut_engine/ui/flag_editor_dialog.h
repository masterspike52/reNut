#pragma once
#include "imgui.h"
#include "renut_engine/game/flag_editor.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

// The flag editor (F5): player 1's game flags (normal, global, volatile) and
// counters (notes, jiggies, Jinjo tokens, scores, timers...) by the game's own
// names, a tab each. A group and a search narrow the list; a flag is ticked
// on or off, a count typed (Enter). Changes go in on the game's next frame
// and are saved with its next save (game/flag_editor.cpp).
class FlagEditorDialog : public renut::gpu::overlay::Dialog {
public:
    void Draw() override {
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
            visible_ = !visible_;
        if (!visible_)
            return;

        namespace flags = renut::game::flags;
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(ImVec2(720, 560), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.9f);
        if (ImGui::Begin("Game Flags (F5)", &visible_,
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
            if (!flags::HasProfile()) {
                ImGui::TextDisabled("Load a save first: these are player 1's.");
            } else {
                if (flags::Blocked())
                    ImGui::TextColored(kOrange, "Cheats are off in this online session: its host doesn't allow them.");
                if (ImGui::BeginTabBar("##tables")) {
                    for (int t = 0; t < flags::kTableCount; ++t) {
                        if (ImGui::BeginTabItem(kTabNames[t])) {
                            DrawTable(Table(t));
                            ImGui::EndTabItem();
                        }
                    }
                    ImGui::EndTabBar();
                }
            }
        }
        ImGui::End();
    }

private:
    using Table = renut::game::flags::Table;
    static constexpr ImVec4 kOrange{1.0f, 0.75f, 0.3f, 1.0f};
    static constexpr const char* kTabNames[] = {"Flags", "Global flags", "Volatile flags", "Counters"};

    // A table's names, looked at once: without the table's prefix
    // ("gameFlag_Normal_"), in lower case for the search, and grouped by their
    // first word ("Garage", "ShowdownTown", "Score"...).
    struct View {
        size_t prefix = 0;
        std::vector<std::string> lower;
        std::vector<std::string> groups;  // "All", then the first words
        std::vector<int> group_of;        // by id
    };

    // What the tab shows.
    struct Filter {
        int group = 0;
        char search[96] = {};
        bool only_on = false;
        int confirm = 0;  // a bulk change asked for: 1 on, -1 off
        std::vector<uint32_t> shown;
    };

    static std::string Lower(std::string s) {
        for (char& c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    const View& ViewOf(Table table, const std::vector<std::string>& names) {
        View& view = views_[int(table)];
        if (!view.lower.empty() || names.empty())
            return view;
        const std::string& null = names[0];  // "..._Null"
        view.prefix = null.size() >= 4 && null.compare(null.size() - 4, 4, "Null") == 0 ? null.size() - 4 : 0;
        std::vector<std::string> words(names.size());
        for (size_t i = 0; i < names.size(); ++i) {
            const std::string name = names[i].substr(std::min(view.prefix, names[i].size()));
            view.lower.push_back(Lower(name));
            words[i] = name.substr(0, name.find('_'));
            if (i && std::find(view.groups.begin(), view.groups.end(), words[i]) == view.groups.end())
                view.groups.push_back(words[i]);
        }
        std::sort(view.groups.begin(), view.groups.end());
        view.groups.insert(view.groups.begin(), "All");
        for (const std::string& word : words)
            view.group_of.push_back(int(std::find(view.groups.begin(), view.groups.end(), word) - view.groups.begin()));
        return view;
    }

    void DrawTable(Table table) {
        namespace flags = renut::game::flags;
        const std::vector<std::string>& names = flags::Names(table);
        if (names.empty()) {
            ImGui::TextDisabled("The game hasn't set its tables up yet.");
            return;
        }
        const View& view = ViewOf(table, names);
        Filter& filter = filters_[int(table)];
        const bool is_flags = table != Table::kCounters;

        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo("##group", view.groups[filter.group].c_str())) {
            for (int g = 0; g < int(view.groups.size()); ++g) {
                if (ImGui::Selectable(view.groups[g].c_str(), g == filter.group))
                    filter.group = g;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(is_flags ? -90.0f : -1.0f);
        ImGui::InputTextWithHint("##search", "Search (name or id)", filter.search, sizeof(filter.search));
        if (is_flags) {
            ImGui::SameLine();
            ImGui::Checkbox("Only on", &filter.only_on);
        }

        const std::string search = Lower(filter.search);
        filter.shown.clear();
        for (uint32_t i = 1; i < names.size(); ++i) {
            if (filter.group && view.group_of[i] != filter.group)
                continue;
            if (!search.empty() && view.lower[i].find(search) == std::string::npos && std::to_string(i) != search)
                continue;
            if (filter.only_on && !flags::Get(table, i))
                continue;
            filter.shown.push_back(i);
        }

        const bool blocked = flags::Blocked();
        if (blocked)
            ImGui::BeginDisabled();
        if (is_flags)
            DrawBulk(table, filter);

        const float footer = ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
        if (ImGui::BeginTable("##rows", 3,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
                              ImVec2(0, -footer))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(is_flags ? "On" : "Value", ImGuiTableColumnFlags_WidthFixed,
                                    is_flags ? 0.0f : 130.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(int(filter.shown.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const uint32_t i = filter.shown[row];
                    ImGui::PushID(int(i));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (is_flags) {
                        bool on = flags::Get(table, i) != 0;
                        if (ImGui::Checkbox("##v", &on))
                            flags::Set(table, i, on);
                    } else {
                        int value = flags::Get(table, i);
                        ImGui::SetNextItemWidth(-1.0f);
                        if (ImGui::InputInt("##v", &value, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
                            flags::Set(table, i, value);
                    }
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(names[i].c_str() + std::min(view.prefix, names[i].size()));
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("%s", names[i].c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%u", i);
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        if (blocked)
            ImGui::EndDisabled();

        ImGui::TextDisabled("%zu of %zu shown.%s", filter.shown.size(), names.size() - 1,
                            is_flags ? "" : " Type a value and press Enter.");
        ImGui::TextDisabled("Saved with the game's next save. A world reads most of these as it loads: "
                            "leave it and go back to see them.");
    }

    // Turning every flag shown on or off, asked twice.
    void DrawBulk(Table table, Filter& filter) {
        namespace flags = renut::game::flags;
        if (!filter.confirm) {
            if (ImGui::SmallButton("Turn all shown on"))
                filter.confirm = 1;
            ImGui::SameLine();
            if (ImGui::SmallButton("Turn all shown off"))
                filter.confirm = -1;
            return;
        }
        ImGui::TextColored(kOrange, "Turn %zu flag(s) %s?", filter.shown.size(), filter.confirm > 0 ? "on" : "off");
        ImGui::SameLine();
        if (ImGui::SmallButton("Yes")) {
            for (uint32_t i : filter.shown)
                flags::Set(table, i, filter.confirm > 0);
            filter.confirm = 0;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("No"))
            filter.confirm = 0;
    }

    bool visible_ = false;
    std::array<View, renut::game::flags::kTableCount> views_;
    std::array<Filter, renut::game::flags::kTableCount> filters_;
};
