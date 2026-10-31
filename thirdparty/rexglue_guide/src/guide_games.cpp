// Achievements, leaderboards and what the Guide reports about the title.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/ui/overlay/achievement_icon_cache.h>

#include "guide_dialog.h"
#include "image.h"
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

std::vector<std::pair<uint32_t, std::string>> GuideDialog::ListedLeaderboards() {
  const TitleInfo* t = backend_->title(CurrentTitleId());
  auto hidden = [t](uint32_t id) {
    return t && std::find(t->hidden_boards.begin(), t->hidden_boards.end(), id) != t->hidden_boards.end();
  };
  std::vector<std::pair<uint32_t, std::string>> list;
  for (const auto& board : stats::Leaderboards()) {
    if (hidden(board.id)) continue;
    const auto named = t ? t->boards.find(board.id) : decltype(t->boards.end()){};
    list.push_back({board.id, t && named != t->boards.end() ? named->second : board.name});
  }
  // Ones only an admin named (the game left them "...").
  if (t) {
    for (const auto& [id, name] : t->boards) {
      if (std::none_of(list.begin(), list.end(), [id](const auto& b) { return b.first == id; })) list.push_back({id, name});
    }
  }
  return list;
}

void GuideDialog::LeaderboardsRows(std::vector<Row>& rows, Page&) {
  for (const auto& [id, name] : ListedLeaderboards()) {
    rows.push_back({.label = name, .activate = [this, id, name] {
                      Push({PageKind::kLeaderboard, name, std::to_string(id)});
                    }});
  }
  if (rows.empty()) rows.push_back({.label = "This game has no leaderboards", .disabled = true});
}

std::string GuideDialog::CurrentTitleId() const {
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) return "";
  char title_id[9];
  std::snprintf(title_id, sizeof(title_id), "%08X", kernel->title_id());
  return title_id;
}

ImTextureID GuideDialog::GameIcon(const std::string& title_id, int64_t version, bool* loading) {
  if (loading) *loading = false;
  if (version < 0) {
    const TitleInfo* t = backend_->title(title_id);
    version = t ? t->icon : 0;
  }
  if (!version) {
    // No icon from an admin: the game's own tile from the Gamerpic Database,
    // which has its title ids in lowercase.
    std::string id = title_id;
    std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return GameGamerpicTexture(id, "8000", false, loading);
  }
  const std::string key = title_id + "_" + std::to_string(version);
  if (auto it = game_icons_.find(key); it != game_icons_.end()) return it->second;
  const std::string file = backend_->TitleIcon(title_id, version);
  if (file.empty()) {
    if (loading) *loading = true;
    return 0;  // downloading
  }
  int w = 0, h = 0;
  const std::vector<uint8_t> rgba = image::Decode(file.data(), file.size(), &w, &h);
  return game_icons_[key] = rgba.empty() ? ImTextureID(0) : textures_.Rgba("game_icon:" + key, rgba.data(), w, h);
}

namespace {

// How long a profile's games stay as read before a page showing them reads
// them again.
constexpr float kProfileRefresh = 30.0f;

std::string AchievementCount(uint32_t unlocked, uint32_t total) {
  return std::to_string(unlocked) + (total ? " of " + std::to_string(total) : "") +
         (total == 1 || (!total && unlocked == 1) ? " achievement" : " achievements");
}

}  // namespace

