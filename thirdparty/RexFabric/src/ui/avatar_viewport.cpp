#include "avatar_viewport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <numbers>
#include <optional>

#include "../content/values.h"
#include "../content/viewport.h"
#include "glass.h"
#include "imguizmo.h"

namespace rexfabric::ui {
namespace {

using Matrix = std::array<float, 16>;

// Where the camera starts: a little round from the front and above, at the game's own framing.
constexpr double kStartYaw = 25.0;
constexpr double kStartPitch = 12.0;
constexpr double kMaxPitch = 89.0;
constexpr double kMinZoom = 0.1;
constexpr double kMaxZoom = 20.0;
constexpr double kDegreesPerPixel = 0.4;
constexpr double kZoomPerNotch = 1.15;

// The viewport's own camera, when the game gives none: perspective, round the middle of the components, far enough off
// to see them all, at least a few units.
constexpr float kOwnFieldOfView = 45.0f;
constexpr float kOwnMinRadius = 2.0f;
constexpr float kOwnDistancePerRadius = 3.0f;
constexpr float kOwnNearPerDistance = 0.01f;
constexpr float kOwnFarPerDistance = 50.0f;
// The ground's grid: lines each side of the avatar's feet, a step apart that's a round number of units (1, 2 or 5
// times a power of ten) near a twentieth of the camera's distance, so it reads the same near and far; the two through
// the feet brighter. Each line is cut where it passes behind the camera.
constexpr int kGridLines = 10;
constexpr float kGridStepsPerDistance = 0.05f;
constexpr float kGridNearPerStep = 0.01f;
constexpr ImU32 kGridLine = IM_COL32(200, 205, 215, 90);
constexpr ImU32 kGridAxis = IM_COL32(225, 228, 235, 170);

constexpr float kGizmoSize = 0.16f;  // of the viewport's height, in clip space (ImGuizmo's default is 0.1)
constexpr float kMarkerRadius = 5.0f;
constexpr float kPickRadius = 9.0f;
constexpr ImU32 kBackground = IM_COL32(28, 31, 38, 255);
constexpr ImU32 kInheritedMarker = IM_COL32(150, 156, 170, 220);
constexpr ImU32 kOwnMarker = IM_COL32(255, 170, 60, 235);
constexpr ImU32 kSelectedMarker = IM_COL32(255, 235, 120, 255);
constexpr ImU32 kMarkerOutline = IM_COL32(0, 0, 0, 200);
constexpr ImVec4 kMessageColor{0.92f, 0.78f, 0.45f, 1.0f};

constexpr const char* kNoRenderer = "No viewport renderer: the game has not registered one, so avatars can't be previewed. Components can still be edited by their values.";

// The rows of `a` times `b`, row vectors.
Matrix Multiply(const Matrix& a, const Matrix& b) {
  Matrix out{};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
  }
  return out;
}

// A right handed look from `eye` at `at`, y up: the view of row vectors.
Matrix LookAt(const std::array<float, 3>& eye, const std::array<float, 3>& at) {
  const auto normalised = [](std::array<float, 3> v) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (float& x : v) x /= length;
    return v;
  };
  const auto cross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) { return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; };
  const auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  const std::array<float, 3> back = normalised({eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]});
  const std::array<float, 3> right = normalised(cross({0.0f, 1.0f, 0.0f}, back));
  const std::array<float, 3> up = cross(back, right);
  return {right[0], up[0], back[0], 0.0f, right[1], up[1], back[1], 0.0f, right[2], up[2], back[2], 0.0f, -dot(right, eye), -dot(up, eye), -dot(back, eye), 1.0f};
}

Matrix Perspective(float fov_degrees, float aspect, float near_z, float far_z) {
  const float f = 1.0f / std::tan(fov_degrees * std::numbers::pi_v<float> / 360.0f);
  return {f / aspect, 0.0f, 0.0f, 0.0f, 0.0f, f, 0.0f, 0.0f, 0.0f, 0.0f, (far_z + near_z) / (near_z - far_z), -1.0f, 0.0f, 0.0f, 2.0f * far_z * near_z / (near_z - far_z), 0.0f};
}

