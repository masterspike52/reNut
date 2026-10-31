#pragma once
// reNut's first screen, before the game starts: the blueprint main menu
// (icon/launcher_background.png). PLAY starts the game once its files are
// there; the rest sets them up (app/game_setup.h): extracting the game disc's
// image and Title Update #3, adding DLC packages, and choosing the game,
// update and save folders, or pointing them at files already extracted.
// WORKSHOP downloads community vehicles into the save (app/workshop.h);
// FULLSCREEN is the SDK's fullscreen setting. Mouse, keyboard or controller.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <rex/rex_app.h>

#include "renut_engine/gpu/overlay/overlay_ui.h"

namespace rex::ui {
class Window;
}

class LauncherMenu : public renut::gpu::overlay::Dialog {
public:
    using PlayFn = std::function<void(rex::PathConfig)>;
    using QuitFn = std::function<void()>;

    LauncherMenu();
    ~LauncherMenu() override;

    // From OnFinalizePaths: shows the menu. config_path is renut.toml, where
    // OPTIONS saves; window is the game's, whose keys a key bind takes.
    // on_play gets the chosen paths (saved to renut.cfg) when PLAY is picked;
    // on_quit closes reNut.
    void Init(const std::string &app_name, const rex::PathConfig &defaults,
              const std::filesystem::path &config_path, rex::ui::Window *window, PlayFn on_play,
              QuitFn on_quit);

    void Draw() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
