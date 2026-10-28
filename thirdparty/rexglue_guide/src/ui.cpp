#include "ui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>

#include <rex/ui/image_decode.h>
#include <rex/ui/immediate_drawer.h>

namespace rexglue_guide::ui {
namespace {

// The atlas can't be re-rasterised at draw time (no dynamic font textures),
// so each face is baked twice and drawn from the closer size.
constexpr float kPi = 3.14159265f;
// Frames a replaced texture is kept: more than a renderer keeps in flight.
constexpr int kRetireFrames = 4;
constexpr float kSmallBake = 28.0f;
constexpr float kLargeBake = 56.0f;

// (Not "small"/"large": windows.h defines small as a macro.)
struct Face {
  ImFont* low = nullptr;
  ImFont* high = nullptr;
};
Face g_faces[size_t(assets::Font::kCount)];
float g_scale = 1.0f;

// Marks which baked font a public ImFont* stands for, so drawing can swap in
// the other bake by size.
const Face* FaceOf(ImFont* font) {
  for (const Face& face : g_faces) {
    if (face.low == font || face.high == font) return &face;
  }
  return nullptr;
}

ImFont* Pick(ImFont* font, float size) {
  const Face* face = FaceOf(font);
  if (!face || !face->low || !face->high) return font;
  return size <= kSmallBake * 1.15f ? face->low : face->high;
}

}  // namespace

void ConfigureFonts(ImFontAtlas* atlas) {
  static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2013, 0x2026, 0};
  for (size_t i = 0; i < size_t(assets::Font::kCount); ++i) {
    const auto ttf = assets::FontTtf(assets::Font(i));
    for (float size : {kSmallBake, kLargeBake}) {
      ImFontConfig config;
      config.FontDataOwnedByAtlas = false;
      config.OversampleH = 2;
      config.OversampleV = 1;
      config.PixelSnapH = false;
      ImFont* font = atlas->AddFontFromMemoryTTF(const_cast<uint8_t*>(ttf.data()), int(ttf.size()),
                                                 size, &config, kRanges);
      (size == kSmallBake ? g_faces[i].low : g_faces[i].high) = font;
    }
  }
}

ImFont* Regular() {
  return g_faces[size_t(assets::Font::kRegular)].high;
}
ImFont* Semibold() {
  return g_faces[size_t(assets::Font::kSemibold)].high;
}
ImFont* Light() {
  return g_faces[size_t(assets::Font::kLight)].high;
}

float S() {
  return g_scale;
}

void SetScale(float scale) {
  g_scale = scale;
}

ImU32 WithAlpha(ImU32 col, float alpha) {
  const float a = float((col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
  return (col & ~IM_COL32_A_MASK) | (ImU32(a + 0.5f) << IM_COL32_A_SHIFT);
}

ImVec2 Measure(ImFont* font, float size, std::string_view text) {
  font = Pick(font ? font : ImGui::GetFont(), size);
  return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data(), text.data() + text.size());
}

void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, std::string_view text) {
  font = Pick(font ? font : ImGui::GetFont(), size);
  dl->AddText(font, size, ImVec2(std::floor(pos.x), std::floor(pos.y)), col, text.data(),
              text.data() + text.size());
}

void TextFit(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col,
             std::string_view text, float max_width) {
  if (Measure(font, size, text).x <= max_width) {
    Text(dl, font, size, pos, col, text);
    return;
  }
  std::string clipped(text);
  while (!clipped.empty() && Measure(font, size, clipped + "...").x > max_width) {
    clipped.pop_back();
    // Don't leave half of a UTF-8 sequence behind.
    while (!clipped.empty() && (static_cast<unsigned char>(clipped.back()) & 0xC0) == 0x80) {
      clipped.pop_back();
    }
    if (!clipped.empty() && static_cast<unsigned char>(clipped.back()) >= 0xC0) clipped.pop_back();
  }
  Text(dl, font, size, pos, col, clipped + "...");
}

void TextVertical(ImDrawList* dl, ImFont* font, float size, ImVec2 center, ImU32 col,
                  std::string_view text) {
  const ImVec2 extent = Measure(font, size, text);
  const int first = dl->VtxBuffer.Size;
  Text(dl, font, size, ImVec2(center.x - extent.x * 0.5f, center.y - extent.y * 0.5f), col, text);
  // Turn 90 degrees clockwise about the centre: (x, y) -> (-y, x).
  for (int i = first; i < dl->VtxBuffer.Size; ++i) {
    ImVec2& p = dl->VtxBuffer[i].pos;
    const float dx = p.x - center.x;
    const float dy = p.y - center.y;
    p = ImVec2(center.x - dy, center.y + dx);
  }
}