// The grid's step for a view: from how far its camera is from the avatar's feet.
float GridStep(const Matrix& view) {
  // The eye is where the view puts the origin's opposite: -t times the turn's transpose.
  float eye[3] = {};
  for (int i = 0; i < 3; ++i) eye[i] = -(view[12] * view[i * 4] + view[13] * view[i * 4 + 1] + view[14] * view[i * 4 + 2]);
  const float wanted = std::max(1e-3f, std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]) * kGridStepsPerDistance);
  const float power = std::pow(10.0f, std::floor(std::log10(wanted)));
  for (const float multiple : {1.0f, 2.0f, 5.0f}) {
    if (power * multiple >= wanted) return power * multiple;
  }
  return power * 10.0f;
}

// The ground's grid (y = 0 in the avatar's space) under the camera. Each line is taken into the camera's space, the part
// behind its near plane cut off (the camera looks down its -z), then projected.
void DrawGround(ImDrawList* draw, const Matrix& view, const Matrix& projection, const ImVec2& origin, const ImVec2& size) {
  const float step = GridStep(view);
  const float near_z = -step * kGridNearPerStep;
  const auto to_camera = [&view](float x, float z) {
    std::array<float, 3> c{};
    for (int k = 0; k < 3; ++k) c[k] = x * view[k] + z * view[8 + k] + view[12 + k];
    return c;
  };
  const auto to_screen = [&](const std::array<float, 3>& c) {
    float clip[4];
    for (int k = 0; k < 4; ++k) clip[k] = c[0] * projection[k] + c[1] * projection[4 + k] + c[2] * projection[8 + k] + projection[12 + k];
    return ImVec2(origin.x + (clip[0] / clip[3] * 0.5f + 0.5f) * size.x, origin.y + (0.5f - clip[1] / clip[3] * 0.5f) * size.y);
  };
  const auto line = [&](float x0, float z0, float x1, float z1, ImU32 colour) {
    std::array<float, 3> a = to_camera(x0, z0), b = to_camera(x1, z1);
    if (a[2] > near_z && b[2] > near_z) return;
    // The part in front of the near plane.
    if (a[2] > near_z || b[2] > near_z) {
      const float t = (near_z - a[2]) / (b[2] - a[2]);
      const std::array<float, 3> cut{a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, near_z};
      (a[2] > near_z ? a : b) = cut;
    }
    draw->AddLine(to_screen(a), to_screen(b), colour);
  };
  const float reach = step * static_cast<float>(kGridLines);
  for (int i = -kGridLines; i <= kGridLines; ++i) {
    const float at = step * static_cast<float>(i);
    const ImU32 colour = i == 0 ? kGridAxis : kGridLine;
    line(at, -reach, at, reach, colour);
    line(-reach, at, reach, at, colour);
  }
}

// The camera the viewport keeps itself: round `at` at the yaw and pitch, as far off as the radius asks, nearer with the
// zoom.
void OwnCamera(double yaw, double pitch, double zoom, const std::array<float, 3>& at, float radius, float aspect, Matrix& view, Matrix& projection) {
  constexpr double kRadians = std::numbers::pi / 180.0;
  const float distance = static_cast<float>(radius * kOwnDistancePerRadius / zoom);
  const float cp = static_cast<float>(std::cos(pitch * kRadians));
  const std::array<float, 3> eye{at[0] + distance * cp * static_cast<float>(std::sin(yaw * kRadians)), at[1] + distance * static_cast<float>(std::sin(pitch * kRadians)), at[2] + distance * cp * static_cast<float>(std::cos(yaw * kRadians))};
  view = LookAt(eye, at);
  projection = Perspective(kOwnFieldOfView, aspect, distance * kOwnNearPerDistance, distance * kOwnFarPerDistance);
}

