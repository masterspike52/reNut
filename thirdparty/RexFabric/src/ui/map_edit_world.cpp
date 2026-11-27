#include "map_edit_world.h"

#include <algorithm>
#include <cmath>

#include <imgui_internal.h>

#include "../content/components.h"
#include "../content/map_edits.h"
#include "../content/world_view.h"
#include "glass.h"
#include "imguizmo.h"

namespace rexfabric::ui {
namespace {

using Matrix = std::array<float, 16>;
using Point = std::array<double, 3>;

constexpr float kGizmoSize = 0.12f;  // of the picture's height, in clip space
constexpr float kMarkerRadius = 6.0f;
constexpr float kPickRadius = 11.0f;
constexpr float kCrossSize = 12.0f;
constexpr ImU32 kMarker = IM_COL32(255, 170, 60, 235);
constexpr ImU32 kSelectedMarker = IM_COL32(255, 235, 120, 255);
constexpr ImU32 kHoveredMarker = IM_COL32(255, 210, 140, 255);
constexpr ImU32 kMarkerOutline = IM_COL32(0, 0, 0, 200);
constexpr ImU32 kPlaceCross = IM_COL32(120, 230, 140, 255);
constexpr ImU32 kTextShadow = IM_COL32(0, 0, 0, 190);
// How far a ray from the pointer reaches, and the span a drop tests: from above the edit to well below it.
constexpr double kReach = 2000.0;
constexpr double kDropAbove = 50.0;
constexpr double kDropBelow = 500.0;
// A hit's ray counts as the pointer's when it starts this near the eye.
constexpr double kSameEye = 1e-3;

constexpr ImGuiWindowFlags kOverlayFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;

Matrix Multiply(const Matrix& a, const Matrix& b) {
  Matrix out{};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
  }
  return out;
}

// The inverse of a 4x4 matrix by cofactors, in doubles; none for one that has none.
std::optional<std::array<double, 16>> Inverse(const Matrix& f) {
  std::array<double, 16> m{}, inv{};
  for (int i = 0; i < 16; ++i) m[i] = f[i];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::abs(det) < 1e-12) return std::nullopt;
  for (double& x : inv) x /= det;
  return inv;
}

// Where a point of the world lands on the picture; none behind the camera.
std::optional<ImVec2> Project(const Point& point, const Matrix& view_projection, const ImVec2& origin, const ImVec2& size) {
  double clip[4];
  for (int c = 0; c < 4; ++c) clip[c] = point[0] * view_projection[c] + point[1] * view_projection[4 + c] + point[2] * view_projection[8 + c] + view_projection[12 + c];
  if (clip[3] <= 1e-6) return std::nullopt;
  return ImVec2(origin.x + static_cast<float>((clip[0] / clip[3] * 0.5 + 0.5) * size.x), origin.y + static_cast<float>((0.5 - clip[1] / clip[3] * 0.5) * size.y));
}

// The point of the world at a place on the picture and a depth of clip space.
Point Unproject(const std::array<double, 16>& inverse, double x, double y, double z) {
  double out[4];
  for (int c = 0; c < 4; ++c) out[c] = x * inverse[c] + y * inverse[4 + c] + z * inverse[8 + c] + inverse[12 + c];
  return {out[0] / out[3], out[1] / out[3], out[2] / out[3]};
}