std::string Upper(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (static_cast<unsigned char>(c) < 0x80) c = char(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

float CapsWidth(ImFont* font, float size, std::string_view text, float spacing) {
  const std::string upper = Upper(text);
  return Measure(font, size, upper).x + spacing * float(std::max<size_t>(upper.size(), 1) - 1);
}

void Caps(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, std::string_view text,
          float spacing) {
  const std::string upper = Upper(text);
  float x = pos.x;
  size_t i = 0;
  while (i < upper.size()) {
    // Step one UTF-8 character at a time.
    size_t len = 1;
    const unsigned char c = static_cast<unsigned char>(upper[i]);
    if (c >= 0xF0) len = 4;
    else if (c >= 0xE0) len = 3;
    else if (c >= 0xC0) len = 2;
    const std::string_view ch(upper.data() + i, std::min(len, upper.size() - i));
    Text(dl, font, size, ImVec2(x, pos.y), col, ch);
    x += Measure(font, size, ch).x + spacing;
    i += len;
  }
}

void Shadow(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float spread, float alpha) {
  constexpr int kSteps = 8;
  for (int i = kSteps; i >= 1; --i) {
    const float t = float(i) / kSteps;
    const float grow = spread * t;
    const float a = alpha * (1.0f - t) * (1.0f - t) * 0.35f;
    dl->AddRectFilled(ImVec2(min.x - grow, min.y - grow * 0.6f), ImVec2(max.x + grow, max.y + grow),
                      IM_COL32(0, 0, 0, int(a * 255)), rounding + grow);
  }
}

void FaceButton(ImDrawList* dl, ImVec2 center, float radius, char letter) {
  ImU32 col = color::kButtonA;
  switch (letter) {
    case 'B': col = color::kButtonB; break;
    case 'X': col = color::kButtonX; break;
    case 'Y': col = color::kButtonY; break;
    default: break;
  }
  dl->AddCircleFilled(center, radius, col, 32);
  // A soft highlight across the top, as on the 360 buttons.
  dl->AddCircleFilled(ImVec2(center.x, center.y - radius * 0.28f), radius * 0.62f,
                      IM_COL32(255, 255, 255, 34), 32);
  dl->AddCircle(center, radius, IM_COL32(0, 0, 0, 70), 32, std::max(1.0f, radius * 0.08f));
  const char text[2] = {letter, 0};
  const float size = radius * 1.35f;
  const ImVec2 extent = Measure(Semibold(), size, text);
  Text(dl, Semibold(), size, ImVec2(center.x - extent.x * 0.5f, center.y - extent.y * 0.52f),
       color::kWhite, text);
}

void DrawGlyph(ImDrawList* dl, Glyph glyph, ImVec2 c, float s, ImU32 col) {
  const float t = std::max(1.5f, s * 0.08f);  // stroke
  switch (glyph) {
    case Glyph::kBell: {
      const float w = s * 0.62f;
      dl->PathArcTo(ImVec2(c.x, c.y - s * 0.08f), w * 0.5f, kPi, 2 * kPi, 16);
      dl->PathLineTo(ImVec2(c.x + w * 0.5f, c.y + s * 0.22f));
      dl->PathLineTo(ImVec2(c.x + w * 0.62f, c.y + s * 0.3f));
      dl->PathLineTo(ImVec2(c.x - w * 0.62f, c.y + s * 0.3f));
      dl->PathLineTo(ImVec2(c.x - w * 0.5f, c.y + s * 0.22f));
      dl->PathStroke(col, ImDrawFlags_Closed, t);
      dl->AddCircleFilled(ImVec2(c.x, c.y + s * 0.4f), s * 0.07f, col);
      dl->AddCircleFilled(ImVec2(c.x, c.y - s * 0.42f), s * 0.05f, col);
      break;
    }
    case Glyph::kPeople: {
      for (float dx : {-0.18f, 0.2f}) {
        const ImVec2 head(c.x + dx * s, c.y - s * 0.16f);
        dl->AddCircleFilled(head, s * 0.15f, col, 20);
        dl->PathArcTo(ImVec2(head.x, c.y + s * 0.38f), s * 0.24f, kPi, 2 * kPi, 16);
        dl->PathFillConvex(col);
      }
      break;
    }
    case Glyph::kAddPerson: {
      const ImVec2 head(c.x - s * 0.1f, c.y - s * 0.16f);
      dl->AddCircleFilled(head, s * 0.16f, col, 20);
      dl->PathArcTo(ImVec2(head.x, c.y + s * 0.4f), s * 0.27f, kPi, 2 * kPi, 16);
      dl->PathFillConvex(col);
      const ImVec2 p(c.x + s * 0.3f, c.y - s * 0.05f);
      dl->AddLine(ImVec2(p.x - s * 0.13f, p.y), ImVec2(p.x + s * 0.13f, p.y), col, t * 1.3f);
      dl->AddLine(ImVec2(p.x, p.y - s * 0.13f), ImVec2(p.x, p.y + s * 0.13f), col, t * 1.3f);
      break;
    }
    case Glyph::kTrophy: {
      const float w = s * 0.46f;
      dl->PathLineTo(ImVec2(c.x - w * 0.5f, c.y - s * 0.36f));
      dl->PathLineTo(ImVec2(c.x + w * 0.5f, c.y - s * 0.36f));
      dl->PathArcTo(ImVec2(c.x, c.y - s * 0.12f), w * 0.5f, 0.0f, kPi, 16);
      dl->PathFillConvex(col);
      dl->AddCircle(ImVec2(c.x - w * 0.55f, c.y - s * 0.2f), s * 0.11f, col, 16, t);
      dl->AddCircle(ImVec2(c.x + w * 0.55f, c.y - s * 0.2f), s * 0.11f, col, 16, t);
      dl->AddRectFilled(ImVec2(c.x - s * 0.05f, c.y + s * 0.1f), ImVec2(c.x + s * 0.05f, c.y + s * 0.28f),
                        col);
      dl->AddRectFilled(ImVec2(c.x - s * 0.2f, c.y + s * 0.28f), ImVec2(c.x + s * 0.2f, c.y + s * 0.38f),
                        col, s * 0.03f);
      break;
    }
    case Glyph::kCrate: {
      const ImVec2 a(c.x - s * 0.36f, c.y - s * 0.32f), b(c.x + s * 0.36f, c.y + s * 0.36f);
      dl->AddRect(a, b, col, s * 0.04f, 0, t);
      dl->AddLine(ImVec2(a.x, a.y + s * 0.2f), ImVec2(b.x, a.y + s * 0.2f), col, t);
      dl->AddLine(ImVec2(a.x, b.y - s * 0.2f), ImVec2(b.x, b.y - s * 0.2f), col, t);
      dl->AddLine(ImVec2(a.x, a.y + s * 0.2f), ImVec2(b.x, b.y - s * 0.2f), col, t);
      break;
    }
    case Glyph::kInvite: {
      const ImVec2 a(c.x - s * 0.4f, c.y - s * 0.26f), b(c.x + s * 0.4f, c.y + s * 0.3f);
      dl->AddRect(a, b, col, s * 0.04f, 0, t);
      dl->AddLine(a, ImVec2(c.x, c.y + s * 0.04f), col, t);
      dl->AddLine(ImVec2(b.x, a.y), ImVec2(c.x, c.y + s * 0.04f), col, t);
      break;
    }
    case Glyph::kInfo: {
      dl->AddCircle(c, s * 0.4f, col, 32, t);
      dl->AddCircleFilled(ImVec2(c.x, c.y - s * 0.18f), s * 0.05f, col);
      dl->AddLine(ImVec2(c.x, c.y - s * 0.04f), ImVec2(c.x, c.y + s * 0.22f), col, t * 1.2f);
      break;
    }
    case Glyph::kCheck: {
      dl->AddPolyline(std::array<ImVec2, 3>{ImVec2(c.x - s * 0.3f, c.y), ImVec2(c.x - s * 0.08f, c.y + s * 0.22f),
                                            ImVec2(c.x + s * 0.32f, c.y - s * 0.24f)}
                          .data(),
                      3, col, 0, t * 1.4f);
      break;
    }
  }
}

void Spinner(ImDrawList* dl, ImVec2 center, float radius, float time, ImU32 col) {
  constexpr int kDots = 8;
  for (int i = 0; i < kDots; ++i) {
    const float a = float(i) / kDots * 2.0f * kPi;
    const float phase = std::fmod(time * 1.4f - float(i) / kDots + 1.0f, 1.0f);
    dl->AddCircleFilled(ImVec2(center.x + std::cos(a) * radius, center.y + std::sin(a) * radius),
                        radius * 0.16f, WithAlpha(col, 0.25f + 0.75f * (1.0f - phase)), 12);
  }
}

Textures::Textures(rex::ui::ImmediateDrawer* drawer) : drawer_(drawer) {}
Textures::~Textures() = default;

namespace {

// The look of the 360's gamer tiles: a dark rounded border, a thin light rim
// inside it, and a gloss over the top that ends in a shallow curve.
void ApplyTileStyle(std::vector<uint8_t>& rgba, int w, int h) {
  const float size = float(std::min(w, h));
  const float border = std::max(2.0f, size * 0.03f);
  const float radius = size * 0.09f;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
      // Distance inside the rounded square (negative outside its corners).
      const float px = std::min(x + 0.5f, w - x - 0.5f), py = std::min(y + 0.5f, h - y - 0.5f);
      float inside = std::min(px, py);
      if (px < radius && py < radius) inside = radius - std::hypot(radius - px, radius - py);
      // Gloss: inside an ellipse above the picture, strongest at the top.
      const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
      const float e = std::pow((u - 0.5f) / 1.05f, 2.0f) + std::pow(v + 0.55f, 2.0f);
      const float gloss_edge = std::clamp((1.0f - e) * size * 0.5f, 0.0f, 1.0f);
      const float gloss = gloss_edge * (0.20f - 0.08f * std::clamp(v / 0.45f, 0.0f, 1.0f));
      const float rim = std::clamp(1.0f - std::abs(inside - border - 0.75f), 0.0f, 1.0f) * 0.35f;
      const float dark = std::clamp(border - inside + 0.5f, 0.0f, 1.0f);
      for (int c = 0; c < 3; ++c) {
        float col = p[c];
        col += (255.0f - col) * (gloss + rim);
        col *= 1.0f - dark * 0.7f;
        p[c] = uint8_t(std::clamp(col, 0.0f, 255.0f));
      }
      p[3] = 255;
    }
  }
}

}  // namespace

