// The look of the node editor's canvas, after Toon's node graph editors: nodes tinted by what they are, that fade
// in when added and light up when hovered or selected, over a slowly shifting backdrop.
//
// With RexGlass in the game, the nodes are glass panels over Toon's aurora, as in Toon. Without it they're flat,
// tinted shapes drawn with ImGui's draw lists.
//
// One per canvas. Each frame: DrawBackdrop behind the whole window, before anything else in it, then BeginNode and
// EndNode around each node's ed::BeginNode and ed::EndNode. The backdrop fills the window, so the panels around the
// canvas sit on the same light as the nodes.

#pragma once

#include <limits>
#include <unordered_map>

#include <imgui.h>

namespace rexfabric::ui {

class NodeGraphStyle {
 public:
  // EndNode's header_bottom for a node drawn without a header band, such as a variable's Get.
  static constexpr float kNoHeader = std::numeric_limits<float>::lowest();

  // Sets the canvas style: rounding, link flow, and the transparent backgrounds the style draws over. Call once
  // with the canvas current.
  static void Apply();

  // The backdrop over [min, max] in the current window's draw list, its bottom corners rounded by `rounding` as the
  // window's are. Advances the animation; once a frame.
  void DrawBackdrop(const ImVec2& min, const ImVec2& max, float rounding);

  // Before ed::BeginNode: fades the node's contents in while it appears.
  void BeginNode(int node);
  // After ed::EndNode: the node's body and header band in `color`, its kind. `header_bottom` is the bottom of its
  // title in canvas units, or kNoHeader. A node with a problem gets a red rim.
  void EndNode(int node, const ImVec4& color, float header_bottom, bool problem);

  // True on the frame each interval elapses, for regular pulses of light along links.
  bool Pulse(float seconds) const;
  // The node is hovered or selected, so its links flow.
  bool IsNodeActive(int node) const;

 private:
  struct NodeState {
    float appear = 0.0f;  // 0 to 1, negative while waiting its turn to fade in
    float hover = 0.0f;
    float selected = 0.0f;
  };

  std::unordered_map<int, NodeState> nodes_;
  float time_ = 0.0f;
  float delta_ = 0.0f;
  int appearing_ = 0;
  // Where the aurora was drawn this frame, on screen, for glass nodes to show it through themselves.
  ImVec2 aurora_min_;
  ImVec2 aurora_max_;
};

}  // namespace rexfabric::ui
