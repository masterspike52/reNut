#pragma once
#include "generated/renut_init.h"
#include <rex/app/app.h>
#include <rex/app/title_app.h>
#include <rex/input_keys.h>
#include <rex/kernel/title_runtime.h>
#include <rex/logging/logger.h>
#include <rex/path_settings.h>
#include <rex/platform/process.h>
#include <rex/result.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/string/text.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/ui_system.h>
#include <rex/utility.h>
#include "renut_engine/ui/fps_overlay.h"
#include "renut_engine/ui/position_overlay.h"
#include "renut_engine/app/renut_logging.h"
#include "renut_engine/ui/renut_logging_overlay.h"
#include "renut_engine/ui/launcher_menu.h"
#include <rexcrash/rexcrash.h>
#include <rexfabric/rexfabric.h>
#include "renut_engine/ui/settings_dialog.h"
#include "renut_engine/ui/flag_editor_dialog.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#include "renut_engine/gpu/overlay/draw_list_split.h"
#include "renut_engine/app/crash_dump.h"
#include "renut_engine/app/app_paths.h"
#include "renut_engine/app/dlc_install.h"
#include "renut_engine/app/game_setup.h"
#include "renut_engine/app/path_config_store.h"
#include "renut_engine/app/renut_settings.h"
#include "renut_engine/app/save_headers.h"
#include "renut_engine/mods/mod_loader.h"
#include "renut_engine/game/cheats.h"
#include "renut_engine/game/mouse_camera.h"
#include "renut_engine/game/controllers.h"
#include "renut_engine/game/keyboard_input.h"
#include "renut_engine/game/graphics_menu.h"
#include "renut_engine/game/custom_worlds.h"
#include "renut_engine/game/custom_npcs.h"
#include "renut_engine/game/custom_shapes.h"
#include "renut_engine/game/custom_materials.h"
#include "renut_engine/game/sound_api.h"
#include "renut_engine/game/menu_api.h"
#include "renut_engine/online/net_api.h"
#include "renut_engine/game/mod_data.h"
#include "renut_engine/game/renut_lua.h"
#include "renut_engine/game/part_api.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>

#include <rexglass/rexglass.h>
#include <rexglue_guide/guide.h>

class RenutApp : public rex::app::TitleApp, public rex::ui::IUIEventListener {
public:
    static constexpr const char *kAppName = "renut";

    RenutApp() : rex::app::TitleApp(kAppName, kPPCImageConfig), app_name_(kAppName) {
        renut::paths::PrepareWritableFolder();
        renut::crash::InstallCrashDumps();
    }

    // The launcher answered: PLAY set the title's folders, or reNut is closing.
    bool LauncherDone() const { return launcher_done_; }

protected:
    // reNut presents through its own renderer, so no Xenos driver loads.
    rex::AppSysRequirements GetRequirements() const override {
        return {.graphics_family = rex::GraphicsFamily::kNone, .audio = true, .input = true, .ui = true};
    }

    // RexCrash's reporter asks the player, so the SDK's own shows no dialog.
    rex::platform::CrashReporterOptions GetCrashOptions() override {
        rex::platform::CrashReporterOptions options = rex::app::TitleApp::GetCrashOptions();
        options.app_name = app_name_;
        options.show_dialog = false;
        return options;
    }

    rex::Result<void> OnConfigure(rex::SettingsRegistry &settings) override {
        REX_TRY_VOID(renut::settings::RegisterAll(settings));
        REX_TRY_VOID(REX_LOGGER().AddCategories(rex::log::kRenutLog));
        REX_TRY_VOID(rexcrash::Configure(settings));
        REX_TRY_VOID(ConfigureThirdPartyUI(settings));

        const auto title = [&settings](std::string_view name, std::string_view value) {
            (void)settings.SetText(rex::SettingLayer::kTitle, name, value);
        };
        if (renut::paths::IsAppImage()) {
            const std::filesystem::path folder = renut::paths::WritableFolder();
            title("app.config_file", rex::string::PathToUTF8(folder / (app_name_ + ".toml")));
            title("app.log_root", rex::string::PathToUTF8(folder / "logs"));
        }
        // The SDK's settings window moves to F8: F4 is reNut's own.
        title("ui.bind.settings", "F8");
        // No title memory cap, as before the SDK added one (480 MiB): multiplayer and reNut's GPU records go past it.
        title("runtime.title_memory_mb", "0");
        // The save folder is fixed from startup, so the launcher's choice (renut.cfg) applies here.
        if (auto saved = PathConfigStore::TryLoad(app_name_); saved && !saved->user_data_root.empty())
            (void)settings.SetText(rex::SettingLayer::kCommandLine, "app.user_root",
                                   rex::string::PathToUTF8(saved->user_data_root));
        return rex::Ok();
    }