// Where a point of the avatar's space lands in `rect`; none behind the camera.
std::optional<ImVec2> Project(const std::array<double, 3>& point, const Matrix& view_projection, const ImVec2& origin, const ImVec2& size) {
  float clip[4];
  for (int c = 0; c < 4; ++c) clip[c] = static_cast<float>(point[0]) * view_projection[c] + static_cast<float>(point[1]) * view_projection[4 + c] + static_cast<float>(point[2]) * view_projection[8 + c] + view_projection[12 + c];
  if (clip[3] <= 1e-6f) return std::nullopt;
  return ImVec2(origin.x + (clip[0] / clip[3] * 0.5f + 0.5f) * size.x, origin.y + (0.5f - clip[1] / clip[3] * 0.5f) * size.y);
}

// A component where the avatar puts it, for drawing: its place, the place of what it's on, and whether it's the class's
// own (so movable), with the component itself when it is.
struct Placed {
  std::string name;
  content::Transform world;
  content::Transform parent;
  bool has_place = true;
  const content::Component* component = nullptr;
  std::shared_ptr<const content::ComponentType> type;
  content::Component* own = nullptr;
};

void Place(const std::vector<content::Component>& components, const content::Transform& parent, std::vector<content::Component>* own, std::vector<Placed>& out) {
  for (size_t i = 0; i < components.size(); ++i) {
    const content::Component& component = components[i];
    const std::shared_ptr<const content::ComponentType> type = content::FindComponentType(component.type);
    const bool has_place = !type || type->transform;
    // A component without a place of its own is where the one above it is, as its children are.
    const content::Transform world = has_place ? content::Compose(parent, component.transform) : parent;
    out.push_back({.name = component.name, .world = world, .parent = parent, .has_place = has_place, .component = &component, .type = type, .own = own ? &(*own)[i] : nullptr});
    Place(component.children, world, own ? &(*own)[i].children : nullptr, out);
  }
}

// A value kept to what a file reads well: four decimals.
double Tidy(double value) {
  const double tidy = std::round(value * 10000.0) / 10000.0;
  return tidy == 0.0 ? 0.0 : tidy;
}

}  // namespace

AvatarViewport::AvatarViewport() : id_(content::NewViewport()), yaw_(kStartYaw), pitch_(kStartPitch), zoom_(1.0) {}

AvatarViewport::~AvatarViewport() {
  content::CloseViewport(id_);
}

void AvatarViewport::DrawToolbar() {
  if (glass::SmallButton(playing_ ? "Pause" : "Play")) playing_ = !playing_;
  ImGui::SetItemTooltip("Whether its time runs: what its components do (an emitter playing) and how the avatar moves");
  ImGui::SameLine();
  if (glass::SmallButton("Replay")) ++replays_;
  ImGui::SetItemTooltip("Everything from the start, as when an avatar of the class is spawned");
  ImGui::SameLine();
  const auto tool = [this](const char* label, Tool which, const char* tip) {
    if (glass::SmallButton(label, tool_ == which ? glass::kAccent : glass::kNeutral)) tool_ = which;
    ImGui::SetItemTooltip("%s", tip);
    ImGui::SameLine();
  };
  tool("Move", Tool::kMove, "Move the selected component (W)");
  tool("Turn", Tool::kTurn, "Turn the selected component (E)");
  tool("Scale", Tool::kScale, "Scale the selected component (R)");
  if (glass::SmallButton(local_ ? "Its axes" : "Avatar's axes")) local_ = !local_;
  ImGui::SetItemTooltip("Whether the gizmo follows the component's own axes or the avatar's");
  ImGui::SameLine();
  if (glass::SmallButton("Reset View")) {
    yaw_ = kStartYaw;
    pitch_ = kStartPitch;
    zoom_ = 1.0;
  }
  ImGui::SetItemTooltip("Back to the camera it started with. Drag to go round the avatar, the wheel to come nearer");
}

