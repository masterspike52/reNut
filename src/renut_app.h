#pragma once
#include <rex/rex_app.h>
#include "renut_engine/ui/fps_overlay.h"
#include "renut_engine/app/renut_logging.h"
#include "renut_engine/ui/renut_logging_overlay.h"
#include "renut_engine/ui/path_setup_wizard.h"
#include "renut_engine/ui/online_dialog.h"
#include "renut_engine/ui/friends_dialog.h"
#include "renut_engine/ui/settings_dialog.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#include "renut_engine/app/crash_dump.h"
#include "renut_engine/app/app_paths.h"
#include "renut_engine/mods/mod_loader.h"
#include "renut_engine/game/mouse_camera.h"
#include "renut_engine/game/keyboard_input.h"
#include "renut_engine/game/graphics_menu.h"
#include <rex/ui/window.h>
//#include <rex/discord_rpc.h>
#include <functional>
#include <string>

class RenutApp : public rex::ReXApp {
public:
    RenutApp(rex::ui::WindowedAppContext& ctx, std::string_view name, rex::PPCImageInfo info)
        : rex::ReXApp(ctx, name, info), app_name_(name) {
        // Before anything is written (see app_paths.h).
        renut::paths::PrepareWritableFolder();
        renut::crash::InstallCrashDumps();
    }

    // renut.toml and the logs go where reNut can write: next to the
    // executable, or the user data folder when running as an AppImage.
    void OnConfigurePaths(rex::PathConfig& paths) override {
        if (renut::paths::IsAppImage())
            paths.config_path = renut::paths::WritableFolder() / (app_name_ + ".toml");
        config_path_ = paths.config_path;
        renut::graphics_menu::SetConfigPath(config_path_);
    }

    void OnConfigureLogging(rex::LogConfig& config) override {
        if (renut::paths::IsAppImage())
            config.log_dir = renut::paths::WritableFolder() / "logs";
    }

    static std::unique_ptr<rex::ui::WindowedApp> Create(
        rex::ui::WindowedAppContext& ctx) {
        return std::unique_ptr<RenutApp>(new RenutApp(ctx, "renut", PPCImageConfig));
    }


    // void OnPostSetup() override {
    //     rex::discord_rpc::Presence rpc;

    //     rpc.details_ = "";
    //     rpc.state_ = "";
    //     rpc.large_image_key_ = "e242d6b6-c34e-47a1-8c2a-5297fe33bce7";
    //     rpc.large_image_text_ = "renut";

    //     //rex::discord_rpc::Start(Application ID, Settings);
    //     rex::discord_rpc::Start("1520303728047951892", rpc);

    //     rex::cvar::LoadConfig("renut.toml"); 
    // }

    // The game folder is mounted and the XEX loaded; the game has not
    // started yet, so .nbpatch mods can still change what GAME: serves.
    void OnPostSetup() override {
        renut::mods::LoadMods(runtime());
    }

    // reNut's dialogs live in its own overlay (gpu/overlay_ui.h); the SDK's
    // drawer keeps only its own menus.
    void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
        (void)drawer;
        // F4 opens reNut's settings (RenutSettingsDialog); the SDK's, registered
        // just before this as bind_settings on F4, moves to F8. Binds go by
        // registration order, so the SDK's would take F4 first.
        if (rex::cvar::GetFlagByName("bind_settings") == "F4")
            rex::cvar::SetFlagByName("bind_settings", "F8");
        settings_dialog_ = std::make_unique<RenutSettingsDialog>(config_path_);
        renut::gpu::overlay::AddDialog(settings_dialog_.get());
        fps_overlay_ = std::make_unique<FpsOverlay>();
        log_overlay_ = std::make_unique<RenuLogOverlayDialog>();
        path_wizard_ = std::make_unique<PathSetupWizard>();
        online_dialog_ = std::make_unique<OnlineDialog>();
        friends_dialog_ = std::make_unique<FriendsDialog>();
        renut::gpu::overlay::AddDialog(fps_overlay_.get());
        renut::gpu::overlay::AddDialog(log_overlay_.get());
        renut::gpu::overlay::AddDialog(path_wizard_.get());
        renut::gpu::overlay::AddDialog(online_dialog_.get());
        renut::gpu::overlay::AddDialog(friends_dialog_.get());
    }

    void OnShutdown() override {
        renut::game::DetachKeyboard();
        renut::game::DetachMouseCamera();
        renut::gpu::ShutdownRenderer();
        renut::gpu::overlay::RemoveDialog(settings_dialog_.get());
        renut::gpu::overlay::RemoveDialog(fps_overlay_.get());
        renut::gpu::overlay::RemoveDialog(log_overlay_.get());
        renut::gpu::overlay::RemoveDialog(path_wizard_.get());
        renut::gpu::overlay::RemoveDialog(online_dialog_.get());
        renut::gpu::overlay::RemoveDialog(friends_dialog_.get());
    }

    // Native renderer: the SDK runs without its GPU backend and the overlay is
    // drawn by renut::gpu.
    void OnPreSetup(rex::RuntimeConfig& config) override {
        renut::gpu::ConfigureRuntime(config);
    }

    std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
        return renut::gpu::CreateOverlayDrawer();
    }

    void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
        (void)pixel_width;
        (void)pixel_height;
        renut::gpu::OnWindowResized();
    }

    std::optional<rex::PathConfig> OnFinalizePaths(
        const rex::PathConfig& defaults,
        std::function<void(rex::PathConfig)> resume) override
    {
        // Runtime setup is done: take the unhandled exception filter back if
        // anything replaced it.
        renut::crash::InstallCrashDumps();

        // The path wizard is drawn before the game starts, so the renderer
        // (and its overlay-only presentation) comes up here.
        renut::gpu::StartRenderer(app_context(), [this] { return imgui_drawer(); });
        renut::game::AttachMouseCamera(window());
        renut::game::AttachKeyboard(window());

        path_wizard_->Init(app_name_, defaults, [resume](rex::PathConfig resolved) {
            resume(resolved);
            });
        return std::nullopt;
    }

private:
    std::string      app_name_;
    std::filesystem::path config_path_;
    std::unique_ptr<RenutSettingsDialog> settings_dialog_;
    std::unique_ptr<PathSetupWizard> path_wizard_;
    std::unique_ptr<FpsOverlay> fps_overlay_;
    std::unique_ptr<RenuLogOverlayDialog> log_overlay_;
    std::unique_ptr<OnlineDialog> online_dialog_;
    std::unique_ptr<FriendsDialog> friends_dialog_;
};