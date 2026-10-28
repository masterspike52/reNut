// The Guide: install, the dialog's frame, opening and closing, page stacks.

#include <rexglue_guide/guide.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/achievement_icon_cache.h>

#include "audio.h"
#include "guide_dialog.h"
#include "imgui.h"
#include "live.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

namespace {

// The XUID of the gamer card a title asked for (XShowGamerCardUI), until the
// Guide opens on it; 0 for none.
std::atomic<uint64_t> g_gamercard_request{0};

rex::ui::ImGuiDrawer* g_drawer = nullptr;
GuideDialog* g_dialog = nullptr;

}  // namespace

std::mutex g_credits_mutex;

std::vector<Credit>& AppCredits() {
  static std::vector<Credit> credits;
  return credits;
}

GuideDialog::GuideDialog(rex::ReXApp* app, rex::ui::ImGuiDrawer* drawer, Config config)
    : rex::ui::ImGuiDialog(drawer),
      app_(app),
      config_(std::move(config)),
      textures_(config_.immediate_drawer) {
  // REXGLUE_GUIDE_DATA lets a second copy on the same PC use another account.
  if (const char* dir = std::getenv("REXGLUE_GUIDE_DATA"); dir && *dir) config_.data_dir = dir;
  if (config_.data_dir.empty()) config_.data_dir = rex::filesystem::GetExecutableFolder() / "guide";
  if (config_.title_name.empty()) config_.title_name = "This Game";
  LoadPreferences();
  backend_ = &BackendFor(0);
  textures_.server_gamerpic = [this](int gamerpic) { return backend_->ServerGamerpic(gamerpic); };
  if (app_) input_.SetUiThread(&app_->app_context());
  rex::ui::RegisterBind("bind_guide", "F10", "Open the Guide", [this] { toggle_requested_ = true; });
}

GuideDialog::~GuideDialog() {
  for (int user = 0; user < live::kMaxUsers; ++user) live::Attach(user, nullptr);
  rex::ui::UnregisterBind("bind_guide");
  if (achievement_listener_) {
    if (auto* kernel = REX_KERNEL_STATE()) kernel->achievements().UnregisterCallback(achievement_listener_);
  }
}

