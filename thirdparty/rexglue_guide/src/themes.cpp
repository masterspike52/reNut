#include "themes.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iterator>

#include <nlohmann/json.hpp>

#include "ui.h"

namespace rexglue_guide::themes {
namespace {

using json = nlohmann::json;

constexpr ElementInfo kElements[kElementCount] = {
    {"accent", "Accent"},
    {"accent_light", "Accent Highlight"},
    {"blade", "Blade"},
    {"blade_strip", "Blade Strip"},
    {"side_blade", "Side Blades"},
    {"side_blade_far", "Far Side Blades"},
    {"side_text", "Side Blade Text"},
    {"text", "Text"},
    {"text_dim", "Dim Text"},
    {"selected_sub", "Selected Row Subtext"},
    {"backdrop", "Backdrop"},
    {"popup", "Pop-ups"},
};

// The built-in themes' colours: an accent pair, and the blades in Light mode
// (Dark mode's blades are the same for all of them).
struct BuiltInColors {
  const char* name;
  ImU32 accent, accent_light, blade, blade_strip, side_blade, side_blade_far, side_text;
};
constexpr BuiltInColors kBuiltIn[] = {
    {"Default", IM_COL32(108, 176, 64, 255), IM_COL32(132, 196, 84, 255), IM_COL32(52, 61, 74, 245),
     IM_COL32(38, 45, 55, 255), IM_COL32(233, 236, 241, 235), IM_COL32(214, 219, 226, 215), IM_COL32(86, 92, 102, 255)},
    {"Blue", IM_COL32(40, 122, 214, 255), IM_COL32(74, 152, 236, 255), IM_COL32(44, 56, 74, 245),
     IM_COL32(32, 42, 56, 255), IM_COL32(226, 234, 244, 235), IM_COL32(206, 216, 230, 215), IM_COL32(70, 86, 108, 255)},
    {"Orange", IM_COL32(222, 112, 26, 255), IM_COL32(242, 142, 58, 255), IM_COL32(60, 56, 54, 245),
     IM_COL32(44, 41, 39, 255), IM_COL32(242, 236, 230, 235), IM_COL32(226, 218, 210, 215), IM_COL32(104, 90, 78, 255)},
    {"Purple", IM_COL32(132, 78, 200, 255), IM_COL32(160, 110, 224, 255), IM_COL32(56, 52, 74, 245),
     IM_COL32(41, 38, 56, 255), IM_COL32(236, 232, 244, 235), IM_COL32(218, 212, 230, 215), IM_COL32(92, 84, 110, 255)},
    {"Pink", IM_COL32(214, 66, 134, 255), IM_COL32(234, 100, 160, 255), IM_COL32(66, 52, 64, 245),
     IM_COL32(49, 38, 47, 255), IM_COL32(244, 232, 238, 235), IM_COL32(230, 212, 222, 215), IM_COL32(110, 84, 98, 255)},
};

Palette MakePalette(const BuiltInColors& c, bool dark) {
  Palette p;
  p.colors[kAccent] = c.accent;
  p.colors[kAccentLight] = c.accent_light;
  p.colors[kBlade] = dark ? IM_COL32(24, 28, 34, 248) : c.blade;
  p.colors[kBladeStrip] = dark ? IM_COL32(15, 18, 22, 255) : c.blade_strip;
  p.colors[kSideBlade] = dark ? IM_COL32(64, 70, 80, 240) : c.side_blade;
  p.colors[kSideBladeFar] = dark ? IM_COL32(48, 53, 61, 225) : c.side_blade_far;
  p.colors[kSideText] = dark ? IM_COL32(196, 202, 210, 255) : c.side_text;
  p.colors[kText] = IM_COL32(240, 243, 247, 255);
  p.colors[kTextDim] = IM_COL32(176, 186, 199, 255);
  p.colors[kSelectedSub] = IM_COL32(235, 245, 228, 255);
  p.colors[kBackdrop] = IM_COL32(6, 9, 13, 185);
  p.colors[kPopup] = IM_COL32(26, 31, 38, 240);
  return p;
}

std::vector<Theme> MakeBuiltIn() {
  std::vector<Theme> themes;
  for (const BuiltInColors& c : kBuiltIn) {
    Theme t;
    t.key = std::string("builtin:") + c.name;
    t.name = c.name;
    t.light = MakePalette(c, false);
    t.dark = MakePalette(c, true);
    themes.push_back(std::move(t));
  }
  return themes;
}

json PaletteJson(const Palette& p) {
  json j = json::object();
  for (int e = 0; e < kElementCount; ++e) j[kElements[e].key] = Hex(p.colors[e]);
  return j;
}

void ParsePalette(const json& j, Palette* p) {
  if (!j.is_object()) return;
  for (int e = 0; e < kElementCount; ++e) {
    const auto it = j.find(kElements[e].key);
    if (it != j.end() && it->is_string()) ParseHex(it->get<std::string>(), &p->colors[e]);
  }
}

}  // namespace

const ElementInfo& Info(int element) {
  return kElements[std::clamp(element, 0, int(kElementCount) - 1)];
}

const std::vector<Theme>& BuiltIn() {
  static const std::vector<Theme> themes = MakeBuiltIn();
  return themes;
}

const Theme& Default() {
  return BuiltIn().front();
}

const Theme* FindBuiltIn(const std::string& key) {
  for (const Theme& t : BuiltIn()) {
    if (t.key == key) return &t;
  }
  return nullptr;
}

std::string DataJson(const Theme& theme) {
  return json{{"light", PaletteJson(theme.light)},
              {"dark", PaletteJson(theme.dark)},
              {"image_alpha", theme.image_alpha},
              {"image_blur", theme.image_blur}}
      .dump();
}

void ParseData(const std::string& text, Theme* theme) {
  theme->light = Default().light;
  theme->dark = Default().dark;
  try {
    const json j = json::parse(text);
    ParsePalette(j.value("light", json()), &theme->light);
    ParsePalette(j.value("dark", json()), &theme->dark);
    theme->image_alpha = std::clamp(j.value("image_alpha", 0.6f), 0.0f, 1.0f);
    theme->image_blur = std::clamp(j.value("image_blur", 0.0f), 0.0f, 1.0f);
  } catch (...) {
  }
}

std::string Hex(ImU32 color) {
  char buf[10];
  std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", unsigned(color >> IM_COL32_R_SHIFT) & 0xFF,
                unsigned(color >> IM_COL32_G_SHIFT) & 0xFF, unsigned(color >> IM_COL32_B_SHIFT) & 0xFF,
                unsigned(color >> IM_COL32_A_SHIFT) & 0xFF);
  return buf;
}

bool ParseHex(std::string text, ImU32* color) {
  text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }), text.end());
  if (!text.empty() && text[0] == '#') text.erase(0, 1);
  if (text.size() != 6 && text.size() != 8) return false;
  if (!std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c); })) return false;
  if (text.size() == 6) text += "FF";
  const unsigned long v = std::stoul(text, nullptr, 16);
  *color = IM_COL32((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
  return true;
}

void Apply(const Palette& p) {
  namespace color = ui::color;
  color::kGreen = p.colors[kAccent];
  color::kGreenLight = p.colors[kAccentLight];
  color::kBlade = p.colors[kBlade];
  color::kBladeStrip = p.colors[kBladeStrip];
  color::kSideBlade = p.colors[kSideBlade];
  color::kSideBladeFar = p.colors[kSideBladeFar];
  color::kSideText = p.colors[kSideText];
  color::kText = p.colors[kText];
  color::kTextDim = p.colors[kTextDim];
  color::kSelectedSub = p.colors[kSelectedSub];
  color::kBackdrop = p.colors[kBackdrop];
  color::kPopup = p.colors[kPopup];
}

}  // namespace rexglue_guide::themes
