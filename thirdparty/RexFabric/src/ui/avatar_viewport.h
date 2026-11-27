// The Avatar editor's viewport (docs/content_and_classes.md, The viewport): one avatar of the class, drawn by the game's
// viewport renderer (rexfabric/content.h), with the class's components over it. Each component with a place is a
// marker, its name beside it; the selected one of the class's own has ImGuizmo's gizmo, which moves, turns or scales
// it, its transform changing as it's dragged.
//
//   drag            go round the avatar (left or right button, away from the gizmo)
//   wheel           nearer and further
//   click a marker  select its component
//   W, E, R         move, turn, scale, while the pointer is over it
//   Play, Replay    its time running or held, and everything from the start (an emitter's play)
//
// The game is given the components too, with their places and values, to show what they do. The game draws with its
// own camera and says how it looks at the avatar, so the gizmos line up with what it drew. With
// no renderer, or one that can't say, the viewport keeps a camera of its own over a plain background and a grid.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <imgui.h>
#include <rexfabric/content.h>

#include "../content/components.h"

namespace rexfabric::ui {

class AvatarViewport {
 public:
  AvatarViewport();
  ~AvatarViewport();
  AvatarViewport(const AvatarViewport&) = delete;
  AvatarViewport& operator=(const AvatarViewport&) = delete;

  // Draws it into `size`: the game's image of `view` (its camera filled in here), the inherited components fixed, and
  // `own` movable. `selected` names the component with the gizmo, empty for none, and changes when a marker is clicked.
  // True when the gizmo changed one of `own`.
  bool Draw(const ImVec2& size, content::ViewportView view, const std::vector<content::Component>& inherited, std::vector<content::Component>& own, std::string& selected);
  // The gizmo is being dragged: what it changes is one edit, done when it's let go.
  bool held() const { return held_; }

 private:
  enum class Tool { kMove, kTurn, kScale };

  void DrawToolbar();
  void TakeCamera(bool hovered, bool dragging);

  uint64_t id_;
  double yaw_;
  double pitch_;
  double zoom_;
  Tool tool_ = Tool::kMove;
  bool local_ = true;      // the gizmo along the component's own axes, not the avatar's
  bool dragging_ = false;  // the camera is being dragged round
  bool held_ = false;
  bool playing_ = true;
  uint32_t replays_ = 0;
  // Its own camera's framing (with no camera from the game): the middle of the components and how far they reach.
  std::array<float, 3> own_centre_{};
  float own_radius_ = 1.0f;
};

}  // namespace rexfabric::ui