void GuideDialog::OnDraw(ImGuiIO& io) {
  // A host that compiles its own ImGui into the executable (reNut, for its
  // overlay) runs the Guide's ImGui calls on that copy, whose current context
  // is the host's, not this drawer's: drawing there broke the host's frames.
  // Draw in the drawer's context, then hand the host's back.
  struct ContextScope {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext* ctx) { ImGui::SetCurrentContext(ctx); }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
  } context_scope(io.Ctx);

  const float dt = std::min(io.DeltaTime, 0.1f);
  time_ += dt;
  display_ = io.DisplaySize;
  SetScale(std::clamp(std::min(display_.y / 1080.0f, display_.x / 1920.0f), 0.5f, 4.0f));
  bool signin_changed = false;
  for (int user = 0; user < live::kMaxUsers; ++user) {
    Backend* b = backends_[user].get();
    if (!b) continue;
    b->Update(time_);
    if (b->signed_in() != was_signed_in_[user]) {
      was_signed_in_[user] = b->signed_in();
      signin_changed = true;
    }
  }
  live::Pump();
  if (signin_changed) live::NotifySigninChanged();
  WatchAchievements();
  ReportTitle();
  PumpExternalKeyboard();
  PumpPhotoPicks();
  PumpThemeImagePicks();
  FollowSelectedTheme();
  // A designer left some other way than Back (a page stack reset) has nothing
  // to show for.
  if (draft_ && !DesignerOpen()) draft_.reset();
  ApplyShownTheme();

  NavInput in = input_.Poll(dt, keyboard_.active(), screen_ == Screen::kClosed ? -1 : controller_);
  pad_ = in.pad_used;
  Startup();

  // The app's keyboard is up for the Guide: it has the screen and the input.
  if (external_keyboard_open_) {
    toggle_requested_ = false;
    DrawToasts(ImGui::GetForegroundDrawList(), dt);
    return;
  }

  if (toggle_requested_.exchange(false)) {
    // The keyboard's Guide key: player 1's.
    if (screen_ != Screen::kClosed && !closing_) Close();
    else if (screen_ == Screen::kClosed) OpenFor(0, -1);
  } else if (in.guide_from >= 0) {
    const int player = PlayerFor(in.guide_from);
    if (player < 0) {
      // A controller that's no player: nothing to open.
    } else if (screen_ == Screen::kClosed) {
      OpenFor(player, in.guide_from);
    } else if (!closing_ && player == user_) {
      Close();
    } else if (!closing_ && !busy_ && !keyboard_.active()) {
      OpenFor(player, in.guide_from);  // another player's Guide button: theirs now
    }
  }
  if (const uint64_t xuid = g_gamercard_request.exchange(0)) ShowTitleGamercard(xuid);

  if (screen_ != Screen::kClosed) {
    open_t_ = closing_ ? std::max(0.0f, open_t_ - dt / 0.14f) : std::min(1.0f, open_t_ + dt / 0.18f);
    if (closing_ && open_t_ <= 0.0f) {
      screen_ = Screen::kClosed;
      closing_ = false;
      drain_ = true;
      if (std::exchange(title_ui_, false)) {
        if (auto* kernel = REX_KERNEL_STATE()) kernel->BroadcastNotification(kNotifySystemUi, 0);
      }
    }
  }
  if (screen_ != Screen::kClosed) {
    // An invisible window over everything keeps the mouse away from the
    // windows underneath.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display_);
    ImGui::SetNextWindowFocus();
    ImGui::Begin("##rexglue_guide", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoNav);
    ImGui::End();
    if (!closing_) HandleInput(in, dt);
    Draw(ImGui::GetForegroundDrawList());
  } else if (drain_ && !in.pad_held && !ImGui::IsAnyMouseDown()) {
    drain_ = false;
  }
  DrawToasts(ImGui::GetForegroundDrawList(), dt);
  textures_.EndFrame();
}

void GuideDialog::Startup() {
  if (started_) return;
  if (time_ < 1.0f || display_.x <= 0) return;
  if (config_.ready_for_sign_in && !config_.ready_for_sign_in()) return;
  started_ = true;
  // Player 1, from any controller.
  user_ = 0;
  controller_ = -1;
  backend_ = &BackendFor(0);
  if (!backend_->remembered_gamertag().empty()) {
    backend_->SignInRemembered([this](const Result& r) {
      if (r.ok) SignedInToast();
      else OpenSignIn();
    });
  } else {
    OpenSignIn();
  }
}

Backend& GuideDialog::BackendFor(int user) {
  if (!backends_[user]) {
    const std::filesystem::path dir =
        user == 0 ? config_.data_dir : config_.data_dir / ("player" + std::to_string(user + 1));
    const char* mock = std::getenv("REXLIVELESS_MOCK");
    backends_[user] = mock && *mock == '1' ? CreateMockBackend(dir) : CreateServerBackend(dir);
    backends_[user]->on_notification = [this](const Notification& n) { OnNotification(n); };
    live::Attach(user, backends_[user].get());
  }
  return *backends_[user];
}

int GuideDialog::PlayerFor(int controller) const {
  if (controller == kGuideFromSdl) return 0;
  if (controller == kGuideFromKeyboard) return std::max(0, Input::KeyboardPlayer());
  const int player = config_.player_for_controller ? config_.player_for_controller(controller) : controller;
  return player >= 0 && player < live::kMaxUsers ? player : -1;
}

void GuideDialog::OpenFor(int user, int controller) {
  user_ = user;
  controller_ = controller;
  backend_ = &BackendFor(user);
  message_.reset();
  if (backend_->signed_in()) {
    OpenGuide();
  } else {
    OpenSignIn();
  }
}

