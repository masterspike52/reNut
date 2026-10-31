// Button and key icons for the on-screen keyboard: Kenney's Input Prompts
// (CC0, assets/keys/LICENSE.txt), 128 px. Generated from the icon list in
// keyboard.cpp; add a file to assets/keys and a line here for another.

#include "key_icons.h"

#include <cstring>
#include <iterator>


namespace rexglue_guide::assets {
namespace {

constexpr uint8_t k_xbox_button_color_a[] = {
#include "keys/xbox_button_color_a.png.inc"
};
constexpr uint8_t k_xbox_button_color_b[] = {
#include "keys/xbox_button_color_b.png.inc"
};
constexpr uint8_t k_xbox_button_color_x[] = {
#include "keys/xbox_button_color_x.png.inc"
};
constexpr uint8_t k_xbox_button_color_y[] = {
#include "keys/xbox_button_color_y.png.inc"
};
constexpr uint8_t k_xbox_lb[] = {
#include "keys/xbox_lb.png.inc"
};
constexpr uint8_t k_xbox_rb[] = {
#include "keys/xbox_rb.png.inc"
};
constexpr uint8_t k_xbox_lt[] = {
#include "keys/xbox_lt.png.inc"
};
constexpr uint8_t k_xbox_rt[] = {
#include "keys/xbox_rt.png.inc"
};
constexpr uint8_t k_xbox_stick_l_press[] = {
#include "keys/xbox_stick_l_press.png.inc"
};
constexpr uint8_t k_xbox_button_start[] = {
#include "keys/xbox_button_start.png.inc"
};
constexpr uint8_t k_xbox_button_menu[] = {
#include "keys/xbox_button_menu.png.inc"
};
constexpr uint8_t k_playstation_button_color_cross[] = {
#include "keys/playstation_button_color_cross.png.inc"
};
constexpr uint8_t k_playstation_button_color_circle[] = {
#include "keys/playstation_button_color_circle.png.inc"
};
constexpr uint8_t k_playstation_button_color_square[] = {
#include "keys/playstation_button_color_square.png.inc"
};
constexpr uint8_t k_playstation_button_color_triangle[] = {
#include "keys/playstation_button_color_triangle.png.inc"
};
constexpr uint8_t k_playstation_trigger_l1[] = {
#include "keys/playstation_trigger_l1.png.inc"
};
constexpr uint8_t k_playstation_trigger_r1[] = {
#include "keys/playstation_trigger_r1.png.inc"
};
constexpr uint8_t k_playstation_trigger_l2[] = {
#include "keys/playstation_trigger_l2.png.inc"
};
constexpr uint8_t k_playstation_trigger_r2[] = {
#include "keys/playstation_trigger_r2.png.inc"
};
constexpr uint8_t k_playstation_button_l3[] = {
#include "keys/playstation_button_l3.png.inc"
};
constexpr uint8_t k_playstation3_button_start[] = {
#include "keys/playstation3_button_start.png.inc"
};
constexpr uint8_t k_playstation4_button_options[] = {
#include "keys/playstation4_button_options.png.inc"
};
constexpr uint8_t k_playstation5_button_options[] = {
#include "keys/playstation5_button_options.png.inc"
};
constexpr uint8_t k_switch_button_a[] = {
#include "keys/switch_button_a.png.inc"
};
constexpr uint8_t k_switch_button_b[] = {
#include "keys/switch_button_b.png.inc"
};
constexpr uint8_t k_switch_button_x[] = {
#include "keys/switch_button_x.png.inc"
};
constexpr uint8_t k_switch_button_y[] = {
#include "keys/switch_button_y.png.inc"
};
constexpr uint8_t k_switch_button_l[] = {
#include "keys/switch_button_l.png.inc"
};
constexpr uint8_t k_switch_button_r[] = {
#include "keys/switch_button_r.png.inc"
};
constexpr uint8_t k_switch_button_zl[] = {
#include "keys/switch_button_zl.png.inc"
};
constexpr uint8_t k_switch_button_zr[] = {
#include "keys/switch_button_zr.png.inc"
};
constexpr uint8_t k_switch_stick_l_press[] = {
#include "keys/switch_stick_l_press.png.inc"
};
constexpr uint8_t k_switch_button_plus[] = {
#include "keys/switch_button_plus.png.inc"
};
constexpr uint8_t k_steamdeck_button_a[] = {
#include "keys/steamdeck_button_a.png.inc"
};
constexpr uint8_t k_steamdeck_button_b[] = {
#include "keys/steamdeck_button_b.png.inc"
};
constexpr uint8_t k_steamdeck_button_x[] = {
#include "keys/steamdeck_button_x.png.inc"
};
constexpr uint8_t k_steamdeck_button_y[] = {
#include "keys/steamdeck_button_y.png.inc"
};
constexpr uint8_t k_steamdeck_button_l1[] = {
#include "keys/steamdeck_button_l1.png.inc"
};
constexpr uint8_t k_steamdeck_button_r1[] = {
#include "keys/steamdeck_button_r1.png.inc"
};
constexpr uint8_t k_steamdeck_button_l2[] = {
#include "keys/steamdeck_button_l2.png.inc"
};
constexpr uint8_t k_steamdeck_button_r2[] = {
#include "keys/steamdeck_button_r2.png.inc"
};
constexpr uint8_t k_steamdeck_stick_l_press[] = {
#include "keys/steamdeck_stick_l_press.png.inc"
};
constexpr uint8_t k_steamdeck_button_options[] = {
#include "keys/steamdeck_button_options.png.inc"
};
constexpr uint8_t k_keyboard_arrow_left[] = {
#include "keys/keyboard_arrow_left.png.inc"
};
constexpr uint8_t k_keyboard_arrow_right[] = {
#include "keys/keyboard_arrow_right.png.inc"
};
constexpr uint8_t k_keyboard_tab_icon[] = {
#include "keys/keyboard_tab_icon.png.inc"
};
constexpr uint8_t k_keyboard_shift_icon[] = {
#include "keys/keyboard_shift_icon.png.inc"
};
constexpr uint8_t k_keyboard_capslock_icon[] = {
#include "keys/keyboard_capslock_icon.png.inc"
};
constexpr uint8_t k_keyboard_backspace_icon[] = {
#include "keys/keyboard_backspace_icon.png.inc"
};
constexpr uint8_t k_keyboard_space_icon[] = {
#include "keys/keyboard_space_icon.png.inc"
};
constexpr uint8_t k_keyboard_enter[] = {
#include "keys/keyboard_enter.png.inc"
};
constexpr uint8_t k_mouse_left[] = {
#include "keys/mouse_left.png.inc"
};
constexpr uint8_t k_keyboard_escape[] = {
#include "keys/keyboard_escape.png.inc"
};

struct KeyIconFile {
  const char* name;
  std::span<const uint8_t> png;
};
constexpr KeyIconFile kKeyIcons[] = {
    {"xbox_button_color_a", k_xbox_button_color_a},
    {"xbox_button_color_b", k_xbox_button_color_b},
    {"xbox_button_color_x", k_xbox_button_color_x},
    {"xbox_button_color_y", k_xbox_button_color_y},
    {"xbox_lb", k_xbox_lb},
    {"xbox_rb", k_xbox_rb},
    {"xbox_lt", k_xbox_lt},
    {"xbox_rt", k_xbox_rt},
    {"xbox_stick_l_press", k_xbox_stick_l_press},
    {"xbox_button_start", k_xbox_button_start},
    {"xbox_button_menu", k_xbox_button_menu},
    {"playstation_button_color_cross", k_playstation_button_color_cross},
    {"playstation_button_color_circle", k_playstation_button_color_circle},
    {"playstation_button_color_square", k_playstation_button_color_square},
    {"playstation_button_color_triangle", k_playstation_button_color_triangle},
    {"playstation_trigger_l1", k_playstation_trigger_l1},
    {"playstation_trigger_r1", k_playstation_trigger_r1},
    {"playstation_trigger_l2", k_playstation_trigger_l2},
    {"playstation_trigger_r2", k_playstation_trigger_r2},
    {"playstation_button_l3", k_playstation_button_l3},
    {"playstation3_button_start", k_playstation3_button_start},
    {"playstation4_button_options", k_playstation4_button_options},
    {"playstation5_button_options", k_playstation5_button_options},
    {"switch_button_a", k_switch_button_a},
    {"switch_button_b", k_switch_button_b},
    {"switch_button_x", k_switch_button_x},
    {"switch_button_y", k_switch_button_y},
    {"switch_button_l", k_switch_button_l},
    {"switch_button_r", k_switch_button_r},
    {"switch_button_zl", k_switch_button_zl},
    {"switch_button_zr", k_switch_button_zr},
    {"switch_stick_l_press", k_switch_stick_l_press},
    {"switch_button_plus", k_switch_button_plus},
    {"steamdeck_button_a", k_steamdeck_button_a},
    {"steamdeck_button_b", k_steamdeck_button_b},
    {"steamdeck_button_x", k_steamdeck_button_x},
    {"steamdeck_button_y", k_steamdeck_button_y},
    {"steamdeck_button_l1", k_steamdeck_button_l1},
    {"steamdeck_button_r1", k_steamdeck_button_r1},
    {"steamdeck_button_l2", k_steamdeck_button_l2},
    {"steamdeck_button_r2", k_steamdeck_button_r2},
    {"steamdeck_stick_l_press", k_steamdeck_stick_l_press},
    {"steamdeck_button_options", k_steamdeck_button_options},
    {"keyboard_arrow_left", k_keyboard_arrow_left},
    {"keyboard_arrow_right", k_keyboard_arrow_right},
    {"keyboard_tab_icon", k_keyboard_tab_icon},
    {"keyboard_shift_icon", k_keyboard_shift_icon},
    {"keyboard_capslock_icon", k_keyboard_capslock_icon},
    {"keyboard_backspace_icon", k_keyboard_backspace_icon},
    {"keyboard_space_icon", k_keyboard_space_icon},
    {"keyboard_enter", k_keyboard_enter},
    {"mouse_left", k_mouse_left},
    {"keyboard_escape", k_keyboard_escape},
};

}  // namespace

std::span<const uint8_t> KeyIconPng(std::string_view name) {
  for (const KeyIconFile& icon : kKeyIcons) {
    if (name == icon.name) return icon.png;
  }
  return {};
}

}  // namespace rexglue_guide::assets

