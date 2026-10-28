// Drawing a page's contents: rows, the gamer card and the gamerpic grid.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "guide_dialog.h"
#include "imgui.h"
#include "live_stats.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

void GuideDialog::DrawList(ImDrawList* dl, ImVec2 l0, ImVec2 l1, float a) {
  Page& page = Current();
  row_hits_.clear();
  slider_hits_.clear();
  grid_cells_.clear();
  const float slide_x = slide_ * Px(60);
  const float ca = a * (1.0f - std::min(1.0f, std::abs(slide_)) * 0.9f);
  dl->PushClipRect(l0, l1, true);

  float y = l0.y;
  if (Stack().size() > 1) {
    Text(dl, Light(), Px(30), ImVec2(l0.x + Px(14) + slide_x, y), WithAlpha(color::kText, ca), page.title);
    y += Px(46);
  }
  if (page.kind == PageKind::kGamercard) y = DrawGamercard(dl, ImVec2(l0.x + slide_x, y), l1.x, page.arg, ca);
  if (page.kind == PageKind::kColorEditor) {
    y = DrawColorPreview(dl, ImVec2(l0.x + slide_x, y), l1.x, std::atoi(page.arg.c_str()), ca);
  }
  if (page.kind == PageKind::kAchievements) {
    const std::string summary = AchievementSummary();
    if (!summary.empty()) {
      Caps(dl, Semibold(), Px(14), ImVec2(l0.x + Px(16) + slide_x, y), WithAlpha(color::kTextDim, ca),
           summary + " unlocked", Px(1.2f));
      y += Px(28);
    }
  }
  if (page.kind == PageKind::kLeaderboard) {
    const uint32_t total = stats::TopRows(uint32_t(std::strtoul(page.arg.c_str(), nullptr, 10))).total;
    if (total) {
      Caps(dl, Semibold(), Px(14), ImVec2(l0.x + Px(16) + slide_x, y), WithAlpha(color::kTextDim, ca),
           std::to_string(total) + (total == 1 ? " player" : " players"), Px(1.2f));
      y += Px(28);
    }
  }
  if (page.kind == PageKind::kThemes) {
    DrawThemeGallery(dl, ImVec2(l0.x + Px(14) + slide_x, y + Px(8)), l1, page, ca);
    dl->PopClipRect();
    return;
  }
  if (page.kind == PageKind::kGamerpics) {
    DrawGamerpicGrid(dl, ImVec2(l0.x + Px(14) + slide_x, y + Px(6)), l1.y, page, ca);
    dl->PopClipRect();
    return;
  }

  // Row heights, then scroll so the selection stays in view.
  std::vector<float> heights;
  float total = 0.0f;
  for (const Row& r : rows_) {
    heights.push_back(r.sub.empty() ? Px(56) : Px(72));
    total += heights.back();
  }
  const float view = l1.y - y;
  float sel_top = 0.0f;
  for (int i = 0; i < page.selected && i < int(heights.size()); ++i) sel_top += heights[i];
  const float sel_h = page.selected < int(heights.size()) ? heights[page.selected] : 0.0f;
  float target = page.scroll;
  if (sel_top < target) target = sel_top;
  if (sel_top + sel_h > target + view) target = sel_top + sel_h - view;
  target = std::clamp(target, 0.0f, std::max(0.0f, total - view));
  page.scroll += (target - page.scroll) * std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f);

  // Rows scroll under the page's title and header, not over them.
  dl->PushClipRect(ImVec2(l0.x, y), l1, true);
  float ry = y - page.scroll;
  for (size_t i = 0; i < rows_.size(); ++i) {
    const Row& row = rows_[i];
    const ImVec2 r0(l0.x + slide_x, ry), r1(l1.x + slide_x, ry + heights[i] - Px(2));
    row_hits_.push_back({ImVec2(r0.x, std::max(r0.y, y)), ImVec2(r1.x, std::min(r1.y, l1.y))});
    std::pair<ImVec2, ImVec2> slider{};
    DrawRow(dl, row, r0, r1, int(i) == page.selected, ca, &slider);
    slider_hits_.push_back(slider);
    ry += heights[i];
  }
  if (total > view) {
    // Scroll bar.
    const float bar_h = view * view / total;
    const float bar_y = y + (view - bar_h) * (page.scroll / (total - view));
    dl->AddRectFilled(ImVec2(l1.x - Px(4), bar_y), ImVec2(l1.x, bar_y + bar_h), WithAlpha(color::kTextDim, ca * 0.6f), Px(2));
  }
  dl->PopClipRect();
  dl->PopClipRect();
}

