// Android: SDL's Java activity (android/) loads librenut.so and calls SDL_main.
// This stands in for the SDK's SDL entry point (windowed_app_main_sdl.cpp,
// taken out of the Android build in CMakeLists.txt), which expects one app per
// executable: on Android the SDK registers apps by name instead
// (XE_UI_WINDOWED_APPS_IN_LIBRARY, rex/ui/windowed_app.h). Same steps, after
// the Android setup.

#include <unistd.h>

#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include <SDL3/SDL_system.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/thread.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>

extern "C" __attribute__((visibility("default"))) int SDL_main(int argc, char *argv[]) {
    // The libc/libandroid functions the SDK's POSIX code loads at run time.
    rex::memory::AndroidInitialize();
    rex::thread::AndroidInitialize();
    // The app's external files directory is reNut's folder (app_paths.h) and
    // the SDK's user data folder; relative paths resolve there.
    if (const char *home = SDL_GetAndroidExternalStoragePath()) {
        setenv("XDG_DATA_HOME", home, 1);
        chdir(home);
    }

    auto remaining = rex::cvar::Init(argc, argv);
    rex::cvar::ApplyEnvironment();
    rex::InitLoggingEarly();

    int result = EXIT_FAILURE;
    {
        rex::ui::SDLWindowedAppContext app_context;
        auto creator = rex::ui::WindowedApp::GetCreator("renut");
        if (creator && app_context.Initialize()) {
            std::unique_ptr<rex::ui::WindowedApp> app = creator(app_context);
            const auto &option_names = app->GetPositionalOptions();
            std::map<std::string, std::string> parsed;
            for (size_t i = 0; i < remaining.size() && i < option_names.size(); ++i)
                parsed[option_names[i]] = remaining[i];
            app->SetParsedArguments(std::move(parsed));
            result = app->OnInitialize() ? app_context.RunMainMessageLoop() : EXIT_FAILURE;
            app->InvokeOnDestroy();
        }
    }

    rex::thread::AndroidShutdown();
    rex::memory::AndroidShutdown();
    return result;
}
