#pragma once
#include <rex/ui/imgui_dialog.h>
#include <rex/rex_app.h>
#include "imgui.h"
#include "renut_engine/path_config_store.h"
#include <filesystem>
#include <functional>
#include <string>
#include <shobjidl.h>
#include <windows.h>

class PathSetupWizard : public rex::ui::ImGuiDialog {
public:
    using CompletionFn = std::function<void(rex::PathConfig)>;

    explicit PathSetupWizard(rex::ui::ImGuiDrawer* drawer)
        : rex::ui::ImGuiDialog(drawer) {}

    // Call this from OnFinalizePaths. Returns true if saved paths were found
    // and resume was called immediately (wizard will not be shown).
    // Returns false if the wizard is now open and will call resume itself.
    bool Init(const std::string& app_name,
        const rex::PathConfig& defaults,
        CompletionFn on_complete)
    {
        app_name_ = app_name;

        // First-run check — if a valid saved config exists, skip the wizard entirely
        if (auto saved = PathConfigStore::TryLoad(app_name)) {
            rex::PathConfig cfg;
            cfg.game_data_root = saved->game_data_root;
            cfg.user_data_root = saved->user_data_root;
            cfg.update_data_root = saved->update_data_root;
            on_complete(cfg);
            return true;
        }

        // No saved config — show the wizard
        game_data_buf_ = defaults.game_data_root.string();
        user_data_buf_ = defaults.user_data_root.string();
        update_data_buf_ = defaults.update_data_root.string();

        game_data_buf_.resize(kBufSize, '\0');
        user_data_buf_.resize(kBufSize, '\0');
        update_data_buf_.resize(kBufSize, '\0');

        on_complete_ = std::move(on_complete);
        error_.clear();
        visible_ = true;
        return false;
    }

    void OnDraw(ImGuiIO& io) override {
        if (!visible_) return;

        ImGui::SetNextWindowSize(ImVec2(580, 230), ImGuiCond_Always);
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
            ImGuiCond_Always, ImVec2(0.5f, 0.5f));

        if (ImGui::Begin("Game Path Setup", nullptr,
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings)) {

            ImGui::TextUnformatted("Set paths before the runtime starts.");
            ImGui::Separator();
            ImGui::Spacing();

            DrawPathRow("Game Data Root", "##game_data", game_data_buf_);
            ImGui::Spacing();
            DrawPathRow("User Data Root", "##user_data", user_data_buf_);
            ImGui::Spacing();
            DrawPathRow("Update Data Root (opt.)", "##update_data", update_data_buf_);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (!error_.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", error_.c_str());
                ImGui::Spacing();
            }

            if (ImGui::Button("Continue", ImVec2(120, 0)))
                TryCommit();
        }
        ImGui::End();
    }

private:
    static constexpr size_t kBufSize = 512;

    void DrawPathRow(const char* label, const char* id, std::string& buf) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(160);
        ImGui::SetNextItemWidth(330);
        ImGui::InputText(id, buf.data(), kBufSize);
        ImGui::SameLine();
        std::string btn_id = std::string("Browse##") + id;
        if (ImGui::Button(btn_id.c_str(), ImVec2(70, 0))) {
            std::string picked = BrowseForFolder(buf.c_str());
            if (!picked.empty()) {
                buf = picked;
                buf.resize(kBufSize, '\0');
            }
        }
    }

    static std::string BrowseForFolder(const char* default_path) {
        std::string result;
        IFileDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
            return result;

        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);

        if (default_path && default_path[0]) {
            std::wstring wide(strlen(default_path) + 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, default_path, -1,
                wide.data(), (int)wide.size());
            IShellItem* initial = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(wide.c_str(), nullptr,
                IID_PPV_ARGS(&initial)))) {
                dlg->SetFolder(initial);
                initial->Release();
            }
        }

        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path_w = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path_w))) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, path_w, -1,
                        nullptr, 0, nullptr, nullptr);
                    if (len > 0) {
                        result.resize(len - 1);
                        WideCharToMultiByte(CP_UTF8, 0, path_w, -1,
                            result.data(), len, nullptr, nullptr);
                    }
                    CoTaskMemFree(path_w);
                }
                item->Release();
            }
        }

        dlg->Release();
        return result;
    }

    void TryCommit() {
        std::filesystem::path game = game_data_buf_.c_str();
        std::filesystem::path user = user_data_buf_.c_str();
        std::filesystem::path update = update_data_buf_.c_str();

        if (game.empty()) {
            error_ = "Game Data Root cannot be empty.";
            return;
        }
        if (!std::filesystem::exists(game)) {
            error_ = "Game Data Root path does not exist.";
            return;
        }
        if (user.empty()) {
            error_ = "User Data Root cannot be empty.";
            return;
        }

        visible_ = false;
        error_.clear();

        rex::PathConfig cfg;
        cfg.game_data_root = std::move(game);
        cfg.user_data_root = std::move(user);
        cfg.update_data_root = std::move(update);

        PathConfigStore::SavedPaths saved;
        saved.game_data_root = cfg.game_data_root;
        saved.user_data_root = cfg.user_data_root;
        saved.update_data_root = cfg.update_data_root;
        PathConfigStore::Save(app_name_, saved);

        on_complete_(std::move(cfg));
    }

    std::string  app_name_;
    bool         visible_ = false;
    std::string  game_data_buf_;
    std::string  user_data_buf_;
    std::string  update_data_buf_;
    std::string  error_;
    CompletionFn on_complete_;
};