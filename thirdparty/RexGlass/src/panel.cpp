// Glass panels as ImGui geometry. The look follows the Toon engine's Glass.hlsl, piece by piece, with what a draw list
// can do in place of a pixel shader:
//
//   shadow        the panel's shape dropped a little, fading out from it, drawn only outside the panel
//   body          what's behind, frosted, magnified a touch toward the middle: the screen as drawn so far when the
//                 game's renderer hands it over, else the game's blurred frame
//   rim           a ring of triangles between the edge and the bezel, whose texture coordinates reach outside the
//                 panel at the edge, so what's behind bends there like thick glass, clearer at the very edge
//   canvas        over a canvas there's no image of, the canvas drawn through the same bend: its colors, and its grid
//                 split into color along the rim
//   tint          the body and header colors over it; without a backdrop, more solid, standing in for frost
//   light         brighter at the top, and along the thick rim, with an iridescent band drifting around it
//   edge          a darker line just inside the outline and a specular outline lit from the top left
//   glow, sheen   a rim glow for hover and selection, and a faint sheen sweeping across now and then

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include <rexglass/panel.h>
#include <rexglass/rexglass.h>

namespace rexglass {
namespace {

constexpr float kTau = 6.2831853f;
// How far the shadow reaches past the panel, and how far it drops, at scale 1.
constexpr float kShadowExtent = 18.0f;
constexpr float kShadowDrop = 4.0f;
constexpr int kShadowRings = 8;
// The light comes from the top left.
const ImVec2 kLight(-0.5430f, -0.8397f);

float Saturate(float value) { return std::clamp(value, 0.0f, 1.0f); }

ImU32 Color(float r, float g, float b, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, Saturate(a))); }
ImU32 Color(const ImVec4& rgb, float a) { return Color(rgb.x, rgb.y, rgb.z, a); }

// A rounded rectangle's outline, one point per step around its corners, with the outward direction at each point.
// Shapes made with the same `steps` match point for point, which the rings between them rely on.
struct Outline {
  std::vector<ImVec2> points;
  std::vector<ImVec2> normals;
};

int CornerSteps(float radius) { return std::clamp(static_cast<int>(radius * 0.6f), 3, 12); }

// The corners of `want` that are rounded, as ImGui's draw calls take them: 0 would mean all of them.
ImDrawFlags Corners(ImDrawFlags corners, ImDrawFlags want = ImDrawFlags_RoundCornersAll) {
  if ((corners & ImDrawFlags_RoundCornersMask_) == 0) corners = ImDrawFlags_RoundCornersAll;
  const ImDrawFlags both = corners & want & ImDrawFlags_RoundCornersMask_;
  return both ? both : ImDrawFlags_RoundCornersNone;
}

// [min, max] shrunk by `inset` on every side, its rounded corners rounded by what's left of `radius`.
Outline RoundedOutline(const ImVec2& min, const ImVec2& max, float radius, float inset, int steps, ImDrawFlags corners) {
  const ImVec2 lo(min.x + inset, min.y + inset);
  const ImVec2 hi(max.x - inset, max.y - inset);
  // Corners clockwise from the top left, the angle each one's arc starts at, and whether it's rounded.
  const float starts[4] = {kTau * 0.5f, kTau * 0.75f, 0.0f, kTau * 0.25f};
  const ImDrawFlags flags[4] = {ImDrawFlags_RoundCornersTopLeft, ImDrawFlags_RoundCornersTopRight, ImDrawFlags_RoundCornersBottomRight, ImDrawFlags_RoundCornersBottomLeft};
  const ImDrawFlags rounded = Corners(corners);
  Outline outline;
  outline.points.reserve(static_cast<size_t>(4 * (steps + 1)));
  outline.normals.reserve(outline.points.capacity());
  for (int corner = 0; corner < 4; ++corner) {
    // A square corner is an arc of no radius: its points all sit on the corner, still turning, so rings stay matched.
    const float r = (rounded & flags[corner]) ? std::max(radius - inset, 0.0f) : 0.0f;
    const ImVec2 center(corner == 0 || corner == 3 ? lo.x + r : hi.x - r, corner < 2 ? lo.y + r : hi.y - r);
    for (int step = 0; step <= steps; ++step) {
      const float angle = starts[corner] + kTau * 0.25f * static_cast<float>(step) / static_cast<float>(steps);
      const ImVec2 normal(std::cos(angle), std::sin(angle));
      outline.points.emplace_back(center.x + normal.x * r, center.y + normal.y * r);
      outline.normals.push_back(normal);
    }
  }
  return outline;
}

