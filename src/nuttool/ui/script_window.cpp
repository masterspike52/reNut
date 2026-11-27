// Tools > Script: a Lua editor that runs scripts on a worker thread (src/script.hpp).
#include "app.hpp"

#include "script.hpp"

namespace nut::gui {

namespace {

const char* kExample = R"(-- NutTool script. print() writes to the output below; reNut's docs/nuttool/cli.md has the API.
-- nut.assets is the open assets folder: relative paths start there.

local path = "Bundle/4f/685374"
if not nut.exists(path) then error(path .. " is not in this assets folder") end
local f = nut.open(path)
for _, name in ipairs(f:models()) do
  local m = f:model(name)
  print(name, m.meshes .. " meshes", m.vertices .. " vertices", #m.bones .. " bones")
end
)";

} // namespace

fs::path App::scriptAutosavePath() const {
    return settingsPath_.empty() ? fs::path() : settingsPath_.parent_path() / "script.lua";
}

void App::saveScriptText() {
    fs::path p = scriptAutosavePath();
    if (p.empty() || !scriptLoaded_ || !scriptAutosave_) return;
    try {
        writeFile(p, scriptText_.data(), scriptText_.size());
    } catch (std::exception&) {
    }
}

bool App::scriptRunning() const {
    for (auto& t : tasks_)
        if (t.get() == scriptTask_) return true;
    return false;
}

void App::runScript() {
    if (scriptRunning()) return;
    saveScriptText();
    auto out = scriptOutput_;
    {
        std::lock_guard<std::mutex> lk(out->m);
        out->text.clear();
    }
    std::string code = scriptText_;
    std::string chunk = scriptPath_.empty() ? std::string("script") : toUtf8(scriptPath_.filename());
    fs::path root = root_, dir = scriptPath_.empty() ? fs::path() : scriptPath_.parent_path();
    std::vector<std::string> args = splitCommandLine(scriptArgs_);
    runTask("Running script", false, [out, code, chunk, root, dir, args](Task& t) -> std::function<void()> {
        script::Options opt;
        opt.assets = root;
        opt.scriptDir = dir;
        opt.args = args;
        opt.cancel = &t.cancel;
        opt.output = [out](const std::string& s) {
            std::lock_guard<std::mutex> lk(out->m);
            out->text += s;
            out->scroll = true;
        };
        bool ok = script::run(code, chunk, opt);
        return [ok, out] {
            std::lock_guard<std::mutex> lk(out->m);
            out->text += ok ? "-- finished\n" : "-- failed\n";
            out->scroll = true;
        };
    });
    scriptTask_ = tasks_.back().get();
}

void App::drawScript() {
    if (!startupScript_.empty() && !root_.empty()) {
        // --script <file>: load it and run it once the assets folder is known.
        showScript_ = scriptLoaded_ = true;
        scriptAutosave_ = false;
        scriptPath_ = std::exchange(startupScript_, {});
        try {
            scriptText_ = readText(scriptPath_);
            runScript();
        } catch (std::exception& e) {
            error(e.what());
        }
    }
    if (!showScript_) return;
    if (!scriptLoaded_) {
        scriptLoaded_ = true;
        fs::path p = scriptAutosavePath();
        try {
            scriptText_ = !p.empty() && fs::exists(p) ? readText(p) : kExample;
        } catch (std::exception&) {
            scriptText_ = kExample;
        }
    }
    ImGui::SetNextWindowSize(ImVec2(em(760), em(620)), ImGuiCond_Appearing);
    bool open = showScript_;
    if (!ImGui::Begin("Script", &open)) {
        ImGui::End();
        showScript_ = open;
        return;
    }
    if (!open) saveScriptText();
    showScript_ = open;

    bool running = scriptRunning();
    ImGui::BeginDisabled(running || !script::available());
    if (ImGui::Button("Run") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl &&
                                 ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !running && script::available()))
        runScript();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ctrl+Enter");
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Stop") && running) scriptTask_->cancel = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    if (ImGui::Button("New")) {
        scriptText_ = kExample;
        scriptPath_.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open..."))
        openFileDialog("Open script", {{"Lua script", "lua"}}, [this](const fs::path& p) {
            try {
                scriptText_ = readText(p);
                scriptPath_ = p;
            } catch (std::exception& e) {
                error(e.what());
            }
        }, scriptPath_.empty() ? lastDir_ : scriptPath_.parent_path());
    ImGui::EndDisabled();
    auto saveTo = [this](const fs::path& p) {
        try {
            writeFile(p, scriptText_.data(), scriptText_.size());
            scriptPath_ = p;
            status("Saved " + toUtf8(p));
        } catch (std::exception& e) {
            error(e.what());
        }
    };
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        if (scriptPath_.empty()) saveFileDialog("Save script", {{"Lua script", "lua"}}, saveTo, lastDir_);
        else saveTo(scriptPath_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save as...")) saveFileDialog("Save script", {{"Lua script", "lua"}}, saveTo, scriptPath_.empty() ? lastDir_ : scriptPath_.parent_path());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", scriptPath_.empty() ? "(unsaved script)" : toUtf8(scriptPath_).c_str());
    if (!script::available()) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "This build has no Lua: scripts cannot run.");
    if (root_.empty()) ImGui::TextDisabled("No assets folder is open: relative paths start at the current folder.");
    ImGui::SetNextItemWidth(em(420));
    inputTextWithHint("Arguments", "the script's arg table, as on the command line", scriptArgs_);
    helpMarker("Words separated by spaces (double quotes keep spaces in one), given to the script as arg[1], arg[2]... as "
               "'nuttool script <file.lua> <arguments>' does.");

    float avail = ImGui::GetContentRegionAvail().y;
    float editorH = std::max(em(80), avail * 0.62f);
    inputTextMultiline("##script", scriptText_, ImVec2(-FLT_MIN, editorH), ImGuiInputTextFlags_AllowTabInput);
    if (ImGui::IsItemDeactivatedAfterEdit()) saveScriptText();

    ImGui::TextUnformatted("Output");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        std::lock_guard<std::mutex> lk(scriptOutput_->m);
        scriptOutput_->text.clear();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        std::lock_guard<std::mutex> lk(scriptOutput_->m);
        ImGui::SetClipboardText(scriptOutput_->text.c_str());
    }
    if (ImGui::BeginChild("##scriptout", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        std::lock_guard<std::mutex> lk(scriptOutput_->m);
        ImGui::TextUnformatted(scriptOutput_->text.data(), scriptOutput_->text.data() + scriptOutput_->text.size());
        if (scriptOutput_->scroll) {
            ImGui::SetScrollHereY(1.0f);
            scriptOutput_->scroll = false;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace nut::gui
