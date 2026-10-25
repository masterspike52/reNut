/**
 * @file    game/status_dials.cpp
 * @brief   Colours for the filled part of the garage's stat dials (power, ammo,
 *          fuel, weight, parts, strength) and the speedometer, set by colour
 *          code. The dial types seen: power, ammo, ammoedit, fuel, fueledit,
 *          weight, parts, strength, mapspeedo.
 *
 * N&B (TU3, IDA): the dials are XuiClass_StatusDial_c elements (EditorDisplay_c's
 * powerDial … strengthDial; sub_825B3D80, sub_825C3238). Each takes its look
 * from its DialType's 64-byte record in the statusdials asset (sub_823B4978;
 * the type's name is kept at + 228) and draws itself every frame into its own
 * target (sub_821FF250): an arc of the dial's texture in up to three pieces
 * (sub_82200770), the filled part, the step past it and the rest, with pixel
 * shader constant c0 (device + 0x1780) multiplying what it draws: 1 a
 * channel, more while the dial flashes (0x821FF738..48 store it, 0x821FF760
 * draws). The colour multiplies c0 there, tinting the dial's fill (the clear
 * rest stays clear). The pieces' vertex colours don't work for this: the
 * shader doesn't take them as a tint (red came out light blue, or white).
 */
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include "renut_engine/app/renut_logging.h"

REXCVAR_DEFINE_STRING(dial_fill_color_power, "#FFFFFF", "Nuts&Bolts/Garage",
                      "Colour of the power dial's fill, a colour code like #FF0000 (red); #FFFFFF "
                      "leaves the game's");
REXCVAR_DEFINE_STRING(dial_fill_color_ammo, "#FFFFFF", "Nuts&Bolts/Garage",
                      "Colour of the ammo dial's fill, a colour code like #FF0000 (red); #FFFFFF "
                      "leaves the game's");
REXCVAR_DEFINE_STRING(dial_fill_color_fuel, "#FFFFFF", "Nuts&Bolts/Garage",
                      "Colour of the fuel dial's fill, a colour code like #FF0000 (red); #FFFFFF "
                      "leaves the game's");
REXCVAR_DEFINE_STRING(dial_fill_color_weight, "#FFFFFF", "Nuts&Bolts/Garage",
                      "Colour of the weight dial's fill, a colour code like #FF0000 (red); #FFFFFF "
                      "leaves the game's");
REXCVAR_DEFINE_STRING(dial_fill_color_parts, "#FFFFFF", "Nuts&Bolts/Garage",
                      "Colour of the parts (capacity) dial's fill, a colour code like #FF0000 (red); "
                      "#FFFFFF leaves the game's");
REXCVAR_DEFINE_STRING(dial_fill_color_strength, "#FFFFFF", "Nuts&Bolts/Garage",
                      "Colour of the strength dial's fill, a colour code like #FF0000 (red); #FFFFFF "
                      "leaves the game's");
REXCVAR_DEFINE_STRING(dial_fill_color_speed, "#FFFFFF", "Nuts&Bolts/Garage",
                      "Colour of the speedometer's fill (driving), a colour code like #FF0000 (red); "
                      "#FFFFFF leaves the game's");

namespace {

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
const std::string *ColourFor(const std::string &type) {
  if (type.find("speedo") != std::string::npos)
    return &REXCVAR_GET(dial_fill_color_speed);
  if (type.find("power") != std::string::npos)
    return &REXCVAR_GET(dial_fill_color_power);
  if (type.find("ammo") != std::string::npos)
    return &REXCVAR_GET(dial_fill_color_ammo);
  if (type.find("fuel") != std::string::npos)
    return &REXCVAR_GET(dial_fill_color_fuel);
  if (type.find("weight") != std::string::npos)
    return &REXCVAR_GET(dial_fill_color_weight);
  if (type.find("part") != std::string::npos)
    return &REXCVAR_GET(dial_fill_color_parts);
  if (type.find("strength") != std::string::npos || type.find("armour") != std::string::npos ||
      type.find("health") != std::string::npos)
    return &REXCVAR_GET(dial_fill_color_strength);
  return nullptr;
}

// Logs each dial type once, so a type the names above miss shows up.
void NoteType(const std::string &type, bool matched) {
  static std::mutex mutex;
  static std::unordered_set<std::string> seen;
  std::lock_guard lock(mutex);
  if (seen.insert(type).second)
    RNUT_INFO("Status dial type '{}'{}", type, matched ? "" : " (no fill colour setting)");
}

}  // namespace

// sub_821FF250 at 0x821FF758, before the arc's draw: c0 is stored and marked
// for upload; r9 is the device, r30 the dial.
void StatusDial_fill_colour(PPCRegister &r9, PPCRegister &r30) {
  auto *memory = REX_KERNEL_MEMORY();
  const char *name = memory->TranslateVirtual<const char *>(r30.u32 + kDialTypeName);
  std::string type(name, strnlen(name, kDialTypeNameSize));
  for (char &c : type)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  const std::string *code = ColourFor(type);
  NoteType(type, code != nullptr);
  float rgba[4];
  if (!code || !ParseColour(*code, rgba))
    return;
  auto *c0 = memory->TranslateVirtual<uint32_t *>(r9.u32 + kPixelConstant0);
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