// Where on an image of what's behind a point of the panel's draw list is.
struct BackdropMap {
  ImTextureID texture = ImTextureID_Invalid;
  ImVec2 origin;  // the screen's top left
  ImVec2 size;    // the screen's size
  ImVec4 uv;
  // The draw list to the screen, for a transformed draw list; null when it's in screen coordinates.
  const std::function<ImVec2(const ImVec2&)>* to_screen = nullptr;

  ImVec2 At(const ImVec2& point) const {
    const ImVec2 screen = to_screen ? (*to_screen)(point) : point;
    return ImVec2(uv.x + (screen.x - origin.x) / size.x * (uv.z - uv.x), uv.y + (screen.y - origin.y) / size.y * (uv.w - uv.y));
  }
};

// A ring of triangles from `outer` to `inner`, which match point for point.
void AddRing(ImDrawList* draw, const Outline& outer, const Outline& inner, const auto& outer_vertex, const auto& inner_vertex) {
  const int count = static_cast<int>(outer.points.size());
  draw->PrimReserve(count * 6, count * 2);
  const unsigned int base = draw->_VtxCurrentIdx;
  for (int i = 0; i < count; ++i) {
    const auto [outer_uv, outer_color] = outer_vertex(i);
    const auto [inner_uv, inner_color] = inner_vertex(i);
    draw->PrimWriteVtx(outer.points[static_cast<size_t>(i)], outer_uv, outer_color);
    draw->PrimWriteVtx(inner.points[static_cast<size_t>(i)], inner_uv, inner_color);
  }
  for (int i = 0; i < count; ++i) {
    const unsigned int a = base + static_cast<unsigned int>(i) * 2;
    const unsigned int b = base + static_cast<unsigned int>((i + 1) % count) * 2;
    draw->PrimWriteIdx(static_cast<ImDrawIdx>(a));
    draw->PrimWriteIdx(static_cast<ImDrawIdx>(b));
    draw->PrimWriteIdx(static_cast<ImDrawIdx>(a + 1));
    draw->PrimWriteIdx(static_cast<ImDrawIdx>(a + 1));
    draw->PrimWriteIdx(static_cast<ImDrawIdx>(b));
    draw->PrimWriteIdx(static_cast<ImDrawIdx>(b + 1));
  }
}

// The outline filled as a fan from its middle, colored per point: a gradient over a rounded shape.
void FillGradient(ImDrawList* draw, const Outline& outline, const ImVec2& middle, ImU32 middle_color, const auto& point_color) {
  const int count = static_cast<int>(outline.points.size());
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  draw->PrimReserve(count * 3, count + 1);
  const unsigned int base = draw->_VtxCurrentIdx;
  draw->PrimWriteVtx(middle, uv, middle_color);
  for (int i = 0; i < count; ++i) draw->PrimWriteVtx(outline.points[static_cast<size_t>(i)], uv, point_color(i));
  for (int i = 0; i < count; ++i) {
    draw->PrimWriteIdx(static_cast<ImDrawIdx>(base));
    draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + static_cast<unsigned int>(i)));
    draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + static_cast<unsigned int>((i + 1) % count)));
  }
}