void GuideDialog::DrawRow(ImDrawList* dl, const Row& row, ImVec2 r0, ImVec2 r1, bool selected, float a,
                          std::pair<ImVec2, ImVec2>* slider) {
  if (selected) {
    dl->AddRectFilled(r0, r1, WithAlpha(color::kGreen, a), Px(6));
    dl->AddRectFilledMultiColor(r0, ImVec2(r1.x, (r0.y + r1.y) * 0.5f), WithAlpha(IM_COL32(255, 255, 255, 26), a),
                                WithAlpha(IM_COL32(255, 255, 255, 26), a), 0, 0);
  }
  const ImU32 text = WithAlpha(row.disabled ? color::kTextDim : color::kText, a);
  const ImU32 dim = WithAlpha(selected ? color::kSelectedSub : color::kTextDim, a);
  float x = r0.x + Px(18);
  const float h = r1.y - r0.y;
  const float cy = (r0.y + r1.y) * 0.5f;

  if (row.swatch) {
    const float s = Px(30);
    dl->AddRectFilled(ImVec2(x, cy - s * 0.5f), ImVec2(x + s, cy + s * 0.5f), WithAlpha(*row.swatch, a), Px(6));
    dl->AddRect(ImVec2(x, cy - s * 0.5f), ImVec2(x + s, cy + s * 0.5f), WithAlpha(IM_COL32(255, 255, 255, 90), a),
                Px(6), 0, Px(1.5f));
    x += s + Px(16);
  } else if (row.gamerpic >= 0 || row.icon) {
    const float s = row.sub.empty() ? (row.gamerpic >= 0 ? Px(36) : Px(42)) : Px(52);
    const ImVec2 i0(x, cy - s * 0.5f), i1(x + s, cy + s * 0.5f);
    const ImTextureID tex = row.gamerpic >= 0 ? textures_.Gamerpic(row.gamerpic) : row.icon;
    const ImU32 tint = row.icon_dim ? WithAlpha(IM_COL32(150, 150, 150, 255), a) : WithAlpha(color::kWhite, a);
    Image(dl, tex, i0, i1, tint, row.gamerpic >= 0 ? Px(4) : 0.0f);
    x += s + Px(16);
  }
  // Right side: avatars, glyph, detail.
  float right = r1.x - Px(18);
  for (auto it = row.avatars.rbegin(); it != row.avatars.rend(); ++it) {
    const float s = Px(34);
    Image(dl, textures_.Gamerpic(*it), ImVec2(right - s, cy - s * 0.5f), ImVec2(right, cy + s * 0.5f),
          WithAlpha(color::kWhite, a), s * 0.5f);
    right -= s - Px(8);
  }
  if (!row.avatars.empty()) right -= Px(18);
  if (row.slider) {
    // Track, the filled part, and a knob at the value.
    const float w = Px(220), bh = Px(8);
    const ImVec2 b0(right - w, cy - bh * 0.5f), b1(right, cy + bh * 0.5f);
    const ImU32 fill = WithAlpha(selected ? color::kWhite : color::kGreen, a);
    dl->AddRectFilled(b0, b1, WithAlpha(selected ? color::kWhite : color::kTextDim, a * 0.35f), bh * 0.5f);
    const float fx = b0.x + w * std::clamp(*row.slider, 0.0f, 1.0f);
    dl->AddRectFilled(b0, ImVec2(fx, b1.y), fill, bh * 0.5f);
    dl->AddCircleFilled(ImVec2(fx, cy), Px(11), fill, 20);
    if (slider) *slider = {ImVec2(b0.x - Px(12), r0.y), ImVec2(b1.x + Px(12), r1.y)};
    right -= w + Px(28);
  }
  if (row.glyph) {
    DrawGlyph(dl, *row.glyph, ImVec2(right - Px(16), cy), Px(32), text);
    right -= Px(44);
  }
  if (row.unread) {
    dl->AddCircleFilled(ImVec2(right - Px(6), cy), Px(5), WithAlpha(selected ? color::kWhite : color::kGreenLight, a));
    right -= Px(20);
  }
  if (!row.detail.empty()) {
    const float dw = CapsWidth(Semibold(), Px(14), row.detail, Px(1.2f));
    Caps(dl, Semibold(), Px(14), ImVec2(right - dw, cy - Px(8)), dim, row.detail, Px(1.2f));
    right -= dw + Px(18);
  }

  const float max_w = right - x;
  if (row.sub.empty()) {
    TextFit(dl, Regular(), Px(30), ImVec2(x, cy - Px(19)), text, row.label, max_w);
  } else {
    TextFit(dl, Regular(), Px(27), ImVec2(x, r0.y + h * 0.5f - Px(29)), text, row.label, max_w);
    TextFit(dl, Regular(), Px(19), ImVec2(x, r0.y + h * 0.5f + Px(4)), dim, row.sub, max_w);
  }
}