double Distance(const Point& a, const Point& b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

// The ray from the eye through a place on the picture, reaching kReach. Clip space's depth runs either way (the game
// may draw with depth reversed), so the further of its two ends sets the direction.
std::optional<content::WorldRay> PointerRay(const content::WorldCamera& camera, const ImVec2& mouse, const ImVec2& origin, const ImVec2& size) {
  const std::optional<std::array<double, 16>> inverse = Inverse(Multiply(camera.view, camera.projection));
  if (!inverse) return std::nullopt;
  const double x = (mouse.x - origin.x) / size.x * 2.0 - 1.0;
  const double y = 1.0 - (mouse.y - origin.y) / size.y * 2.0;
  const Point a = Unproject(*inverse, x, y, 0.0), b = Unproject(*inverse, x, y, 1.0);
  const Point& far_end = Distance(a, camera.eye) > Distance(b, camera.eye) ? a : b;
  const double length = Distance(far_end, camera.eye);
  if (length < 1e-9) return std::nullopt;
  content::WorldRay ray{.from = camera.eye};
  for (int i = 0; i < 3; ++i) ray.to[i] = camera.eye[i] + (far_end[i] - camera.eye[i]) / length * kReach;
  return ray;
}

// Whether the edit has a place in the world: a spawn always, a game's kind when it says so.
bool HasPlace(const content::MapEditEntry& edit) {
  if (edit.kind == content::kSpawnEdit) return true;
  const std::shared_ptr<const content::MapEditKind> kind = content::FindMapEditKind(edit.kind);
  return kind && kind->transform;
}

// A value kept to what a file reads well: four decimals.
double Tidy(double value) {
  const double tidy = std::round(value * 10000.0) / 10000.0;
  return tidy == 0.0 ? 0.0 : tidy;
}

void Label(ImDrawList* draw, const ImVec2& at, ImU32 colour, const std::string& text) {
  draw->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), kTextShadow, text.c_str());
  draw->AddText(at, colour, text.c_str());
}

}  // namespace

void MapEditWorld::DrawToolbar(bool available, const std::string& reason, bool has_selected) {
  available_ = available;
  if (!available) Stop();
  ImGui::BeginDisabled(!available);
  if (glass::SmallButton("Edit in World", active() ? glass::kAccent : glass::kNeutral)) {
    on_ = !on_;
    if (!on_) Stop();
  }
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("%s", available ? "Markers and a gizmo for the edits over the game's picture: drag to move them, and they move in the world as you do" : reason.c_str());
  if (!active()) return;
  const auto tool = [this](const char* label, Tool which, const char* tip) {
    ImGui::SameLine();
    if (glass::SmallButton(label, tool_ == which ? glass::kAccent : glass::kNeutral)) tool_ = which;
    ImGui::SetItemTooltip("%s", tip);
  };
  tool("Move", Tool::kMove, "Move the selected edit (W)");
  tool("Turn", Tool::kTurn, "Turn the selected edit (E)");
  tool("Scale", Tool::kScale, "Scale the selected edit (R)");
  ImGui::SameLine();
  if (glass::SmallButton(local_ ? "Its axes" : "World axes")) local_ = !local_;
  ImGui::SetItemTooltip("Whether the gizmo follows the edit's own axes or the world's");
  ImGui::SameLine();
  if (glass::SmallButton("Place", placing_ ? glass::kAccent : glass::kCreate)) placing_ = !placing_;
  ImGui::SetItemTooltip("Click in the world to place a new edit there, on what's under the pointer: a spawn, or one of the game's kinds, which changes what's there; hold Shift to place more. Right click or Escape stops");
  ImGui::SameLine();
  ImGui::BeginDisabled(!has_selected || drop_ray_.has_value());
  if (glass::SmallButton("Drop")) drop_wanted_ = true;
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Put the selected edit down on the ground under it");
  ImGui::SameLine();
  if (glass::SmallButton("Ground", ground_ ? glass::kAccent : glass::kNeutral)) ground_ = !ground_;
  ImGui::SetItemTooltip("Whether an edit moved with the gizmo is put down on the ground when it's let go");
}

void MapEditWorld::Stop() {
  placing_ = false;
  held_ = false;
  drop_wanted_ = false;
  drop_ray_.reset();
  dropping_.clear();
  under_pointer_.reset();
  if (aimed_) content::AimIntoWorld({});
  aimed_ = false;
}

