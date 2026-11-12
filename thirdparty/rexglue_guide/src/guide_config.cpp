// System Settings > Config: the game's settings (the settings registry), as the
// settings overlay has them, by scope.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <rex/setting.h>
#include <rex/settings_registry.h>

#include "audio.h"
#include "guide_dialog.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

namespace {

// Whether `category` is `parent` or inside it ("gpu/d3d12" is in "gpu").
bool Within(const std::string& category, const std::string& parent) {
  if (parent.empty()) return true;
  return category == parent ||
         (category.size() > parent.size() && category.compare(0, parent.size(), parent) == 0 &&
          category[parent.size()] == '/');
}

std::string LastPart(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A setting's category: its owner's scope, a level per dot ("gpu.d3d12" is "gpu/d3d12").
std::string CategoryOf(const rex::SettingBase& s) {
  std::string category = s.GetOwner().GetScope();
  std::replace(category.begin(), category.end(), '.', '/');
  return category;
}

bool IsNumber(rex::SettingType type) {
  using rex::SettingType;
  return type == SettingType::kI32 || type == SettingType::kI64 || type == SettingType::kU32 ||
         type == SettingType::kU64 || type == SettingType::kF64;
}

// A value as shown on its row.
std::string Shown(const rex::SettingBase& s, const std::string& value) {
  using rex::SettingType;
  if (s.GetType() == SettingType::kBool) return value == "true" || value == "1" ? "On" : "Off";
  if (s.GetType() == SettingType::kF64) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", std::strtod(value.c_str(), nullptr));
    return buf;
  }
  if ((s.GetType() == SettingType::kString || s.GetType() == SettingType::kPath) && value.empty()) return "None";
  return value;
}

}  // namespace

// The settings in `page.arg`'s category: its subcategories first, then its
// own settings, then Reset.
void GuideDialog::ConfigRows(std::vector<Row>& rows, Page& page) {
  const std::string& here = page.arg;
  std::map<std::string, int> subcategories;  // path below `here` -> settings in it
  std::vector<rex::SettingBase*> settings;
  for (rex::SettingBase* s : REX_SETTINGS().Enumerate()) {
    if (config_.show_setting && !config_.show_setting(s->GetName())) continue;
    const std::string category = CategoryOf(*s);
    if (!Within(category, here)) continue;
    if (category == here) {
      settings.push_back(s);
    } else {
      const size_t from = here.empty() ? 0 : here.size() + 1;
      const size_t slash = category.find('/', from);
      ++subcategories[category.substr(0, slash)];
    }
  }

  for (const auto& [path, count] : subcategories) {
    rows.push_back({.label = LastPart(path),
                    .detail = std::to_string(count) + (count == 1 ? " setting" : " settings"),
                    .activate = [this, path] { Push({PageKind::kConfig, LastPart(path), path}); }});
  }

  std::sort(settings.begin(), settings.end(), [](const auto* a, const auto* b) { return a->GetName() < b->GetName(); });
  std::vector<std::string> names;
  for (const rex::SettingBase* s : settings) {
    using rex::SettingType;
    using rex::SettingLife;
    const std::string name = s->GetName();
    names.push_back(name);
    const std::string value = s->GetText();
    const rex::SettingConstraints& c = s->GetConstraints();
    Row row{.label = s->GetHelp().empty() ? name : s->GetHelp(), .detail = Shown(*s, value)};
    row.sub = name;
    if (c.life == SettingLife::kRestart) row.sub += "  -  applies after a restart";
    if (c.life == SettingLife::kStartupOnly || s->IsFrozen()) {
      row.sub += "  -  set at startup";
      row.disabled = true;
      rows.push_back(std::move(row));
      continue;
    }
    if (s->GetType() == SettingType::kBool) {
      row.activate = [this, name, value] { ChangeSetting(name, value == "true" || value == "1" ? "false" : "true"); };
    } else if (!c.allowed.empty()) {
      // Each press takes the next one.
      row.activate = [this, name, value, allowed = c.allowed] {
        auto it = std::find(allowed.begin(), allowed.end(), value);
        ChangeSetting(name, it == allowed.end() || ++it == allowed.end() ? allowed.front() : *it);
      };
    } else if (IsNumber(s->GetType()) && c.min && c.max && *c.max > *c.min) {
      const double lo = *c.min, hi = *c.max;
      const bool whole = s->GetType() != SettingType::kF64;
      row.slider = float(std::clamp((std::strtod(value.c_str(), nullptr) - lo) / (hi - lo), 0.0, 1.0));
      row.slider_step = whole && hi - lo <= 50.0 ? float(1.0 / (hi - lo)) : 0.02f;
      row.set_slider = [this, name, lo, hi, whole](float t) {
        const double v = lo + (hi - lo) * double(t);
        char buf[64];
        if (whole) std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(v)));
        else std::snprintf(buf, sizeof(buf), "%.3f", v);
        ChangeSetting(name, buf, false);
      };
    } else {
      // Anything else is typed in.
      row.activate = [this, name, value, label = row.label] {
        OpenKeyboard({label, name, value, false, 256, [this, name](std::optional<std::string> text) {
                        if (text) ChangeSetting(name, *text);
                      }});
      };
    }
    rows.push_back(std::move(row));
  }

  if (!names.empty()) {
    rows.push_back({.label = "Reset to Defaults", .activate = [this, names] {
                      ShowMessage({"Reset to Defaults", "Put every setting on this page back to its default?",
                                   {"Reset", "Cancel"}, 0, [this, names](int i) {
                                     if (i != 0) return;
                                     for (const std::string& name : names) (void)REX_SETTINGS().Reset(name);
                                     SaveSettings();
                                   }});
                    }});
  }
  if (rows.empty()) rows.push_back({.label = "No settings here", .disabled = true});
}

void GuideDialog::ChangeSetting(const std::string& name, const std::string& value, bool notify) {
  if (!REX_SETTINGS().SetText(name, value)) {
    if (notify) ShowMessage({"Config", "That value isn't allowed for " + name + ".", {"OK"}});
    return;
  }
  SaveSettings();
  const auto found = REX_SETTINGS().Find(name);
  if (notify && found && (*found)->GetConstraints().life == rex::SettingLife::kRestart) {
    const std::string& help = (*found)->GetHelp();
    ShowToast({.glyph = Glyph::kInfo, .line1 = "Restart to apply", .line2 = help.empty() ? name : help});
  }
}

void GuideDialog::SaveSettings() {
  settings_dirty_ = true;
  settings_changed_at_ = time_;
}

// Once changes settle (a slider is let go), and at shutdown. Without the app's
// config file (Config::config_file) they last until the game is closed.
void GuideDialog::FlushSettings(bool now) {
  if (!settings_dirty_ || (!now && (time_ - settings_changed_at_ < 0.5f || list_.dragging_slider >= 0))) return;
  settings_dirty_ = false;
  if (config_.save_settings) {
    config_.save_settings();
  } else if (!config_.config_file.empty()) {
    (void)REX_SETTINGS().Save(config_.config_file);
  }
}

}  // namespace rexglue_guide
