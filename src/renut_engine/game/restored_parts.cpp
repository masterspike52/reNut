/**
 * @file    game/restored_parts.cpp
 * @brief   The game's own parts reNut gives the player from the start.
 *
 * N&B (TU3, IDA, the bundles): the Remote Control is a whole part no crate,
 * blockset, shop or unlock list gives (only the end-of-game flag list
 * aid_misc_banjox_flaglist_spiral_endofgame names it):
 * aid_objparams_banjox_vehicleblock_gadgets_remotecontrol (category
 * accessory), its model, collision, garage icon, description and text
 * (block__remotecontrol), and its class in the class table beside the
 * Stereo's (0x821017A8 vehicleBlockRemoteControl; parent 0x10C). The
 * Autopilot (_autopilot, 0x82101790) is the same; scrapped by the user. The
 * Stereo is a Jinjo Bingo prize (crate 11, Humba crate 6), given early for
 * its music (stereo.cpp).
 */
#include "renut_engine/game/restored_parts.h"

#include <rex/cvar.h>

#include "renut_engine/mods/custom_parts.h"

REXCVAR_DEFINE_BOOL(renut_cut_parts, true, "Nuts&Bolts/Vehicles",
                    "Gives you the Remote Control, a part the game has but never gives out.");
REXCVAR_DECLARE(bool, renut_stereo);

namespace renut::game {

std::vector<RestoredPart> RestoredParts() {
  constexpr uint8_t kTypeObjparams = 0x1F;
  std::vector<RestoredPart> parts;
  if (REXCVAR_GET(renut_stereo))
    parts.push_back({renut::mods::AssetId(kTypeObjparams, "banjox_vehicleblock_gadgets_stereo"), 1});
  if (REXCVAR_GET(renut_cut_parts))
    parts.push_back({renut::mods::AssetId(kTypeObjparams, "banjox_vehicleblock_gadgets_remotecontrol"), 1});
  return parts;
}

}  // namespace renut::game
