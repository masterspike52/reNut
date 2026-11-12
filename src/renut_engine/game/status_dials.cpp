#include <rex/kernel/memory_view.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/context.h>
#include <rex/setting.h>
#include <rex/types.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include "renut_engine/app/renut_settings.h"

namespace {

struct StatusDialSettings final : rex::SettingOwner {
  StatusDialSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
  REX_SETTING(std::string, dial_fill_color_power, "#FFFFFF",
              "Colour of the power dial's fill, a colour code like #FF0000 (red); #FFFFFF "
              "leaves the game's");
  REX_SETTING(std::string, dial_fill_color_ammo, "#FFFFFF",
              "Colour of the ammo dial's fill, a colour code like #FF0000 (red); #FFFFFF "
              "leaves the game's");
  REX_SETTING(std::string, dial_fill_color_fuel, "#FFFFFF",
              "Colour of the fuel dial's fill, a colour code like #FF0000 (red); #FFFFFF "
              "leaves the game's");
  REX_SETTING(std::string, dial_fill_color_weight, "#FFFFFF",
              "Colour of the weight dial's fill, a colour code like #FF0000 (red); #FFFFFF "
              "leaves the game's");
  REX_SETTING(std::string, dial_fill_color_parts, "#FFFFFF",
              "Colour of the parts (capacity) dial's fill, a colour code like #FF0000 (red); "
              "#FFFFFF leaves the game's");
  REX_SETTING(std::string, dial_fill_color_strength, "#FFFFFF",
              "Colour of the strength dial's fill, a colour code like #FF0000 (red); #FFFFFF "
              "leaves the game's");
  REX_SETTING(std::string, dial_fill_color_speed, "#FFFFFF",
              "Colour of the speedometer's fill (driving), a colour code like #FF0000 (red); "
              "#FFFFFF leaves the game's");
};
StatusDialSettings &g_dials = *new StatusDialSettings();  // never freed, registered by RenutApp::OnConfigure

// The dial's DialType name (statusdials record name), 16 bytes at + 228.
constexpr uint32_t kDialTypeName = 228;
constexpr size_t kDialTypeNameSize = 16;

// The pixel shader constant c0, four big-endian floats.
constexpr uint32_t kPixelConstant0 = 0x1780;

// A colour code as red, green, blue and alpha from 0 to 1: #RRGGBB (opaque)
// or #RRGGBBAA, '#' or "0x" optional. False when it isn't one.
bool ParseColour(const std::string &code, float (&rgba)[4]) {
  std::string hex = code;
  if (!hex.empty() && hex[0] == '#')
    hex.erase(0, 1);
  else if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
    hex.erase(0, 2);
  if (hex.size() != 6 && hex.size() != 8)
    return false;
  uint32_t value = 0;
  for (const char c : hex) {
    if (!std::isxdigit(static_cast<unsigned char>(c)))
      return false;
    value = value * 16 + uint32_t(std::isdigit(static_cast<unsigned char>(c))
                                      ? c - '0'
                                      : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
  }
  if (hex.size() == 6)
    value = (value << 8) | 0xFF;
  for (int i = 0; i < 4; ++i)
    rgba[i] = float((value >> (24 - 8 * i)) & 0xFF) / 255.0f;
  return true;
}

// The colour code set for a dial type, or nothing for a type of no stat here.
std::optional<std::string> ColourFor(const std::string &type) {
  if (type.find("speedo") != std::string::npos)
    return g_dials.dial_fill_color_speed.Get();
  if (type.find("power") != std::string::npos)
    return g_dials.dial_fill_color_power.Get();
  if (type.find("ammo") != std::string::npos)
    return g_dials.dial_fill_color_ammo.Get();
  if (type.find("fuel") != std::string::npos)
    return g_dials.dial_fill_color_fuel.Get();
  if (type.find("weight") != std::string::npos)
    return g_dials.dial_fill_color_weight.Get();
  if (type.find("part") != std::string::npos)
    return g_dials.dial_fill_color_parts.Get();
  if (type.find("strength") != std::string::npos || type.find("armour") != std::string::npos ||
      type.find("health") != std::string::npos)
    return g_dials.dial_fill_color_strength.Get();
  return std::nullopt;
}

}  // namespace

// sub_821FF250 at 0x821FF758, before the arc's draw: c0 is stored and marked
// for upload; r9 is the device, r30 the dial.
void StatusDial_fill_colour(PPCRegister &r9, PPCRegister &r30) {
  const rex::kernel::MemoryView memory = REX_GUEST_MEMORY()->GetView();
  const char *name = memory.TranslateVirtual<const char *>(r30.u32 + kDialTypeName);
  std::string type(name, strnlen(name, kDialTypeNameSize));
  for (char &c : type)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  const std::optional<std::string> code = ColourFor(type);
  float rgba[4];
  if (!code || !ParseColour(*code, rgba))
    return;
  auto *c0 = memory.TranslateVirtual<uint32_t *>(r9.u32 + kPixelConstant0);
  for (int i = 0; i < 4; ++i) {
    float value;
    const uint32_t bits = rex::byte_swap(c0[i]);
    std::memcpy(&value, &bits, 4);
    value *= rgba[i];
    uint32_t out;
    std::memcpy(&out, &value, 4);
    c0[i] = rex::byte_swap(out);
  }
}
