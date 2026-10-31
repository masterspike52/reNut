// Drawing the blades: the blurred game, blade stack, header and legend, sign-in.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "guide_dialog.h"
#include "imgui.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

void GuideDialog::Draw(ImDrawList* dl) {
  const float a = Ease(open_t_);
  // The live game blurred behind it (as on the console), darkened so the
  // blades stand out; without the app's frame, just darkened.
  blurred_ = DrawBlurredGame(dl, a);
  // (Less over the blurred game: the blur already sets the blades apart.)
  dl->AddRectFilled(ImVec2(0, 0), display_, WithAlpha(color::kBackdrop, a * (blurred_ ? 0.65f : 1.0f)));
  slide_ += (0.0f - slide_) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
  if (std::abs(slide_) < 0.002f) slide_ = 0.0f;

  list_.rows = BuildRows(Current());
  if (screen_ == Screen::kSignIn) DrawSignIn(dl, a);
  else DrawGuide(dl, a);

  if (message_) DrawMessage(dl);
  if (busy_) DrawBusy(dl);
}

bool GuideDialog::DrawBlurredGame(ImDrawList* dl, float a) {
  if (!config_.game_frame) return false;
  float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  rex::ui::ImmediateTexture* frame = config_.game_frame(uv);
  if (!frame || display_.x <= 0 || display_.y <= 0) return false;
  const ImTextureID tex = reinterpret_cast<ImTextureID>(frame);
  // How blurred is the theme's (0.5: the console's look).
  const float blur = ShownTheme().backdrop_blur;
  if (blur < 0.01f) {
    dl->AddImage(tex, ImVec2(0, 0), display_, ImVec2(uv[0], uv[1]), ImVec2(uv[2], uv[3]), WithAlpha(color::kWhite, a));
    return true;
  }

  // 0.5 is a sigma of 10 (at 1080p), 1.0 about 14. Wider blurs take more
  // copies, so they stay close enough for linear filtering to fill between.
  const float sigma = Px(10) * std::sqrt(2.0f * blur);
  const int kHalf = sigma > Px(10.5f) ? 5 : 4;  // 11 x 11 or 9 x 9 copies
  const float step = 2.2f * sigma / float(kHalf);
  struct Tap {
    float x, y, w;
  };
  std::vector<Tap> taps;
  float total = 0.0f;
  for (int y = -kHalf; y <= kHalf; ++y) {
    for (int x = -kHalf; x <= kHalf; ++x) {
      const float ox = float(x) * step, oy = float(y) * step;
      const float w = std::exp(-(ox * ox + oy * oy) / (2.0f * sigma * sigma));
      taps.push_back({ox, oy, w});
      total += w;
    }
  }
  // Lightest first: their blend factors are the smallest, where 8-bit
  // rounding matters least.
  std::sort(taps.begin(), taps.end(), [](const Tap& l, const Tap& r) { return l.w < r.w; });
  const float du = (uv[2] - uv[0]) / display_.x, dv = (uv[3] - uv[1]) / display_.y;
  float after = 0.0f;  // weight of the copies still to come
  for (const Tap& t : taps) after += t.w / total * a;
  for (const Tap& t : taps) {
    const float w = t.w / total * a;
    after -= w;
    const float blend = w / std::max(1e-6f, 1.0f - after);
    const ImVec2 o(t.x * du, t.y * dv);
    dl->AddImage(tex, ImVec2(0, 0), display_, ImVec2(uv[0] + o.x, uv[1] + o.y), ImVec2(uv[2] + o.x, uv[3] + o.y),
                 WithAlpha(color::kWhite, blend));
  }
  return true;
}

