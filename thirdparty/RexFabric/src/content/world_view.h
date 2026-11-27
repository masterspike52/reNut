// The game's view of the world it's in (rexfabric/content.h, SetWorldView), as the map edit editor reaches it to edit
// a world in place (docs/content_and_classes.md, Editing in the world).

#pragma once

#include <optional>
#include <vector>

#include <rexfabric/content.h>

namespace rexfabric::content {

// Whether the game registered a view of its world.
bool HasWorldView();

// The camera of the picture shown now; none without a view or while no world is shown. Any thread.
std::optional<WorldCamera> WorldCameraNow();

// The rays to hit test, replacing the ones before; nothing without a view. Any thread.
void AimIntoWorld(std::vector<WorldRay> rays);

// What the rays last tested hit, each with its ray. Any thread.
std::vector<WorldHit> WorldHits();

}  // namespace rexfabric::content