float GuideDialog::DrawColorPreview(ImDrawList* dl, ImVec2 pos, float right, int element, float a) {
  if (!draft_) return pos.y;
  const ImU32 color = draft_->theme.palette(draft_->dark).colors[std::clamp(element, 0, int(themes::kElementCount) - 1)];
  const ImVec2 c0(pos.x + Px(14), pos.y), c1(right - Px(4), pos.y + Px(84));
  // On a checkerboard, so its opacity shows.
  dl->PushClipRect(c0, c1, true);
  const float cell = Px(14);
  for (int row = 0; c0.y + row * cell < c1.y; ++row) {
    for (int col = 0; c0.x + col * cell < c1.x; ++col) {
      const ImVec2 p(c0.x + col * cell, c0.y + row * cell);
      const ImU32 check = (row + col) % 2 ? IM_COL32(200, 200, 200, 255) : IM_COL32(120, 120, 120, 255);
      dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), WithAlpha(check, a));
    }
  }
  dl->AddRectFilled(c0, c1, WithAlpha(color, a));
  dl->PopClipRect();
  dl->AddRect(c0, c1, WithAlpha(IM_COL32(255, 255, 255, 90), a), 0.0f, 0, Px(1.5f));
  // Dark lettering on light colours, light on dark.
  const float r = float((color >> IM_COL32_R_SHIFT) & 0xFF), g = float((color >> IM_COL32_G_SHIFT) & 0xFF),
              b = float((color >> IM_COL32_B_SHIFT) & 0xFF), alpha = float((color >> IM_COL32_A_SHIFT) & 0xFF);
  const bool light = alpha > 140.0f && (0.299f * r + 0.587f * g + 0.114f * b) > 150.0f;
  const ImU32 ink = WithAlpha(light ? IM_COL32(24, 26, 30, 255) : IM_COL32(250, 250, 252, 255), a);
  Text(dl, Semibold(), Px(30), ImVec2(c0.x + Px(18), (c0.y + c1.y) * 0.5f - Px(19)), ink, themes::Hex(color));
  const std::string mode = draft_->dark ? "Dark Mode" : "Light Mode";
  Caps(dl, Semibold(), Px(14), ImVec2(c1.x - CapsWidth(Semibold(), Px(14), mode, Px(1.2f)) - Px(16), (c0.y + c1.y) * 0.5f - Px(8)),
       ink, mode, Px(1.2f));
  return c1.y + Px(12);
}