std::string GuideDialog::PlayerLabel() const {
  return user_ == 0 ? std::string() : "Player " + std::to_string(user_ + 1);
}

void GuideDialog::OpenGuide() {
  screen_ = Screen::kGuide;
  closing_ = false;
  open_t_ = 0.0f;
  if (draft_) {
    // Closed mid-design: back where they were.
    blade_ = kSettingsBlade;
  } else {
    blade_ = kHomeBlade;
    for (int b = 0; b < kBladeCount; ++b) stacks_[b] = {RootPage(b)};
  }
  slide_ = 0.0f;
  audio::Play(Sound::kOpen);
}

void GuideDialog::OpenSignIn() {
  screen_ = Screen::kSignIn;
  closing_ = false;
  open_t_ = 0.0f;
  signin_stack_ = {Page{PageKind::kSignInHome, user_ == 0 ? "Sign In" : "Sign In - " + PlayerLabel()}};
  audio::Play(Sound::kOpen);
}

void GuideDialog::Close() {
  if (screen_ == Screen::kClosed) return;
  closing_ = true;
  audio::Play(Sound::kClose);
}

Page GuideDialog::RootPage(int blade) {
  switch (blade) {
    case kSocialBlade: return {PageKind::kSocial, "Social"};
    case kGamesBlade: return {PageKind::kGames, "Games & Apps"};
    case kSettingsBlade: return {PageKind::kSettings, "Settings"};
    default: return {PageKind::kHome, ""};
  }
}

void GuideDialog::Push(Page page) {
  Stack().push_back(std::move(page));
  slide_ = 1.0f;
}

void GuideDialog::Pop() {
  if (Current().kind == PageKind::kThemeDesigner && !LeaveDesigner([this] { Pop(); })) return;
  if (Stack().size() > 1) {
    Stack().pop_back();
    slide_ = -1.0f;
    audio::Play(Sound::kBack);
  } else if (screen_ == Screen::kGuide) {
    Close();
  } else {
    Close();  // leaving the sign-in screen plays offline
  }
}

void GuideDialog::SwitchBlade(int delta) {
  const int next = std::clamp(blade_ + delta, 0, int(kBladeCount) - 1);
  if (next == blade_) return;
  blade_ = next;
  // The settings blade keeps a theme being designed.
  if (!(draft_ && blade_ == kSettingsBlade)) stacks_[blade_] = {RootPage(blade_)};
  slide_ = float(delta);
  audio::Play(delta < 0 ? Sound::kPageLeft : Sound::kPageRight);
}

void ConfigureFonts(ImFontAtlas* atlas) {
  ui::ConfigureFonts(atlas);
}

void AddCredit(std::string who, std::string what) {
  std::lock_guard lock(g_credits_mutex);
  AppCredits().push_back({std::move(who), std::move(what)});
}

void Install(rex::ReXApp* app, rex::ui::ImGuiDrawer* drawer, Config config) {
  if (g_dialog || !drawer) return;
  // The Guide answers every Live and network import itself. Keep the SDK's own
  // web Live client (which starts lazily) from running alongside it; a no-op
  // on SDKs without that setting.
  rex::cvar::SetFlagByName("xlive_web_enabled", "false");
  g_drawer = drawer;
  g_dialog = new GuideDialog(app, drawer, std::move(config));
  drawer->AddDialog(g_dialog);
}

void Shutdown() {
  if (g_dialog) {
    g_drawer->RemoveDialog(g_dialog);
    delete g_dialog;
    g_dialog = nullptr;
  }
  audio::Shutdown();
}

bool IsOpen() {
  return g_dialog && g_dialog->input_held();
}

bool SignedIn(int user) {
  return live::Snapshot(user).signed_in;
}

void live::RequestGamercard(uint64_t xuid) {
  g_gamercard_request = xuid;
}

}  // namespace rexglue_guide