// How far a point is outside a rounded box, as Glass.hlsl's RoundedBox: negative inside.
float RoundedBoxDistance(const ImVec2& point, const ImVec2& min, const ImVec2& max, float radius) {
  const ImVec2 half((max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f);
  const float r = std::min(radius, std::min(half.x, half.y));
  const float qx = std::abs(point.x - (min.x + max.x) * 0.5f) - half.x + r;
  const float qy = std::abs(point.y - (min.y + max.y) * 0.5f) - half.y + r;
  const float outside = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) + std::max(qy, 0.0f) * std::max(qy, 0.0f));
  return outside + std::min(std::max(qx, qy), 0.0f) - r;
}

// A soft shadow dropped under the panel, as in Glass.hlsl: the panel's shape moved down by the drop, darkest at its
// edge and fading with distance from it. So it hugs the panel's sides, shows as a thin band above and a fuller one
// below. It's drawn only outside the panel, from the panel's own edge outward, so it's never under the panel and
// isn't part of what the panel shows of what's behind it. Smooth, as rings of triangles whose corners carry the fade.
void AddShadow(ImDrawList* draw, const Panel& panel, float radius, int steps, float opacity) {
  const float scale = panel.scale;
  const float drop = kShadowDrop * scale;
  const float extent = kShadowExtent * scale;
  const ImVec2 low(panel.min.x, panel.min.y + drop);
  const ImVec2 high(panel.max.x, panel.max.y + drop);
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  const auto shade = [&](float distance) { return Color(0.0f, 0.0f, 0.0f, 0.38f * std::exp(-std::max(distance, 0.0f) / (6.0f * scale)) * opacity); };

  // From the panel's edge, shaded by how far each point is from the dropped shape, to the dropped shape grown by
  // the drop, which is outside the panel all round. Then out from there to nothing.
  Outline inner = RoundedOutline(panel.min, panel.max, radius, 0.0f, steps, panel.corners);
  std::vector<ImU32> inner_colors;
  for (const ImVec2& point : inner.points) inner_colors.push_back(shade(RoundedBoxDistance(point, low, high, radius)));
  for (int ring = 0; ring <= kShadowRings; ++ring) {
    const float distance = drop + extent * static_cast<float>(ring) / kShadowRings;
    Outline outer = RoundedOutline(low, high, radius, -distance, steps, panel.corners);
    const ImU32 outer_color = ring == kShadowRings ? Color(0.0f, 0.0f, 0.0f, 0.0f) : shade(distance);
    AddRing(draw, outer, inner, [&](int) { return std::pair(uv, outer_color); }, [&](int i) { return std::pair(uv, inner_colors[static_cast<size_t>(i)]); });
    inner = std::move(outer);
    inner_colors.assign(inner.points.size(), outer_color);
  }
}

// How the glass bends what's behind it. Each point shown on the panel shows the point behind it: along the rim, one
// pulled from outside the panel, the more the nearer the edge, and in the middle one a touch nearer the center, so
// the middle magnifies. It's a mesh of triangles inside each of which the bend is affine, so an image (its texture
// coordinates) and lines (crossing the triangles) are bent alike through it. The rim's triangles come first.
struct Warp {
  std::vector<ImVec2> shown;
  std::vector<ImVec2> behind;
  std::vector<int> triangles;  // three vertices each
  size_t rim_triangles = 0;

  // Rim vertices alternate between the edge and the inner side; the middle's center comes last.
  bool IsEdge(size_t vertex) const { return vertex + 1 < shown.size() && vertex % 2 == 0; }
};

