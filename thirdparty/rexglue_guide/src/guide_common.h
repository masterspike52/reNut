#pragma once
// What the Guide's parts share: its pages and rows, prompts and toasts, and
// small helpers.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "imgui.h"
#include "ui.h"

namespace rexglue_guide {

enum class Screen { kClosed, kGuide, kSignIn };

enum class PageKind {
  kHome,
  kSocial,
  kGames,
  kSettings,
  kFriends,
  kRequests,
  kNotifications,
  kAchievements,
  kLeaderboards,
  kLeaderboard,  // arg: the view id
  kGamercard,
  kGamerpics,
  kPreferences,
  kSystem,
  kAccount,
  kCredits,
  kThemes,
  kMessages,
  kConversation,
  kNewMessage,
  kSignInHome,
  kThemeDesigner,
  kColorEditor,  // arg: the themes::Element
  kMyThemes,     // an admin's themes
  kConfig,       // arg: the settings category ("" for the top)
  kPlayerGames,  // arg: the gamertag
  kPlayerGame,   // arg: gamertag '|' title id
  kChangeGamerpic,  // RexLiveless gamerpics, the Gamerpic Database or an upload
  kGamerpicGames,  // the Gamerpic Database's games; arg: the search text
  kGamerpicGame,   // a game's gamerpics, drawn as a grid; arg: its title id
  // An app's menu (AddMenu), on its blade or in its window. arg: the menu's id,
  // then a kMenuPath and the id of each page in from its first; the last, after
  // a kMenuList, a choice whose options it lists.
  kMenu,
};

constexpr char kMenuPath = '\x1f';
constexpr char kMenuList = '\x1e';

// Settings > Credits: the app's (AddCredit), then the Guide's own.
struct Credit {
  std::string who;
  std::string what;
};
extern std::mutex g_credits_mutex;
std::vector<Credit>& AppCredits();  // under g_credits_mutex

struct Page {
  PageKind kind;
  std::string title;
  std::string arg;
  int selected = 0;
  float scroll = 0.0f;
  // The mouse has the page (its wheel, or hovering picked the selection): the
  // view goes to scroll_goal rather than following the selection, so it
  // doesn't move what's under the pointer. Pad and keys hand it back.
  bool free_scroll = false;
  float scroll_goal = 0.0f;
};

inline void HoldScroll(Page& page) {
  if (page.free_scroll) return;
  page.free_scroll = true;
  page.scroll_goal = page.scroll;
}

// Eases page.scroll toward `follow` (with the selection in view), or toward
// its goal while the mouse has it, within [0, max]. Returns where it's going.
inline float ScrollTowards(Page& page, float follow, float max) {
  const float target = std::clamp(page.free_scroll ? page.scroll_goal : follow, 0.0f, std::max(0.0f, max));
  if (page.free_scroll) page.scroll_goal = target;
  page.scroll += (target - page.scroll) * std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f);
  return target;
}

// For a grid scrolled by the mouse: the selection, moved onto the rows from
// `top` (a row, maybe fractional) that are on screen, at least half of each, in
// a view `fit` rows tall. Counting only whole rows, a grid that just fails to
// fit scrolled by a sliver (a touchpad's scroll), and its first row counted
// as gone, pushing the selection a row down.
inline int KeepInGrid(int selected, int count, int columns, float top, float fit) {
  if (count <= 0) return 0;
  const int first = int(std::ceil(top - 0.5f));
  const int last = std::max(first, int(std::floor(top + fit - 0.5f)));
  const int row = std::clamp(selected / columns, first, last);
  return std::clamp(row * columns + selected % columns, 0, count - 1);
}

