#pragma once

namespace renut::game {

// Any thread: a mod's vertical field of view in degrees (renut.camera.set_fov), in place of the game's from the
// next frame; 0 gives the game's (and renut_field_of_view's) back.
void SetFieldOfView(float degrees);

}  // namespace renut::game