void GuideDialog::DrawThemeBackground(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float round, float a) {
  const themes::Theme& t = ShownTheme();
  std::string id, image;
  if (draft_ && draft_->image == ThemeDraft::Image::kNew) {
    id = "draft" + std::to_string(draft_->image_serial);
    image = draft_->image_file;
  } else if (!(draft_ && draft_->image == ThemeDraft::Image::kRemoved) && t.server_id && t.has_image) {
    id = t.key + "_" + std::to_string(t.updated);
    image = backend_->ThemeImage(t.server_id, t.updated);
  }
  ImVec2 size;
  const ImTextureID tex = background_.Get(textures_, id, image, t.image_blur, &size);
  const ImVec2 area(p1.x - p0.x, p1.y - p0.y);
  if (!tex || size.x <= 0 || size.y <= 0 || area.x <= 0 || area.y <= 0) return;
  // It covers the blade, cropped to the blade's shape.
  const float scale = std::max(area.x / size.x, area.y / size.y);
  const float u = area.x / scale / size.x, v = area.y / scale / size.y;
  dl->AddImageRounded(tex, p0, p1, ImVec2((1.0f - u) * 0.5f, (1.0f - v) * 0.5f), ImVec2((1.0f + u) * 0.5f, (1.0f + v) * 0.5f),
                      WithAlpha(color::kWhite, t.image_alpha * a), round);
}

void GuideDialog::BladeShadow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float round, float spread, float a) {
  if (!blurred_) return Shadow(dl, p0, p1, round, spread, a);
  constexpr int kRings = 12;
  const float step = spread / kRings;
  for (int i = 0; i < kRings; ++i) {
    const float d = (float(i) + 0.5f) * step;
    const float t = d / spread;
    const float alpha = a * (1.0f - t) * (1.0f - t) * 0.45f;
    dl->AddRect(ImVec2(p0.x - d, p0.y - d * 0.6f), ImVec2(p1.x + d, p1.y + d), IM_COL32(0, 0, 0, int(alpha * 255)),
                round + d, 0, step + 0.5f);
  }
}

void GuideDialog::DrawGuide(ImDrawList* dl, float a) {
  const float w = Px(820), h = Px(470), strip = Px(52);
  const ImVec2 c(display_.x * 0.5f, display_.y * 0.5f + Px(12) + (1.0f - a) * Px(16));
  const ImVec2 p0(std::floor(c.x - w * 0.5f), std::floor(c.y - h * 0.5f));
  const ImVec2 p1(p0.x + w, p0.y + h);
  const float round = Px(12);
  blade_hits_.clear();

  auto side = [&](int b, int level, bool left) {
    const float inset = Px(9) * float(level);
    const float x_out = left ? p0.x - strip * float(level) : p1.x + strip * float(level);
    // They tuck under the open blade, unless it's see-through: then they
    // stop at its edge, rounded only on the outside.
    const bool see_through = blurred_;
    const float tuck = see_through ? 0.0f : round * 2;
    const ImVec2 b0(left ? x_out : p1.x - tuck, p0.y + inset);
    const ImVec2 b1(left ? p0.x + tuck : x_out, p1.y - inset);
    const ImDrawFlags corners = !see_through ? ImDrawFlags_None
                                : left       ? ImDrawFlags_RoundCornersLeft
                                             : ImDrawFlags_RoundCornersRight;
    dl->AddRectFilled(b0, b1, WithAlpha(level == 1 ? color::kSideBlade : color::kSideBladeFar, a), Px(10), corners);
    const float lx = left ? x_out + strip * 0.5f : x_out - strip * 0.5f;
    const std::string label = BladeLabel(b);
    TextVertical(dl, Regular(), Px(26), ImVec2(lx, (b0.y + b1.y) * 0.5f), WithAlpha(color::kSideText, a), label);
    blade_hits_.push_back({b, {ImVec2(left ? x_out : x_out - strip, b0.y), ImVec2(left ? x_out + strip : x_out, b1.y)}});
  };
  for (int b = 0; b < blade_; ++b) side(b, blade_ - b, true);
  for (int b = int(blades_.size()) - 1; b > blade_; --b) side(b, b - blade_, false);

  // Open blade.
  BladeShadow(dl, p0, p1, round, Px(28), a);
  dl->AddRectFilled(p0, p1, WithAlpha(color::kBlade, a * BladeSeeThrough()), round);
  DrawThemeBackground(dl, p0, p1, round, a);
  const ImVec2 s0(p0.x + Px(9), p0.y + Px(9)), s1(p0.x + Px(9) + Px(72), p1.y - Px(9));
  dl->AddRectFilled(s0, s1, WithAlpha(color::kBladeStrip, a * StripSeeThrough()), Px(8));
  const ImVec2 icon_c((s0.x + s1.x) * 0.5f, s0.y + Px(38));
  DrawBladeIcon(dl, blade_, icon_c, a);
  const std::string label = BladeLabel(blade_);
  TextVertical(dl, Regular(), Px(30), ImVec2(icon_c.x, (icon_c.y + Px(40) + s1.y) * 0.5f),
               WithAlpha(color::kText, a), label);

  const ImVec2 l0(s1.x + Px(12), p0.y + Px(14)), l1(p1.x - Px(14), p1.y - Px(14));
  DrawList(dl, l0, l1, a);

  DrawHeader(dl, p0, p1, a);
  DrawLegend(dl, ImVec2(p0.x + Px(20), p1.y + Px(34)), a);
  DrawDisclaimer(dl, p0, p1, a);
}

