#pragma once
// The Guide's dialog. Its parts are defined across the guide*.cpp files, one
// per area (each section below names its file), so changes to different parts
// of the Guide land in different files.

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rex/system/achievement_manager.h>
#include <rex/ui/imgui_dialog.h>
#include <rexglue_guide/guide.h>

#include "backend.h"
#include "guide_common.h"
#include "imgui.h"
#include "input.h"
#include "keyboard.h"
#include "live.h"
#include "theme_background.h"
#include "themes.h"
#include "ui.h"

namespace rex {
class ReXApp;
}
namespace rex::ui {
class AchievementIconCache;
class ImGuiDrawer;
}  // namespace rex::ui

namespace rexglue_guide {

// App menus (AddMenu), in the order added. Any thread.
std::vector<std::shared_ptr<const MenuDef>> AppMenus();
std::shared_ptr<const MenuDef> FindAppMenu(std::string_view id);

class GuideDialog final : public rex::ui::ImGuiDialog {
 public:
  GuideDialog(rex::ReXApp* app, rex::ui::ImGuiDrawer* drawer, Config config);
  ~GuideDialog() override;
  void OnDraw(ImGuiIO& io) override;
  // The Guide, its sign-in screen or the on-screen keyboard is up (or the
  // buttons that closed it are still held).
  bool input_held() const {
    return screen_ != Screen::kClosed || drain_ || keyboard_.active() || window_holds_input_;
  }

 private:
  // guide.cpp -----------------------------------------------------------------
  void Startup();
  // A player's account, made the first time it's needed. Player 1's lives in
  // the data folder itself (as before there were players); 2-4 in player2..4
  // below it, so each remembers its own.
  Backend& BackendFor(int user);
  // The player a controller is (as the title reads it), or -1.
  int PlayerFor(int controller) const;
  // The Guide for a player, driven by their controller: the sign-in screen
  // first when they aren't signed in.
  void OpenFor(int user, int controller);
  // "Player 2" etc. on the sign-in screen when it isn't player 1's.
  std::string PlayerLabel() const;
  void OpenGuide();
  void OpenSignIn();
  void Close();
  // The blades, left to right: the app's left ones, the Guide's four, then
  // the app's right ones (the first added on each side next to the Guide's).
  void LayoutBlades();
  int BladeOf(int builtin) const;  // a Blade's place among them
  Page RootPage(int blade) const;
  void ResetStacks();  // every blade back to its first page
  void Push(Page page);
  void Pop();
  void SwitchBlade(int delta);

  // guide_pages.cpp -----------------------------------------------------------
  std::vector<Row> BuildRows(Page& page);
  // Disabled with "SIGN IN" while signed out.
  Row NeedSignIn(Row row) const;
  void HomeRows(std::vector<Row>& rows, Page& page);
  void SocialRows(std::vector<Row>& rows, Page& page);
  void GamesRows(std::vector<Row>& rows, Page& page);
  void SettingsRows(std::vector<Row>& rows, Page& page);
  void SystemRows(std::vector<Row>& rows, Page& page);
  void CreditsRows(std::vector<Row>& rows, Page& page);
  void AccountRows(std::vector<Row>& rows, Page& page);

  // guide_friends.cpp ---------------------------------------------------------
  void FriendsRows(std::vector<Row>& rows, Page& page);
  void RequestsRows(std::vector<Row>& rows, Page& page);
  void NotificationsRows(std::vector<Row>& rows, Page& page);
  void GamercardRows(std::vector<Row>& rows, Page& page);
  const Friend* FindFriend(const std::string& gamertag) const;
  // The gamertag of a player by XUID: the signed-in account, a friend, or a
  // player hosting a session of this title. Empty when not known.
  std::string GamertagOf(uint64_t xuid) const;
  // XShowGamerCardUI: the Guide opens on that player's gamer card (Back
  // closes it), and the title hears system UI is up until it closes, as it
  // would on a console.
  void ShowTitleGamercard(uint64_t xuid);
  void AddFriend();
  void AnswerRequest(const std::string& gamertag);
  void OpenNotification(const Notification& n);

  // guide_messages.cpp --------------------------------------------------------
  void MessagesRows(std::vector<Row>& rows, Page& page);
  void ConversationRows(std::vector<Row>& rows, Page& page);
  void NewMessageRows(std::vector<Row>& rows, Page& page);
  int UnreadMessages() const;
  void Compose(const std::string& gamertag);
  void OpenConversation(const std::string& gamertag);