MapEditWorld::Result MapEditWorld::Draw(std::vector<content::MapEditEntry>& edits, std::string& selected, bool editor_focused) {
  Result result;
  // A move let go last frame ends on the ground, when that's wanted.
  if (ground_ && held_ && tool_ == Tool::kMove) drop_wanted_ = !ImGuizmo::IsUsing();
  held_ = false;
  if (!active()) return result;
  const std::optional<content::WorldCamera> camera = content::WorldCameraNow();
  if (!camera) return result;
  BeginGizmoFrame();

  ImGuiIO& io = ImGui::GetIO();
  if (editor_focused && !io.WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) tool_ = Tool::kMove;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) tool_ = Tool::kTurn;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) tool_ = Tool::kScale;
  }
  if (placing_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) placing_ = false;
  // The selected edit removed or copied: with the editor's keyboard, or with the pointer over the world.
  if (!io.WantTextInput && (editor_focused || !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))) {
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) result.remove = true;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) result.duplicate = true;
  }

  // The picture is the whole of the main viewport. Another window under the pointer has the mouse.
  const ImGuiViewport* screen = ImGui::GetMainViewport();
  const ImVec2 origin = screen->Pos, size = screen->Size;
  const bool over_ui = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
  const ImVec2 mouse = io.MousePos;

  // The rays: the pointer's, and a drop's while it's wanted.
  std::vector<content::WorldRay> rays;
  const std::optional<content::WorldRay> pointer = over_ui ? std::nullopt : PointerRay(*camera, mouse, origin, size);
  if (pointer) rays.push_back(*pointer);
  if (drop_wanted_) {
    drop_wanted_ = false;
    const auto chosen = std::find_if(edits.begin(), edits.end(), [&selected](const content::MapEditEntry& edit) { return edit.name == selected; });
    if (chosen != edits.end() && HasPlace(*chosen)) {
      const Point& at = chosen->transform.position;
      drop_ray_ = content::WorldRay{.from = {at[0], at[1] + kDropAbove, at[2]}, .to = {at[0], at[1] - kDropBelow, at[2]}};
      dropping_ = chosen->name;
    }
  }
  if (drop_ray_) rays.push_back(*drop_ray_);
  content::AimIntoWorld(rays);
  aimed_ = true;
  for (const content::WorldHit& hit : content::WorldHits()) {
    if (drop_ray_ && hit.ray == *drop_ray_) {
      const auto chosen = std::find_if(edits.begin(), edits.end(), [this](const content::MapEditEntry& edit) { return edit.name == dropping_; });
      if (hit.point && chosen != edits.end()) {
        chosen->transform.position[1] = Tidy((*hit.point)[1]);
        result.changed = true;
      }
      drop_ray_.reset();
      dropping_.clear();
    } else if (Distance(hit.ray.from, camera->eye) < kSameEye) {
      under_pointer_ = hit.point;
    }
  }
  if (!pointer) under_pointer_.reset();

  // The overlay: the whole picture, behind every window, taking no input of its own.
  ImGui::SetNextWindowPos(origin);
  ImGui::SetNextWindowSize(size);
  ImGui::Begin("##map_edit_world", nullptr, kOverlayFlags);
  ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const Matrix view_projection = Multiply(camera->view, camera->projection);

  // The markers.
  const content::MapEditEntry* under_mouse = nullptr;
  for (const content::MapEditEntry& edit : edits) {
    if (!HasPlace(edit)) continue;
    const std::optional<ImVec2> at = Project(edit.transform.position, view_projection, origin, size);
    if (!at) continue;
    const float dx = mouse.x - at->x, dy = mouse.y - at->y;
    const bool hovered = !over_ui && !placing_ && dx * dx + dy * dy <= kPickRadius * kPickRadius;
    if (hovered) under_mouse = &edit;
    const ImU32 colour = edit.name == selected ? kSelectedMarker : hovered ? kHoveredMarker : kMarker;
    draw->AddCircleFilled(*at, kMarkerRadius, colour);
    draw->AddCircle(*at, kMarkerRadius, kMarkerOutline, 0, 1.5f);
    Label(draw, ImVec2(at->x + kMarkerRadius * 2.0f, at->y - ImGui::GetTextLineHeight() * 0.5f), colour, edit.name);
  }

  // The gizmo on the selected edit.
  const auto chosen = std::find_if(edits.begin(), edits.end(), [&selected](const content::MapEditEntry& edit) { return edit.name == selected && HasPlace(edit); });
  bool over_gizmo = false;
  if (chosen != edits.end() && !placing_) {
    ImGuizmo::SetDrawlist(draw);
    ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetGizmoSizeClipSpace(kGizmoSize);
    ImGuizmo::PushID("map_edit_world");
    Matrix matrix = content::MatrixOf(chosen->transform);
    const ImGuizmo::OPERATION operation = tool_ == Tool::kMove ? ImGuizmo::TRANSLATE : tool_ == Tool::kTurn ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
    const ImGuizmo::MODE mode = local_ || tool_ == Tool::kScale ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    if (ImGuizmo::Manipulate(camera->view.data(), camera->projection.data(), operation, mode, matrix.data())) {
      content::Transform moved = content::TransformOf(matrix);
      for (int i = 0; i < 3; ++i) {
        // Only what the tool changes: the rest stays as written, rather than as read back through a matrix.
        if (tool_ != Tool::kMove) moved.position[i] = chosen->transform.position[i];
        if (tool_ != Tool::kTurn) moved.rotation[i] = chosen->transform.rotation[i];
        if (tool_ != Tool::kScale) moved.scale[i] = chosen->transform.scale[i];
        moved.position[i] = Tidy(moved.position[i]);
        moved.rotation[i] = Tidy(moved.rotation[i]);
        moved.scale[i] = Tidy(moved.scale[i]);
      }
      if (!(moved == chosen->transform)) {
        chosen->transform = moved;
        result.changed = true;
      }
    }
    held_ = ImGuizmo::IsUsing();
    over_gizmo = ImGuizmo::IsOver() || ImGuizmo::IsUsingAny();
    ImGuizmo::PopID();
  }

  // Placing: a cross where the pointer meets the world, and a click there asks for a spawn.
  bool wants_mouse = over_gizmo || held_;
  if (placing_ && !over_ui) {
    wants_mouse = true;
    if (under_pointer_) {
      if (const std::optional<ImVec2> at = Project(*under_pointer_, view_projection, origin, size)) {
        draw->AddLine(ImVec2(at->x - kCrossSize, at->y), ImVec2(at->x + kCrossSize, at->y), kPlaceCross, 2.0f);
        draw->AddLine(ImVec2(at->x, at->y - kCrossSize), ImVec2(at->x, at->y + kCrossSize), kPlaceCross, 2.0f);
        Label(draw, ImVec2(at->x + kCrossSize, at->y + kCrossSize * 0.5f), kPlaceCross, "Place here");
      }
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && under_pointer_) {
      result.place_at = *under_pointer_;
      if (!io.KeyShift) placing_ = false;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) placing_ = false;
  } else if (under_mouse && !over_gizmo) {
    wants_mouse = true;
    // What the marker is: a spawn's class, or the game's kind of edit.
    const std::string what = under_mouse->kind == content::kSpawnEdit ? "spawns " + under_mouse->spawn : under_mouse->kind;
    ImGui::SetTooltip("%s: %s", under_mouse->name.c_str(), what.c_str());
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) selected = under_mouse->name;
  }
  // The game keeps the mouse unless the overlay uses it: from the next frame, before the click that follows the hover.
  if (wants_mouse && !over_ui) ImGui::SetNextFrameWantCaptureMouse(true);
  ImGui::End();
  return result;
}

}  // namespace rexfabric::ui
