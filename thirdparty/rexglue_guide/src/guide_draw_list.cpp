// Drawing a page's contents: rows, the gamer card and the gamerpic grid.

#include <algorithm>
#include <cctype>
#include <cfloat>
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

namespace {

// Small caps no wider than max_width, cut short with "..." if need be.
std::string FitCaps(std::string text, float max_width) {
  if (CapsWidth(Semibold(), Px(14), text, Px(1.2f)) <= max_width) return text;
  while (text.size() > 1 && CapsWidth(Semibold(), Px(14), text + "...", Px(1.2f)) > max_width) {
    text.pop_back();
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();  // whole UTF-8 characters
  }
  return text + "...";
}

}  // namespace

void GuideDialog::DrawList(ImDrawList* dl, ImVec2 l0, ImVec2 l1, float a) {
  DrawList(dl, list_, Stack(), l0, l1, a, slide_);
}

float GuideDialog::RowHeight(const Row& row, float width) const {
  if (row.header) return Px(44);
  if (!row.text.empty()) return Regular()->CalcTextSizeA(Px(22), FLT_MAX, width - Px(36), row.text.c_str()).y + Px(14);
  if (row.picture_aspect > 0.0f) {
    const float h = (width - Px(36)) / row.picture_aspect;
    return (row.picture_height > 0.0f ? std::min(h, Px(row.picture_height)) : h) + Px(12);
  }
  return row.sub.empty() ? Px(56) : Px(72);
}