  // guide_games.cpp -----------------------------------------------------------
  void AchievementsRows(std::vector<Row>& rows, Page& page);
  void LeaderboardsRows(std::vector<Row>& rows, Page& page);
  void LeaderboardRows(std::vector<Row>& rows, Page& page);
  std::string AchievementSummary();
  // Achievement unlocks arrive on the game's threads.
  void WatchAchievements();
  // Presence and unlocked achievements for the server, every few seconds.
  void ReportTitle();
  ImTextureID AchievementIcon(const rex::system::AchievementInfo& info);
  void UploadAchievementIcons(Backend& backend, const std::string& title_id);
  // Another player's achievement's icon from the server (*loading while it downloads).
  ImTextureID RemoteAchievementIcon(const std::string& title_id, uint32_t id, int64_t version, bool* loading);
  // The running game's leaderboards to list: its SPA's names with the
  // server's (admins') over them.
  std::vector<std::pair<uint32_t, std::string>> ListedLeaderboards();
  // A profile's games, and one game of theirs (arg: gamertag '|' title id).
  void PlayerGamesRows(std::vector<Row>& rows, Page& page);
  void PlayerGameRows(std::vector<Row>& rows, Page& page);
  std::string PlayerGameSummary(const Page& page);  // its achievements and gamerscore
  // A game's icon an admin put on the server, or 0 (none, or downloading).
  // `version`: the icon's, as a listing gave it; -1: the server's game list.
  ImTextureID GameIcon(const std::string& title_id, int64_t version = -1, bool* loading = nullptr);
  std::string CurrentTitleId() const;

  // guide_preferences.cpp -----------------------------------------------------
  void PreferencesRows(std::vector<Row>& rows, Page& page);
  // <data dir>/preferences.json: sounds, pop-ups and theme.
  void LoadPreferences();
  void SavePreferences();

  // guide_account.cpp ---------------------------------------------------------
  void SignInHomeRows(std::vector<Row>& rows, Page& page);
  void StartCreate();
  void StartSignIn();
  void AskGamertag(const std::string& title, std::function<void(std::string)> next);
  void ChooseGamerpic(int index);
  void ChangeGamerpicRows(std::vector<Row>& rows, Page& page);
  void WithProfile(std::function<void()> next);
  void BackToProfile();
  void FinishSignUp();

  // guide_game_gamerpics.cpp --------------------------------------------------
  // The gamerpic archive's games (search, then the list) and one game's grid.
  void GamerpicGamesRows(std::vector<Row>& rows, Page& page);
  // A game's gamerpic ids (null while they load; *error if they couldn't).
  const std::vector<std::string>* GameGamerpicIds(const std::string& title_id, std::string* error);
  // A game gamerpic's picture, or 0 (*loading: it's on its way, rather than
  // missing); `tile`: with the 360 gamer-tile frame (not on a game's own tile,
  // id 8000).
  ImTextureID GameGamerpicTexture(const std::string& title_id, const std::string& id, bool tile = true,
                                  bool* loading = nullptr);
  // The gamerpic grids (kGamerpics, kGamerpicGame): how many, each picture,
  // and picking one.
  int GridCount(const Page& page);
  ImTextureID GridPicture(const Page& page, int index, bool* loading);
  void ChooseGridPicture(const Page& page, int index);

  void SignInDone(const Result& r);
  void SignedInToast();
  void EditMotto();
  void ConfirmSignOut();
  void ChoosePhoto();
  void PumpPhotoPicks();

  // guide_input.cpp -----------------------------------------------------------
  void HandleInput(const NavInput& in, float dt);
  // A list's selection, sliders, steppers, scroll bar and mouse; `mouse`: the
  // mouse is the list's (over it, for a window).
  ListInput HandleList(const NavInput& in, ListView& view, Page& page, bool mouse);
  void Activate(const Row& row);
  // Text entry through the app's keyboard (Config::show_keyboard) when it has
  // one, else the Guide's. The app's answer can come from any thread, so it is
  // queued for the next frame.
  void OpenKeyboard(KeyboardRequest request);
  // A title's text entry waiting (ShowTitleKeyboard): opened once the keyboard
  // is free.
  void PumpTitleKeyboards();
  // An invisible window over everything keeps the mouse from the windows under it.
  void CaptureMouse();
  void PumpExternalKeyboard();

