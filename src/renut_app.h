// renut - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.
#pragma once
#include <rex/rex_app.h>
#include "renut_engine/overlays/fps_overlay_dialog.h"
#include "renut_engine/overlays/renut_logging_overlay.h"



class RenutApp : public rex::ReXApp {
public:
    using rex::ReXApp::ReXApp;
    static std::unique_ptr<rex::ui::WindowedApp> Create(
        rex::ui::WindowedAppContext& ctx) {
        return std::unique_ptr<RenutApp>(new RenutApp(ctx, "renut",
            PPCImageConfig));
    }
    // Override virtual hooks for customization:
    // void OnPreSetup(rex::RuntimeConfig& config) override {}
    // void OnPostSetup() override {}
    // void OnShutdown() override {}
    // void OnConfigurePaths(rex::PathConfig& paths) override {}

    void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
        drawer->AddDialog(new FpsOverlayDialog(drawer));
    }
    //void OnConfigurePaths(rex::PathConfig& paths) override {
    //    // Redirect game data root from assets/debug to assets/bundle
    //    paths.game_data_root = paths.game_data_root.parent_path() / "assets";
    //}
};