float GuideDialog::DrawGamercard(ImDrawList* dl, ImVec2 pos, float right, const std::string& gamertag, float a) {
  Profile profile;
  std::string status;
  if (gamertag == backend_->me().gamertag) {
    profile = backend_->me();
    status = "Online";
  } else if (const Friend* f = FindFriend(gamertag)) {
    profile = f->profile;
    status = f->online ? (f->status.empty() ? "Online" : f->status) : "Last seen " + TimeAgo(f->last_seen);
  } else {
    profile.gamertag = gamertag;
  }
  const float s = Px(112);
  const ImVec2 c0(pos.x + Px(14), pos.y), c1(right - Px(4), pos.y + s + Px(20));
  dl->AddRectFilled(c0, c1, WithAlpha(IM_COL32(255, 255, 255, 16), a), Px(8));
  const ImVec2 i0(c0.x + Px(10), c0.y + Px(10));
  Image(dl, textures_.Gamerpic(profile.gamerpic), i0, ImVec2(i0.x + s, i0.y + s), WithAlpha(color::kWhite, a), Px(6));
  const float tx = i0.x + s + Px(20);
  Text(dl, Semibold(), Px(32), ImVec2(tx, i0.y + Px(2)), WithAlpha(color::kText, a), profile.gamertag);
  Text(dl, Regular(), Px(20), ImVec2(tx, i0.y + Px(44)), WithAlpha(color::kTextDim, a),
       std::to_string(profile.gamerscore) + " G");
  TextFit(dl, Regular(), Px(20), ImVec2(tx, i0.y + Px(70)), WithAlpha(color::kTextDim, a), status, right - tx - Px(14));
  if (!profile.motto.empty()) {
    const ImVec2 ms = Measure(Regular(), Px(18), "\"" + profile.motto + "\"");
    Text(dl, Regular(), Px(18), ImVec2(c1.x - ms.x - Px(14), c0.y + Px(14)), WithAlpha(color::kTextDim, a),
         "\"" + profile.motto + "\"");
  }
  return c1.y + Px(12);
}

void GuideDialog::DrawGamerpicGrid(ImDrawList* dl, ImVec2 pos, float bottom, Page& page, float a) {
  constexpr int kCols = 5;
  const float s = Px(104), gap = Px(16), pitch = s + gap;
  const std::vector<int> choices = backend_->gamerpic_choices();
  const int rows = (int(choices.size()) + kCols - 1) / kCols;
  const int visible = std::max(1, int((bottom - pos.y + gap) / pitch));
  const int sel_row = page.selected / kCols;
  float target = page.scroll;
  if (sel_row < target) target = float(sel_row);
  if (sel_row >= target + visible) target = float(sel_row - visible + 1);
  target = std::clamp(target, 0.0f, float(std::max(0, rows - visible)));
  page.scroll += (target - page.scroll) * std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f);
  for (int i = 0; i < int(choices.size()); ++i) {
    const ImVec2 i0(pos.x + (i % kCols) * pitch, pos.y + (float(i / kCols) - page.scroll) * pitch);
    const ImVec2 i1(i0.x + s, i0.y + s);
    grid_cells_.push_back({i0, i1});
    if (i1.y < pos.y - pitch || i0.y > bottom) continue;
    if (i == page.selected) {
      dl->AddRectFilled(ImVec2(i0.x - Px(6), i0.y - Px(6)), ImVec2(i1.x + Px(6), i1.y + Px(6)),
                        WithAlpha(color::kGreen, a), Px(8));
    }
    Image(dl, textures_.Gamerpic(choices[i]), i0, i1, WithAlpha(color::kWhite, a), Px(5));
  }
}

}  // namespace rexglue_guide
