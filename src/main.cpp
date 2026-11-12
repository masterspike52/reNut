// renut - ReXGlue Recompiled Project
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#include "generated/renut_init.h"

#include <cstdio>
#include <cstdlib>

#include <rex/app/app.h>
#include <rex/app/title_app.h>

#include "renut_app.h"

namespace {

// The launcher runs before the title: frames until PLAY sets the title's folders or reNut closes.
int RenutMain(RenutApp &app, const rex::CLIArgList &args) {
    if (app.Init(args) && !rex::app::IsShutdownRequested()) {
        while (!app.LauncherDone() && app.RunFrame()) {
        }
        if (!rex::app::IsShutdownRequested() && app.LaunchTitle()) {
            app.Run();
            // Shutdown unloads the title first, so the game's queued GPU work runs while its memory is mapped.
            renut::gpu::StopGuestRendering();
        }
    }
    const int code = app.Shutdown();
    // Shutdown has flushed the log, stopped crash reporting and started any restart. The renderer's statics hold GPU
    // objects whose device is gone, so the process ends here without running static destructors.
    std::fflush(nullptr);
    std::_Exit(code);
}

}  // namespace

REX_TITLE_APP_MAIN(RenutApp, RenutMain)
