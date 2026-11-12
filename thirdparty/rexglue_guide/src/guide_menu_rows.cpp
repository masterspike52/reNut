// App menus' rows with places of their own: a Bar's buttons side by side, and a
// Lists row's two lists. How they're drawn, and what the pad and mouse do in them.

#include <algorithm>
#include <string>
#include <vector>

#include "audio.h"
#include "guide_dialog.h"
#include "imgui.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

namespace {

constexpr float kListHead = 36.0f;  // a list's title
constexpr float kItemHeight = 64.0f;
constexpr float kArrowRadius = 14.0f;
constexpr float kArrowStep = 34.0f;

enum class Dir { kLeft, kRight, kUp, kDown };

void Arrow(ImDrawList* dl, ImVec2 c, Dir dir, float size, ImU32 col) {
  const float k = size * 0.5f;
  switch (dir) {
    case Dir::kLeft: dl->AddTriangleFilled(ImVec2(c.x - k, c.y), ImVec2(c.x + k * 0.6f, c.y - k), ImVec2(c.x + k * 0.6f, c.y + k), col); break;
    case Dir::kRight: dl->AddTriangleFilled(ImVec2(c.x + k, c.y), ImVec2(c.x - k * 0.6f, c.y + k), ImVec2(c.x - k * 0.6f, c.y - k), col); break;
    case Dir::kUp: dl->AddTriangleFilled(ImVec2(c.x, c.y - k), ImVec2(c.x + k, c.y + k * 0.6f), ImVec2(c.x - k, c.y + k * 0.6f), col); break;
    case Dir::kDown: dl->AddTriangleFilled(ImVec2(c.x, c.y + k), ImVec2(c.x - k, c.y - k * 0.6f), ImVec2(c.x + k, c.y - k * 0.6f), col); break;
  }
}

// The selection's look, as the Guide's rows have it.
void Lit(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float a) {
  dl->AddRectFilled(p0, p1, WithAlpha(color::kGreen, a), Px(6));
  dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, (p0.y + p1.y) * 0.5f), WithAlpha(IM_COL32(255, 255, 255, 26), a), WithAlpha(IM_COL32(255, 255, 255, 26), a), 0, 0);
}

// The selected thing's place in list c: where its id is now, else where it
// was. -1 when the list is empty.
int Selected(const RowLists& lists, InnerState& state, int c) {
  const auto& items = lists.items[c];
  if (items.empty()) return -1;
  for (int i = 0; i < int(items.size()); ++i) {
    if (items[size_t(i)].id == state.item[c]) return state.index[c] = i;
  }
  return std::clamp(state.index[c], 0, int(items.size()) - 1);
}

void Select(const RowLists& lists, InnerState& state, int c, int i) {
  state.column = c;
  state.index[c] = i;
  if (i >= 0 && i < int(lists.items[c].size())) state.item[c] = lists.items[c][size_t(i)].id;
  state.free_scroll[c] = false;
}

// Where a thing sent across lands: the end of the right list, or the left's top (that list is the app's to sort).
int AcrossIndex(const RowLists& lists, int to) {
  return to == 1 ? int(lists.items[1].size()) : 0;
}

// Sends the thing at (c, i) across, the selection staying in its list on the
// thing that takes its place.
void SendAcross(const Row& row, InnerState& state, int c, int i) {
  const RowLists& lists = *row.lists;
  const auto& items = lists.items[c];
  const int to = 1 - c;
  row.lists_done({.id = items[size_t(i)].id, .list = to, .index = AcrossIndex(lists, to)});
  const int next = i + 1 < int(items.size()) ? i + 1 : i - 1;
  state.item[c] = next >= 0 ? items[size_t(next)].id : std::string();
  state.index[c] = std::max(0, std::min(i, int(items.size()) - 2));
  audio::Play(Sound::kMove);
}

void Carry(const Row& row, InnerState& state, const std::string& id, int to, int index) {
  row.lists_done({.id = id, .list = to, .index = index});
  state.column = to;
  state.item[to] = id;
  state.index[to] = index;
  state.free_scroll[to] = false;
  audio::Play(Sound::kMove);
}

}  // namespace

