#pragma once
// reNut's launcher menu before the game starts: PLAY, game file setup (app/game_setup.h),
// the vehicle WORKSHOP (app/workshop.h), options and launcher music.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "renut_engine/app/game_setup.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

namespace rex::ui {
class IUISystem;
}

class LauncherMenu : public renut::gpu::overlay::Dialog {
public:
    using PlayFn = std::function<void(renut::setup::Paths)>;
    using QuitFn = std::function<void()>;

    LauncherMenu();
    ~LauncherMenu() override;

    // From RenutApp::OnInit: shows the menu (OPTIONS saves to config_path). defaults are the
    // folders in use; on_play gets the chosen ones, saved to renut.cfg; on_quit closes reNut.
    void Init(const std::string &app_name, const renut::setup::Paths &defaults,
              const std::filesystem::path &config_path, rex::ui::IUISystem &ui, PlayFn on_play,
              QuitFn on_quit);

    void Draw() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
