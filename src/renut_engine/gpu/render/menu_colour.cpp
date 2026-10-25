/**
 * @file    gpu/render/menu_colour.cpp
 */
#include "renut_engine/gpu/render/menu_colour.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include <rex/cvar.h>

#include "renut_engine/gpu/log.h"

REXCVAR_DEFINE_STRING(menu_background_color, "#293463", "Nuts&Bolts/Menus",
                      "Colour of the see-through blue panels behind menus (the garage's dials and "
                      "part list, blueprints, the pause menu), a colour code like #FF0000 (red); "
                      "#293463 is the game's. #RRGGBBAA also sets how see-through they are: CC is "
                      "the game's, FF solid.");

namespace renut::gpu {

namespace {

// The panels' blue (41, 52, 99) as BC2 keeps it, 5:6:5.
constexpr int kPanelRed = 5, kPanelGreen = 13, kPanelBlue = 12;
// The game's panel alpha (204), as BC2's 4 bits.
constexpr float kPanelAlpha = 12.0f;
// A texture is a panel when at least a third of its blocks are the blue.
constexpr uint32_t kPanelShareDivisor = 3;
constexpr uint32_t kMinPanelBlocks = 16;
// How far (0 to 1, a channel) a colour may be from a mix of the blue and
// white and still be recoloured.
constexpr float kMixTolerance = 0.07f;

struct Latched {
  bool active = false;
  float colour[3] = {};
  bool alpha = false;
  float alpha_scale = 1.0f;  // of BC2's 4-bit alpha
};
Latched g_latched;
std::string g_code;
uint32_t g_generation = 0;

// A colour code as 0xRRGGBBAA: #RRGGBB or #RRGGBBAA, '#' or "0x" optional;
// 'has_alpha' when it gave one. False when it isn't one.
bool ParseColour(const std::string &code, uint32_t &rgba, bool &has_alpha) {
  std::string hex = code;
  if (!hex.empty() && hex[0] == '#')
    hex.erase(0, 1);
  else if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
    hex.erase(0, 2);
  if (hex.size() != 6 && hex.size() != 8)
    return false;
  for (const char c : hex) {
    if (!std::isxdigit(static_cast<unsigned char>(c)))
      return false;
  }
  rgba = uint32_t(std::strtoul(hex.c_str(), nullptr, 16));
  has_alpha = hex.size() == 8;
  if (!has_alpha)
    rgba = (rgba << 8) | 0xFF;
  return true;
}

void Expand565(uint16_t value, float (&c)[3]) {
  c[0] = float(value >> 11) / 31.0f;
  c[1] = float((value >> 5) & 63) / 63.0f;
  c[2] = float(value & 31) / 31.0f;
}

uint16_t Pack565(const float (&c)[3]) {
  auto quantize = [](float v, float max) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * max + 0.5f); };
  return uint16_t(quantize(c[0], 31.0f) << 11 | quantize(c[1], 63.0f) << 5 | quantize(c[2], 31.0f));
}

bool IsPanelBlue(uint16_t value) {
  const int r = value >> 11, g = (value >> 5) & 63, b = value & 31;
  return std::abs(r - kPanelRed) <= 1 && std::abs(g - kPanelGreen) <= 2 && std::abs(b - kPanelBlue) <= 1;
}

// 'c' as a * blue + w * white (a, w >= 0), least squares; the colour with the
// latched one for the blue, or 'c' as it was when it is no such mix.
uint16_t Recolour(uint16_t value) {
  static const float blue[3] = {kPanelRed / 31.0f, kPanelGreen / 63.0f, kPanelBlue / 31.0f};
  float c[3];
  Expand565(value, c);
  const float bb = blue[0] * blue[0] + blue[1] * blue[1] + blue[2] * blue[2];
  const float b1 = blue[0] + blue[1] + blue[2];
  const float bc = blue[0] * c[0] + blue[1] * c[1] + blue[2] * c[2];
  const float c1 = c[0] + c[1] + c[2];
  const float det = bb * 3.0f - b1 * b1;
  float a = (3.0f * bc - b1 * c1) / det;
  float w = (bb * c1 - b1 * bc) / det;
  if (a < 0.0f) {
    a = 0.0f;
    w = c1 / 3.0f;
  } else if (w < 0.0f) {
    w = 0.0f;
    a = bc / bb;
  }
  float out[3];
  for (int i = 0; i < 3; ++i) {
    if (std::fabs(c[i] - (a * blue[i] + w)) > kMixTolerance)
      return value;
    out[i] = a * g_latched.colour[i] + w;
  }
  return Pack565(out);
}

