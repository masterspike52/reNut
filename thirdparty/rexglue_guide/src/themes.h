#pragma once
// Guide themes: a colour for each part of the Guide, once for Light mode and
// once for Dark, and an optional background picture. A few are built in; the
// rest come from RexLiveless, made in the Guide's Theme Designer.

#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"

namespace rexglue_guide::themes {

// The parts of the Guide a theme colours.
enum Element {
  kAccent,        // selection, buttons
  kAccentLight,   // the Ring of Light, unread dots, the spinner
  kBlade,         // the open blade and message boxes
  kBladeStrip,    // the open blade's icon strip
  kSideBlade,     // blades beside it
  kSideBladeFar,  // blades further out
  kSideText,      // labels on the side blades
  kText,
  kTextDim,
  kSelectedSub,   // the second line of the selected row
  kBackdrop,      // over the game behind the Guide (its alpha darkens it)
  kPopup,         // notification pop-ups
  kElementCount,
};

struct ElementInfo {
  const char* key;   // in a theme's JSON
  const char* name;  // in the designer
};
const ElementInfo& Info(int element);

struct Palette {
  ImU32 colors[kElementCount] = {};
};

struct Theme {
  std::string key;     // "builtin:<name>" or "server:<id>"
  std::string name;
  std::string author;  // gamertag; empty when built in
  uint64_t author_xuid = 0;
  int server_id = 0;   // 0 when built in
  int64_t updated = 0;
  Palette light, dark;
  bool has_image = false;  // a server theme's background picture
  float image_alpha = 0.6f;
  float image_blur = 0.0f;  // 0..1
  // Server themes: marked NSFW, likes, players using it, whether this player
  // likes it, when it was made.
  bool nsfw = false;
  int likes = 0;
  int users = 0;
  bool liked = false;
  int64_t created = 0;

  const Palette& palette(bool dark_mode) const { return dark_mode ? dark : light; }
};

// Default first.
const std::vector<Theme>& BuiltIn();
const Theme& Default();
const Theme* FindBuiltIn(const std::string& key);

// A theme's "data": {light, dark, image_alpha, image_blur}. Colours it
// doesn't give are the default theme's.
std::string DataJson(const Theme& theme);
void ParseData(const std::string& json, Theme* theme);

// #RRGGBBAA, and back (#RRGGBB is opaque).
std::string Hex(ImU32 color);
bool ParseHex(std::string text, ImU32* color);

// Sets ui::color to the palette.
void Apply(const Palette& palette);

}  // namespace rexglue_guide::themes