// `inset` is where the rim starts inside the edge; `spread` scales the bend, for the color split.
Warp MakeWarp(const Panel& panel, float radius, float bezel, int steps, float inset, float spread) {
  const ImVec2 center((panel.min.x + panel.max.x) * 0.5f, (panel.min.y + panel.max.y) * 0.5f);
  const auto magnified = [&center](const ImVec2& point) { return ImVec2(center.x + (point.x - center.x) * 0.96f, center.y + (point.y - center.y) * 0.96f); };
  const Outline outer = RoundedOutline(panel.min, panel.max, radius, inset, steps, panel.corners);
  const Outline inner = RoundedOutline(panel.min, panel.max, radius, bezel, steps, panel.corners);
  const float bend = std::max(panel.refraction, 0.0f) * bezel * 1.25f * spread;
  const int count = static_cast<int>(outer.points.size());

  Warp warp;
  for (int i = 0; i < count; ++i) {
    const ImVec2& point = outer.points[static_cast<size_t>(i)];
    const ImVec2& normal = outer.normals[static_cast<size_t>(i)];
    warp.shown.push_back(point);
    warp.behind.emplace_back(point.x + normal.x * bend, point.y + normal.y * bend);
    warp.shown.push_back(inner.points[static_cast<size_t>(i)]);
    warp.behind.push_back(magnified(inner.points[static_cast<size_t>(i)]));
  }
  for (int i = 0; i < count; ++i) {
    const int a = i * 2;
    const int b = ((i + 1) % count) * 2;
    warp.triangles.insert(warp.triangles.end(), {a, b, a + 1, a + 1, b, b + 1});
  }
  warp.rim_triangles = warp.triangles.size() / 3;
  const int middle = static_cast<int>(warp.shown.size());
  warp.shown.push_back(center);
  warp.behind.push_back(center);
  for (int i = 0; i < count; ++i) warp.triangles.insert(warp.triangles.end(), {middle, i * 2 + 1, ((i + 1) % count) * 2 + 1});
  return warp;
}

// Draws the warp's triangles [first, last), `vertex` giving each vertex, by its index and the point behind it, its
// texture coordinate and color.
void AddWarp(ImDrawList* draw, const Warp& warp, size_t first, size_t last, const auto& vertex) {
  draw->PrimReserve(static_cast<int>((last - first) * 3), static_cast<int>(warp.shown.size()));
  const unsigned int base = draw->_VtxCurrentIdx;
  for (size_t i = 0; i < warp.shown.size(); ++i) {
    const auto [uv, color] = vertex(i, warp.behind[i]);
    draw->PrimWriteVtx(warp.shown[i], uv, color);
  }
  for (size_t i = first * 3; i < last * 3; ++i) draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + static_cast<unsigned int>(warp.triangles[i])));
}

// Lines at every multiple of `spacing` across and down, as seen through the warp's triangles [first, last): each
// triangle shows the piece of a line that crosses what's behind it, where that piece appears on the panel.
void AddLinesThrough(ImDrawList* draw, const Warp& warp, size_t first, size_t last, float spacing, ImU32 color, float thickness) {
  // Pieces meet at the triangles' edges; without anti-aliasing they meet without overlapping.
  const ImDrawListFlags flags = draw->Flags;
  draw->Flags &= ~ImDrawListFlags_AntiAliasedLines;
  for (size_t t = first; t < last; ++t) {
    const int* corner = &warp.triangles[t * 3];
    for (int axis = 0; axis < 2; ++axis) {
      const auto along = [axis](const ImVec2& point) { return axis == 0 ? point.x : point.y; };
      float low = along(warp.behind[static_cast<size_t>(corner[0])]);
      float high = low;
      for (int k = 1; k < 3; ++k) {
        low = std::min(low, along(warp.behind[static_cast<size_t>(corner[k])]));
        high = std::max(high, along(warp.behind[static_cast<size_t>(corner[k])]));
      }
      for (float line = std::ceil(low / spacing) * spacing; line <= high; line += spacing) {
        ImVec2 ends[2];
        int found = 0;
        for (int k = 0; k < 3 && found < 2; ++k) {
          const size_t a = static_cast<size_t>(corner[k]);
          const size_t b = static_cast<size_t>(corner[(k + 1) % 3]);
          const float from = along(warp.behind[a]);
          const float to = along(warp.behind[b]);
          if (from == to || (from - line) * (to - line) > 0.0f) continue;
          const float at = (line - from) / (to - from);
          ends[found++] = ImVec2(warp.shown[a].x + (warp.shown[b].x - warp.shown[a].x) * at, warp.shown[a].y + (warp.shown[b].y - warp.shown[a].y) * at);
        }
        if (found == 2) draw->AddLine(ends[0], ends[1], color, thickness);
      }
    }
  }
  draw->Flags = flags;
}