void GuideDialog::PlayerGamesRows(std::vector<Row>& rows, Page& page) {
  const std::string gamertag = page.arg;
  auto& f = player_games_[gamertag];
  if (!f.loading && time_ - f.at >= kProfileRefresh) {
    f.loading = true;
    backend_->PlayerGames(gamertag, [this, gamertag](const Result& r, std::vector<PlayedGame> games) {
      auto& g = player_games_[gamertag];
      g.loading = false;
      g.at = time_;
      g.error = r.ok ? "" : r.error;
      if (r.ok) g.value = std::move(games);
    });
  }
  if (f.at < -999.0f) {
    rows.push_back({.label = "Loading...", .disabled = true});
    return;
  }
  if (!f.error.empty() && f.value.empty()) {
    rows.push_back({.label = "Couldn't load the games", .sub = f.error, .disabled = true});
    return;
  }
  for (const PlayedGame& g : f.value) {
    rows.push_back({.label = g.name,
                    .sub = AchievementCount(g.achievements, g.achievements_total) + "  -  " + TimeAgo(g.last_played),
                    .detail = std::to_string(g.gamerscore) +
                              (g.gamerscore_total ? " / " + std::to_string(g.gamerscore_total) : "") + " G",
                    .lazy_icon = [this, id = g.title_id, v = g.icon](bool* loading) { return GameIcon(id, v, loading); },
                    .activate = [this, gamertag, g] {
                      Push({PageKind::kPlayerGame, g.name, gamertag + "|" + g.title_id});
                    }});
  }
  if (rows.empty()) rows.push_back({.label = "No games played yet", .disabled = true});
}

void GuideDialog::PlayerGameRows(std::vector<Row>& rows, Page& page) {
  const size_t bar = page.arg.find('|');
  const std::string gamertag = page.arg.substr(0, bar);
  const std::string title_id = bar == std::string::npos ? "" : page.arg.substr(bar + 1);
  auto& f = player_game_[page.arg];
  if (!f.loading && time_ - f.at >= kProfileRefresh) {
    f.loading = true;
    backend_->PlayerGame(gamertag, title_id, [this, key = page.arg](const Result& r, PlayedGameDetail detail) {
      auto& g = player_game_[key];
      g.loading = false;
      g.at = time_;
      g.error = r.ok ? "" : r.error;
      if (r.ok) g.value = std::move(detail);
    });
  }
  if (f.at < -999.0f) {
    rows.push_back({.label = "Loading...", .disabled = true});
    return;
  }
  if (!f.error.empty() && f.value.achievements.empty() && f.value.boards.empty()) {
    rows.push_back({.label = "Couldn't load the game", .sub = f.error, .disabled = true});
    return;
  }
  // The game running here has its achievements' pictures and descriptions;
  // others have only what the server keeps.
  const bool running = title_id == CurrentTitleId();
  std::map<uint32_t, rex::system::AchievementInfo> local;
  if (auto* kernel = running ? REX_KERNEL_STATE() : nullptr) {
    for (const auto& a : kernel->achievements().ListAchievements()) local[a.id] = a;
  }
  for (const BoardPlace& b : f.value.boards) {
    Row row{.label = b.name,
            .sub = "#" + std::to_string(b.rank) + " of " + std::to_string(b.total),
            .detail = FormatScore(std::llround(b.rating)),
            .glyph = Glyph::kPeople};
    if (running) row.activate = [this, b] { Push({PageKind::kLeaderboard, b.name, std::to_string(b.id)}); };
    rows.push_back(std::move(row));
  }
  for (const AchievementRecord& a : f.value.achievements) {
    Row row{.label = a.label, .sub = "Unlocked " + TimeAgo(a.unlocked), .detail = std::to_string(a.gamerscore) + " G"};
    if (auto it = local.find(a.id); it != local.end()) {
      row.icon = AchievementIcon(it->second);
      if (!it->second.description.empty()) row.sub = it->second.description;
    } else if (a.icon) {
      // From the server (an admin's Guide gave it the game's icons).
      row.lazy_icon = [this, title_id, id = a.id, v = a.icon](bool* loading) {
        return RemoteAchievementIcon(title_id, id, v, loading);
      };
    } else {
      row.glyph = Glyph::kTrophy;
    }
    rows.push_back(std::move(row));
  }
  if (rows.empty()) rows.push_back({.label = "Nothing unlocked or ranked yet", .disabled = true});
}