std::string GuideDialog::HomeLabel() const {
  return backend_->signed_in() ? backend_->me().gamertag : "Not signed in";
}

std::string GuideDialog::BladeLabel(int blade) const {
  const BladeSlot& slot = blades_[size_t(blade)];
  if (!slot.menu.empty()) {
    const auto menu = FindAppMenu(slot.menu);
    return menu ? menu->title : "";
  }
  static constexpr const char* kLabels[kBladeCount] = {"Social", "Games & Apps", "", "Settings"};
  return slot.builtin == kHomeBlade ? HomeLabel() : kLabels[slot.builtin];
}

void GuideDialog::DrawBladeIcon(ImDrawList* dl, int blade, ImVec2 c, float a) {
  const ImU32 col = WithAlpha(color::kText, a);
  if (!blades_[size_t(blade)].menu.empty()) return DrawMenuMark(dl, blades_[size_t(blade)].menu, c, a);
  switch (blades_[size_t(blade)].builtin) {
    case kSocialBlade: DrawGlyph(dl, Glyph::kPeople, c, Px(40), col); break;
    case kGamesBlade: {
      // apps.png's tiles fill only 35 x 24 of its 128 x 128 (from 46, 47):
      // draw just those, as wide as the Social blade's glyph.
      const ImVec2 uv0(46.0f / 128.0f, 47.0f / 128.0f), uv1(81.0f / 128.0f, 71.0f / 128.0f);
      const float w = Px(40), h = w * 24.0f / 35.0f;
      dl->AddImage(textures_.Icon(assets::Icon::kApps), ImVec2(c.x - w * 0.5f, c.y - h * 0.5f),
                   ImVec2(c.x + w * 0.5f, c.y + h * 0.5f), uv0, uv1, col);
      break;
    }
    case kSettingsBlade:
      Image(dl, textures_.Icon(assets::Icon::kSystem), ImVec2(c.x - Px(18), c.y - Px(18)), ImVec2(c.x + Px(18), c.y + Px(18)), col);
      break;
    default:
      DrawRing(dl, c, a, backend_->signed_in());
      break;
  }
}

void GuideDialog::DrawRing(ImDrawList* dl, ImVec2 c, float a, int lit_player, float radius, float width) {
  constexpr int kQuarterOfPlayer[4] = {0, 1, 3, 2};
  const int lit_quarter = lit_player >= 0 && lit_player < 4 ? kQuarterOfPlayer[lit_player] : -1;
  const float gap = 0.2f + width / radius;  // radians between quarters' centrelines
  for (int q = 0; q < 4; ++q) {
    const float start = -kPi + q * kPi * 0.5f + gap * 0.5f;
    const float end = start + kPi * 0.5f - gap;
    const ImU32 seg = WithAlpha(q == lit_quarter ? color::kGreenLight : color::kText, a);
    dl->PathArcTo(c, radius, start, end, 24);
    dl->PathStroke(seg, 0, width);
    for (float t : {start, end}) {
      dl->AddCircleFilled(ImVec2(c.x + std::cos(t) * radius, c.y + std::sin(t) * radius), width * 0.5f, seg, 12);
    }
  }
}

