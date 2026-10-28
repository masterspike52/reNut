// Achievements, leaderboards and what the Guide reports about the title.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include <rex/system/kernel_state.h>
#include <rex/ui/overlay/achievement_icon_cache.h>

#include "guide_dialog.h"
#include "live_sessions.h"
#include "live_stats.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

void GuideDialog::AchievementsRows(std::vector<Row>& rows, Page&) {
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) {
    rows.push_back({.label = "Achievements load with the game", .disabled = true});
    return;
  }
  auto& manager = kernel->achievements();
  auto list = manager.ListAchievements();
  std::stable_sort(list.begin(), list.end(), [&](const auto& a, const auto& b) {
    return manager.IsUnlocked(a.id) > manager.IsUnlocked(b.id);
  });
  for (const auto& a : list) {
    const bool unlocked = manager.IsUnlocked(a.id);
    rows.push_back({.label = a.label,
                    .sub = unlocked || a.unachieved_description.empty() ? a.description
                                                                       : a.unachieved_description,
                    .detail = std::to_string(a.gamerscore) + " G",
                    .icon = AchievementIcon(a),
                    .icon_dim = !unlocked});
  }
  if (rows.empty()) rows.push_back({.label = "This game has no achievements", .disabled = true});
}

void GuideDialog::LeaderboardsRows(std::vector<Row>& rows, Page&) {
  for (const auto& board : stats::Leaderboards()) {
    rows.push_back({.label = board.name, .activate = [this, board] {
                      Push({PageKind::kLeaderboard, board.name, std::to_string(board.id)});
                    }});
  }
  if (rows.empty()) rows.push_back({.label = "This game has no leaderboards", .disabled = true});
}

void GuideDialog::LeaderboardRows(std::vector<Row>& rows, Page& page) {
  const bool in = backend_->signed_in();
  const stats::Board board = stats::TopRows(uint32_t(std::strtoul(page.arg.c_str(), nullptr, 10)));
  if (board.loading) {
    rows.push_back({.label = "Loading...", .disabled = true});
    return;
  }
  if (board.failed) {
    rows.push_back({.label = "Couldn't reach RexLiveless", .sub = "Try again in a moment.", .disabled = true});
    return;
  }
  const uint64_t me = backend_->me().xuid;
  for (const stats::BoardRow& r : board.rows) {
    const bool mine = r.xuid == me;
    rows.push_back({.label = std::to_string(r.rank) + ".  " + r.gamertag,
                    .detail = FormatScore(r.rating),
                    .unread = mine,
                    .activate = [this, gt = r.gamertag, mine] { Push({PageKind::kGamercard, mine ? "Profile" : gt, gt}); }});
  }
  if (rows.empty()) rows.push_back({.label = "No scores yet", .sub = "Be the first on this leaderboard.", .disabled = true});
}

std::string GuideDialog::AchievementSummary() {
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) return "";
  auto& manager = kernel->achievements();
  int total = 0, unlocked = 0;
  for (const auto& a : manager.ListAchievements()) {
    ++total;
    unlocked += manager.IsUnlocked(a.id);
  }
  return total ? std::to_string(unlocked) + " of " + std::to_string(total) : "";
}

void GuideDialog::WatchAchievements() {
  if (achievement_listener_) {
    std::vector<rex::system::AchievementEvent> events;
    {
      std::lock_guard lock(unlock_mutex_);
      events.swap(unlocks_);
    }
    for (const auto& e : events) {
      Toast t;
      t.icon = AchievementIcon(e.achievement);
      if (!t.icon) t.glyph = Glyph::kTrophy;
      t.line1 = "Achievement unlocked";
      t.line2 = std::to_string(e.achievement.gamerscore) + "G - " + e.achievement.label;
      ShowToast(std::move(t));
    }
    return;
  }
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) return;
  achievement_listener_ = kernel->achievements().RegisterUnlockCallback(
      [this](const rex::system::AchievementEvent& e) {
        std::lock_guard lock(unlock_mutex_);
        unlocks_.push_back(e);
      });
}

void GuideDialog::ReportTitle() {
  if (time_ - last_report_ < 5.0f) return;
  last_report_ = time_;
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) return;
  char title_id[9];
  std::snprintf(title_id, sizeof(title_id), "%08X", kernel->title_id());
  // Every player is playing; achievements are player 1's (the runtime's
  // profile).
  for (auto& b : backends_) {
    if (b) b->SetTitle(title_id, config_.title_name);
  }
  Backend* player1 = backends_[0].get();
  if (!player1->signed_in() || !player1->connected()) return;
  sessions::RefreshFriendSessions();
  std::vector<AchievementRecord> unlocked;
  auto& manager = kernel->achievements();
  for (const auto& a : manager.ListAchievements()) {
    if (!manager.IsUnlocked(a.id)) continue;
    // FILETIME (100 ns since 1601) to unix seconds.
    const uint64_t ft = manager.GetUnlockTime(a.id);
    const int64_t unix_time = ft ? int64_t(ft / 10000000ULL) - 11644473600LL : int64_t(std::time(nullptr));
    unlocked.push_back({a.id, a.gamerscore, a.label, unix_time});
  }
  player1->ReportAchievements(title_id, unlocked);
}

ImTextureID GuideDialog::AchievementIcon(const rex::system::AchievementInfo& info) {
  if (!icon_cache_) {
    auto* kernel = REX_KERNEL_STATE();
    rex::Runtime* runtime = kernel ? kernel->emulator() : nullptr;
    if (!runtime || !config_.immediate_drawer) return 0;
    icon_cache_ = std::make_unique<rex::ui::AchievementIconCache>(config_.immediate_drawer, runtime);
  }
  return reinterpret_cast<ImTextureID>(icon_cache_->GetIcon(info));
}

}  // namespace rexglue_guide