void AvatarViewport::TakeCamera(bool hovered, bool dragging) {
  const ImGuiIO& io = ImGui::GetIO();
  if (dragging) {
    yaw_ -= io.MouseDelta.x * kDegreesPerPixel;
    pitch_ = std::clamp(pitch_ + io.MouseDelta.y * kDegreesPerPixel, -kMaxPitch, kMaxPitch);
    yaw_ = std::remainder(yaw_, 360.0);
  }
  if (hovered && io.MouseWheel != 0.0f) zoom_ = std::clamp(zoom_ * std::pow(kZoomPerNotch, io.MouseWheel), kMinZoom, kMaxZoom);
  if (hovered && !io.WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) tool_ = Tool::kMove;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) tool_ = Tool::kTurn;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) tool_ = Tool::kScale;
  }
}

bool AvatarViewport::Draw(const ImVec2& size, content::ViewportView view, const std::vector<content::Component>& inherited, std::vector<content::Component>& own, std::string& selected) {
  BeginGizmoFrame();

  DrawToolbar();
  const ImVec2 room = ImGui::GetContentRegionAvail();
  const ImVec2 canvas(std::max(size.x, 1.0f), std::max(std::min(size.y, room.y), 1.0f));
  // A window of its own that doesn't move or scroll: a drag in it is the camera's, and the wheel the zoom.
  ImGui::BeginChild("##viewport", canvas, ImGuiChildFlags_None, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 end(origin.x + canvas.x, origin.y + canvas.y);

  // Every component where the avatar puts it, the inherited ones first.
  std::vector<Placed> placed;
  Place(inherited, {}, nullptr, placed);
  Place(own, {}, &own, placed);

  view.yaw = yaw_;
  view.pitch = pitch_;
  view.zoom = zoom_;
  view.width = static_cast<uint32_t>(canvas.x);
  view.height = static_cast<uint32_t>(canvas.y);
  view.playing = playing_;
  view.replays = replays_;
  view.hold_framing = held_;
  for (const Placed& component : placed) {
    content::ViewportPart part{.name = component.name, .type = component.component->type, .world = component.world, .own = component.own != nullptr};
    if (component.type) {
      for (const auto& [name, value] : content::ComponentValues(*component.type, *component.component)) part.values[name] = content::ToPropertyValue(value);
    }
    view.parts.push_back(std::move(part));
  }
  const std::optional<content::ViewportImage> image = content::ShowViewport(id_, view);

  Matrix camera_view;
  Matrix projection;
  const bool game_camera = image && image->camera;
  if (game_camera) {
    camera_view = image->view;
    projection = image->projection;
  } else {
    // Round the middle of the avatar's place and its components; kept while a component is dragged, so the camera
    // doesn't move under the gizmo.
    if (!held_) {
      std::array<double, 3> low{0.0, 0.0, 0.0}, high{0.0, 0.0, 0.0};
      for (const Placed& component : placed) {
        for (int i = 0; i < 3; ++i) {
          low[i] = std::min(low[i], component.world.position[i]);
          high[i] = std::max(high[i], component.world.position[i]);
        }
      }
      for (int i = 0; i < 3; ++i) own_centre_[i] = static_cast<float>((low[i] + high[i]) / 2);
      const double dx = high[0] - low[0], dy = high[1] - low[1], dz = high[2] - low[2];
      own_radius_ = std::max(kOwnMinRadius, static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz) / 2));
    }
    OwnCamera(yaw_, pitch_, zoom_, own_centre_, own_radius_, canvas.x / canvas.y, camera_view, projection);
  }
  // The background, the ground's grid on it, then the game's image over both: what the game draws covers the grid,
  // and its own background is clear, so the grid shows round it.
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->PushClipRect(origin, end, true);
  draw->AddRectFilled(origin, end, kBackground);
  ImGuizmo::SetDrawlist(draw);
  ImGuizmo::SetGizmoSizeClipSpace(kGizmoSize);
  ImGuizmo::SetRect(origin.x, origin.y, canvas.x, canvas.y);
  ImGuizmo::SetOrthographic(game_camera && image->orthographic);
  ImGuizmo::PushID(static_cast<int>(id_));
  DrawGround(draw, camera_view, projection, origin, canvas);
  if (image && image->texture != 0) draw->AddImage(ImTextureRef(static_cast<ImTextureID>(image->texture)), origin, end, ImVec2(0.0f, 0.0f), ImVec2(image->uv_max[0], image->uv_max[1]));

  // The markers: every component with a place, the class's own over the inherited ones.
  const Matrix view_projection = Multiply(camera_view, projection);
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const Placed* under_mouse = nullptr;
  for (const Placed& component : placed) {
    if (!component.has_place) continue;
    const std::optional<ImVec2> at = Project(component.world.position, view_projection, origin, canvas);
    if (!at) continue;
    const bool is_selected = component.own && component.name == selected;
    const ImU32 color = is_selected ? kSelectedMarker : component.own ? kOwnMarker : kInheritedMarker;
    draw->AddCircleFilled(*at, kMarkerRadius, color);
    draw->AddCircle(*at, kMarkerRadius, kMarkerOutline, 0, 1.5f);
    draw->AddText(ImVec2(at->x + kMarkerRadius * 2.0f, at->y - ImGui::GetTextLineHeight() * 0.5f), color, component.name.c_str());
    const float dx = mouse.x - at->x, dy = mouse.y - at->y;
    if (component.own && dx * dx + dy * dy <= kPickRadius * kPickRadius) under_mouse = &component;
  }

  // The gizmo on the selected component, when it's the class's own and has a place.
  bool changed = false;
  const auto chosen = std::find_if(placed.begin(), placed.end(), [&selected](const Placed& component) { return component.own && component.has_place && component.name == selected; });
  if (chosen != placed.end()) {
    Matrix matrix = content::MatrixOf(chosen->world);
    const ImGuizmo::OPERATION operation = tool_ == Tool::kMove ? ImGuizmo::TRANSLATE : tool_ == Tool::kTurn ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
    // Scaling only goes along the component's own axes.
    const ImGuizmo::MODE mode = local_ || tool_ == Tool::kScale ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    if (ImGuizmo::Manipulate(camera_view.data(), projection.data(), operation, mode, matrix.data())) {
      content::Transform local = content::Relative(chosen->parent, content::TransformOf(matrix));
      for (int i = 0; i < 3; ++i) {
        // Only what the tool changes: the rest stays as written, rather than as read back through a matrix.
        if (tool_ != Tool::kMove) local.position[i] = chosen->own->transform.position[i];
        if (tool_ != Tool::kTurn) local.rotation[i] = chosen->own->transform.rotation[i];
        if (tool_ != Tool::kScale) local.scale[i] = chosen->own->transform.scale[i];
        local.position[i] = Tidy(local.position[i]);
        local.rotation[i] = Tidy(local.rotation[i]);
        local.scale[i] = Tidy(local.scale[i]);
      }
      if (!(local == chosen->own->transform)) {
        chosen->own->transform = local;
        changed = true;
      }
    }
  }
  held_ = chosen != placed.end() && ImGuizmo::IsUsing();
  ImGuizmo::PopID();

  // The canvas takes the clicks the gizmo doesn't: a marker selects, a drag goes round. It isn't there while the
  // pointer is on the gizmo, as ImGuizmo only takes a click when no item was hovered the frame before.
  const bool over_gizmo = chosen != placed.end() && (ImGuizmo::IsOver() || ImGuizmo::IsUsingAny());
  bool hovered = ImGui::IsWindowHovered();
  if (!over_gizmo || dragging_) {
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##canvas", canvas, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated()) {
      if (under_mouse) selected = under_mouse->name;
      dragging_ = !under_mouse;
    }
    if (!ImGui::IsItemActive()) dragging_ = false;
  }
  TakeCamera(hovered, dragging_);

  // Why there's nothing to see, over the background.
  const char* message = !image ? kNoRenderer : !image->message.empty() ? image->message.c_str() : nullptr;
  if (message) {
    const float wrap = canvas.x - ImGui::GetStyle().WindowPadding.x * 2.0f;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + ImGui::GetStyle().WindowPadding.x, origin.y + ImGui::GetStyle().WindowPadding.y));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextColored(kMessageColor, "%s", message);
    ImGui::PopTextWrapPos();
  }
  draw->PopClipRect();
  ImGui::EndChild();
  return changed;
}

}  // namespace rexfabric::ui
