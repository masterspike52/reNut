#pragma once
// Drawing helpers and theme for the Guide. Everything is laid out for a
// 1080p screen and multiplied by ui::S().

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "assets.h"
#include "imgui.h"

namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
}  // namespace rex::ui

namespace rexglue_guide::ui {

void ConfigureFonts(ImFontAtlas* atlas);
ImFont* Regular();
ImFont* Semibold();
ImFont* Light();

// Layout scale: 1.0 at 1080p.
float S();
void SetScale(float scale);
inline float Px(float v) {
  return v * S();
}

namespace color {
constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
// Set by themes::Apply. kGreen and kGreenLight are the accent (selection, the
// Live ring), whatever colour the theme makes them.
inline ImU32 kText = IM_COL32(240, 243, 247, 255);
inline ImU32 kTextDim = IM_COL32(176, 186, 199, 255);
inline ImU32 kSelectedSub = IM_COL32(235, 245, 228, 255);
inline ImU32 kBackdrop = IM_COL32(6, 9, 13, 185);
inline ImU32 kPopup = IM_COL32(26, 31, 38, 240);
inline ImU32 kKeyboardHeader = IM_COL32(168, 171, 176, 255);
inline ImU32 kKeyboardBody = IM_COL32(219, 220, 222, 255);
inline ImU32 kKey = IM_COL32(247, 247, 248, 255);
inline ImU32 kKeyLabel = IM_COL32(58, 58, 60, 255);
inline ImU32 kKeyboardField = IM_COL32(236, 236, 237, 255);
inline ImU32 kGreen = IM_COL32(108, 176, 64, 255);
inline ImU32 kGreenLight = IM_COL32(132, 196, 84, 255);
inline ImU32 kBlade = IM_COL32(52, 61, 74, 245);
inline ImU32 kBladeStrip = IM_COL32(38, 45, 55, 255);
inline ImU32 kSideBlade = IM_COL32(233, 236, 241, 235);
inline ImU32 kSideBladeFar = IM_COL32(214, 219, 226, 215);
inline ImU32 kSideText = IM_COL32(86, 92, 102, 255);
constexpr ImU32 kRow = IM_COL32(255, 255, 255, 14);
constexpr ImU32 kButtonA = IM_COL32(96, 172, 52, 255);
constexpr ImU32 kButtonB = IM_COL32(214, 40, 40, 255);
constexpr ImU32 kButtonX = IM_COL32(32, 108, 214, 255);
constexpr ImU32 kButtonY = IM_COL32(236, 186, 22, 255);
}  // namespace color

ImU32 WithAlpha(ImU32 col, float alpha);

ImVec2 Measure(ImFont* font, float size, std::string_view text);
void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, std::string_view text);
// Clips with "..." to max_width.
void TextFit(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col,
             std::string_view text, float max_width);
// Text turned 90 degrees clockwise (reads top to bottom), centred on center.
void TextVertical(ImDrawList* dl, ImFont* font, float size, ImVec2 center, ImU32 col,
                  std::string_view text);
// Uppercase with wide letter spacing, as on the 360's small labels.
void Caps(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, std::string_view text,
          float spacing);
float CapsWidth(ImFont* font, float size, std::string_view text, float spacing);
std::string Upper(std::string_view text);

void Shadow(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float spread, float alpha);
// A coloured face button glyph (A/B/X/Y).
void FaceButton(ImDrawList* dl, ImVec2 center, float radius, char letter);
// Bell, person, trophy and the like, drawn as shapes so they need no assets.
enum class Glyph { kBell, kPeople, kTrophy, kCrate, kInvite, kAddPerson, kInfo, kCheck };
void DrawGlyph(ImDrawList* dl, Glyph glyph, ImVec2 center, float size, ImU32 col);
void Spinner(ImDrawList* dl, ImVec2 center, float radius, float time, ImU32 col);
class Textures;
// A system's badge (Profile::platforms): a dark see-through tile `size`
// across from `min`, with the system's mark in white. Nothing for one it
// doesn't know.
void PlatformBadge(ImDrawList* dl, Textures& textures, std::string_view platform, ImVec2 min, float size,
                   float alpha);

// Textures for the embedded images, created on first use.
class Textures {
 public:
  explicit Textures(rex::ui::ImmediateDrawer* drawer);
  ~Textures();
  ImTextureID Icon(assets::Icon icon);
  // Embedded gamerpics, or a server one through server_gamerpic (the first
  // embedded picture stands in while it downloads).
  ImTextureID Gamerpic(int index);
  // A button or key icon (key_icons.h), by name; null if it isn't included.
  ImTextureID KeyIcon(std::string_view name);
  std::function<std::string(int)> server_gamerpic;
  // A texture made from RGBA pixels under `key`, replacing any it had. The one
  // replaced lives on a few frames, for draws already recorded with it.
  ImTextureID Rgba(const std::string& key, const uint8_t* rgba, int width, int height);
  // A picture file (PNG, JPEG...) under `key`, kept once made; `tile` and
  // `fit` as for gamerpics (Load). Call it only once the file is there.
  ImTextureID Picture(const std::string& key, std::span<const uint8_t> file, bool tile = false, int fit = 0) {
    return Load(key, file, tile, fit);
  }
  void Drop(const std::string& key);
  // Once a frame: frees replaced textures no frame in flight can still use.
  void EndFrame();

 private:
  // tile: give the picture the 360 gamer-tile frame and gloss the built-in
  // gamerpics have baked in. fit: shrink a bigger picture to that many pixels
  // across (0: as it is).
  ImTextureID Load(const std::string& key, std::span<const uint8_t> png, bool tile = false, int fit = 0);
  rex::ui::ImmediateDrawer* drawer_;
  std::unordered_map<std::string, std::unique_ptr<rex::ui::ImmediateTexture>> textures_;
  std::vector<std::pair<int, std::unique_ptr<rex::ui::ImmediateTexture>>> retired_;  // frames left, texture
};

void Image(ImDrawList* dl, ImTextureID tex, ImVec2 min, ImVec2 max, ImU32 tint = color::kWhite,
           float rounding = 0.0f);

}  // namespace rexglue_guide::ui