std::string GuideDialog::PlayerGameSummary(const Page& page) {
  const size_t bar = page.arg.find('|');
  if (bar == std::string::npos) return "";
  auto it = player_games_.find(page.arg.substr(0, bar));
  if (it == player_games_.end()) return "";
  for (const PlayedGame& g : it->second.value) {
    if (g.title_id != page.arg.substr(bar + 1)) continue;
    return AchievementCount(g.achievements, g.achievements_total) + "  -  " + std::to_string(g.gamerscore) +
           (g.gamerscore_total ? " of " + std::to_string(g.gamerscore_total) : "") + " G";
  }
  return "";
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
  auto& manager = kernel->achievements();
  // The game's name, achievement totals and named leaderboards, for profiles
  // and the server's list of games (sent once a session).
  uint32_t achievements_total = 0, gamerscore_total = 0;
  for (const auto& a : manager.ListAchievements()) {
    ++achievements_total;
    gamerscore_total += a.gamerscore;
  }
  std::map<uint32_t, std::string> boards;
  for (const auto& b : stats::Leaderboards()) boards[b.id] = b.name;
  player1->ReportTitleInfo(title_id, config_.title_name, achievements_total, gamerscore_total, boards);
  UploadAchievementIcons(*player1, title_id);
  std::vector<AchievementRecord> unlocked;
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

namespace {

// An achievement's icon file where the runtime looks for it
// (rex::ui::AchievementIconCache): its own metadata path, icons/<image id>.png
// in the metadata, then the title's XDBF image. Empty if none.
std::string AchievementIconFile(rex::system::KernelState* kernel, const rex::system::AchievementInfo& info) {
  rex::Runtime* runtime = kernel->emulator();
  auto metadata = [runtime](const std::filesystem::path& relative) -> std::string {
    if (!runtime || relative.empty()) return {};
    if (const auto path = runtime->FindMetadataPath(relative)) {
      if (std::ifstream file(*path, std::ios::binary); file) {
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      }
    }
    if (const auto embedded = runtime->FindEmbeddedMetadata(relative)) {
      return std::string(reinterpret_cast<const char*>(embedded->bytes.data()), embedded->bytes.size());
    }
    return {};
  };
  std::string file = metadata(info.icon_path);
  if (file.empty() && info.image_id) file = metadata(std::filesystem::path("icons") / (std::to_string(info.image_id) + ".png"));
  if (file.empty() && info.image_id) {
    const auto xdbf = kernel->title_xdbf();
    if (xdbf.is_valid()) {
      if (const auto block = xdbf.GetEntry(rex::system::util::XdbfSection::kImage, info.image_id)) {
        file.assign(reinterpret_cast<const char*>(block.buffer), block.size);
      }
    }
  }
  return file;
}

}  // namespace

// An admin's Guide gives the server the running game's achievement icons it
// hasn't got, so profiles show them to anyone (once a session).
void GuideDialog::UploadAchievementIcons(Backend& backend, const std::string& title_id) {
  if (!backend.is_admin() || achievement_icons_checked_ == title_id) return;
  achievement_icons_checked_ = title_id;
  backend.AchievementIconIds(title_id, [this, &backend, title_id](const Result& r, std::vector<uint32_t> have) {
    auto* kernel = REX_KERNEL_STATE();
    if (!r.ok || !kernel) {
      achievement_icons_checked_.clear();  // again on a later report
      return;
    }
    int sent = 0;
    for (const auto& a : kernel->achievements().ListAchievements()) {
      if (std::find(have.begin(), have.end(), a.id) != have.end()) continue;
      std::string file = AchievementIconFile(kernel, a);
      if (file.empty()) continue;
      backend.UploadAchievementIcon(title_id, a.id, std::move(file), nullptr);
      ++sent;
    }
    if (sent) REXLOG_INFO("Rexglue Guide: uploading {} achievement icon(s) for {}", sent, title_id);
  });
}

// Another player's achievement in a game the server has its icon for.
ImTextureID GuideDialog::RemoteAchievementIcon(const std::string& title_id, uint32_t id, int64_t version,
                                               bool* loading) {
  const std::string file = backend_->AchievementIconImage(title_id, id, version);
  if (loading) *loading = file.empty();
  if (file.empty()) return 0;
  const std::string key = "ach:" + title_id + "/" + std::to_string(id) + "_" + std::to_string(version);
  return textures_.Picture(key, std::span(reinterpret_cast<const uint8_t*>(file.data()), file.size()));
}

}  // namespace rexglue_guide
