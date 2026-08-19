#pragma once
#include <rex/rex_app.h>
#include "renut_engine/overlays/fps_overlay_dialog.h"
#include "renut_engine/renut_logging.h"
#include "renut_engine/overlays/renut_logging_overlay.h"
#include "renut_engine/overlays/path_setup_wizard.h"
#include "renut_engine/overlays/mnk_controls_dialog.h"
#include "renut_engine/mnk_controls.h"
#include "renut_engine/Timer.h"
#include "renut_engine/Fps.h"
#include "renut_engine/hooks.h"
#include <rex/cvar.h>
#include <rex/ui/window.h>
#ifdef _WIN32
#include <rex/discord_rpc.h>
#endif
#include <functional>
#include <string>

// DebugHubOverlayDialog (Performance/A-B-benchmark/Native-shader tabs) --
// cross-platform (Windows/Linux/macOS): its plugin-symbol lookups go through
// dl_compat.h's dlopen/dlsym shim, which resolves to GetModuleHandle/
// GetProcAddress on Windows and dlopen/dlsym elsewhere. Every tab degrades
// gracefully (shows "not available") when its data source -- a patched SDK,
// or the nativevk plugin -- isn't present, rather than failing to build.
#include "renut_engine/overlays/debug_hub_overlay.h"

#ifndef _WIN32
#include "renut_engine/linuxfixes/xdg_paths.h"

// Defined in the SDK (src/core/logging.cpp) at global scope. When non-empty it
// takes precedence over the exe-relative logs/ directory that rex_app.cpp would
// otherwise hardcode.
REXCVAR_DECLARE(std::string, log_file);
#endif

class RenutApp : public rex::ReXApp {
public:
    RenutApp(rex::ui::WindowedAppContext& ctx, std::string_view name, rex::PPCImageInfo info)
        : rex::ReXApp(ctx, name, info), app_name_(name) {}

    static std::unique_ptr<rex::ui::WindowedApp> Create(
        rex::ui::WindowedAppContext& ctx) {
        return std::unique_ptr<RenutApp>(new RenutApp(ctx, "renut", PPCImageConfig));
    }

#ifndef _WIN32
    // Runs before the SDK loads the config and before logging is initialised
    // (rex_app.cpp:146), which is the only point where both destinations can
    // still be redirected.
    //
    // Without this, renut writes <exe_dir>/renut.toml and <exe_dir>/logs/ --
    // fine in a build tree, fatal inside a read-only AppImage/Flatpak mount.
    void OnConfigurePaths(rex::PathConfig& paths) override {
        namespace lf = renut::linuxfixes;

        // ~/.config/renut/renut.toml, migrating any existing exe-relative copy.
        paths.config_path = lf::ResolveWithMigration(lf::ConfigDir(),
                                                     std::string(GetName()) + ".toml");

        // ~/.local/state/renut/logs/renut_NNN.log. Setting log_file is the only
        // way to move the log directory, since rex_app.cpp hardcodes exe_dir/logs
        // whenever this cvar is empty -- so we do the sequential numbering that
        // the SDK would otherwise have done for us.
        if (REXCVAR_GET(log_file).empty()) {
            auto log_path = lf::NextSequentialLog(lf::LogDir(), std::string(GetName()));
            if (!log_path.empty()) {
                REXCVAR_SET(log_file, log_path.string());
            }
            // If the state dir could not be created we leave the cvar empty and
            // let the SDK fall back to its exe-relative default.
        }
    }
#endif

    void OnPostSetup() override {
    #ifdef _WIN32
        rex::discord_rpc::Presence rpc;

        rpc.details_ = "";
        rpc.state_ = "";
        rpc.large_image_key_ = "e242d6b6-c34e-47a1-8c2a-5297fe33bce7";
        rpc.large_image_text_ = "renut";

        //rex::discord_rpc::Start(Application ID, Settings);
        rex::discord_rpc::Start("1520303728047951892", rpc);

        rex::cvar::LoadConfig("renut.toml");
    #endif
        rex::cvar::SetFlagByName("gpu_allow_invalid_fetch_constants", "true");
        rex::cvar::SetFlagByName("readback_resolve", "none");
        rex::cvar::SetFlagByName("readback_memexport", "false");
    }

    void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
        //drawer->AddDialog(new FpsOverlayDialog(drawer));
        fps_dialog_ = std::make_unique<FpsOverlayDialog>(drawer);
        fps_dialog_->fpsManager = &fpsManager;
        drawer->AddDialog(fps_dialog_.get());
        drawer->AddDialog(new RenuLogOverlayDialog(drawer));
        drawer->AddDialog(new DebugHubOverlayDialog(drawer));
        path_wizard_ = new PathSetupWizard(drawer);
        drawer->AddDialog(path_wizard_);

        // Keyboard & mouse: the window exists by the time dialogs are created,
        // so this is where the host-side listener gets attached.
        mnk_dialog_ = std::make_unique<MnkControlsDialog>(drawer);
        drawer->AddDialog(mnk_dialog_.get());
        renut::mnk::AttachWindow(window());
    }

    void OnShutdown() override {
        renut::mnk::DetachWindow();
    }

    std::optional<rex::PathConfig> OnFinalizePaths(
        const rex::PathConfig& defaults,
        std::function<void(rex::PathConfig)> resume) override
    {
        if (!path_wizard_) {
            RNUT_WARN("path setup wizard unavailable (no graphics/ImGui surface); "
                      "using default paths");
            return defaults;
        }

        path_wizard_->Init(app_name_, defaults, [resume](rex::PathConfig resolved) {
            resume(resolved);
            });
        return std::nullopt;
    }

private:
    std::string      app_name_;
    PathSetupWizard* path_wizard_ = nullptr;
    std::unique_ptr<FpsOverlayDialog> fps_dialog_;
    std::unique_ptr<MnkControlsDialog> mnk_dialog_;
};