// What shows through: what's behind, frosted, shrunk a touch toward the middle, and bent outward along the rim.
// `sharp`, when there's one, is what's behind unblurred: the rim is clearer than the middle, as in Toon, where the
// bend shows best.
void AddBackdrop(ImDrawList* draw, const Panel& panel, const BackdropMap& frosted, const BackdropMap* sharp, float radius, float bezel, int steps, float opacity) {
  const ImVec2 center((panel.min.x + panel.max.x) * 0.5f, (panel.min.y + panel.max.y) * 0.5f);
  const auto magnified = [&center](const ImVec2& point) { return ImVec2(center.x + (point.x - center.x) * 0.96f, center.y + (point.y - center.y) * 0.96f); };
  const ImU32 white = Color(1.0f, 1.0f, 1.0f, opacity);
  draw->AddImageRounded(ImTextureRef(frosted.texture), panel.min, panel.max, frosted.At(magnified(panel.min)), frosted.At(magnified(panel.max)), white, radius, Corners(panel.corners));
  if (panel.refraction <= 0.0f) return;

  // Only the rim, over the body, which already shows the middle. It starts a little inside the edge, so the body's
  // anti-aliased edge stays.
  const Warp warp = MakeWarp(panel, radius, bezel, steps, 0.75f, 1.0f);
  draw->PushTexture(ImTextureRef(frosted.texture));
  AddWarp(draw, warp, 0, warp.rim_triangles, [&](size_t, const ImVec2& behind) { return std::pair(frosted.At(behind), white); });
  draw->PopTexture();
  if (!sharp) return;

  // The sharp image over it, solid at the edge and gone where the rim meets the middle.
  draw->PushTexture(ImTextureRef(sharp->texture));
  const ImU32 clear = Color(1.0f, 1.0f, 1.0f, 0.0f);
  const ImU32 edge = Color(1.0f, 1.0f, 1.0f, 0.75f * opacity);
  AddWarp(draw, warp, 0, warp.rim_triangles, [&](size_t vertex, const ImVec2& behind) { return std::pair(sharp->At(behind), warp.IsEdge(vertex) ? edge : clear); });
  draw->PopTexture();
}

// What shows through over a canvas: its colors and its grid, bent alike, the grid split into color along the rim as
// light of each color bends by its own amount.
void AddCanvas(ImDrawList* draw, const Panel& panel, const Canvas& canvas, float radius, float bezel, int steps, float opacity) {
  const Warp warp = MakeWarp(panel, radius, bezel, steps, 0.0f, 1.0f);
  const size_t all = warp.triangles.size() / 3;
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  AddWarp(draw, warp, 0, all, [&](size_t, const ImVec2& behind) {
    ImVec4 color = ImGui::ColorConvertU32ToFloat4(canvas.color(behind));
    color.w *= opacity;
    return std::pair(uv, ImGui::ColorConvertFloat4ToU32(color));
  });
  if (canvas.grid_spacing <= 0.0f || (canvas.grid_color & IM_COL32_A_MASK) == 0) return;

  const ImVec4 grid = ImGui::ColorConvertU32ToFloat4(canvas.grid_color);
  const float alpha = grid.w * opacity;
  const float thickness = canvas.grid_thickness;
  // Red bends more and blue less than the rest, as in Toon's Glass.hlsl. Where the bend fades they meet again, under
  // the line drawn last.
  if (panel.refraction > 0.0f) {
    const Warp red = MakeWarp(panel, radius, bezel, steps, 0.0f, 1.3f);
    const Warp blue = MakeWarp(panel, radius, bezel, steps, 0.0f, 0.7f);
    AddLinesThrough(draw, red, 0, red.rim_triangles, canvas.grid_spacing, Color(1.0f, 0.35f, 0.35f, alpha), thickness);
    AddLinesThrough(draw, blue, 0, blue.rim_triangles, canvas.grid_spacing, Color(0.35f, 0.55f, 1.0f, alpha), thickness);
  }
  AddLinesThrough(draw, warp, 0, all, canvas.grid_spacing, Color(grid, alpha), thickness);
}