    rex::Result<void> OnInit() override {
        (void)renut::gpu::StartRenderer(GetUI(), GetGraphics());
        config_path_ = REX_PATHS().config_file.Get();
        renut::graphics_menu::SetConfigPath(config_path_);

        rexfabric::Setup({.mods_dir = renut::mods::ModsFolder(), .menu_key = "F9", .node_editor_key = "F10"});
        renut::game::InstallModInput();
        renut::game::part_api::Declare();
        renut::game::custom_worlds::Declare();
        renut::game::custom_npcs::Declare();
        renut::game::custom_shapes::Declare();
        renut::game::custom_materials::Declare();
        renut::game::sound_api::Declare();
        renut::game::menu_api::Declare();
        renut::online::net_api::Declare();
        renut::game::mod_data::Declare();
        renut::game::renut_lua::Declare();  // last: its Lua builds on the others'
        rexcrash::Start({.environment = RENUT_SENTRY_ENVIRONMENT,
                         .app_name = app_name_,
                         .database_path = renut::paths::WritableFolder() / "sentry",
                         .config_path = config_path_,
                         .mods = [] {
                             std::vector<rexcrash::Mod> mods;
                             for (const rexfabric::InstalledMod& mod : rexfabric::InstalledMods())
                                 mods.push_back({mod.name, mod.version, mod.enabled});
                             for (std::string& name : renut::mods::LoadedMods())
                                 mods.push_back({std::move(name), "", true});
                             return mods;
                         }});
        rexcrash::SetTag("build", RENUT_BUILD);

        settings_dialog_ = std::make_unique<RenutSettingsDialog>(config_path_);
        renut::gpu::overlay::AddDialog(settings_dialog_.get());
        flag_editor_ = std::make_unique<FlagEditorDialog>();
        renut::gpu::overlay::AddDialog(flag_editor_.get());
        fps_overlay_ = std::make_unique<FpsOverlay>();
        position_overlay_ = std::make_unique<PositionOverlay>();
        log_overlay_ = std::make_unique<RenuLogOverlayDialog>();
        launcher_ = std::make_unique<LauncherMenu>();
        renut::gpu::overlay::AddDialog(launcher_.get());
        renut::gpu::overlay::AddDialog(fps_overlay_.get());
        renut::gpu::overlay::AddDialog(position_overlay_.get());
        renut::gpu::overlay::AddDialog(log_overlay_.get());
        (void)REX_SETTINGS().SetText(rex::SettingLayer::kRuntime, "hid.guide_button", "true");
        InstallThirdPartyUI();
        renut::gpu::overlay::SplitLargeDrawLists(GetUI().GetImGuiIO().Ctx);

        // Sentry's filter doesn't chain, so reNut's minidump filter goes back on top of it.
        renut::crash::InstallCrashDumps();
        // Listeners hear events in the order added: the launcher's key capture, the keyboard, then the mouse camera.
        launcher_->Init(
            app_name_, CurrentPaths(), config_path_, GetUI(),
            [this](renut::setup::Paths paths) { Play(paths); },
            [] { rex::app::RequestShutdown(0); });
        renut::game::AttachKeyboard(GetUI());
        ui_listener_guard_ = GetUI().AddEventListener(*this);
        return rex::Ok();
    }

    rex::Result<void> OnTitleLaunch(rex::kernel::TitleRuntime &title) override {
        renut::game::WatchCheats();
        renut::dlc::InstallPackages(title);
        renut::mods::LoadMods(title);
        rexfabric::Start();
        renut::mods::FinishCustomParts();
        return rex::Ok();
    }

    // The mods and hooks belong to one launch, so a title that ends ends reNut.
    void OnTitleExit(rex::kernel::TitleExitReason reason) override {
        (void)reason;
        rex::app::RequestShutdown(0);
    }

    void OnShutdown() override {
        ui_listener_guard_.Reset();
        ShutdownThirdPartyUI();
        renut::game::DetachKeyboard();
        renut::game::DetachMouseCamera();
        renut::gpu::ShutdownRenderer();
        renut::gpu::overlay::RemoveDialog(settings_dialog_.get());
        renut::gpu::overlay::RemoveDialog(flag_editor_.get());
        renut::gpu::overlay::RemoveDialog(fps_overlay_.get());
        renut::gpu::overlay::RemoveDialog(position_overlay_.get());
        renut::gpu::overlay::RemoveDialog(log_overlay_.get());
        renut::gpu::overlay::RemoveDialog(launcher_.get());
        // Their listener and log sink guards go while the UI system and the logger are still up.
        launcher_.reset();
        log_overlay_.reset();
        rexfabric::Stop();
        rexcrash::Stop();
    }

    // The window closing: the mods stop before the title's threads do (Stop runs once).
    void OnUIEvent(const rex::ui::UIEvent &event) override {
        if (event.GetType() == rex::ui::UIEvent::Type::kClosing)
            rexfabric::Stop();
    }

    void OnInputEvent(rex::InputEvent &event) override { (void)event; }

private:
    // The Guide's and RexFabric's settings and log categories, before the config loads.
    rex::Result<void> ConfigureThirdPartyUI(rex::SettingsRegistry &settings) {
        REX_TRY_VOID(rexfabric::Configure(settings));
        REX_TRY_VOID(rexglue_guide::Configure(settings));
        return rex::Ok();
    }

