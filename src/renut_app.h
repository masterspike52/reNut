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
#include <rex/ui/window.h>
//#include <rex/discord_rpc.h>
#include <functional>
#include <string>

class RenutApp : public rex::ReXApp {
public:
    RenutApp(rex::ui::WindowedAppContext& ctx, std::string_view name, rex::PPCImageInfo info)
        : rex::ReXApp(ctx, name, info), app_name_(name) {}

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

    void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
        //drawer->AddDialog(new FpsOverlayDialog(drawer));
        fps_dialog_ = std::make_unique<FpsOverlayDialog>(drawer);
        fps_dialog_->fpsManager = &fpsManager;
        drawer->AddDialog(fps_dialog_.get());
        drawer->AddDialog(new RenuLogOverlayDialog(drawer));
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