  // guide_themes.cpp ----------------------------------------------------------
  // The gallery (PageKind::kThemes): a grid of every theme.
  std::vector<themes::Theme> GalleryThemes() const;
  void DrawThemeGallery(ImDrawList* dl, ImVec2 pos, ImVec2 end, Page& page, float a);
  void DrawThemeTile(ImDrawList* dl, const themes::Theme& theme, ImVec2 p0, ImVec2 p1, bool selected, float a);
  void HandleGalleryInput(const NavInput& in, Page& page);
  void LikeTheme(const themes::Theme& theme);
  void ThemeDesignerRows(std::vector<Row>& rows, Page& page);
  void ColorEditorRows(std::vector<Row>& rows, Page& page);
  void MyThemesRows(std::vector<Row>& rows, Page& page);
  static themes::Theme FromServer(const ServerTheme& server);
  void SelectTheme(const themes::Theme& theme);
  // The selected server theme changed or was removed there.
  void FollowSelectedTheme();
  // What the Guide shows: the theme being designed, else the selected one.
  const themes::Theme& ShownTheme() const;
  bool ShownDark() const;
  void ApplyShownTheme();
  // The designer, on a theme of the player's (0: a new one).
  void OpenDesigner(int server_id);
  bool DesignerOpen() const;
  // Leaving the designer: true now, or false after asking to save first
  // (then `leave` runs once it's been answered).
  bool LeaveDesigner(std::function<void()> leave);
  void SaveDraft(std::function<void()> then);
  void ChooseThemeImage();
  void PumpThemeImagePicks();

  // guide_config.cpp ----------------------------------------------------------
  void ConfigRows(std::vector<Row>& rows, Page& page);
  // Sets a setting and saves; `notify`: say if it's refused or needs a restart.
  void ChangeSetting(const std::string& name, const std::string& value, bool notify = true);
  void SaveSettings();
  void FlushSettings(bool now = false);

  // guide_draw.cpp ------------------------------------------------------------
  void Draw(ImDrawList* dl);
  // The theme's background picture over the open blade [p0, p1], under its
  // strip and rows.
  void DrawThemeBackground(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float round, float a);
  // The app's live frame (Config::game_frame), Gaussian-blurred over the
  // screen: copies of it on a regular grid of offsets, each weighted by the
  // Gaussian at its offset, the whole faded in by 'a'. Copies blend in turn,
  // so copy k is drawn at w_k / (1 - the weights of the copies after it),
  // which leaves each with exactly its weight. False when there's no frame.
  bool DrawBlurredGame(ImDrawList* dl, float a);
  // The open blade's shadow. Shadow() fills under the blade as well, which a
  // see-through blade would show as a dark middle: then it's rounded rings
  // from the blade's edge outwards, following its rounded corners, fading out
  // (and, like Shadow(), reaching less far above it than below).
  void BladeShadow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float round, float spread, float a);
  // The blade stack: side blades fold out to the left and right of the open one.
  void DrawGuide(ImDrawList* dl, float a);
  std::string HomeLabel() const;
  std::string BladeLabel(int blade) const;
  void DrawBladeIcon(ImDrawList* dl, int blade, ImVec2 c, float a);
  // The ring at any size, with a player's quarter lit (-1: none).
  void DrawRing(ImDrawList* dl, ImVec2 c, float a, int lit_player, float radius, float width);
  void DrawHeader(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float a);
  // Button hints under the blade. Keyboard and mouse users get key names.
  void DrawLegend(ImDrawList* dl, ImVec2 pos, float a);
  void DrawDisclaimer(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float a);
  void DrawServerWarning(ImDrawList* dl);
  // Sign-in: one blade on its own, like the 360's profile picker.
  void DrawSignIn(ImDrawList* dl, float a);

  // guide_menus.cpp -----------------------------------------------------------
  // The rows of an app menu's page (PageKind::kMenu), from its build function.
  // push and pop go to the stack it's on (the blade's, or its window's).
  void MenuRows(std::vector<Row>& rows, Page& page, std::function<void(Page)> push, std::function<void()> pop,
                bool detached);
  // Requests from ShowMenu, and which menus are shown, for MenuShown.
  void PumpMenus();
  // Detached menus' windows, over the game while the Guide is closed. The
  // topmost holding input gets `in`; the mouse goes to the one under it.
  void DrawMenuWindows(ImDrawList* dl, const NavInput& in, float dt);
  // A menu's icon, or 0 (the title's first letter stands in).
  ImTextureID MenuIcon(const std::string& id);
  // The menu's mark in a strip, centred on c.
  void DrawMenuMark(ImDrawList* dl, const std::string& id, ImVec2 c, float a);