// Logs each size of panel once.
void NotePanel(uint32_t blocks_w, uint32_t blocks_h) {
  static std::mutex mutex;
  static std::unordered_set<uint32_t> seen;
  std::lock_guard lock(mutex);
  if (seen.insert(blocks_w << 16 | blocks_h).second)
    RGPU_INFO("Menu panel texture {}x{} recoloured", blocks_w * 4, blocks_h * 4);
}

} // namespace

void LatchMenuColourLocked() {
  const std::string &code = REXCVAR_GET(menu_background_color);
  if (code == g_code)
    return;
  g_code = code;
  Latched latched;
  uint32_t rgba = 0;
  bool has_alpha = false;
  if (!ParseColour(code, rgba, has_alpha)) {
    RGPU_WARN("menu_background_color '{}' is no colour code (#RRGGBB): the game's colour", code);
  } else if ((rgba >> 8) != 0x293463 || has_alpha) {
    latched.active = true;
    for (int i = 0; i < 3; ++i)
      latched.colour[i] = float((rgba >> (24 - 8 * i)) & 0xFF) / 255.0f;
    latched.alpha = has_alpha;
    latched.alpha_scale = float(rgba & 0xFF) / 255.0f * 15.0f / kPanelAlpha;
  }
  if (latched.active != g_latched.active ||
      std::memcmp(latched.colour, g_latched.colour, sizeof(latched.colour)) != 0 ||
      latched.alpha != g_latched.alpha || latched.alpha_scale != g_latched.alpha_scale) {
    g_latched = latched;
    ++g_generation;
  }
}

uint32_t MenuColourGeneration() {
  return g_generation;
}

void RecolourMenuPanel(uint8_t *blocks, uint32_t blocks_w, uint32_t blocks_h, uint32_t row_pitch) {
  if (!g_latched.active || blocks_w * blocks_h < kMinPanelBlocks)
    return;
  // A BC2 block: 64 bits of alpha (4 a texel), then the two 5:6:5 colours
  // and their 2-bit indices (BC2 always mixes the two, whatever their order).
  uint32_t blue = 0;
  for (uint32_t y = 0; y < blocks_h; ++y) {
    const uint8_t *row = blocks + size_t(y) * row_pitch;
    for (uint32_t x = 0; x < blocks_w; ++x) {
      uint16_t colours[2];
      std::memcpy(colours, row + size_t(x) * 16 + 8, sizeof(colours));
      blue += IsPanelBlue(colours[0]) && IsPanelBlue(colours[1]);
    }
  }
  if (blue * kPanelShareDivisor < blocks_w * blocks_h)
    return;
  NotePanel(blocks_w, blocks_h);
  for (uint32_t y = 0; y < blocks_h; ++y) {
    uint8_t *row = blocks + size_t(y) * row_pitch;
    for (uint32_t x = 0; x < blocks_w; ++x) {
      uint8_t *block = row + size_t(x) * 16;
      uint16_t colours[2];
      std::memcpy(colours, block + 8, sizeof(colours));
      colours[0] = Recolour(colours[0]);
      colours[1] = Recolour(colours[1]);
      std::memcpy(block + 8, colours, sizeof(colours));
      if (g_latched.alpha) {
        for (uint32_t i = 0; i < 8; ++i) {
          const uint32_t low = std::min(15u, uint32_t(float(block[i] & 15) * g_latched.alpha_scale + 0.5f));
          const uint32_t high = std::min(15u, uint32_t(float(block[i] >> 4) * g_latched.alpha_scale + 0.5f));
          block[i] = uint8_t(high << 4 | low);
        }
      }
    }
  }
}

} // namespace renut::gpu
