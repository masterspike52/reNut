// System Settings > Config: the game's settings (rexglue cvars), as the F4
// settings overlay has them, by category.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <rex/cvar.h>

#include "audio.h"
#include "guide_dialog.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

namespace {

// Whether `category` is `parent` or inside it ("GPU/D3D12" is in "GPU").
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

bool IsNumber(rex::cvar::FlagType type) {
  using rex::cvar::FlagType;
  return type == FlagType::Int32 || type == FlagType::Int64 || type == FlagType::Uint32 ||
         type == FlagType::Uint64 || type == FlagType::Double;
}

// A value as shown on its row.
std::string Shown(const rex::cvar::FlagEntry& f, const std::string& value) {
  using rex::cvar::FlagType;
  if (f.type == FlagType::Boolean) return value == "true" || value == "1" ? "On" : "Off";
  if (f.type == FlagType::Double) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", std::strtod(value.c_str(), nullptr));
    return buf;
  }
  if (f.type == FlagType::String && value.empty()) return "None";
  return value;
}

}  // namespace

// The settings in `page.arg`'s category: its subcategories first, then its
// own settings, then Reset.
void GuideDialog::ConfigRows(std::vector<Row>& rows, Page& page) {
  const std::string& here = page.arg;
  std::map<std::string, int> subcategories;  // path below `here` -> settings in it
  std::vector<const rex::cvar::FlagEntry*> settings;
  for (const std::string& name : rex::cvar::ListFlags()) {
    const rex::cvar::FlagEntry* f = rex::cvar::GetFlagInfo(name);
    if (!f || f->type == rex::cvar::FlagType::Command) continue;  // console commands
    if (config_.show_setting && !config_.show_setting(f->name)) continue;
    if (!Within(f->category, here)) continue;
    if (f->category == here) {
      settings.push_back(f);
    } else {
      const size_t from = here.empty() ? 0 : here.size() + 1;
      const size_t slash = f->category.find('/', from);
      ++subcategories[f->category.substr(0, slash)];
    }
  }

  for (const auto& [path, count] : subcategories) {
    rows.push_back({.label = LastPart(path),
                    .detail = std::to_string(count) + (count == 1 ? " setting" : " settings"),
                    .activate = [this, path] { Push({PageKind::kConfig, LastPart(path), path}); }});
  }

  std::sort(settings.begin(), settings.end(), [](const auto* a, const auto* b) { return a->name < b->name; });
  for (const rex::cvar::FlagEntry* f : settings) {
    using rex::cvar::FlagType;
    using rex::cvar::Lifecycle;
    const std::string name = f->name;
    const std::string value = rex::cvar::GetFlagByName(name);
    Row row{.label = f->description.empty() ? name : f->description, .detail = Shown(*f, value)};
    row.sub = name;
    if (f->lifecycle == Lifecycle::kRequiresRestart) row.sub += "  -  applies after a restart";
    if (f->lifecycle == Lifecycle::kInitOnly) {
      row.sub += "  -  set at startup";
      row.disabled = true;
      rows.push_back(std::move(row));
      continue;
    }
    const auto& c = f->constraints;
    if (f->type == FlagType::Boolean) {
      row.activate = [this, name, value] { ChangeSetting(name, value == "true" || value == "1" ? "false" : "true"); };
    } else if (c.HasAllowedValues()) {
      // Each press takes the next one.
      row.activate = [this, name, value, allowed = c.allowed_values] {
        auto it = std::find(allowed.begin(), allowed.end(), value);
        ChangeSetting(name, it == allowed.end() || ++it == allowed.end() ? allowed.front() : *it);
      };
    } else if (IsNumber(f->type) && c.min && c.max && *c.max > *c.min) {
      const double lo = *c.min, hi = *c.max;
      const bool whole = f->type != FlagType::Double;
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

  if (!settings.empty()) {
    rows.push_back({.label = "Reset to Defaults", .activate = [this, settings] {
                      ShowMessage({"Reset to Defaults", "Put every setting on this page back to its default?",
                                   {"Reset", "Cancel"}, 0, [this, settings](int i) {
                                     if (i != 0) return;
                                     for (const auto* f : settings) rex::cvar::ResetToDefault(f->name);
                                     SaveSettings();
                                   }});
                    }});
  }
  if (rows.empty()) rows.push_back({.label = "No settings here", .disabled = true});
}

void GuideDialog::ChangeSetting(const std::string& name, const std::string& value, bool notify) {
  if (!rex::cvar::SetFlagByName(name, value)) {
    if (notify) ShowMessage({"Config", "That value isn't allowed for " + name + ".", {"OK"}});
    return;
  }
  SaveSettings();
  const rex::cvar::FlagEntry* f = rex::cvar::GetFlagInfo(name);
  if (notify && f && f->lifecycle == rex::cvar::Lifecycle::kRequiresRestart) {
    ShowToast({.glyph = Glyph::kInfo, .line1 = "Restart to apply", .line2 = f->description.empty() ? name : f->description});
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
  if (!config_.config_file.empty()) rex::cvar::SaveConfig(config_.config_file);
}

}  // namespace rexglue_guide
