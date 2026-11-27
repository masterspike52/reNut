// ImGuizmo (thirdparty/imguizmo) as RexFabric includes it. ImGuizmo declares its functions with ImGui's IMGUI_API,
// which marks the SDK's ImGui functions as imported from its runtime library; ImGuizmo is linked in statically
// (rexfabric_imguizmo), so its own declarations are made plain.

#pragma once

#include <imgui.h>

#pragma push_macro("IMGUI_API")
#undef IMGUI_API
#define IMGUI_API
#include <ImGuizmo.h>
#pragma pop_macro("IMGUI_API")

namespace rexfabric::ui {

// ImGuizmo's frame, begun once per ImGui frame however many places draw a gizmo in it: beginning it again would forget
// which gizmo the pointer was over.
inline void BeginGizmoFrame() {
  static int begun = -1;
  if (begun == ImGui::GetFrameCount()) return;
  ImGuizmo::BeginFrame();
  begun = ImGui::GetFrameCount();
}

}  // namespace rexfabric::ui