  // guide_draw_list.cpp -------------------------------------------------------
  // Rows of the current page inside [l0, l1].
  void DrawList(ImDrawList* dl, ImVec2 l0, ImVec2 l1, float a);
  // Rows of `stack`'s top page into `view`; slide: its page sliding in (-1..1).
  void DrawList(ImDrawList* dl, ListView& view, std::vector<Page>& stack, ImVec2 l0, ImVec2 l1, float a, float slide);
  float RowHeight(const Row& row, float width) const;
  // *slider gets a slider row's bar.
  void DrawRow(ImDrawList* dl, const Row& row, ImVec2 r0, ImVec2 r1, bool selected, float a,
               std::pair<ImVec2, ImVec2>* slider);
  // The colour being edited, large, over its sliders.
  float DrawColorPreview(ImDrawList* dl, ImVec2 pos, float right, int element, float a);
  float DrawGamercard(ImDrawList* dl, ImVec2 pos, float right, const std::string& gamertag, float a);
  // The gamerpic choices in rows of five, scrolled to keep the selection in view.
  void DrawGamerpicGrid(ImDrawList* dl, ImVec2 pos, float bottom, Page& page, float a);

  // guide_popups.cpp ----------------------------------------------------------
  void ShowMessage(Prompt box);
  void DrawMessage(ImDrawList* dl);
  void DrawBusy(ImDrawList* dl);
  void DrawToasts(ImDrawList* dl, float dt);
  void ShowToast(Toast toast);
  void OnNotification(const Notification& n);

  // inline -------------------------------------------------------------------
  std::vector<Page>& Stack() { return screen_ == Screen::kSignIn ? signin_stack_ : stacks_[size_t(blade_)]; }
  Page& Current() { return Stack().back(); }
  void Busy(std::string text) { busy_ = std::move(text); }
  // With the blurred game behind it, the open blade lets it show through, as on
  // the console (its strip a little less, so the icon and label stay clear).
  // Without one it stays solid: there'd be only the unblurred game to see.
  float BladeSeeThrough() const { return blurred_ ? 0.7f : 1.0f; }
  float StripSeeThrough() const { return blurred_ ? 0.8f : 1.0f; }
  // The Ring of Light: four rounded quarters with gaps between them, the
  // shown player's green when lit (clockwise from the top left: players 1,
  // 2, 4, 3, as on the console). ImGui strokes have square ends, so each end
  // gets a dot; the gap allows for the dots.
  void DrawRing(ImDrawList* dl, ImVec2 c, float a, bool lit) { DrawRing(dl, c, a, lit ? user_ : -1, ui::Px(17), ui::Px(4.5f)); }

  // state --------------------------------------------------------------------
  rex::ReXApp* app_;
  Config config_;
  ui::Textures textures_;
  // Each player's account (players 2-4 made when their controller first
  // opens the Guide); backend_ is the one the Guide is showing.
  std::unique_ptr<Backend> backends_[live::kMaxUsers];
  Backend* backend_ = nullptr;
  int user_ = 0;         // the player the Guide is showing
  int controller_ = -1;  // the controller driving it (NavInput::guide_from), -1: any
  std::unique_ptr<rex::ui::AchievementIconCache> icon_cache_;
  Input input_;
  Keyboard keyboard_;
  bool blurred_ = false;  // this frame has the live game blurred behind the Guide