struct Row {
  std::optional<ImU32> swatch;  // a colour square on the left (themes)
  std::string label;
  std::string sub;     // second line; makes the row tall
  std::string detail;  // small caps on the right
  int gamerpic = -1;   // avatar on the left
  ImTextureID icon = 0;
  // The icon, asked for only while the row is on screen (one that downloads,
  // in a long list); its place is kept while it's 0, with a spinner while
  // *loading.
  std::function<ImTextureID(bool* loading)> lazy_icon;
  bool icon_dim = false;
  std::optional<ui::Glyph> glyph;  // drawn on the right
  std::vector<int> avatars;        // small avatars on the right
  bool unread = false;
  bool disabled = false;
  std::function<void()> activate;
  // A slider (0..1) on the right: left/right and the mouse move it.
  std::optional<float> slider;
  std::function<void(float)> set_slider;
  float slider_step = 0.05f;
  // A switch on the right, on or off.
  std::optional<bool> toggle;
  // Left and right step it (-1, 1): a choice or a number, its value the detail.
  std::function<void(int)> step;
  // Rows that aren't picked: a heading, text wrapped over as many lines as it
  // takes (in text_color, else dim), or a picture across the row (width over
  // height; 0 while there's none yet), no taller than picture_height.
  bool header = false;
  std::string text;
  std::optional<ImU32> text_color;
  ImTextureID picture = 0;
  float picture_aspect = 0.0f;
  float picture_height = 0.0f;
  bool Selectable() const { return !header && text.empty() && picture_aspect <= 0.0f; }
};

// A list of rows as last drawn, and what the mouse is doing with it: the
// Guide's open page, or a detached menu's.
struct ListView {
  std::vector<Row> rows;
  std::vector<std::pair<ImVec2, ImVec2>> row_hits;
  std::vector<std::pair<ImVec2, ImVec2>> slider_hits;  // per row; empty unless a slider
  int dragging_slider = -1;
  // The scroll bar as last drawn, and its knob being dragged (dragging_scroll,
  // held drag_grab below the knob's top).
  struct ScrollBar {
    ImVec2 hit0, hit1;  // the mouse's area for it
    float top = 0.0f, bottom = 0.0f;
    float knob_y = 0.0f, knob_h = 0.0f;
    float max = 0.0f;  // the page's scroll at the bottom
    bool shown = false;
  } scroll_bar;
  bool dragging_scroll = false;
  float drag_grab = 0.0f;
};

// What an app menu's row did, for its build function to hear next frame.
struct MenuEvent {
  bool chosen = false;
  int step = 0;  // left and right, added up
  std::optional<int> index;
  std::optional<double> number;
  std::optional<std::string> text;
};
constexpr char kMenuRow = '\x1d';  // between a page's arg and a row's id

// What a list's input asks of the one showing it.
struct ListInput {
  int sideways = 0;  // left or right, unused by the selected row
  bool back = false;
};

struct Prompt {
  std::string title;
  std::string body;
  std::vector<std::string> buttons;
  int selected = 0;
  std::function<void(int)> done;  // button index, -1 = cancelled
};

struct Toast {
  int gamerpic = -1;
  ImTextureID icon = 0;
  std::optional<ui::Glyph> glyph;
  std::string line1, line2;
  int ring_player = -1;  // a sign-in: the Ring of Light around the picture, this player lit
  float t = 0.0f;
};

enum Blade { kSocialBlade, kGamesBlade, kHomeBlade, kSettingsBlade, kBladeCount };

constexpr float kToastTime = 4.6f;
constexpr float kPi = 3.14159265f;

// XN_SYS_UI: system UI is showing (1) or gone (0).
constexpr uint32_t kNotifySystemUi = 0x00000009;

inline float Ease(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

// A leaderboard rating with thousands separators.
inline std::string FormatScore(int64_t score) {
  std::string digits = std::to_string(score < 0 ? -score : score);
  for (int i = int(digits.size()) - 3; i > 0; i -= 3) digits.insert(size_t(i), ",");
  return score < 0 ? "-" + digits : digits;
}

inline std::string TimeAgo(int64_t then) {
  const int64_t s = std::max<int64_t>(0, int64_t(std::time(nullptr)) - then);
  if (s < 60) return "just now";
  if (s < 3600) return std::to_string(s / 60) + (s < 120 ? " minute ago" : " minutes ago");
  if (s < 86400) return std::to_string(s / 3600) + (s < 7200 ? " hour ago" : " hours ago");
  return std::to_string(s / 86400) + (s < 172800 ? " day ago" : " days ago");
}

inline std::string Clock() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char buf[16];
  std::strftime(buf, sizeof(buf), "%I:%M %p", &local);
  return buf[0] == '0' ? std::string(buf + 1) : std::string(buf);
}

}  // namespace rexglue_guide
