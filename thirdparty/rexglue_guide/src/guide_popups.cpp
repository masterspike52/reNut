// Message boxes, the busy box and toasts.

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <utility>

#include "audio.h"
#include "guide_dialog.h"
#include "imgui.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

void GuideDialog::ShowMessage(Prompt box) {
  box.selected = 0;
  message_ = std::move(box);
}

void GuideDialog::DrawMessage(ImDrawList* dl) {
  const Prompt& box = *message_;
  dl->AddRectFilled(ImVec2(0, 0), display_, IM_COL32(0, 0, 0, 110));
  const float w = Px(640);
  const float wrap = w - Px(64);
  const ImVec2 body = Regular()->CalcTextSizeA(Px(22), FLT_MAX, wrap, box.body.c_str());
  const float h = Px(84) + body.y + Px(24) + Px(56) * float(box.buttons.size()) + Px(20);
  const ImVec2 p0(std::floor((display_.x - w) * 0.5f), std::floor((display_.y - h) * 0.5f)), p1(p0.x + w, p0.y + h);
  Shadow(dl, p0, p1, Px(12), Px(32), 1.0f);
  dl->AddRectFilled(p0, p1, color::kBlade, Px(12));
  Text(dl, Semibold(), Px(30), ImVec2(p0.x + Px(32), p0.y + Px(24)), color::kText, box.title);
  dl->AddText(Regular(), Px(22), ImVec2(p0.x + Px(32), p0.y + Px(76)), color::kTextDim, box.body.c_str(), nullptr, wrap);
  message_buttons_.clear();
  float y = p0.y + Px(84) + body.y + Px(20);
  for (size_t i = 0; i < box.buttons.size(); ++i) {
    const ImVec2 b0(p0.x + Px(24), y), b1(p1.x - Px(24), y + Px(52));
    if (int(i) == box.selected) dl->AddRectFilled(b0, b1, color::kGreen, Px(6));
    Text(dl, Regular(), Px(26), ImVec2(b0.x + Px(18), b0.y + Px(10)), color::kText, box.buttons[i]);
    message_buttons_.push_back({b0, b1});
    y += Px(56);
  }
}

void GuideDialog::DrawBusy(ImDrawList* dl) {
  dl->AddRectFilled(ImVec2(0, 0), display_, IM_COL32(0, 0, 0, 110));
  const ImVec2 ts = Measure(Regular(), Px(26), *busy_);
  const float w = std::max(Px(420), ts.x + Px(140)), h = Px(96);
  const ImVec2 p0(std::floor((display_.x - w) * 0.5f), std::floor((display_.y - h) * 0.5f)), p1(p0.x + w, p0.y + h);
  dl->AddRectFilled(p0, p1, color::kBlade, Px(12));
  Spinner(dl, ImVec2(p0.x + Px(52), (p0.y + p1.y) * 0.5f), Px(18), time_, color::kGreenLight);
  Text(dl, Regular(), Px(26), ImVec2(p0.x + Px(92), (p0.y + p1.y) * 0.5f - ts.y * 0.5f), color::kText, *busy_);
}

void GuideDialog::DrawToasts(ImDrawList* dl, float dt) {
  if (toasts_.empty()) return;
  Toast& t = toasts_.front();
  if (t.t == 0.0f) audio::Play(Sound::kNotify);
  t.t += dt;
  if (t.t >= kToastTime) {
    toasts_.pop_front();
    return;
  }
  const float in = Ease(t.t / 0.3f);
  const float out = 1.0f - std::clamp((t.t - (kToastTime - 0.4f)) / 0.4f, 0.0f, 1.0f);
  const float a = in * out;
  const float h = Px(76);
  const float text_w = std::max(Measure(Semibold(), Px(24), t.line1).x, Measure(Regular(), Px(20), t.line2).x);
  const float w = std::max(Px(420), text_w + h + Px(48));
  const ImVec2 p0(std::floor((display_.x - w) * 0.5f), std::floor(display_.y - Px(150) + (1.0f - in) * Px(30)));
  const ImVec2 p1(p0.x + w, p0.y + h);
  Shadow(dl, p0, p1, h * 0.5f, Px(20), a);
  dl->AddRectFilled(p0, p1, WithAlpha(color::kPopup, a), h * 0.5f);
  dl->AddRect(p0, p1, WithAlpha(IM_COL32(255, 255, 255, 30), a), h * 0.5f, 0, Px(1.5f));
  const ImVec2 ic(p0.x + h * 0.5f, p0.y + h * 0.5f);
  const float r = h * 0.5f - Px(9);
  if (t.ring_player >= 0) {
    // A sign-in: the Ring of Light around the gamerpic, the player's quarter lit.
    DrawRing(dl, ic, a, t.ring_player, r + Px(4), Px(3.5f));
  } else {
    dl->AddCircleFilled(ic, r + Px(3), WithAlpha(color::kGreen, a), 40);
  }
  if (t.gamerpic >= 0) {
    Image(dl, textures_.Gamerpic(t.gamerpic), ImVec2(ic.x - r, ic.y - r), ImVec2(ic.x + r, ic.y + r), WithAlpha(color::kWhite, a), r);
  } else if (t.icon) {
    Image(dl, t.icon, ImVec2(ic.x - r, ic.y - r), ImVec2(ic.x + r, ic.y + r), WithAlpha(color::kWhite, a), r);
  } else {
    dl->AddCircleFilled(ic, r, WithAlpha(IM_COL32(40, 48, 58, 255), a), 40);
    DrawGlyph(dl, t.glyph.value_or(Glyph::kInfo), ic, r * 1.3f, WithAlpha(color::kText, a));
  }
  Text(dl, Semibold(), Px(24), ImVec2(p0.x + h + Px(8), p0.y + Px(10)), WithAlpha(color::kText, a), t.line1);
  Text(dl, Regular(), Px(20), ImVec2(p0.x + h + Px(8), p0.y + Px(40)), WithAlpha(color::kTextDim, a), t.line2);
}

void GuideDialog::ShowToast(Toast toast) {
  if (!popups_) return;
  toasts_.push_back(std::move(toast));
}

void GuideDialog::OnNotification(const Notification& n) {
  Toast t;
  t.gamerpic = n.from.empty() ? -1 : n.from_gamerpic;
  if (n.from.empty()) t.glyph = Glyph::kInfo;
  t.line1 = n.from.empty() ? "RexLiveless" : n.from;
  t.line2 = n.text;
  ShowToast(std::move(t));
}

}  // namespace rexglue_guide