void AddTint(ImDrawList* draw, const Panel& panel, float radius, bool backdrop, float opacity) {
  // Without anything to show through, frost has nothing to blur, so it makes the tint more solid instead.
  const auto alpha = [&](float a) { return (backdrop ? a : a + (1.0f - a) * panel.frost * 0.5f) * opacity; };
  // Glass lifts what's behind it a little toward white.
  if (backdrop) draw->AddRectFilled(panel.min, panel.max, Color(1.0f, 1.0f, 1.0f, 0.05f * opacity), radius, Corners(panel.corners));

  const float header = std::min(panel.header_height * panel.scale, panel.max.y - panel.min.y);
  if (header <= 0.0f) {
    draw->AddRectFilled(panel.min, panel.max, Color(panel.tint, alpha(panel.tint.w)), radius, Corners(panel.corners));
    return;
  }
  const float split = panel.min.y + header;
  draw->AddRectFilled(panel.min, ImVec2(panel.max.x, split), Color(panel.header_tint, alpha(panel.header_tint.w)), radius, Corners(panel.corners, ImDrawFlags_RoundCornersTop));
  if (split < panel.max.y) draw->AddRectFilled(ImVec2(panel.min.x, split), panel.max, Color(panel.tint, alpha(panel.tint.w)), radius, Corners(panel.corners, ImDrawFlags_RoundCornersBottom));
}

void AddLight(ImDrawList* draw, const Panel& panel, float radius, float bezel, int steps, float seconds, float opacity) {
  // Lit from above: brighter toward the top.
  const Outline outline = RoundedOutline(panel.min, panel.max, radius, 0.0f, steps, panel.corners);
  const float height = std::max(panel.max.y - panel.min.y, 1.0f);
  const ImVec2 middle((panel.min.x + panel.max.x) * 0.5f, (panel.min.y + panel.max.y) * 0.5f);
  FillGradient(draw, outline, middle, Color(1.0f, 1.0f, 1.0f, 0.035f * opacity), [&](int i) { return Color(1.0f, 1.0f, 1.0f, 0.07f * (1.0f - (outline.points[static_cast<size_t>(i)].y - panel.min.y) / height) * opacity); });

  // Where the glass is thick, along the rim, it's brighter, with an iridescent band drifting slowly around it.
  const Outline outer = RoundedOutline(panel.min, panel.max, radius, 0.5f, steps, panel.corners);
  const Outline inner = RoundedOutline(panel.min, panel.max, radius, bezel * 0.45f, steps, panel.corners);
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  AddRing(
      draw, outer, inner,
      [&](int i) {
        const ImVec2& normal = outer.normals[static_cast<size_t>(i)];
        const float turn = std::atan2(normal.y, normal.x) / kTau + seconds * 0.04f;
        const float r = 0.5f + 0.5f * std::cos(kTau * turn);
        const float g = 0.5f + 0.5f * std::cos(kTau * (turn + 0.33f));
        const float b = 0.5f + 0.5f * std::cos(kTau * (turn + 0.67f));
        return std::pair(uv, Color(0.4f + 0.6f * r, 0.4f + 0.6f * g, 0.4f + 0.6f * b, 0.2f * opacity));
      },
      [&](int) { return std::pair(uv, Color(1.0f, 1.0f, 1.0f, 0.0f)); });
}

