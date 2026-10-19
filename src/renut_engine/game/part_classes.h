/**
 * @file    game/part_classes.h
 * @brief   New vehicle part classes, whose behaviour runs in reNut (part_classes.cpp).
 */
#pragma once

#include <rex/ppc/context.h>

#include <cstdint>
#include <string>

namespace renut::mods {
struct CustomPart;
}

namespace renut::game {

// Game thread, from a hook (its guest context): the part class 'name'
// deriving from the class named 'parent', whose callbacks reach 'part''s
// behaviour. Registered once; later calls return the same class. Its id, or 0
// (logged) when it can't be made.
uint32_t RegisterScriptedPartClass(PPCContext &ctx, uint8_t *base, const std::string &name,
                                   const std::string &parent, const renut::mods::CustomPart *part);

} // namespace renut::game
