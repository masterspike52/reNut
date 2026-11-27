// Animation keys from a hand-written JSON timeline: named poses of bone rotations and offsets,
// placed at times and eased between. Bones without keys keep the animation's own motion.
//
//   {
//     "base": "keep",                       // "keep" (default) or "rest" (bind pose)
//     "poses": {
//       "armUp": { "RT_S": { "rot": [0, 0, -75] }, "BASE": { "move": [0, 0.1, 0] } }
//     },
//     "timeline": [
//       { "time": 0.0, "pose": "armUp", "hold": 0.3 },
//       { "time": 1.0, "pose": ["a", "b"], "bones": { "RT_E": { "rot": "base" } } }
//     ]
//   }
//
// "rot" is the bone's local rotation as XYZ Euler degrees (X turns first) or an x y z w quaternion;
// it replaces the animation's rotation. "move" is the offset from the bone's rest position in
// metres, as the game stores it. "base" takes the animation's own value at that time. A bone's keys
// hold before its first and after its last; "hold" repeats an entry's keys that many seconds later.
#pragma once

#include "model.hpp"

namespace nut::model {

struct KeysReport {
    size_t bones = 0;   // bones given keys
    size_t entries = 0; // timeline entries used
    std::vector<std::string> notes;
};

// Keys for every stored frame of `a` (frames * bones), from the JSON text.
std::vector<Key> keysFromJson(const Animation& a, const std::string& text, KeysReport* report = nullptr);

} // namespace nut::model