void GuideDialog::InnerInput(NavInput& in, const Row& row, InnerState& state) {
  if (!row.bar.empty()) {
    const int n = int(row.bar.size());
    state.column = std::clamp(state.column, 0, n - 1);
    if (in.left && state.column > 0) {
      --state.column;
      in.left = false;
      audio::Play(Sound::kMove);
    }
    if (in.right && state.column < n - 1) {
      ++state.column;
      in.right = false;
      audio::Play(Sound::kMove);
    }
    if (in.accept) {
      audio::Play(Sound::kSelect);
      row.bar_chosen(state.column);
      in.accept = false;
    }
    return;
  }

  const RowLists& lists = *row.lists;
  int c = std::clamp(state.column, 0, 1);
  if (lists.items[c].empty() && !lists.items[1 - c].empty()) c = 1 - c;
  state.column = c;
  const int i = Selected(lists, state, c);
  const int n = int(lists.items[c].size());

  if (state.held) {
    // Carried: the directions move it, and any face button puts it down.
    if (i >= 0) {
      const std::string id = lists.items[c][size_t(i)].id;
      if (in.up && c == 1 && i > 0) Carry(row, state, id, 1, i - 1);
      if (in.down && c == 1 && i + 1 < n) Carry(row, state, id, 1, i + 1);
      if (in.left && c == 1) Carry(row, state, id, 0, AcrossIndex(lists, 0));
      if (in.right && c == 0) Carry(row, state, id, 1, std::min(i, int(lists.items[1].size())));
    }
    if (i < 0 || in.accept || in.back || in.x || in.y) {
      state.held = false;
      if (i >= 0) audio::Play(Sound::kSelect);
    }
    in.up = in.down = in.left = in.right = in.accept = in.back = in.x = in.y = false;
    return;
  }

  if (in.up && i > 0) {
    Select(lists, state, c, i - 1);
    in.up = false;
    audio::Play(Sound::kMove);
  }
  if (in.down && i >= 0 && i + 1 < n) {
    Select(lists, state, c, i + 1);
    in.down = false;
    audio::Play(Sound::kMove);
  }
  const int other = 1 - c;
  if (((in.left && c == 1) || (in.right && c == 0)) && !lists.items[other].empty()) {
    state.column = other;
    state.free_scroll[other] = false;
    in.left = in.right = false;
    audio::Play(Sound::kMove);
  }
  if (i < 0) return;
  if (in.accept) {
    audio::Play(Sound::kSelect);
    row.lists_done({.open = true, .id = lists.items[c][size_t(i)].id});
    in.accept = false;
  } else if (in.x) {
    SendAcross(row, state, c, i);
    in.x = false;
  } else if (in.y) {
    state.held = true;
    in.y = false;
    audio::Play(Sound::kSelect);
  }
}

GuideDialog::InnerMouse GuideDialog::InnerMouseInput(ListView& v, Page& page) {
  const ImGuiIO& io = ImGui::GetIO();
  const bool moved = io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
  const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  InnerMouse result;
  // Last drawn on top: a thing's arrows before the thing, the thing before its list.
  for (auto it = v.inner_hits.rbegin(); it != v.inner_hits.rend(); ++it) {
    const InnerHit& hit = *it;
    if (hit.row >= int(v.rows.size()) || !ImGui::IsMouseHoveringRect(hit.p0, hit.p1, false)) continue;
    const Row& row = v.rows[size_t(hit.row)];
    InnerState& state = page.inner[row.key];
    if (hit.kind == InnerHit::Kind::kButton) {
      if (moved || clicked) {
        page.selected = hit.row;
        state.column = hit.column;
      }
      if (clicked) {
        audio::Play(Sound::kSelect);
        row.bar_chosen(hit.column);
        result.clicked = true;
      }
      return result;
    }
    if (!row.lists) continue;
    const RowLists& lists = *row.lists;
    if (hit.kind == InnerHit::Kind::kList) {
      if (io.MouseWheel != 0.0f) {
        state.free_scroll[hit.column] = true;
        state.scroll[hit.column] -= io.MouseWheel * Px(kItemHeight * 2.0f);
        result.wheel = true;
      }
      continue;
    }
    if (hit.index >= int(lists.items[hit.column].size())) continue;
    const std::string& id = lists.items[hit.column][size_t(hit.index)].id;
    if (moved || clicked) {
      page.selected = hit.row;
      state.column = hit.column;
      state.item[hit.column] = id;
      state.index[hit.column] = hit.index;
    }
    if (!clicked) continue;
    state.held = false;
    result.clicked = true;
    switch (hit.kind) {
      case InnerHit::Kind::kItem:
        audio::Play(Sound::kSelect);
        row.lists_done({.open = true, .id = id});
        break;
      case InnerHit::Kind::kAcross: SendAcross(row, state, hit.column, hit.index); break;
      case InnerHit::Kind::kUp: Carry(row, state, id, 1, hit.index - 1); break;
      case InnerHit::Kind::kDown: Carry(row, state, id, 1, hit.index + 1); break;
      default: break;
    }
    return result;
  }
  return result;
}