  Screen screen_ = Screen::kClosed;
  bool closing_ = false;
  bool drain_ = false;
  bool started_ = false;
  bool pad_ = false;
  bool sounds_ = true;
  bool popups_ = true;
  bool dark_mode_ = false;
  themes::Theme theme_ = themes::Default();
  // The theme in the designer: what it was opened on, as changed so far.
  struct ThemeDraft {
    themes::Theme theme;
    bool dark = false;  // editing (and showing) its Dark colours
    enum class Image { kKept, kNew, kRemoved } image = Image::kKept;
    std::string image_file;  // kNew: the JPEG
    int image_serial = 0;    // a new one each time it's picked
    bool dirty = false;
  };
  std::optional<ThemeDraft> draft_;
  ThemeBackground background_;
  ThemeThumbnails thumbnails_;
  enum class ThemeSort { kMostLiked, kMostUsed, kNewest, kName, kCount };
  ThemeSort theme_sort_ = ThemeSort::kMostLiked;
  bool show_nsfw_ = false;
  std::vector<themes::Theme> gallery_;  // as last drawn
  std::vector<std::pair<ImVec2, ImVec2>> gallery_cells_;
  std::pair<ImVec2, ImVec2> sort_hit_{};
  struct KeyboardResults {
    std::mutex mutex;
    std::vector<std::pair<std::function<void(std::optional<std::string>)>, std::optional<std::string>>> done;
  };
  std::shared_ptr<KeyboardResults> external_results_ = std::make_shared<KeyboardResults>();
  bool external_keyboard_open_ = false;
  float open_t_ = 0.0f;
  float slide_ = 0.0f;
  float time_ = 0.0f;
  float last_report_ = -100.0f;
  std::string achievement_icons_checked_;  // the title UploadAchievementIcons has looked at
  // Profiles' games from the server, by gamertag and by "gamertag|title id";
  // read again when a page shows them after kProfileRefresh.
  template <typename T>
  struct Fetched {
    bool loading = false;
    float at = -1000.0f;  // time_ of the last answer
    std::string error;
    T value{};
  };
  std::map<std::string, Fetched<std::vector<PlayedGame>>> player_games_;
  std::map<std::string, Fetched<PlayedGameDetail>> player_game_;
  std::map<std::string, ImTextureID> game_icons_;  // "<title id>_<version>"; 0: unreadable
  std::map<std::string, Fetched<std::vector<std::string>>> game_gamerpic_ids_;  // by title id
  // Game gamerpic textures by "xgp:<title id>/<id>": the most recently drawn
  // few hundred (a host has room for only so many textures; ~2000 samplers in
  // D3D12), the rest made again when they're back on screen.
  struct GamerpicTexture {
    ImTextureID texture = 0;  // 0: it couldn't be made (tried again in a while)
    float made = 0.0f, used = 0.0f;
  };
  std::map<std::string, GamerpicTexture> game_gamerpic_textures_;
  bool was_signed_in_[live::kMaxUsers] = {};
  ImVec2 display_{};
  std::atomic<bool> toggle_requested_{false};

  // The blades (LayoutBlades): one of the Guide's (a Blade), or an app menu's.
  struct BladeSlot {
    int builtin = -1;
    std::string menu;
  };
  std::vector<BladeSlot> blades_;
  int blade_ = 0;
  std::vector<std::vector<Page>> stacks_;  // per blade
  std::vector<Page> signin_stack_;
  ListView list_;  // the open page's rows
  // App menus: what their rows did, for their build functions to hear next
  // ("<page arg>\x1d<row id>"), and their choice lists' searches (by page arg).
  std::map<std::string, MenuEvent> menu_events_;
  std::map<std::string, std::string> menu_search_;
  // A detached menu's open window.
  struct MenuWindowState {
    std::string id;
    std::vector<Page> stack;
    ListView list;
    float open_t = 0.0f;
    bool closing = false;
    float slide = 0.0f;
    ImVec2 moved{};  // dragged by its title, from where it was placed
    bool dragging = false;
    ImVec2 p0{}, p1{};  // as last drawn
  };
  std::vector<MenuWindowState> windows_;  // the last on top
  std::atomic<bool> window_holds_input_{false};
  // Pictures that aren't here yet are downloaded only while the page being
  // drawn is still (DrawPage); last_wheel_: time_ of the last wheel notch.
  bool allow_downloads_ = true;
  float last_wheel_ = -100.0f;
  bool settings_dirty_ = false;  // Config changes not saved yet
  float settings_changed_at_ = 0.0f;
  std::vector<std::pair<ImVec2, ImVec2>> grid_cells_;
  std::vector<std::pair<ImVec2, ImVec2>> message_buttons_;
  std::vector<std::pair<int, std::pair<ImVec2, ImVec2>>> blade_hits_;  // by place in blades_
  std::optional<Prompt> message_;
  std::optional<std::string> busy_;
  std::deque<Toast> toasts_;

  std::string pending_gamertag_;
  std::string pending_password_;

  rex::system::AchievementListenerHandle achievement_listener_ = 0;
  std::mutex unlock_mutex_;
  std::vector<rex::system::AchievementEvent> unlocks_;

  // The Guide is up for the title (a gamer card it asked for): XN_SYS_UI was
  // sent, and its end goes when the Guide closes.
  bool title_ui_ = false;
};

}  // namespace rexglue_guide
