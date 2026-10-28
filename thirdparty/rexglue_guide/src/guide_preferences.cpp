// Preferences: sounds, pop-ups, Light or Dark, and the theme.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "audio.h"
#include "guide_dialog.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

namespace {

// Before Light and Dark were a mode, they were themes beside Blue, Orange,
// Purple and Pink, saved by name (or, earlier still, by place in the list:
// Green, Blue, Orange, Purple, Pink, Midnight). Their mode and theme now.
void FromOldTheme(const nlohmann::json& saved, bool* dark, std::string* key) {
  std::string name;
  if (saved.is_string()) {
    name = saved.get<std::string>();
  } else if (saved.is_number_integer()) {
    static constexpr const char* kOldOrder[] = {"Light", "Blue", "Orange", "Purple", "Pink", "Dark"};
    const int old = saved.get<int>();
    name = old >= 0 && old < int(std::size(kOldOrder)) ? kOldOrder[old] : "Light";
  }
  *dark = name == "Dark";
  *key = name == "Light" || name == "Dark" || name.empty() ? themes::Default().key : "builtin:" + name;
}

}  // namespace

void GuideDialog::PreferencesRows(std::vector<Row>& rows, Page&) {
  rows.push_back({.label = "Mode", .detail = dark_mode_ ? "Dark" : "Light", .activate = [this] {
                    dark_mode_ = !dark_mode_;
                    SavePreferences();
                  }});
  rows.push_back({.swatch = theme_.palette(dark_mode_).colors[themes::kAccent],
                  .label = "Theme",
                  .detail = theme_.name,
                  .activate = [this] { Push({PageKind::kThemes, "Theme"}); }});
  rows.push_back({.label = "Show NSFW Themes", .detail = show_nsfw_ ? "On" : "Off", .activate = [this] {
                    show_nsfw_ = !show_nsfw_;
                    SavePreferences();
                  }});
  const int quota = backend_->theme_quota();
  if (quota != 0) {
    rows.push_back({.label = "Theme Designer", .activate = [this, quota] {
                      if (quota < 0) return Push({PageKind::kMyThemes, "My Themes"});
                      // Trusted players have one: theirs, or a new one.
                      int mine = 0;
                      for (const ServerTheme& t : backend_->themes()) {
                        if (t.author_xuid == backend_->me().xuid) mine = t.id;
                      }
                      OpenDesigner(mine);
                    }});
  }
  rows.push_back({.label = "Guide Sounds", .detail = sounds_ ? "On" : "Off", .activate = [this] {
                    sounds_ = !sounds_;
                    audio::SetEnabled(sounds_);
                    SavePreferences();
                  }});
  rows.push_back({.label = "Notification Pop-ups", .detail = popups_ ? "On" : "Off", .activate = [this] {
                    popups_ = !popups_;
                    SavePreferences();
                  }});
}

// <data dir>/preferences.json. A server theme is kept whole ("theme_cache"),
// so it shows before (or without) RexLiveless.
void GuideDialog::LoadPreferences() {
  std::ifstream file(config_.data_dir / "preferences.json");
  if (file) {
    try {
      const auto j = nlohmann::json::parse(file);
      sounds_ = j.value("sounds", true);
      popups_ = j.value("popups", true);
      show_nsfw_ = j.value("show_nsfw", false);
      theme_sort_ = ThemeSort(std::clamp(j.value("theme_sort", 0), 0, int(ThemeSort::kCount) - 1));
      std::string key;
      if (j.contains("mode")) {
        dark_mode_ = j.value("mode", "light") == "dark";
        key = j.value("theme", "");
      } else {
        FromOldTheme(j.value("theme", nlohmann::json()), &dark_mode_, &key);
      }
      if (const themes::Theme* builtin = themes::FindBuiltIn(key)) {
        theme_ = *builtin;
      } else if (const auto cache = j.value("theme_cache", nlohmann::json()); cache.is_object() && key.starts_with("server:")) {
        ServerTheme server;
        server.id = cache.value("id", 0);
        server.name = cache.value("name", "");
        server.author = cache.value("author", "");
        server.author_xuid = std::strtoull(cache.value("author_xuid", "0").c_str(), nullptr, 16);
        server.data = cache.value("data", "{}");
        server.has_image = cache.value("has_image", false);
        server.updated = cache.value("updated", int64_t(0));
        theme_ = FromServer(server);
      }
    } catch (...) {
    }
  }
  audio::SetEnabled(sounds_);
  ApplyShownTheme();
}

void GuideDialog::SavePreferences() {
  std::error_code ec;
  std::filesystem::create_directories(config_.data_dir, ec);
  nlohmann::json j = {{"sounds", sounds_},        {"popups", popups_},
                      {"mode", dark_mode_ ? "dark" : "light"}, {"theme", theme_.key},
                      {"show_nsfw", show_nsfw_},   {"theme_sort", int(theme_sort_)}};
  if (theme_.server_id) {
    char xuid[17];
    std::snprintf(xuid, sizeof(xuid), "%016llX", static_cast<unsigned long long>(theme_.author_xuid));
    j["theme_cache"] = {{"id", theme_.server_id},     {"name", theme_.name},
                        {"author", theme_.author},    {"author_xuid", xuid},
                        {"data", themes::DataJson(theme_)}, {"has_image", theme_.has_image},
                        {"updated", theme_.updated}};
  }
  std::ofstream(config_.data_dir / "preferences.json", std::ios::trunc) << j.dump(2);
}

}  // namespace rexglue_guide
