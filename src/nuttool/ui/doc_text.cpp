// String table (loctext) editor.
#include "docs.hpp"

#include "assetdb.hpp"
#include "json.hpp"
#include "loctext.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace nut::gui {
namespace {

class TextDocument : public Document {
public:
    TextDocument(const fs::path& p, const fs::path& root, Loaded l) : l_(std::move(l)) {
        path = p;
        table_ = loctext::read(*l_.caff);
        // Language from the folder: loctext\<language>\<id>; Debug\11 holds English.
        std::string generic = p.generic_string();
        auto at = generic.find("/loctext/");
        if (at != std::string::npos) {
            auto end = generic.find('/', at + 9);
            lang_ = generic.substr(at + 9, end - at - 9);
        } else {
            lang_ = "english";
        }
        title_ = friendlyName(root, p) + " [" + lang_ + "]";
        if (lang_ != "english") {
            if (auto id = assetdb::idFromPath(p)) {
                fs::path ep = assetdb::pathForId(root, *id);
                try {
                    if (fs::exists(ep)) {
                        Bytes d = readFile(ep);
                        for (auto& e : loctext::read(caff::Caff::parse(d)).entries) reference_[e.key] = e.text;
                        showReference_ = true;
                    }
                } catch (...) {
                }
            }
        }
        rebuildOrder();
    }

    std::string title() const override { return title_; }
    bool dirty() const override { return dirty_; }
    bool canSave() const override { return true; }

    void draw(App& app) override {
        ImGui::SetNextItemWidth(em(260));
        inputText("##filter", filter_);
        if (filter_.empty() && !ImGui::IsItemActive())
            ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetItemRectMin().x + ImGui::GetStyle().FramePadding.x, ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                                "Search names and text...");
        ImGui::SameLine();
        if (ImGui::Button("Add string...")) {
            newName_.clear();
            addError_.clear();
            ImGui::OpenPopup("Add string");
        }
        ImGui::SameLine();
        if (ImGui::Button("Export JSON...")) {
            Json j = loctext::toJson(table_);
            if (!l_.caff->symbols.empty()) j["asset"] = assetdb::baseAssetName(l_.caff->symbolName(0));
            std::string text = j.dump();
            app.saveFileDialog("Export strings", {{"JSON", "json"}}, [&app, text](const fs::path& p) {
                writeText(p, text);
                app.status("Exported strings to " + toUtf8(p));
            }, fromUtf8(ops_safe(title_) + ".json"));
        }
        ImGui::SameLine();
        if (ImGui::Button("Import JSON...")) {
            bool prune = prune_;
            app.openFileDialog("Import strings", {{"JSON", "json"}}, [this, &app, prune](const fs::path& p) {
                size_t before = table_.entries.size();
                loctext::applyJson(table_, Json::parse(readText(p)), prune);
                rebuildOrder();
                dirty_ = true;
                app.status("Imported strings from " + toUtf8(p) + " (" + std::to_string(before) + " strings before, " +
                           std::to_string(table_.entries.size()) + " now)");
            });
        }
        ImGui::SameLine();
        ImGui::Checkbox("Drop strings it lacks", &prune_);
        helpMarker("Import matches strings by name and adds new names. With Drop strings it lacks, strings the JSON doesn't list "
                   "are removed (the command line's --prune). Export/import use the same JSON format as the command line tool.");
        if (!reference_.empty()) {
            ImGui::SameLine();
            ImGui::Checkbox("Show English", &showReference_);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu strings", table_.entries.size());
        drawAddPopup(app);

        std::string needle = lower(filter_);
        std::vector<size_t> rows;
        for (size_t idx : order_) {
            const auto& e = table_.entries[idx];
            if (needle.empty() || lower(e.name).find(needle) != std::string::npos || lower(e.text).find(needle) != std::string::npos) rows.push_back(idx);
        }
        float editorH = em(230);
        float tableH = std::max(em(120), ImGui::GetContentRegionAvail().y - editorH);
        int cols = showReference_ ? 3 : 2;
        ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("##strings", cols, tf, ImVec2(0, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.3f);
            ImGui::TableSetupColumn(("Text (" + lang_ + ")").c_str(), ImGuiTableColumnFlags_WidthStretch, 0.4f);
            if (showReference_) ImGui::TableSetupColumn("English", ImGuiTableColumnFlags_WidthStretch, 0.3f);
            ImGui::TableHeadersRow();
            ImGuiListClipper clip;
            clip.Begin(int(rows.size()));
            while (clip.Step()) {
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                    size_t idx = rows[size_t(r)];
                    const auto& e = table_.entries[idx];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(int(idx));
                    if (ImGui::Selectable(e.name.empty() ? "(unnamed)" : e.name.c_str(), selected_ == int(idx), ImGuiSelectableFlags_SpanAllColumns))
                        selected_ = int(idx);
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(firstLine(e.text).c_str());
                    if (showReference_) {
                        ImGui::TableNextColumn();
                        auto it = reference_.find(e.key);
                        ImGui::TextDisabled("%s", it == reference_.end() ? "" : firstLine(it->second).c_str());
                    }
                }
            }
            ImGui::EndTable();
        }
        drawEditor(app);
    }

