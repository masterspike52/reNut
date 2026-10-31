// Game gamerpics: the Xbox 360 gamerpics archived at xboxgamer.pics, picked by
// game; and the gamerpic grids, which show these or the built-in and uploaded
// ones.

#include <algorithm>
#include <cctype>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "guide_dialog.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

namespace {

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

}  // namespace

void GuideDialog::GamerpicGamesRows(std::vector<Row>& rows, Page& page) {
  const std::string filter = page.arg;
  rows.push_back({.label = filter.empty() ? "Search" : "Search: " + filter, .activate = [this, filter] {
                    OpenKeyboard({"Search Games", "Part of a game's name.", filter, false, 40,
                                  [this](std::optional<std::string> text) {
                                    Page& p = Current();
                                    if (!text || p.kind != PageKind::kGamerpicGames) return;
                                    p.arg = *text;
                                    p.selected = 0;
                                    p.scroll = 0.0f;
                                  }});
                  }});
  std::string error;
  const std::vector<GamerpicGame>* games = backend_->gamerpic_games(&error);
  if (!games) {
    rows.push_back({.label = error.empty() ? "Loading..." : "Couldn't load the games", .sub = error, .disabled = true});
    return;
  }
  auto row = [this](const GamerpicGame& g, std::string sub) {
    return Row{.label = g.name,
               .sub = std::move(sub),
               .detail = std::to_string(g.count) + (g.count == 1 ? " gamerpic" : " gamerpics"),
               // The game's own tile from the archive, so games are told apart
               // at a glance; downloaded only once its row is on screen.
               .lazy_icon = [this, id = g.title_id](bool* loading) { return GameGamerpicTexture(id, "8000", false, loading); },
               .activate = [this, g] { Push({PageKind::kGamerpicGame, g.name, g.title_id}); }};
  };
  const std::string needle = Lower(filter);
  const std::string playing = Lower(CurrentTitleId());
  if (needle.empty()) {
    // The game being played first, then the 360's own default gamerpics: the
    // dashboard's, and the community ones it offered too.
    const std::pair<std::string, const char*> kPinned[] = {
        {playing, "Playing now"},
        {"fffe07d1", "Common"},  // Xbox 360 Dashboard
        {"fffe07c3", "Common"},  // Xbox 360 Community
    };
    for (size_t p = 0; p < std::size(kPinned); ++p) {
      const auto& [id, sub] = kPinned[p];
      // Once only (a pinned one being played shows as playing).
      if (p > 0 && id == playing) continue;
      for (const GamerpicGame& g : *games) {
        if (g.title_id == id) rows.push_back(row(g, sub));
      }
    }
  }
  const size_t first = rows.size();
  for (const GamerpicGame& g : *games) {
    if (needle.empty() || Lower(g.name).find(needle) != std::string::npos || g.title_id.starts_with(needle)) {
      rows.push_back(row(g, ""));
    }
  }
  if (rows.size() == first) rows.push_back({.label = "No games match", .disabled = true});
}

const std::vector<std::string>* GuideDialog::GameGamerpicIds(const std::string& title_id, std::string* error) {
  auto& f = game_gamerpic_ids_[title_id];
  // Read once; again a while after a failure.
  if (!f.loading && (f.at < -999.0f || (!f.error.empty() && time_ - f.at >= 15.0f))) {
    f.loading = true;
    backend_->GameGamerpics(title_id, [this, title_id](const Result& r, std::vector<std::string> ids) {
      auto& g = game_gamerpic_ids_[title_id];
      g.loading = false;
      g.at = time_;
      g.error = r.ok ? "" : r.error;
      if (r.ok) g.value = std::move(ids);
    });
  }
  if (error) *error = f.error;
  return f.at < -999.0f || !f.error.empty() ? nullptr : &f.value;
}

ImTextureID GuideDialog::GameGamerpicTexture(const std::string& title_id, const std::string& id, bool tile,
                                             bool* loading) {
  constexpr size_t kKept = 300;
  constexpr float kRetryAfter = 2.0f;
  if (loading) *loading = false;
  const std::string key = "xgp:" + title_id + "/" + id;
  if (auto it = game_gamerpic_textures_.find(key); it != game_gamerpic_textures_.end()) {
    if (it->second.texture || time_ - it->second.made < kRetryAfter) {
      it->second.used = time_;
      return it->second.texture;
    }
    textures_.Drop(key);  // couldn't be made: again now
    game_gamerpic_textures_.erase(it);
  }
  bool missing = false;
  const std::string file = backend_->GameGamerpicImage(title_id, id, &missing, allow_downloads_);
  if (file.empty()) {
    if (loading) *loading = !missing;
    return 0;
  }
  // Room for it: the least recently drawn go (not one drawn this frame).
  while (game_gamerpic_textures_.size() >= kKept) {
    auto oldest = std::min_element(game_gamerpic_textures_.begin(), game_gamerpic_textures_.end(),
                                   [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
    if (oldest->second.used >= time_) break;
    textures_.Drop(oldest->first);  // retired: frames still in flight may draw it
    game_gamerpic_textures_.erase(oldest);
  }
  // A gamerpic with the frame and gloss server gamerpics get
  // (ui::Textures::Gamerpic), as it will look on the profile.
  const ImTextureID texture =
      textures_.Picture(key, std::span(reinterpret_cast<const uint8_t*>(file.data()), file.size()), tile);
  game_gamerpic_textures_[key] = {texture, time_, time_};
  return texture;
}

int GuideDialog::GridCount(const Page& page) {
  if (page.kind == PageKind::kGamerpicGame) {
    const std::vector<std::string>* ids = GameGamerpicIds(page.arg, nullptr);
    return ids ? int(ids->size()) : 0;
  }
  return int(backend_->gamerpic_choices().size());
}

ImTextureID GuideDialog::GridPicture(const Page& page, int index, bool* loading) {
  *loading = false;
  if (page.kind == PageKind::kGamerpicGame) {
    const std::vector<std::string>* ids = GameGamerpicIds(page.arg, nullptr);
    return ids && index < int(ids->size()) ? GameGamerpicTexture(page.arg, (*ids)[index], true, loading) : 0;
  }
  const std::vector<int> choices = backend_->gamerpic_choices();
  return index < int(choices.size()) ? textures_.Gamerpic(choices[index]) : 0;
}

void GuideDialog::ChooseGridPicture(const Page& page, int index) {
  if (page.kind != PageKind::kGamerpicGame) return ChooseGamerpic(index);
  const std::vector<std::string>* ids = GameGamerpicIds(page.arg, nullptr);
  if (!ids || index < 0 || index >= int(ids->size())) return;
  Busy("Changing your gamerpic...");
  backend_->SetGameGamerpic(page.arg, (*ids)[index], [this](const Result& r) {
    busy_.reset();
    if (!r.ok) {
      ShowMessage({"Gamerpic Database", r.error, {"OK"}});
      return;
    }
    BackToProfile();
  });
}

}  // namespace rexglue_guide
