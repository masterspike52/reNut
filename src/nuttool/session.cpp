#include "nuttool/session.h"

#include <rex/string/text.h>
#include <rex/types.h>
#include <rexfabric/rexfabric.h>

#include <cfloat>

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "app.hpp"
#include "nbpatch.hpp"
#include "nuttool/frontend.h"
#include "nuttool/studio.h"
#include "nuttool/workspace.h"
#include "renut_engine/app/app_paths.h"
#include "renut_engine/game/field_of_view.h"
#include "renut_engine/game/guest_call.h"
#include "renut_engine/game/part_api.h"
#include "renut_engine/game/stereo.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"
#include "renut_engine/mods/mod_loader.h"

REX_IMPORT(__imp__sub_82547D00, QuitToTitle, u32());  // the pause menu's quit: the running world ends, the title loads
namespace renut::nuttool {
namespace {

namespace fs = std::filesystem;
using renut::guest::Load32;
using renut::guest::Store32;

constexpr uint32_t kRunningWorld = 0x82E19AB4;
constexpr uint32_t kFinishTasks = 0x82E0B5A0;  // set before the front end loads its scenes

std::mutex g_mutex;
std::deque<GameWork> g_work;
std::atomic<bool> g_in_studio{false};
std::atomic<bool> g_leaving{false};  // Exit pressed: NutTool stops drawing (and taking the camera) at once
std::atomic<bool> g_release{false};  // the studio ended: the next world's first tick checks the camera is the game's
std::atomic<uint64_t> g_visit{0};

// The game's camera and field of view back, from NutTool's studio camera.
void GiveCameraBack() {
  renut::game::part_api::ReleaseCamera();
  renut::game::SetFieldOfView(0);
}

// NutTool over the studio: its window, and reNut's Mod menu in its menu bar.
class Session final : public renut::gpu::overlay::Dialog {
public:
  void Draw() override {
    // NutTool fills the screen: the SDK's menus (RexFabric's node editor among them) go over it while it shows.
    renut::gpu::overlay::SetUnderMenus(InStudio());
    if (!InStudio())
      return;
    if (!app_)
      Start();
    // The node editor over NutTool takes the mouse: NutTool sees it nowhere until the editor closes.
    ImGuiIO &io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool editor = rexfabric::NodeEditorShown();
    if (editor)
      io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
    app_->frame();
    if (editor)
      io.MousePos = mouse;
    if (app_->quitRequested()) {
      app_->clearQuit();
      Leave();
    }
    DrawNewMod();
  }

private:
  void Start() {
    game_ = renut::mods::GameFolder();
    mods_ = renut::mods::ModsFolder();
    CloseMod(game_, mods_);  // the scratch workspace until a mod is opened: the game's files are never written
    std::error_code ec;
    const fs::path settings = NutToolFolder();
    fs::create_directories(settings, ec);
    app_ = std::make_unique<nut::gui::App>(nullptr, game_, settings);
    app_->hostMenus = [this] {
      DrawModMenu();
      DrawMusicMenu();
    };
    // Leaving NutTool: a button at the bar's right end (asking first when a tab has unsaved changes).
    app_->hostMenusRight = [this] {
      const char *label = "Exit NutTool";
      const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 4;
      ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - width));
      ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150, 45, 45, 255));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(190, 60, 60, 255));
      if (ImGui::Button(label))
        app_->requestQuit();
      ImGui::PopStyleColor(2);
    };
  }

  void Open(const fs::path &location) {
    if (!app_->reloadForWorkspace()) {
      app_->error("Save or close the tabs with unsaved changes before opening another mod");
      return;
    }
    std::string error;
    if (OpenMod(game_, mods_, location, &error))
      app_->status("Editing the mod " + OpenModName() + ": saves go into it, and Mod > Save mod packages it");
    else
      app_->error("Couldn't open " + rex::string::PathToUTF8(location.filename()) + ": " + error);
    app_->reloadForWorkspace();
  }

  void Save() {
    const fs::path game = game_;
    const std::string author = app_->modAuthor();
    app_->runTask("Saving the mod " + OpenModName(), true, [this, game, author](nut::gui::Task &) -> std::function<void()> {
      auto log = std::make_shared<std::vector<std::string>>();
      const bool saved = SaveMod(game, author, log.get());
      return [this, log, saved] {
        for (const std::string &line : *log)
          app_->log(line);
        if (saved)
          app_->status("Saved the mod " + OpenModName() + ": reNut applies it the next time the game starts");
        else
          app_->error(log->empty() ? "Couldn't save the mod" : log->back());
      };
    });
  }

  void DrawModMenu() {
    const std::string open = OpenModName();
    if (!ImGui::BeginMenu("Mod"))
      return;
    ImGui::TextDisabled("%s", open.empty() ? "No mod open: edits wait in a scratch space" : ("Editing: " + open).c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("New mod..."))
      newMod_ = true;
    if (ImGui::BeginMenu("Open mod")) {
      const std::vector<ModEntry> mods = ListMods(mods_);
      if (mods.empty())
        ImGui::TextDisabled("The mods folder has none");
      for (const ModEntry &mod : mods)
        if (ImGui::MenuItem((mod.name + (mod.loose ? "  (.nbpatch)" : "")).c_str(), nullptr, mod.name == open))
          Open(mod.location);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Save mod", nullptr, false, !open.empty()))
      Save();
    if (ImGui::MenuItem("Close mod", nullptr, false, !open.empty()) && app_->reloadForWorkspace()) {
      CloseMod(game_, mods_);
      app_->reloadForWorkspace();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Back to the main menu"))
      app_->requestQuit();
    ImGui::EndMenu();
  }

  // The studio's music, cycled as the Stereo part's: the game's songs, then the files in reNut's music folder.
  void DrawMusicMenu() {
    const bool paused = renut::game::StudioMusicPaused();
    const std::string title = renut::game::StudioMusicTitle();
    if (ImGui::BeginMenu("Music")) {
      ImGui::TextDisabled("%s", title.empty() ? "Starting..." : (paused ? "Paused: " + title : "Playing: " + title).c_str());
      ImGui::Separator();
      if (ImGui::MenuItem("Next song", "Ctrl+Right"))
        renut::game::StudioMusicStep(1);
      if (ImGui::MenuItem("Previous song", "Ctrl+Left"))
        renut::game::StudioMusicStep(-1);
      if (ImGui::MenuItem(paused ? "Play" : "Pause", "Ctrl+M"))
        renut::game::StudioMusicPause(!paused);
      ImGui::Separator();
      ImGui::TextDisabled("Your own songs: .mp3, .ogg, .flac or .wav files in reNut's music folder");
      ImGui::EndMenu();
    }
    if (!title.empty())
      ImGui::TextDisabled("%s", ((paused ? "(paused) " : "") + title).c_str());
    const ImGuiIO &io = ImGui::GetIO();
    if (io.KeyCtrl && !io.WantTextInput) {
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
        renut::game::StudioMusicStep(1);
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))
        renut::game::StudioMusicStep(-1);
      if (ImGui::IsKeyPressed(ImGuiKey_M, false))
        renut::game::StudioMusicPause(!paused);
    }
  }

  void DrawNewMod() {
    if (newMod_) {
      ImGui::OpenPopup("New mod");
      newMod_ = false;
      name_.clear();
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("New mod", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
      return;
    ImGui::TextUnformatted("A folder in the mods folder: its edits are saved as its own .nbpatch.");
    nut::gui::inputText("Name", name_);
    const std::string folder = nut::nbpatch::slug(name_);
    std::error_code ec;
    const bool taken = !folder.empty() && fs::exists(mods_ / folder, ec);
    if (taken)
      ImGui::TextDisabled("A mod named %s is already there: Open mod edits it", folder.c_str());
    ImGui::BeginDisabled(folder.empty() || taken);
    if (ImGui::Button("Make it")) {
      Open(mods_ / folder);
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  void Leave() { LeaveStudio(); }

  std::unique_ptr<nut::gui::App> app_;
  fs::path game_, mods_;
  bool newMod_ = false;
  std::string name_;
};

} // namespace

void OnGameThread(GameWork work) {
  std::lock_guard lock(g_mutex);
  g_work.push_back(std::move(work));
}

bool InStudio() { return g_in_studio.load(std::memory_order_relaxed) && !g_leaving.load(); }

uint64_t StudioVisit() { return g_visit.load(); }

void LeaveStudio() {
  g_leaving = true;
  GiveCameraBack();
  OnGameThread([](PPCContext &ctx, uint8_t *base) {
    renut::guest::Guest g(ctx, base);
    Store32(base, kFinishTasks, 1);
    QuitToTitle(g.ctx(), base);
  });
}

void WorldEnding(PPCContext &ctx, uint8_t *base, uint32_t scene) {
  if (!scene || scene != StudioScene())
    return;
  g_in_studio = false;  // the title has no level tick to say so
  SetOpened(false);
  renut::game::StudioMusicTick(ctx, base, false);
  GiveCameraBack();  // the model tabs stay open, but other worlds get the game's camera
  g_release = true;
  std::lock_guard lock(g_mutex);
  g_work.clear();
}

void StudioTick(PPCContext &ctx, uint8_t *base) {
  const uint32_t world = Load32(base, kRunningWorld);
  const bool studio = world && Load32(base, world) == StudioScene();
  if (studio && !g_in_studio.exchange(true)) {
    ++g_visit;
    g_leaving = false;
    std::lock_guard lock(g_mutex);
    g_work.clear();  // work for the last visit's objects
  }
  g_in_studio.store(studio, std::memory_order_relaxed);
  renut::game::StudioMusicTick(ctx, base, studio);
  if (!studio) {
    // A draw racing the studio's end may have taken the camera again after WorldEnding gave it back.
    if (g_release.exchange(false))
      GiveCameraBack();
    return;
  }
  // Banjo is made with every world; the studio keeps him out of sight.
  try {
    renut::game::part_api::HidePlayer(ctx, base);
  } catch (const std::exception &) {
  }
  std::deque<GameWork> work;
  {
    std::lock_guard lock(g_mutex);
    work.swap(g_work);
  }
  for (GameWork &w : work)
    w(ctx, base);  // the tick's caller keeps nothing in the volatile registers these calls use
}

void StartSession() {
  static Session *session = new Session();  // lives as long as reNut
  renut::gpu::overlay::AddDialog(session);
}

} // namespace renut::nuttool