ImTextureID Textures::Load(const std::string& key, std::span<const uint8_t> png, bool tile) {
  auto it = textures_.find(key);
  if (it != textures_.end()) return reinterpret_cast<ImTextureID>(it->second.get());
  auto& slot = textures_[key];
  int w = 0, h = 0;
  auto pixels = rex::ui::DecodeImageRGBA(png.data(), png.size(), w, h);
  if (tile && !pixels.empty()) ApplyTileStyle(pixels, w, h);
  if (drawer_ && !pixels.empty()) {
    slot = drawer_->CreateTexture(uint32_t(w), uint32_t(h),
                                  rex::ui::ImmediateTextureFilter::kLinear, false, pixels.data());
  }
  return reinterpret_cast<ImTextureID>(slot.get());
}

ImTextureID Textures::Rgba(const std::string& key, const uint8_t* rgba, int width, int height) {
  Drop(key);
  if (!drawer_ || width <= 0 || height <= 0) return 0;
  auto& slot = textures_[key];
  slot = drawer_->CreateTexture(uint32_t(width), uint32_t(height), rex::ui::ImmediateTextureFilter::kLinear, false, rgba);
  return reinterpret_cast<ImTextureID>(slot.get());
}

void Textures::Drop(const std::string& key) {
  auto it = textures_.find(key);
  if (it == textures_.end()) return;
  if (it->second) retired_.push_back({kRetireFrames, std::move(it->second)});
  textures_.erase(it);
}

