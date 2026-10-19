/**
 * @file    game/part_scripts.h
 * @brief   Lua scripts of mods' new kinds of parts (part_scripts.cpp).
 *
 * A script (a part's "script" in its .nbpatch) runs in a sandbox of its own:
 * Lua's base functions without file or code loading, math, string, table,
 * utf8, and reNut's API:
 *   log(...)                      writes to reNut's log
 *   vec(x, y, z)                  a vector: .x .y .z, + - * /, :length(),
 *                                 :normalized(), :dot(v), :cross(v)
 *   params                        the part's "params" from its JSON
 * and defines any of these events, each given the part (a table the script
 * may keep its own fields in, besides .params and the methods below):
 *   on_spawn(part)                the part was made
 *   on_attach(part)               it was built onto a vehicle
 *   on_tick(part, dt)             every frame on a vehicle (dt in seconds)
 *   on_button_down(part)          its action button was pressed
 *   on_button_up(part)            ... and let go
 *   on_destroy(part)              it is gone
 * part:vehicle() is the vehicle it is on (nil when none), with:
 *   v:position(), v:set_position(p), v:move(d)        (a teleport)
 *   v:velocity(), v:set_velocity(v)                   (units / second)
 *   v:angular_velocity(), v:set_angular_velocity(v)   (radians / second)
 *   v:forward(), v:up(), v:right()                    (unit vectors)
 * and the game's particle effects:
 *   fx.play(effect, position) one of the game's effects there: its name or
 *                             its asset id ("0x4B32520C"), as
 *                             renut_log_effects lists them while the game
 *                             plays them. Needs the part on a vehicle (the
 *                             effect belongs to it); an effect the current
 *                             level hasn't loaded is skipped. True if played.
 * Errors are logged with the script's name; a script failing too often is
 * stopped. Each call may run a bounded number of instructions.
 */
#pragma once

#include <rex/ppc/context.h>

#include <cstdint>

namespace renut::mods {
struct CustomPart;
}

namespace renut::game::part_scripts {

// Game thread, from the part class's callbacks (their guest context: the API
// calls game functions through it; base: guest memory).
using renut::mods::CustomPart;
void Spawn(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance);
void Destroy(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance);
void Attach(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle);
void Tick(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle, double dt);
void Button(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle,
            bool down);

} // namespace renut::game::part_scripts