    // OnInit, after reNut's dialogs (the Guide draws on top) and before SplitLargeDrawLists (after RexGlass's hook).
    void InstallThirdPartyUI() {
        rexglue_guide::ConfigureFonts(&GetUI().GetFonts());
        rexglue_guide::AddCredit("MadLadMikael", "reNut Lead Programmer");
        rexglue_guide::AddCredit("EtoneDemid", "Nutstaller");
        rexglue_guide::AddCredit("Rayzo", "Making the reNut Title Logo");
        rexglue_guide::AddCredit("Rayzo", "Making the reNut Settings icon");
        rexglue_guide::AddCredit("SolarCookies", "Various Small Things");
        rexglue_guide::AddCredit("You, The Player", "Playing the game.");
        rexglue_guide::Install(GetUI(),
                               {.title_name = "Banjo-Kazooie: Nuts & Bolts",
                                .data_dir = renut::paths::WritableFolder() / "guide",
                                .on_quit = [] { rex::app::RequestShutdown(0); },
                                .ready_for_sign_in = [this] { return paths_ready_.load(); },
                                .create_texture = renut::gpu::CreateOverlayTexture,
                                .destroy_texture = renut::gpu::DestroyOverlayTexture,
                                .player_for_controller = [](int pad) {
                                    return renut::game::ControllerPlayer(pad);
                                },
                                .game_frame = renut::gpu::GameFrameTexture,
                                .blurred_game_frame = renut::gpu::BlurredGameFrameTexture,
                                .config_file = config_path_,
                                .leaderboard_block = [] {
                                    return renut::game::AnyCheatOn()
                                               ? std::string("cheats were used this session; restart the game to post again")
                                               : std::string();
                                },
                                .build = RENUT_BUILD});
        // Glass only for the windows RexFabric opens with it (mods menu, node editor); reNut's own keep their look.
        rexglass::Install(GetUI().GetImGuiIO().Ctx,
                          {.backdrop = renut::gpu::BlurredGameFrameTexture, .screen = renut::gpu::ScreenTexture});
        (void)rexfabric::CreateMenus(GetUI(), {renut::gpu::CreateOverlayTexture, renut::gpu::DestroyOverlayTexture});
    }

    // OnShutdown, first, on the UI thread: the Guide frees its textures while the renderer is up.
    void ShutdownThirdPartyUI() {
        rexglue_guide::Shutdown();
    }

    // The folders in use: the title's from its settings, the save folder fixed at startup.
    static renut::setup::Paths CurrentPaths() {
        return {.game = PathSetting("title.game_root"),
                .update = PathSetting("title.update_root"),
                .user = REX_PATHS().user_root.Get()};
    }

    static std::filesystem::path PathSetting(std::string_view name) {
        auto found = REX_SETTINGS().Find(name);
        if (!found || (*found)->GetType() != rex::SettingType::kPath)
            return {};
        return static_cast<rex::Setting<std::filesystem::path> *>(*found)->Get();
    }

    static bool SameFolder(const std::filesystem::path &a, const std::filesystem::path &b) {
        std::error_code ec;
        return std::filesystem::weakly_canonical(std::filesystem::absolute(a, ec), ec) ==
               std::filesystem::weakly_canonical(std::filesystem::absolute(b, ec), ec);
    }

    // The launcher's PLAY, on the UI thread: RenutMain's loop then launches the title.
    // Another save folder can't apply while running, so reNut restarts into it (renut.cfg has it).
    void Play(const renut::setup::Paths &paths) {
        const std::filesystem::path user = REX_PATHS().user_root.Get();
        if (!SameFolder(paths.user, user)) {
            rex::app::RequestRestart();
            return;
        }
        (void)REX_SETTINGS().SetText(rex::SettingLayer::kRuntime, "title.game_root",
                                     rex::string::PathToUTF8(paths.game));
        (void)REX_SETTINGS().SetText(rex::SettingLayer::kRuntime, "title.update_root",
                                     rex::string::PathToUTF8(paths.update));
        renut::game::AttachMouseCamera(GetUI());
        renut::dlc::SetUserDataRoot(user);
        renut::saves::FixPackageHeaders(user);
        launcher_done_ = true;
        paths_ready_ = true;
    }

    std::string      app_name_;
    std::filesystem::path config_path_;
    std::unique_ptr<RenutSettingsDialog> settings_dialog_;
    std::unique_ptr<FlagEditorDialog> flag_editor_;
    std::unique_ptr<LauncherMenu> launcher_;
    bool launcher_done_ = false;
    std::atomic<bool> paths_ready_{false};
    std::unique_ptr<FpsOverlay> fps_overlay_;
    std::unique_ptr<PositionOverlay> position_overlay_;
    std::unique_ptr<RenuLogOverlayDialog> log_overlay_;
    rex::ScopeGuard ui_listener_guard_;
};