void GuideDialog::DrawList(ImDrawList* dl, ListView& v, std::vector<Page>& stack, ImVec2 l0, ImVec2 l1, float a,
                           float slide) {
  Page& page = stack.back();
  v.row_hits.clear();
  v.slider_hits.clear();
  grid_cells_.clear();
  v.scroll_bar = {};  // (a grid or gallery page has none)
  // Pictures download only once the page stops moving (a list scrolled
  // through would want each row's on the way); until then, what's here.
  if (ImGui::GetIO().MouseWheel != 0.0f) last_wheel_ = time_;
  allow_downloads_ = true;
  const float slide_x = slide * Px(60);
  const float ca = a * (1.0f - std::min(1.0f, std::abs(slide)) * 0.9f);
  dl->PushClipRect(l0, l1, true);

  float y = l0.y;
  if (stack.size() > 1) {
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
  if (page.kind == PageKind::kPlayerGame) {
    const std::string summary = PlayerGameSummary(page);
    if (!summary.empty()) {
      Caps(dl, Semibold(), Px(14), ImVec2(l0.x + Px(16) + slide_x, y), WithAlpha(color::kTextDim, ca), summary, Px(1.2f));
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
  if (page.kind == PageKind::kGamerpics || page.kind == PageKind::kGamerpicGame) {
    DrawGamerpicGrid(dl, ImVec2(l0.x + Px(14) + slide_x, y + Px(6)), l1.y, page, ca);
    dl->PopClipRect();
    return;
  }

  // Row heights, then scroll so the selection stays in view.
  std::vector<float> heights;
  float total = 0.0f;
  for (const Row& r : v.rows) {
    heights.push_back(RowHeight(r, l1.x - l0.x));
    total += heights.back();
  }
  const float view = l1.y - y;
  float sel_top = 0.0f;
  for (int i = 0; i < page.selected && i < int(heights.size()); ++i) sel_top += heights[i];
  const float sel_h = page.selected < int(heights.size()) ? heights[page.selected] : 0.0f;
  float follow = page.scroll;
  if (sel_top < follow) follow = sel_top;
  if (sel_top + sel_h > follow + view) follow = sel_top + sel_h - view;
  const float target = ScrollTowards(page, follow, total - view);
  allow_downloads_ = !v.dragging_scroll && time_ - last_wheel_ > 0.25f && std::abs(target - page.scroll) < Px(4);
  if (page.free_scroll && !heights.empty() && (sel_top < target - 1.0f || sel_top + sel_h > target + view + 1.0f)) {
    // Scrolled off by the mouse: the selection moves to the nearest row on
    // screen, so pad and keys go on from what's shown.
    float top = 0.0f;
    int first = -1, last = -1;
    for (int i = 0; i < int(heights.size()); ++i) {
      if (v.rows[size_t(i)].Selectable() && top >= target - 1.0f && top + heights[i] <= target + view + 1.0f) {
        if (first < 0) first = i;
        last = i;
      }
      top += heights[i];
    }
    if (first >= 0) page.selected = sel_top < target ? first : last;
  }

  // Rows scroll under the page's title and header, not over them.
  dl->PushClipRect(ImVec2(l0.x, y), l1, true);
  float ry = y - page.scroll;
  for (size_t i = 0; i < v.rows.size(); ++i) {
    const Row& row = v.rows[i];
    const ImVec2 r0(l0.x + slide_x, ry), r1(l1.x + slide_x, ry + heights[i] - Px(2));
    v.row_hits.push_back({ImVec2(r0.x, std::max(r0.y, y)), ImVec2(r1.x, std::min(r1.y, l1.y))});
    std::pair<ImVec2, ImVec2> slider{};
    DrawRow(dl, row, r0, r1, int(i) == page.selected, ca, &slider);
    v.slider_hits.push_back(slider);
    ry += heights[i];
  }
  v.scroll_bar = {};
  if (total > view) {
    // Scroll bar: its knob is dragged with the mouse (HandleInput), the track
    // clicked to jump.
    const float bar_w = Px(8);
    const float bar_h = std::max(Px(40), view * view / total);
    const float bar_y = y + (view - bar_h) * std::clamp(page.scroll / (total - view), 0.0f, 1.0f);
    const ImVec2 t0(l1.x - bar_w, y), t1(l1.x, l1.y);
    v.scroll_bar = {.hit0 = ImVec2(t0.x - Px(14), t0.y), .hit1 = ImVec2(t1.x + Px(6), t1.y), .top = y,
                   .bottom = l1.y, .knob_y = bar_y, .knob_h = bar_h, .max = total - view, .shown = true};
    const bool hot = v.dragging_scroll || ImGui::IsMouseHoveringRect(v.scroll_bar.hit0, v.scroll_bar.hit1, false);
    dl->AddRectFilled(t0, t1, WithAlpha(IM_COL32(0, 0, 0, 40), ca), bar_w * 0.5f);
    dl->AddRectFilled(ImVec2(t0.x, bar_y), ImVec2(t1.x, bar_y + bar_h),
                      WithAlpha(hot ? color::kGreenLight : color::kTextDim, ca * (hot ? 1.0f : 0.7f)), bar_w * 0.5f);
    if (v.dragging_scroll && page.kind == PageKind::kGamerpicGames) {
      // Where in the alphabet: the first game in view's initial.
      float top = 0.0f;
      std::string letter;
      for (size_t i = 0; i < v.rows.size(); ++i, top += heights[i - 1]) {
        if (top + heights[i] * 0.5f < page.scroll || i == 0 || !v.rows[i].sub.empty() /* pinned: playing now, the dashboard */ || v.rows[i].label.empty()) continue;
        const unsigned char c = static_cast<unsigned char>(v.rows[i].label[0]);
        letter = std::isalpha(c) ? std::string(1, char(std::toupper(c))) : "#";
        break;
      }
      if (!letter.empty()) {
        const float s = Px(64);
        const ImVec2 b0(t0.x - Px(22) - s, bar_y + bar_h * 0.5f - s * 0.5f), b1(b0.x + s, b0.y + s);
        dl->PushClipRect(ImVec2(0, 0), display_, false);
        dl->AddRectFilled(b0, b1, WithAlpha(color::kGreen, ca), Px(10));
        const ImVec2 ls = Measure(Semibold(), Px(38), letter);
        Text(dl, Semibold(), Px(38), ImVec2((b0.x + b1.x - ls.x) * 0.5f, (b0.y + b1.y - ls.y) * 0.5f),
             WithAlpha(color::kWhite, ca), letter);
        dl->PopClipRect();
      }
    }
  }
  dl->PopClipRect();
  dl->PopClipRect();
}

void GuideDialog::DrawRow(ImDrawList* dl, const Row& row, ImVec2 r0, ImVec2 r1, bool selected, float a,
                          std::pair<ImVec2, ImVec2>* slider) {
  // Rows that aren't picked.
  if (row.header) {
    Caps(dl, Semibold(), Px(14), ImVec2(r0.x + Px(18), r1.y - Px(24)), WithAlpha(color::kTextDim, a), row.label, Px(1.2f));
    return;
  }
  if (!row.text.empty()) {
    dl->AddText(Regular(), Px(22), ImVec2(r0.x + Px(18), r0.y + Px(6)), WithAlpha(row.text_color.value_or(color::kTextDim), a),
                row.text.c_str(), nullptr, r1.x - r0.x - Px(36));
    return;
  }
  if (row.picture_aspect > 0.0f) {
    // As big as fits, keeping its shape, in the middle.
    const float w = r1.x - r0.x - Px(36), h = r1.y - r0.y - Px(10);
    const float pw = std::min(w, h * row.picture_aspect), ph = pw / row.picture_aspect;
    const ImVec2 i0(std::floor(r0.x + Px(18) + (w - pw) * 0.5f), r0.y + Px(4)), i1(i0.x + pw, i0.y + ph);
    if (row.picture) Image(dl, row.picture, i0, i1, WithAlpha(color::kWhite, a), Px(6));
    else dl->AddRectFilled(i0, i1, WithAlpha(IM_COL32(0, 0, 0, 90), a), Px(6));
    return;
  }
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
  } else if (row.gamerpic >= 0 || row.icon || row.lazy_icon) {
    const float s = row.sub.empty() ? (row.gamerpic >= 0 ? Px(36) : Px(42)) : Px(52);
    const ImVec2 i0(x, cy - s * 0.5f), i1(x + s, cy + s * 0.5f);
    const bool on_screen = r1.y >= dl->GetClipRectMin().y && r0.y <= dl->GetClipRectMax().y;
    bool loading = false;
    const ImTextureID tex = row.gamerpic >= 0 ? textures_.Gamerpic(row.gamerpic)
                            : row.icon        ? row.icon
                            : on_screen       ? row.lazy_icon(&loading)
                                              : ImTextureID(0);
    if (!tex && loading) Spinner(dl, ImVec2((i0.x + i1.x) * 0.5f, cy), s * 0.28f, time_, WithAlpha(color::kGreenLight, a));
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
  if (row.toggle) {
    // A switch: the knob to the right and the track filled when it's on.
    const bool on = *row.toggle;
    const float tw = Px(56), th = Px(30);
    const ImVec2 t0(right - tw, cy - th * 0.5f), t1(right, cy + th * 0.5f);
    const ImU32 track = selected ? color::kWhite : on ? color::kGreen : color::kTextDim;
    dl->AddRectFilled(t0, t1, WithAlpha(track, a * (on ? 1.0f : 0.35f)), th * 0.5f);
    const ImVec2 knob(on ? t1.x - th * 0.5f : t0.x + th * 0.5f, cy);
    dl->AddCircleFilled(knob, th * 0.5f - Px(4), WithAlpha(on && selected ? color::kGreen : color::kWhite, a), 20);
    right -= tw + Px(18);
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
    // (At most half the row, so the label shows.)
    const float arrow = Px(6);
    const std::string detail = FitCaps(row.detail, (r1.x - r0.x) * 0.5f);
    const float dw = CapsWidth(Semibold(), Px(14), detail, Px(1.2f));
    if (row.step) {
      // Arrows either side: left and right change it.
      const ImU32 col = WithAlpha(selected ? color::kWhite : color::kTextDim, a);
      dl->AddTriangleFilled(ImVec2(right - arrow, cy - arrow), ImVec2(right - arrow, cy + arrow), ImVec2(right, cy), col);
      right -= arrow + Px(10);
      const float lx = right - dw - Px(10);
      dl->AddTriangleFilled(ImVec2(lx, cy - arrow), ImVec2(lx, cy + arrow), ImVec2(lx - arrow, cy), col);
    }
    Caps(dl, Semibold(), Px(14), ImVec2(right - dw, cy - Px(8)), dim, detail, Px(1.2f));
    right -= dw + Px(18) + (row.step ? arrow + Px(10) : 0.0f);
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
  float badge_y = c0.y + Px(14);
  if (!profile.motto.empty()) {
    const ImVec2 ms = Measure(Regular(), Px(18), "\"" + profile.motto + "\"");
    Text(dl, Regular(), Px(18), ImVec2(c1.x - ms.x - Px(14), c0.y + Px(14)), WithAlpha(color::kTextDim, a),
         "\"" + profile.motto + "\"");
    badge_y += ms.y + Px(8);
  }
  // The systems they play on under the motto, the first one played on leftmost.
  const float badge = Px(26), gap = Px(6);
  float bx = c1.x - Px(14) - float(profile.platforms.size()) * (badge + gap) + gap;
  for (const std::string& platform : profile.platforms) {
    PlatformBadge(dl, textures_, platform, ImVec2(bx, badge_y), badge, a);
    bx += badge + gap;
  }
  return c1.y + Px(12);
}

void GuideDialog::DrawGamerpicGrid(ImDrawList* dl, ImVec2 pos, float bottom, Page& page, float a) {
  constexpr int kCols = 5;
  const float s = Px(104), gap = Px(16), pitch = s + gap;
  const int count = GridCount(page);
  if (page.kind == PageKind::kGamerpicGame && count == 0) {
    std::string error;
    const std::vector<std::string>* ids = GameGamerpicIds(page.arg, &error);
    const bool loading = !ids && error.empty();
    if (loading) Spinner(dl, ImVec2(pos.x + Px(16), pos.y + Px(22)), Px(12), time_, WithAlpha(color::kGreenLight, a));
    Text(dl, Regular(), Px(22), ImVec2(pos.x + (loading ? Px(40) : Px(4)), pos.y + Px(8)), WithAlpha(color::kTextDim, a),
         loading ? "Loading..." : !ids ? error : "This game has no gamerpics");
    return;
  }
  const int rows = (count + kCols - 1) / kCols;
  const float fit = (bottom - pos.y + gap) / pitch;  // rows the view holds, part ones too
  const int visible = std::max(1, int(fit));
  const int sel_row = page.selected / kCols;
  float follow = page.scroll;
  if (sel_row < follow) follow = float(sel_row);
  if (sel_row >= follow + visible) follow = float(sel_row - visible + 1);
  const float target = ScrollTowards(page, follow, float(rows) - fit);
  allow_downloads_ = time_ - last_wheel_ > 0.25f && std::abs(target - page.scroll) < 0.05f;
  if (page.free_scroll) page.selected = KeepInGrid(page.selected, count, kCols, target, fit);
  for (int i = 0; i < count; ++i) {
    const ImVec2 i0(pos.x + (i % kCols) * pitch, pos.y + (float(i / kCols) - page.scroll) * pitch);
    const ImVec2 i1(i0.x + s, i0.y + s);
    grid_cells_.push_back({i0, i1});
    if (i1.y < pos.y - pitch || i0.y > bottom) continue;
    if (i == page.selected) {
      dl->AddRectFilled(ImVec2(i0.x - Px(6), i0.y - Px(6)), ImVec2(i1.x + Px(6), i1.y + Px(6)),
                        WithAlpha(color::kGreen, a), Px(8));
    }
    // (Only the ones on screen: a game gamerpic downloads when it's drawn.)
    bool loading = false;
    if (const ImTextureID picture = GridPicture(page, i, &loading)) {
      Image(dl, picture, i0, i1, WithAlpha(color::kWhite, a), Px(5));
    } else {
      dl->AddRectFilled(i0, i1, WithAlpha(IM_COL32(255, 255, 255, 24), a), Px(5));
      if (loading) Spinner(dl, ImVec2((i0.x + i1.x) * 0.5f, (i0.y + i1.y) * 0.5f), Px(14), time_, WithAlpha(color::kGreenLight, a));
    }
  }
}

}  // namespace rexglue_guide
