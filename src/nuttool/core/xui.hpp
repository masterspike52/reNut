// XUI menus (aid_xuipackage_* symbols): an "XUIZ" package of binary XUIB version 5 scenes, as the
// game's loader reads them (0x82A45F68).
//
// XUIZ (big-endian): "XUIZ", u32 1, u32 package size, u32 1, u32 directory size, u16 scene count,
//   then per scene {u32 size, u32 offset from the end of the directory, u8 name length, UTF-16 name}.
// XUIB: "XUIB", u32 5, u32 flags, u16 12, u32 file size, u16 section count, [flags & 1: 10 u32 counts
//   that size the loader's memory arena; counts[1] is one 16-byte property record each], then
//   {4CC, offset, size} per section: STRN (u16 length + UTF-16 strings, referenced 1-based), VECT
//   (3 floats, 0-based), QUAT, CUST, DATA, DBUG.
// DATA is the element tree: u16 class name, u8 flags (1 properties, 2 children, 4 timelines),
//   [u16 property count, then one property block per class from the base class down], [u32 child
//   count, children], [timelines]. A property block is a byte (low 3 bits: mask size 0/1/2/4; upper
//   bits: properties present - 1), the big-endian mask (bit i = property i), then the present values:
//   bool 1 byte, string u16, int/unsigned/float/color/vector/quaternion/custom 4 bytes, object u16
//   count + its own block; indexed properties start with a packed count.
//
// XuiImage draws its texture at the texture's own pixel size inside its box unless SizeMode stretches
// it (bits: 1 box takes the texture size, 2 centre, 4 stretch, 8/16 stretch keeping the aspect;
// 0x82A35048). A XuiFigure texture fill (0x821C1580) maps position * rotation * Scale / texture
// size, centred on the box, plus Translation in box spans when its flags word has bits 8-9 set (the
// fill's last property; every button in the game's menus, 0x821C2B20), and otherwise
// ((position - box/2) * Scale + box/2) / texture size + Translation (0x82A312C8). Both depend on
// the texture's size in pixels.
#pragma once

#include "common.hpp"

#include <map>

namespace nut::xui {

struct Resize {
    uint32_t fromW = 0, fromH = 0, toW = 0, toH = 0;
};

bool isPackage(const Bytes& d);

// Makes the elements that draw a resized texture cover the same area as they did at its original
// size: XuiImage elements whose box matches the original size are set to stretch, and XuiFigure
// texture fills get a matching scale and offset. `resized` is keyed by lower-case texture asset name
// (aid_texture_...). Elements that cannot be adjusted are reported in `log`. Returns the package
// (unchanged when no element uses those textures); `changed` counts the elements adjusted.
Bytes adaptPackage(const Bytes& xuiz, const std::map<std::string, Resize>& resized, std::vector<std::string>& log, int* changed = nullptr);

} // namespace nut::xui
