// renut - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.
#pragma once
#include <rex/rex_app.h>
#include "renut_engine/overlays/fps_overlay_dialog.h"
#include "renut_engine/sleep.h"
#include "renut_engine/renut_logging.h"
#include "renut_engine/overlays/renut_logging_overlay.h"


class RenutApp : public rex::ReXApp {
public:
    using rex::ReXApp::ReXApp;
    static std::unique_ptr<rex::ui::WindowedApp> Create(
        rex::ui::WindowedAppContext& ctx) {
        return std::unique_ptr<RenutApp>(new RenutApp(ctx, "renut",
            PPCImageConfig));
    }

    void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
        drawer->AddDialog(new FpsOverlayDialog(drawer));
        drawer->AddDialog(new RenuLogOverlayDialog(drawer));
    }

    void OnShutdown() override {
        DisableHighResTimer();
    }
};