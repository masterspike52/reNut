// Editing a map edit in the world itself (docs/content_and_classes.md, Editing in the world): over the game's picture
// of the world it changes, each edit with a place is a marker where it is, its name beside it, and the selected one has
// ImGuizmo's gizmo, which moves, turns or scales it there. The world shows the edits as the editor has them (map
// edits' previews), so a drag moves the avatar as it goes.
//
// It needs the game's view of its world (rexfabric/content.h, SetWorldView): the camera its picture is drawn with,
// and hit tests of rays into it, so a spawn can be placed on what's under the pointer and dropped to the ground.
//
//   Edit in World   the markers and the gizmo over the game's picture, while the game is in the map edit's world
//   Move, Turn, Scale (W, E, R while the editor has the keyboard), and along the edit's axes or the world's
//   Place           the next click in the world places a new edit there: a spawn of a class, or one of the game's
//                   kinds, which then changes what's there (right click or Escape stops)
//   Drop            the selected edit put down on the ground under it
//   Ground          a moved edit put down on the ground when the gizmo is let go
//   Delete, Ctrl+D  the selected edit removed, or copied beside it
//
// The overlay takes the mouse only where it's used: over a marker or the gizmo, and everywhere while placing, so the
// game keeps its own clicks and camera the rest of the time.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "../content/assets.h"

namespace rexfabric::ui {

class MapEditWorld {
 public:
  // What the overlay did this frame.
  struct Result {
    bool changed = false;  // an edit's transform changed
    std::optional<std::array<double, 3>> place_at;  // a click while placing: a new edit is wanted there
    bool remove = false;     // Delete: the selected edit is to go
    bool duplicate = false;  // Ctrl+D: the selected edit is to be copied
  };

  // The tools, in the editor's window. `available` says whether the game is in the map edit's world with a view of it;
  // `reason` why not.
  void DrawToolbar(bool available, const std::string& reason, bool has_selected);

  // Over the game's picture, once a frame while the editor is drawn: the markers, the gizmo, placing and dropping.
  // `selected` is the edit's name, changed by a click on a marker. Nothing while it's off or not available.
  Result Draw(std::vector<content::MapEditEntry>& edits, std::string& selected, bool editor_focused);

  bool active() const { return on_ && available_; }
  bool placing() const { return active() && placing_; }
  // A gizmo is being dragged: the edit is moving, and the editor keeps its history still until it's let go.
  bool held() const { return held_; }
  // Stops everything and lets go of the rays it aimed.
  void Stop();

 private:
  enum class Tool { kMove, kTurn, kScale };

  bool on_ = false;
  bool available_ = false;
  bool placing_ = false;
  bool local_ = true;
  bool held_ = false;
  bool ground_ = false;  // a move ends on the ground
  Tool tool_ = Tool::kMove;
  // A drop asked for: the edit and the ray straight down through it, until its hit comes back.
  std::string dropping_;
  std::optional<content::WorldRay> drop_ray_;
  bool drop_wanted_ = false;
  // The last point under the pointer, as the game found it.
  std::optional<std::array<double, 3>> under_pointer_;
  bool aimed_ = false;
};

}  // namespace rexfabric::ui
