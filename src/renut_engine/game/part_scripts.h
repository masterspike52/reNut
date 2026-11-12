#pragma once

#include <rex/ppc/context.h>

#include <cstdint>
#include <optional>
#include <string>

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

// Game thread: plays 'effect' ("0x<aid>" or an asset ref name) at x, y, z in the level of 'entity' (a vehicle,
// Banjo's avatar) for 'seconds' (0: its own length). False when not loaded there; nullopt (*error) when invalid.
std::optional<bool> PlayEffect(PPCContext &ctx, uint8_t *base, uint32_t entity, const std::string &effect,
                               double x, double y, double z, double seconds, std::string *error);

} // namespace renut::game::part_scripts
