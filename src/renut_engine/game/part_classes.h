#pragma once

#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>

#include <cstdint>
#include <string>

namespace renut::mods {
struct CustomPart;
}

namespace renut::game {

// Game thread, from a hook: registers (once) part class 'name' deriving from
// 'parent', with callbacks reaching 'part''s behaviour; its id, or 0 on failure.
uint32_t RegisterScriptedPartClass(PPCContext &ctx, uint8_t *base, const std::string &name,
                                   const std::string &parent, const renut::mods::CustomPart *part);

// Whether game object 'instance' is of class 'class_name' or one deriving from it.
bool InstanceIsA(const uint8_t *base, uint32_t instance, const std::string &class_name);
// Its class's name ("vehicleBlockEngine"), or empty.
std::string InstanceClassName(const uint8_t *base, uint32_t instance);
// Whether 'instance' is a part added by renut.parts.add with a behaviour (it has callbacks).
bool IsScriptedPart(const uint8_t *base, uint32_t instance);

// The recompiled function at guest address 'va', or null when there's none.
::PPCFunc *GuestFunction(uint32_t va);
// A guest address whose calls run 'fn' (a thunk in the executable's pool), or 0 when none is left.
uint32_t AllocateGuestThunk(::PPCFunc *fn);

} // namespace renut::game