void GuideDialog::DrawHeader(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float a) {
  // Gamerpic above the blade.
  const ImVec2 pic_c((p0.x + p1.x) * 0.5f, p0.y - Px(48));
  const float r = Px(30);
  dl->AddCircleFilled(pic_c, r + Px(3), WithAlpha(IM_COL32(255, 255, 255, 230), a), 48);
  if (backend_->signed_in()) {
    Image(dl, textures_.Gamerpic(backend_->me().gamerpic), ImVec2(pic_c.x - r, pic_c.y - r),
          ImVec2(pic_c.x + r, pic_c.y + r), WithAlpha(color::kWhite, a), r);
  } else {
    dl->AddCircleFilled(pic_c, r, WithAlpha(IM_COL32(90, 100, 115, 255), a), 48);
    DrawGlyph(dl, Glyph::kPeople, pic_c, Px(40), WithAlpha(color::kText, a));
  }
  // Clock.
  const std::string clock = Clock();
  const ImVec2 cs = Measure(Regular(), Px(26), clock);
  Text(dl, Regular(), Px(26), ImVec2(p1.x - cs.x - Px(4), p0.y - Px(48) - cs.y * 0.5f), WithAlpha(color::kText, a), clock);
}

void GuideDialog::DrawLegend(ImDrawList* dl, ImVec2 pos, float a) {
  struct Hint {
    char button;
    const char* key;
    std::string label;
  };
  std::vector<Hint> hints = {{'A', "Enter", "Select"}, {'B', "Esc", Stack().size() > 1 ? "Back" : "Close"}};
  if (screen_ == Screen::kGuide && Current().kind == PageKind::kHome && backend_->signed_in()) {
    hints.push_back({'X', "X", "Sign Out"});
  }
  if (screen_ == Screen::kGuide && Current().kind == PageKind::kThemes) {
    hints.push_back({'X', "X", "Sort"});
    hints.push_back({'Y', "Y", "Like"});
  }
  float x = pos.x;
  const float y = pos.y;
  for (const Hint& h : hints) {
    if (pad_) {
      FaceButton(dl, ImVec2(x + Px(12), y), Px(12), h.button);
      x += Px(30);
    } else {
      const ImVec2 ks = Measure(Semibold(), Px(14), h.key);
      dl->AddRect(ImVec2(x, y - Px(12)), ImVec2(x + ks.x + Px(14), y + Px(12)), WithAlpha(color::kText, a), Px(4), 0, Px(1.5f));
      Text(dl, Semibold(), Px(14), ImVec2(x + Px(7), y - ks.y * 0.5f), WithAlpha(color::kText, a), h.key);
      x += ks.x + Px(22);
    }
    Caps(dl, Semibold(), Px(14), ImVec2(x, y - Px(8)), WithAlpha(color::kText, a), h.label, Px(1.2f));
    x += CapsWidth(Semibold(), Px(14), h.label, Px(1.2f)) + Px(30);
  }
  if (screen_ == Screen::kGuide) {
    // The app's key binds take Q / E when it has a keyboard pad (its bumper
    // keys switch then, like a pad's).
    const char* switch_label = pad_ ? "LB / RB  Switch"
                               : Input::KeyboardPlayer() >= 0 ? "PgUp / PgDn  Switch"
                                                              : "Q / E  Switch";
    Caps(dl, Semibold(), Px(14), ImVec2(x, y - Px(8)), WithAlpha(color::kTextDim, a), switch_label, Px(1.2f));
  }
}