void AddEdge(ImDrawList* draw, const Panel& panel, float radius, int steps, float opacity) {
  const float scale = panel.scale;
  // Just inside the outline the glass turns a little darker, which gives the edge its thickness.
  const float shade = 3.2f * scale;
  if (panel.max.x - panel.min.x > shade * 2.0f && panel.max.y - panel.min.y > shade * 2.0f) draw->AddRect(ImVec2(panel.min.x + shade, panel.min.y + shade), ImVec2(panel.max.x - shade, panel.max.y - shade), Color(0.0f, 0.0f, 0.0f, 0.13f * opacity), std::max(radius - shade, 0.0f), Corners(panel.corners), 1.6f * scale);

  // The specular outline: a thin bright line, strongest where it faces the light and fainter on the opposite side.
  const Outline outline = RoundedOutline(panel.min, panel.max, radius, 0.6f * scale, steps, panel.corners);
  const size_t count = outline.points.size();
  for (size_t i = 0; i < count; ++i) {
    const size_t next = (i + 1) % count;
    const ImVec2 normal((outline.normals[i].x + outline.normals[next].x) * 0.5f, (outline.normals[i].y + outline.normals[next].y) * 0.5f);
    const float facing = normal.x * kLight.x + normal.y * kLight.y;
    const float specular = 0.28f + 0.8f * std::pow(Saturate(facing), 3.0f) + 0.55f * std::pow(Saturate(-facing), 3.0f);
    draw->AddLine(outline.points[i], outline.points[next], Color(1.0f, 1.0f, 1.0f, specular * (0.55f + 0.3f * panel.highlight) * opacity), 1.2f * scale);
  }
}

void AddGlow(ImDrawList* draw, const Panel& panel, float radius, float opacity) {
  if (panel.glow.w <= 0.0f) return;
  const float scale = panel.scale;
  const ImU32 rim = Color(panel.glow, panel.glow.w * 0.8f * opacity);
  draw->AddRect(panel.min, panel.max, rim, radius, Corners(panel.corners), 1.6f * scale);
  // A soft halo just outside.
  for (int ring = 1; ring <= 3; ++ring) {
    const float distance = static_cast<float>(ring) * 1.5f * scale;
    draw->AddRect(ImVec2(panel.min.x - distance, panel.min.y - distance), ImVec2(panel.max.x + distance, panel.max.y + distance), Color(panel.glow, panel.glow.w * 0.22f * opacity / static_cast<float>(ring)), radius + distance, Corners(panel.corners), 1.5f * scale);
  }
}

// A faint sheen sweeping across now and then, out of step between panels.
void AddSheen(ImDrawList* draw, const Panel& panel, float seconds, float opacity) {
  const ImVec2 size(panel.max.x - panel.min.x, panel.max.y - panel.min.y);
  const float length = std::max(std::sqrt(size.x * size.x + size.y * size.y), 1.0f);
  const ImVec2 center((panel.min.x + panel.max.x) * 0.5f, (panel.min.y + panel.max.y) * 0.5f);
  const float phase = seconds * 0.05f + center.x * 0.0013f + center.y * 0.0007f;
  const float sweep = (phase - std::floor(phase)) * 2.2f - 0.6f;
  if (sweep < -0.3f || sweep > 1.3f) return;

  // Lines across the sweep's direction: the band's two faded edges and its bright middle.
  const ImVec2 across(0.8575f, 0.5145f);
  const ImVec2 along(-across.y, across.x);
  const float half = 0.25f * length;
  const auto line = [&](float offset, ImVec2& a, ImVec2& b) {
    const ImVec2 middle(panel.min.x + across.x * (sweep * length + offset), panel.min.y + across.y * (sweep * length + offset));
    a = ImVec2(middle.x - along.x * length * 2.0f, middle.y - along.y * length * 2.0f);
    b = ImVec2(middle.x + along.x * length * 2.0f, middle.y + along.y * length * 2.0f);
  };
  ImVec2 a0, b0, a1, b1, a2, b2;
  line(-half, a0, b0);
  line(0.0f, a1, b1);
  line(half, a2, b2);
  const ImU32 clear = Color(1.0f, 1.0f, 1.0f, 0.0f);
  const ImU32 bright = Color(1.0f, 1.0f, 1.0f, 0.045f * opacity);
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  draw->PushClipRect(panel.min, panel.max, true);
  draw->PrimReserve(12, 6);
  const unsigned int base = draw->_VtxCurrentIdx;
  draw->PrimWriteVtx(a0, uv, clear);
  draw->PrimWriteVtx(b0, uv, clear);
  draw->PrimWriteVtx(a1, uv, bright);
  draw->PrimWriteVtx(b1, uv, bright);
  draw->PrimWriteVtx(a2, uv, clear);
  draw->PrimWriteVtx(b2, uv, clear);
  for (unsigned int quad = 0; quad < 2; ++quad) {
    const unsigned int q = base + quad * 2;
    for (unsigned int index : {q, q + 1, q + 2, q + 2, q + 1, q + 3}) draw->PrimWriteIdx(static_cast<ImDrawIdx>(index));
  }
  draw->PopClipRect();
}

}  // namespace