void Textures::EndFrame() {
  for (auto& r : retired_) --r.first;
  std::erase_if(retired_, [](const auto& r) { return r.first <= 0; });
}

ImTextureID Textures::Icon(assets::Icon icon) {
  return Load("icon" + std::to_string(int(icon)), assets::IconPng(icon));
}

ImTextureID Textures::Gamerpic(int index) {
  if (index >= assets::kGamerpicCount) {
    const std::string key = "pic" + std::to_string(index);
    if (textures_.count(key)) return Load(key, {});
    const std::string png = server_gamerpic ? server_gamerpic(index) : std::string();
    if (png.empty()) index = 0;
    else return Load(key, std::span(reinterpret_cast<const uint8_t*>(png.data()), png.size()), true);
  }
  return Load("pic" + std::to_string(std::max(index, 0)), assets::GamerpicPng(std::max(index, 0)));
}

void Image(ImDrawList* dl, ImTextureID tex, ImVec2 min, ImVec2 max, ImU32 tint, float rounding) {
  if (!tex) return;
  if (rounding > 0.0f) {
    dl->AddImageRounded(tex, min, max, ImVec2(0, 0), ImVec2(1, 1), tint, rounding);
  } else {
    dl->AddImage(tex, min, max, ImVec2(0, 0), ImVec2(1, 1), tint);
  }
}

}  // namespace rexglue_guide::ui
