#include "renut_engine/game/restored_parts.h"

#include "renut_engine/mods/custom_parts.h"

namespace renut::game {

std::vector<RestoredPart> RestoredParts() {
  constexpr uint8_t kTypeObjparams = 0x1F;
  return {
      {renut::mods::AssetId(kTypeObjparams, "banjox_vehicleblock_gadgets_stereo"), 1},
      {renut::mods::AssetId(kTypeObjparams, "banjox_vehicleblock_gadgets_remotecontrol"), 1},
  };
}

}  // namespace renut::game