void AddPanel(ImDrawList* draw, const Panel& panel) {
  const ImVec2 size(panel.max.x - panel.min.x, panel.max.y - panel.min.y);
  if (!draw || size.x <= 1.0f || size.y <= 1.0f || panel.opacity <= 0.0f) return;
  const float opacity = Saturate(panel.opacity);
  const float scale = std::max(panel.scale, 0.01f);
  const float radius = std::min(panel.rounding * scale, std::min(size.x, size.y) * 0.5f);
  // The bezel, the thick rim where the glass bends what's behind it, can't take more than part of the panel.
  const float bezel = std::min(std::max(radius * 1.5f, 16.0f * scale), std::min(size.x, size.y) * 0.3f);
  const int steps = CornerSteps(radius);
  const float seconds = static_cast<float>(ImGui::GetTime());

  // What's behind, best first: the screen as drawn so far, which holds the UI under the panel as well as the game;
  // an image of the game; a canvas the panel describes.
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  BackdropMap frosted;
  frosted.origin = viewport->Pos;
  frosted.size = ImVec2(std::max(viewport->Size.x, 1.0f), std::max(viewport->Size.y, 1.0f));
  frosted.to_screen = panel.to_screen ? &panel.to_screen : nullptr;
  BackdropMap sharp = frosted;
  frosted.texture = panel.see_through ? Screen(true, &frosted.uv) : ImTextureID_Invalid;
  sharp.texture = frosted.texture != ImTextureID_Invalid ? Screen(false, &sharp.uv) : ImTextureID_Invalid;
  if (!panel.see_through) {
    // Tinted glass only.
  } else if (frosted.texture == ImTextureID_Invalid && panel.backdrop != ImTextureID_Invalid) {
    frosted.texture = panel.backdrop;
    frosted.uv = panel.backdrop_uv;
  } else if (frosted.texture == ImTextureID_Invalid && panel.see_game) {
    frosted.texture = Backdrop(&frosted.uv);
  }
  const bool image = frosted.texture != ImTextureID_Invalid;
  const bool canvas = panel.see_through && !image && panel.canvas && panel.canvas->color;

  if (image) AddBackdrop(draw, panel, frosted, sharp.texture != ImTextureID_Invalid ? &sharp : nullptr, radius, bezel, steps, opacity);
  else if (canvas) AddCanvas(draw, panel, *panel.canvas, radius, bezel, steps, opacity);
  if (panel.shadow) AddShadow(draw, panel, radius, steps, opacity);
  AddTint(draw, panel, radius, image || canvas, opacity);
  AddLight(draw, panel, radius, bezel, steps, seconds, opacity);
  AddEdge(draw, panel, radius, steps, opacity);
  AddGlow(draw, panel, radius, opacity);
  AddSheen(draw, panel, seconds, opacity);
}

}  // namespace rexglass