void GuideDialog::DrawSignIn(ImDrawList* dl, float a) {
  const float w = Px(720), h = Px(470);
  const ImVec2 c(display_.x * 0.5f, display_.y * 0.5f + (1.0f - a) * Px(16));
  const ImVec2 p0(std::floor(c.x - w * 0.5f), std::floor(c.y - h * 0.5f)), p1(p0.x + w, p0.y + h);
  BladeShadow(dl, p0, p1, Px(12), Px(28), a);
  dl->AddRectFilled(p0, p1, WithAlpha(color::kBlade, a * BladeSeeThrough()), Px(12));
  DrawThemeBackground(dl, p0, p1, Px(12), a);
  const ImVec2 s0(p0.x + Px(9), p0.y + Px(9)), s1(p0.x + Px(81), p1.y - Px(9));
  dl->AddRectFilled(s0, s1, WithAlpha(color::kBladeStrip, a * StripSeeThrough()), Px(8));
  const ImVec2 icon_c((s0.x + s1.x) * 0.5f, s0.y + Px(38));
  // The player signing in, lit on the ring.
  DrawRing(dl, icon_c, a, true);
  TextVertical(dl, Regular(), Px(30), ImVec2(icon_c.x, (icon_c.y + Px(40) + s1.y) * 0.5f), WithAlpha(color::kText, a),
               "Sign In");
  const ImVec2 l0(s1.x + Px(12), p0.y + Px(14)), l1(p1.x - Px(14), p1.y - Px(14));
  // Title over the list on the root page.
  if (signin_stack_.size() == 1) {
    Text(dl, Light(), Px(34), ImVec2(l0.x + Px(14), l0.y), WithAlpha(color::kText, a), "RexLiveless");
    Text(dl, Regular(), Px(19), ImVec2(l0.x + Px(16), l0.y + Px(44)), WithAlpha(color::kTextDim, a),
         "Sign in to play online, add friends and send crates.");
    DrawList(dl, ImVec2(l0.x, l0.y + Px(84)), l1, a);
  } else {
    DrawList(dl, l0, l1, a);
  }
  DrawLegend(dl, ImVec2(p0.x + Px(20), p1.y + Px(34)), a);
  DrawDisclaimer(dl, p0, p1, a);
}

void GuideDialog::DrawDisclaimer(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float a) {
  // Under the legend, centred on the blade.
  static constexpr const char* kText = "RexLiveless is not affiliated with Microsoft or its affiliates, All assets/layouts/sounds are recreations and not rips of original content.";
  const ImVec2 s = Measure(Regular(), Px(15), kText);
  Text(dl, Regular(), Px(15), ImVec2(std::floor((p0.x + p1.x - s.x) * 0.5f), p1.y + Px(62)), WithAlpha(color::kTextDim, a),
       kText);
}

void GuideDialog::DrawServerWarning(ImDrawList* dl) {
  // A debug build pointed at another server (REXLIVELESS_SERVER, the mock):
  // shown over the game as well as the Guide, so it can't be mistaken for
  // the real one.
  const std::string server = backend_->unofficial_server();
  if (server.empty()) return;
  const std::string text = "DEBUG: not the RexLiveless server (" + server + ")";
  const ImVec2 s = Measure(Semibold(), Px(16), text);
  const ImVec2 b0(std::floor((display_.x - s.x) * 0.5f) - Px(14), Px(10));
  const ImVec2 b1(b0.x + s.x + Px(28), b0.y + s.y + Px(10));
  dl->AddRectFilled(b0, b1, IM_COL32(200, 120, 0, 220), Px(6));
  dl->AddRect(b0, b1, IM_COL32(255, 210, 120, 255), Px(6), 0, Px(1.5f));
  Text(dl, Semibold(), Px(16), ImVec2(b0.x + Px(14), b0.y + Px(5)), color::kWhite, text);
}

}  // namespace rexglue_guide
