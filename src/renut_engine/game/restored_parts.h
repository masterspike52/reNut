#pragma once

#include <cstdint>
#include <vector>

namespace renut::game {

// A part owned from the start, 'count' of it, from the garage inventory's
// build (custom_parts.cpp adds it).
struct RestoredPart {
  uint32_t aid = 0;  // its objparams' asset id
  uint32_t count = 0;
};

// Any thread.
std::vector<RestoredPart> RestoredParts();

}  // namespace renut::game
