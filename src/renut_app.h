#pragma once
#include <rex/rex_app.h>
#include "renut_engine/ui/fps_overlay.h"
#include "renut_engine/app/renut_logging.h"
#include "renut_engine/ui/renut_logging_overlay.h"
#include "renut_engine/ui/path_setup_wizard.h"
#include <rexglue_guide/guide.h>
#include <sentrywork/sentrywork.h>
#include "renut_engine/ui/settings_dialog.h"
#include "renut_engine/ui/flag_editor_dialog.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#include "renut_engine/app/crash_dump.h"
#include "renut_engine/app/app_paths.h"
#include "renut_engine/app/dlc_install.h"
#include "renut_engine/app/save_headers.h"
#include "renut_engine/mods/mod_loader.h"
#include "renut_engine/game/cheats.h"
#include "renut_engine/game/mouse_camera.h"
#include "renut_engine/game/controllers.h"
#include "renut_engine/game/keyboard_input.h"
#include "renut_engine/game/graphics_menu.h"
#include <rex/ui/window.h>
#include <rex/ui/overlay/achievement_notification.h>
//#include <rex/discord_rpc.h>
#include <atomic>
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
    // executable, or the user data folder when running as an AppImage. The
    // SDK puts the logs next to the executable unless log_file is set; it
    // loads renut.toml after this, so a log_file set there still wins.
    void OnConfigurePaths(rex::PathConfig& paths) override {
        if (renut::paths::IsAppImage()) {
            paths.config_path = renut::paths::WritableFolder() / (app_name_ + ".toml");
            if (REXCVAR_GET(log_file).empty())
                rex::cvar::SetFlagByName(
                    "log_file", (renut::paths::WritableFolder() / "logs" / (app_name_ + ".log")).string());
        }
        config_path_ = paths.config_path;
        renut::graphics_menu::SetConfigPath(config_path_);
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

    // Crash reports (thirdparty/sentrywork): renut.toml and the log are
    // loaded now, and the runtime, whose exception handler has to come after
    // Sentry's, is not yet built.
    void OnPostInitLogging() override {
        sentrywork::Init({.app_name = app_name_,
                          .database_dir = renut::paths::WritableFolder() / "sentry",
                          .tags = {{"build", RENUT_BUILD}}});
    }

    // The game folder is mounted and the XEX loaded; the game has not
    // started yet, so .nbpatch mods can still change what GAME: serves, and
    // DLC packages installed now are there when it looks for them.
    void OnPostSetup() override {
        sentrywork::OnRuntimeReady(runtime());
        renut::game::WatchCheats();
        renut::dlc::InstallPackages(runtime());
        renut::mods::LoadMods(runtime());
    }

    void OnConfigureFonts(ImFontAtlas* atlas) override {
        rexglue_guide::ConfigureFonts(atlas);
    }

    // reNut's dialogs live in its own overlay (gpu/overlay_ui.h); the SDK's
    // drawer keeps its own menus and the Rexglue Guide (RexLiveless).
    void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
        if (rex::cvar::GetFlagByName("bind_settings") == "F4")
            rex::cvar::SetFlagByName("bind_settings", "F8");
        settings_dialog_ = std::make_unique<RenutSettingsDialog>(config_path_);
        renut::gpu::overlay::AddDialog(settings_dialog_.get());
        flag_editor_ = std::make_unique<FlagEditorDialog>();
        renut::gpu::overlay::AddDialog(flag_editor_.get());
        fps_overlay_ = std::make_unique<FpsOverlay>();
        log_overlay_ = std::make_unique<RenuLogOverlayDialog>();
        path_wizard_ = std::make_unique<PathSetupWizard>();
        renut::gpu::overlay::AddDialog(fps_overlay_.get());
        renut::gpu::overlay::AddDialog(log_overlay_.get());
        renut::gpu::overlay::AddDialog(path_wizard_.get());
        rex::cvar::SetFlagByName("guide_button", "true");
        rexglue_guide::AddCredit("MadLadMikael", "reNut Lead Programmer");
        rexglue_guide::AddCredit("EtoneDemid", "Nutstaller");
        rexglue_guide::AddCredit("ValcomDrifty", "reNut Title Logo");
        rexglue_guide::AddCredit("SolarCookies", "Various Small Things");
        rexglue_guide::AddCredit("Rayzo", "Making the reNut Settings icon");
        rexglue_guide::AddCredit("You, The Player", "Playing the game.");
        rexglue_guide::Install(this, drawer,
                               {.title_name = "Banjo-Kazooie: Nuts & Bolts",
                                .data_dir = renut::paths::WritableFolder() / "guide",
                                .on_quit = [this] {
                                    if (auto* w = window()) w->RequestClose();
                                },
                                .ready_for_sign_in = [this] { return paths_ready_.load(); },
                                .immediate_drawer = immediate_drawer(),
                                .player_for_controller = [](int pad) {
                                    return renut::game::ControllerPlayer(pad);
                                },
                                .game_frame = renut::gpu::GameFrameTexture,
                                .leaderboard_block = [] {
                                    return renut::game::AnyCheatOn()
                                               ? std::string("cheats were used this session; restart the game to post again")
                                               : std::string();
                                },
                                .build = RENUT_BUILD});
    }

    // The Guide shows achievement toasts itself.
    std::unique_ptr<rex::ui::AchievementNotificationDialog>
    CreateAchievementNotificationDialog() override { return nullptr; }

    void OnShutdown() override {
        rexglue_guide::Shutdown();
        renut::game::DetachKeyboard();
        renut::game::DetachMouseCamera();
        renut::gpu::ShutdownRenderer();
        renut::gpu::overlay::RemoveDialog(settings_dialog_.get());
        renut::gpu::overlay::RemoveDialog(flag_editor_.get());
        renut::gpu::overlay::RemoveDialog(fps_overlay_.get());
        renut::gpu::overlay::RemoveDialog(log_overlay_.get());
        renut::gpu::overlay::RemoveDialog(path_wizard_.get());
        sentrywork::Shutdown();
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

        path_wizard_->Init(app_name_, defaults, [this, resume](rex::PathConfig resolved) {
            renut::dlc::SetUserDataRoot(resolved.user_data_root);
            renut::saves::FixPackageHeaders(resolved.user_data_root);
            resume(resolved);
            paths_ready_ = true;
            });
        return std::nullopt;
    }

private:
    std::string      app_name_;
    std::filesystem::path config_path_;
    std::unique_ptr<RenutSettingsDialog> settings_dialog_;
    std::unique_ptr<FlagEditorDialog> flag_editor_;
    std::unique_ptr<PathSetupWizard> path_wizard_;
    // The path wizard is done and the game is starting: the Guide may show
    // its sign-in screen.
    std::atomic<bool> paths_ready_{false};
    std::unique_ptr<FpsOverlay> fps_overlay_;
    std::unique_ptr<RenuLogOverlayDialog> log_overlay_;
};