std::vector<GuideDialog::LegendHint> GuideDialog::InnerHints(const ListView& v, Page& page) {
  if (page.selected < 0 || page.selected >= int(v.rows.size())) return {};
  const Row& row = v.rows[size_t(page.selected)];
  if (!row.lists) return {};
  const InnerState& state = page.inner[row.key];
  if (state.held) return {{'Y', "Y", "Put Down"}};
  const int c = std::clamp(state.column, 0, 1);
  if (row.lists->items[c].empty()) return {};
  return {{'X', "X", row.lists->across[c]}, {'Y', "Y", "Pick Up"}};
}

void GuideDialog::DrawInnerRow(ImDrawList* dl, ListView& v, Page& page, int index, ImVec2 r0, ImVec2 r1, bool selected, float a) {
  const Row& row = v.rows[size_t(index)];
  InnerState& state = page.inner[row.key];
  const ImVec2 clip0 = dl->GetClipRectMin(), clip1 = dl->GetClipRectMax();
  const auto hit = [&](InnerHit::Kind kind, ImVec2 p0, ImVec2 p1, int column, int item) {
    p0 = ImVec2(std::max(p0.x, clip0.x), std::max(p0.y, clip0.y));
    p1 = ImVec2(std::min(p1.x, clip1.x), std::min(p1.y, clip1.y));
    if (p1.x > p0.x && p1.y > p0.y) v.inner_hits.push_back({kind, p0, p1, index, column, item});
  };
  const auto hovering = [&](ImVec2 p0, ImVec2 p1) {
    return ImGui::IsMouseHoveringRect(ImVec2(std::max(p0.x, clip0.x), std::max(p0.y, clip0.y)), ImVec2(std::min(p1.x, clip1.x), std::min(p1.y, clip1.y)), false);
  };

  if (!row.bar.empty()) {
    const int n = int(row.bar.size());
    const float gap = Px(8);
    const float w = (r1.x - r0.x - gap * float(n - 1)) / float(n);
    for (int k = 0; k < n; ++k) {
      const ImVec2 b0(std::floor(r0.x + float(k) * (w + gap)), r0.y), b1(std::floor(r0.x + float(k) * (w + gap) + w), r1.y);
      if (selected && state.column == k) Lit(dl, b0, b1, a);
      else dl->AddRectFilled(b0, b1, WithAlpha(IM_COL32(255, 255, 255, hovering(b0, b1) ? 34 : 16), a), Px(6));
      const ImVec2 s = Measure(Regular(), Px(26), row.bar[size_t(k)]);
      const float tw = std::min(s.x, w - Px(20));
      TextFit(dl, Regular(), Px(26), ImVec2(std::floor((b0.x + b1.x - tw) * 0.5f), std::floor((b0.y + b1.y - s.y) * 0.5f)), WithAlpha(color::kText, a), row.bar[size_t(k)], w - Px(20));
      hit(InnerHit::Kind::kButton, b0, b1, k, 0);
    }
    return;
  }

  const RowLists& lists = *row.lists;
  const float gap = Px(12);
  const float column_w = std::floor((r1.x - r0.x - gap) * 0.5f);
  const float dt = ImGui::GetIO().DeltaTime;
  for (int c = 0; c < 2; ++c) {
    const auto& items = lists.items[c];
    const int n = int(items.size());
    const float x0 = r0.x + float(c) * (column_w + gap), x1 = x0 + column_w;
    const bool active = selected && state.column == c;
    dl->AddRectFilled(ImVec2(x0, r0.y), ImVec2(x1, r1.y), WithAlpha(IM_COL32(0, 0, 0, 60), a), Px(8));
    const std::string title = lists.title[c] + "  " + std::to_string(n);
    Caps(dl, Semibold(), Px(14), ImVec2(x0 + Px(16), r0.y + Px(12)), WithAlpha(active ? color::kText : color::kTextDim, a), title, Px(1.2f));

    // The list scrolls on its own: after the selection, or where the wheel put it.
    const ImVec2 a0(x0, r0.y + Px(kListHead)), a1(x1, r1.y - Px(4));
    const float view = a1.y - a0.y, ih = Px(kItemHeight);
    const float max_scroll = std::max(0.0f, float(n) * ih - view);
    const int sel = Selected(lists, state, c);
    float target = std::clamp(state.scroll[c], 0.0f, max_scroll);
    if (!state.free_scroll[c] && sel >= 0) target = std::clamp(std::clamp(target, float(sel + 1) * ih - view, float(sel) * ih), 0.0f, max_scroll);
    state.scroll[c] += (target - state.scroll[c]) * std::min(1.0f, dt * 16.0f);
    if (state.free_scroll[c]) state.scroll[c] = target;
    hit(InnerHit::Kind::kList, a0, a1, c, 0);

    const bool bar = max_scroll > 0.0f;
    dl->PushClipRect(a0, a1, true);
    const ImVec2 lclip0 = dl->GetClipRectMin(), lclip1 = dl->GetClipRectMax();
    const int first = std::max(0, int(state.scroll[c] / ih));
    for (int k = first; k < n; ++k) {
      const float y = a0.y + float(k) * ih - state.scroll[c];
      if (y > a1.y) break;
      const RowLists::Item& item = items[size_t(k)];
      const ImVec2 p0(x0 + Px(6), y + Px(2)), p1(x1 - Px(bar ? 16 : 6), y + ih - Px(2));
      const bool hovered = ImGui::IsMouseHoveringRect(ImVec2(p0.x, std::max(p0.y, lclip0.y)), ImVec2(p1.x, std::min(p1.y, lclip1.y)), false);
      const bool lit = active && sel == k;
      if (lit) {
        Lit(dl, p0, p1, a);
        if (state.held) dl->AddRect(p0, p1, WithAlpha(color::kWhite, a), Px(6), 0, Px(2.5f));
      } else if (selected && sel == k) {
        dl->AddRect(p0, p1, WithAlpha(color::kTextDim, a * 0.6f), Px(6), 0, Px(1.5f));
      } else if (hovered) {
        dl->AddRectFilled(p0, p1, WithAlpha(IM_COL32(255, 255, 255, 14), a), Px(6));
      }
      const float cy = (p0.y + p1.y) * 0.5f;
      const float s = Px(44);
      bool loading = false;
      const ImTextureID icon = item.icon ? item.icon(&loading) : ImTextureID(0);
      const ImVec2 i0(p0.x + Px(10), cy - s * 0.5f);
      if (!icon && loading) Spinner(dl, ImVec2(i0.x + s * 0.5f, cy), s * 0.28f, time_, WithAlpha(color::kGreenLight, a));
      Image(dl, icon, i0, ImVec2(i0.x + s, i0.y + s), WithAlpha(color::kWhite, a), 0.0f);
      hit(InnerHit::Kind::kItem, ImVec2(p0.x, std::max(p0.y, a0.y)), ImVec2(p1.x, std::min(p1.y, a1.y)), c, k);

      // Arrows on the thing under the mouse or selected: across, and on the right, up and down.
      float right = p1.x - Px(10);
      if (hovered || lit) {
        std::vector<std::pair<Dir, InnerHit::Kind>> arrows;
        if (c == 1) arrows = {{Dir::kDown, InnerHit::Kind::kDown}, {Dir::kUp, InnerHit::Kind::kUp}, {Dir::kLeft, InnerHit::Kind::kAcross}};
        else arrows = {{Dir::kRight, InnerHit::Kind::kAcross}};
        for (const auto& [dir, kind] : arrows) {
          const ImVec2 ac(right - Px(kArrowRadius), cy);
          const bool usable = !(kind == InnerHit::Kind::kUp && k == 0) && !(kind == InnerHit::Kind::kDown && k + 1 == n);
          const ImVec2 h0(ac.x - Px(kArrowRadius), ac.y - Px(kArrowRadius)), h1(ac.x + Px(kArrowRadius), ac.y + Px(kArrowRadius));
          const bool over = usable && hovering(h0, h1);
          dl->AddCircleFilled(ac, Px(kArrowRadius), WithAlpha(over ? color::kGreenLight : IM_COL32(0, 0, 0, 90), a), 20);
          Arrow(dl, ac, dir, Px(12), WithAlpha(color::kWhite, a * (usable ? 1.0f : 0.3f)));
          if (usable) hit(kind, h0, h1, c, k);
          right -= Px(kArrowStep);
        }
        right -= Px(4);
      }
      const float tx = i0.x + s + Px(14), tw = right - tx;
      const ImU32 dim = WithAlpha(lit ? color::kSelectedSub : color::kTextDim, a);
      if (item.sub.empty()) {
        TextFit(dl, Regular(), Px(26), ImVec2(tx, cy - Px(16)), WithAlpha(color::kText, a), item.label, tw);
      } else {
        TextFit(dl, Regular(), Px(24), ImVec2(tx, cy - Px(26)), WithAlpha(color::kText, a), item.label, tw);
        TextFit(dl, Regular(), Px(18), ImVec2(tx, cy + Px(3)), dim, item.sub, tw);
      }
    }
    dl->PopClipRect();
    if (bar) {
      const float bh = std::max(Px(30), view * view / (float(n) * ih));
      const float by = a0.y + (view - bh) * std::clamp(state.scroll[c] / max_scroll, 0.0f, 1.0f);
      dl->AddRectFilled(ImVec2(x1 - Px(10), by), ImVec2(x1 - Px(5), by + bh), WithAlpha(color::kTextDim, a * 0.7f), Px(3));
    }
  }
}

}  // namespace rexglue_guide