    bool save(App& app, const fs::path* saveAs) override {
        try {
            loctext::write(*l_.caff, table_);
            fs::path target = saveAs ? *saveAs : path;
            if (!saveAs) app.backupBeforeWrite(path);
            saveCaff(target, *l_.caff);
            dirty_ = false;
            return true;
        } catch (std::exception& e) {
            app.error(std::string("save failed: ") + e.what());
            return false;
        }
    }

private:
    static std::string ops_safe(std::string s) {
        for (char& c : s) if (!(std::isalnum(uint8_t(c)) || c == '_' || c == '-')) c = '_';
        return s;
    }

    static std::string firstLine(const std::string& s) {
        auto nl = s.find('\n');
        std::string r = nl == std::string::npos ? s : s.substr(0, nl) + " ...";
        if (r.size() > 300) r = r.substr(0, 300) + "...";
        return r;
    }

    void rebuildOrder() {
        order_.clear();
        std::set<size_t> used;
        if (table_.hasOrder) {
            std::map<uint16_t, size_t> byKey;
            for (size_t i = 0; i < table_.entries.size(); ++i) byKey[table_.entries[i].key] = i;
            for (uint16_t k : table_.order) {
                auto it = byKey.find(k);
                if (it != byKey.end() && used.insert(it->second).second) order_.push_back(it->second);
            }
        }
        for (size_t i = 0; i < table_.entries.size(); ++i) if (used.insert(i).second) order_.push_back(i);
    }

    void drawEditor(App& app) {
        ImGui::Separator();
        if (selected_ < 0 || size_t(selected_) >= table_.entries.size()) {
            ImGui::TextDisabled("Select a string to edit it. Markup such as {MOODSAD} is part of the text.");
            return;
        }
        auto& e = table_.entries[size_t(selected_)];
        ImGui::Text("%s", e.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("key 0x%04x", e.key);
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete string")) {
            table_.entries.erase(table_.entries.begin() + selected_);
            selected_ = -1;
            rebuildOrder();
            dirty_ = true;
            app.status("Deleted string");
            return;
        }
        auto ref = reference_.find(e.key);
        if (ref != reference_.end()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy English")) {
                e.text = ref->second;
                dirty_ = true;
            }
        }
        // ImGui's multi-line input cannot word-wrap, so wrapped previews sit next to it.
        float w = ImGui::GetContentRegionAvail().x;
        float h = ImGui::GetContentRegionAvail().y;
        bool showRef = ref != reference_.end() && showReference_;
        float half = showRef ? w * 0.5f - ImGui::GetStyle().ItemSpacing.x : w;
        ImGui::PushID(selected_);
        ImGui::BeginGroup();
        if (inputTextMultiline("##edit", e.text, ImVec2(half, h * 0.5f))) dirty_ = true;
        ImGui::BeginChild("##preview", ImVec2(half, 0), ImGuiChildFlags_Borders);
        ImGui::TextDisabled("Preview");
        ImGui::TextWrapped("%s", e.text.c_str());
        ImGui::EndChild();
        ImGui::EndGroup();
        if (showRef) {
            ImGui::SameLine();
            ImGui::BeginChild("##ref", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGui::TextDisabled("English");
            ImGui::TextWrapped("%s", ref->second.c_str());
            ImGui::EndChild();
        }
        ImGui::PopID();
    }

    void drawAddPopup(App& app) {
        if (!ImGui::BeginPopup("Add string")) return;
        ImGui::TextUnformatted("Name of the new string (for example dialog__mynewline):");
        ImGui::SetNextItemWidth(em(360));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = inputText("##newname", newName_, ImGuiInputTextFlags_EnterReturnsTrue);
        if (!newName_.empty()) ImGui::TextDisabled("key 0x%04x", loctext::keyHash(newName_));
        if (!addError_.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "%s", addError_.c_str());
        if (ImGui::Button("Add") || enter) {
            uint16_t key = loctext::keyHash(newName_);
            addError_.clear();
            if (newName_.empty()) addError_ = "Enter a name.";
            for (auto& x : table_.entries) {
                if (x.name == newName_) addError_ = "A string with this name already exists.";
                else if (x.key == key) addError_ = "Its key collides with '" + x.name + "'; choose another name.";
            }
            if (addError_.empty()) {
                table_.entries.push_back({key, newName_, ""});
                selected_ = int(table_.entries.size() - 1);
                rebuildOrder();
                dirty_ = true;
                app.status("Added string " + newName_);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    Loaded l_;
    loctext::Table table_;
    std::string title_, lang_;
    std::map<uint16_t, std::string> reference_;
    bool showReference_ = false;
    bool prune_ = false; // Import JSON drops the strings the file doesn't list
    std::vector<size_t> order_;
    std::string filter_;
    int selected_ = -1;
    bool dirty_ = false;
    std::string newName_, addError_;
};

} // namespace

std::unique_ptr<Document> makeTextDocument(const fs::path& p, const fs::path& root, Loaded l) {
    return std::make_unique<TextDocument>(p, root, std::move(l));
}

} // namespace nut::gui
