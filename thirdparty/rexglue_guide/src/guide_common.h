#pragma once
// What the Guide's parts share: its pages and rows, prompts and toasts, and
// small helpers.

#include <algorithm>
#include <atomic>
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
};

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
};

struct Row {
  std::optional<ImU32> swatch;  // a colour square on the left (themes)
  std::string label;
  std::string sub;     // second line; makes the row tall
  std::string detail;  // small caps on the right
  int gamerpic = -1;   // avatar on the left
  ImTextureID icon = 0;
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
