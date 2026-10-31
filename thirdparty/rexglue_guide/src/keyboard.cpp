#include "keyboard.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <ctime>
#include <iterator>
#include <utility>

#include <SDL3/SDL_gamepad.h>

#include "audio.h"
#include "guide_common.h"
#include "imgui_internal.h"

namespace rexglue_guide {
namespace {

using namespace ui;
using assets::Sound;

// Per page: key rows 1-4 (row 0 is digits).
constexpr char16_t kDigits[] = u"1234567890";
constexpr const char16_t* kPageRows[3][4] = {
    {u"qwertyuiop", u"asdfghjkl-", u"zxcvbnm_@.", u""},
    {u",;:'\"!?¡¿%", u"[]{}`$£«»#", u"<>()€¥¢~^\\", u"|=*/+-@_&."},
    {u"àáâãäåñèéê", u"ëþçýìíîïùú", u"ûòóôõðø×œæ", u"ßÿºª.üµš_ö"},
};
constexpr const char* kPageNames[3] = {"Alphabet", "Symbols", "Accents"};

char16_t Upper(char16_t c) {
  if (c >= u'a' && c <= u'z') return char16_t(c - 0x20);
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return char16_t(c - 0x20);
  switch (c) {
    case 0xFF: return 0x178;
    case 0x153: return 0x152;
    case 0x161: return 0x160;
    default: return c;
  }
}

// offsets[i] = byte offset of code unit i, plus one for the end.
std::string ToUtf8(std::u16string_view s, std::vector<size_t>* offsets) {
  std::string out;
  if (offsets) offsets->clear();
  for (size_t i = 0; i < s.size(); ++i) {
    if (offsets) offsets->push_back(out.size());
    uint32_t c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      if (offsets) offsets->push_back(out.size());
      ++i;
    }
    if (c < 0x80) {
      out.push_back(char(c));
    } else if (c < 0x800) {
      out.push_back(char(0xC0 | (c >> 6)));
      out.push_back(char(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
      out.push_back(char(0xE0 | (c >> 12)));
      out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(char(0x80 | (c & 0x3F)));
    } else {
      out.push_back(char(0xF0 | (c >> 18)));
      out.push_back(char(0x80 | ((c >> 12) & 0x3F)));
      out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(char(0x80 | (c & 0x3F)));
    }
  }
  if (offsets) offsets->push_back(out.size());
  return out;
}

// Reference units: panel 1350x823 (a 360 screenshot at 2000 wide). Grid rows
// 0-5, cols 0-11: cols 0/11 side buttons, 1-10 keys, row 5 Backspace/Space.
constexpr float kPanelW = 1350.0f;
constexpr float kPanelH = 823.0f;
constexpr float kUpperH = 348.0f;
constexpr float kBlockTop = -64.0f;
constexpr float kBlockH = 974.0f;
constexpr float kGridX = 279.0f;
constexpr float kGridY = 378.0f;
constexpr float kPitchX = 80.0f;
constexpr float kPitchY = 63.0f;
constexpr float kKeyW = 74.0f;
constexpr float kKeyH = 56.0f;
constexpr float kSideW = 214.0f;
constexpr float kLeftX = 51.0f;
constexpr float kRightX = 1085.0f;
constexpr float kFieldX = 49.0f;
constexpr float kFieldY = 258.0f;
constexpr float kFieldW = 1112.0f;
constexpr float kFieldH = 65.0f;
constexpr float kFieldPad = 15.0f;
constexpr float kFooterY = 856.0f;
constexpr int kRows = 6;
constexpr int kCols = 12;

float CellLeft(int col) {
  return col == 0 ? kLeftX : col == kCols - 1 ? kRightX : kGridX + float(col - 1) * kPitchX;
}
float CellRight(int col) {
  return CellLeft(col) + (col == 0 || col == kCols - 1 ? kSideW : kKeyW);
}

constexpr float kOpenGrace = 0.15f;  // input ignored right after opening
constexpr float kStick = 0.5f;       // stick/trigger travel that counts as pressed
constexpr float kFlashTime = 0.12f;

ImU32 Rgb(int r, int g, int b, int a = 255) {
  return IM_COL32(r, g, b, a);
}

// The colour with its RGB scaled by f (alpha kept).
ImU32 Shade(ImU32 c, float f) {
  auto ch = [&](int shift) { return ImU32(std::clamp(float((c >> shift) & 0xFF) * f, 0.0f, 255.0f)) << shift; };
  return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | (c & IM_COL32_A_MASK);
}

// a's RGB moved t of the way to b's (a's alpha kept).
ImU32 Mix(ImU32 a, ImU32 b, float t) {
  auto ch = [&](int shift) {
    const float x = float((a >> shift) & 0xFF), y = float((b >> shift) & 0xFF);
    return ImU32(x + (y - x) * t) << shift;
  };
  return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | (a & IM_COL32_A_MASK);
}

// Kenney's icon names for a face button, by its position (south, east, west,
// north) and the label the pad has there.
std::string FaceIcon(const PadStyle& style, int position, bool* colored) {
  static constexpr const char* kLetters[] = {"", "a", "b", "x", "y", "cross", "circle", "square", "triangle"};
  static constexpr const char* kXboxFaces[] = {"a", "b", "x", "y"};
  static constexpr const char* kPlayStationFaces[] = {"cross", "circle", "square", "triangle"};
  const int label = style.face_labels[position];
  std::string letter = label > 0 && label < int(std::size(kLetters)) ? kLetters[label] : "";
  if (letter.empty()) letter = style.family == PadFamily::kPlayStation ? kPlayStationFaces[position] : kXboxFaces[position];
  *colored = style.family == PadFamily::kXbox || style.family == PadFamily::kPlayStation;
  switch (style.family) {
    case PadFamily::kPlayStation: return "playstation_button_color_" + letter;
    case PadFamily::kSwitch: return "switch_button_" + letter;
    case PadFamily::kSteamDeck: return "steamdeck_button_" + letter;
    default: return "xbox_button_color_" + letter;
  }
}

// The other buttons the keyboard shows, per pad family.
struct PadNames {
  const char* lb;
  const char* rb;
  const char* lt;
  const char* rt;
  const char* stick_press;
  const char* start;
};

PadNames NamesFor(const PadStyle& style) {
  switch (style.family) {
    case PadFamily::kPlayStation:
      return {"playstation_trigger_l1", "playstation_trigger_r1", "playstation_trigger_l2", "playstation_trigger_r2",
              "playstation_button_l3",
              style.type == SDL_GAMEPAD_TYPE_PS3   ? "playstation3_button_start"
              : style.type == SDL_GAMEPAD_TYPE_PS4 ? "playstation4_button_options"
                                                   : "playstation5_button_options"};
    case PadFamily::kSwitch:
      return {"switch_button_l", "switch_button_r", "switch_button_zl", "switch_button_zr", "switch_stick_l_press",
              "switch_button_plus"};
    case PadFamily::kSteamDeck:
      return {"steamdeck_button_l1", "steamdeck_button_r1", "steamdeck_button_l2", "steamdeck_button_r2",
              "steamdeck_stick_l_press", "steamdeck_button_options"};
    default:
      return {"xbox_lb", "xbox_rb", "xbox_lt", "xbox_rt", "xbox_stick_l_press",
              style.type == SDL_GAMEPAD_TYPE_XBOX360 ? "xbox_button_start" : "xbox_button_menu"};
  }
}

}  // namespace

std::string Utf16ToUtf8(std::u16string_view s) {
  return ToUtf8(s, nullptr);
}

std::u16string Utf8ToUtf16(std::string_view s) {
  std::u16string out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char b = static_cast<unsigned char>(s[i]);
    const int extra = b < 0x80 ? 0 : b < 0xE0 ? 1 : b < 0xF0 ? 2 : 3;
    uint32_t c = extra == 0 ? b : extra == 1 ? b & 0x1F : extra == 2 ? b & 0x0F : b & 0x07;
    for (int k = 1; k <= extra && i + k < s.size(); ++k) c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += size_t(extra) + 1;
    if (c >= 0x10000) {
      c -= 0x10000;
      out.push_back(char16_t(0xD800 + (c >> 10)));
      out.push_back(char16_t(0xDC00 + (c & 0x3FF)));
    } else {
      out.push_back(char16_t(c));
    }
  }
  return out;
}

bool Keyboard::Repeat::Tick(bool down, float dt) {
  if (!down) {
    held = -1.0f;
    return false;
  }
  if (held < 0.0f) {
    held = 0.0f;
    next = 0.4f;
    return true;
  }
  held += dt;
  next -= dt;
  if (next <= 0.0f) {
    next += 0.07f;
    return true;
  }
  return false;
}

// ------------------------------------------------------------ open, close --

void Keyboard::Open(KeyboardRequest request) {
  request_ = std::move(request);
  active_ = true;
  text_ = Utf8ToUtf16(request_.text).substr(0, request_.max_length);
  caret_ = text_.size();
  scroll_ = 0.0f;
  page_ = Page::kAlphabet;
  caps_ = false;
  cursor_ = Cell{1, 1};
  pressed_target_ = -1;
  flash_target_ = -1;
  opened_for_ = 0.0f;
  cancel_clicked_ = false;
  pad_read_ = false;
  pad_armed_ = false;
  for (Repeat* r : {&repeat_up_, &repeat_down_, &repeat_left_, &repeat_right_, &repeat_backspace_,
                    &repeat_caret_left_, &repeat_caret_right_}) {
    *r = Repeat{};
  }
  audio::Play(Sound::kOpen);
}

void Keyboard::Finish(bool accepted) {
  if (!active_) return;
  active_ = false;
  audio::Play(accepted ? Sound::kSelect : Sound::kBack);
  auto done = std::move(request_.done);
  request_ = {};
  if (done) done(accepted ? std::optional<std::string>(Utf16ToUtf8(text_)) : std::nullopt);
}

void Keyboard::Cancel() {
  Finish(false);
}

// ---------------------------------------------------------- keys, focus --

void Keyboard::Layout(ImVec2 display) {
  scale_ = std::min(display.x * 0.82f / kPanelW, display.y * 0.92f / kBlockH);
  origin_ = ImVec2(std::floor((display.x - kPanelW * scale_) * 0.5f),
                   std::floor((display.y - kBlockH * scale_) * 0.5f - kBlockTop * scale_));
}

char16_t Keyboard::KeyChar(int row, int col) const {
  if (col < 1 || col > 10 || row < 0 || row > 4) return 0;
  if (row == 0) return kDigits[col - 1];
  const char16_t* keys = kPageRows[int(page_)][row - 1];
  if (!keys[0]) return 0;
  const char16_t c = keys[col - 1];
  return caps_ && CapsEnabled() ? Upper(c) : c;
}

void Keyboard::BuildTargets() {
  targets_.clear();
  auto add = [&](Action action, int row0, int row1, int col0, int col1, char16_t ch = 0) {
    Target t{action, row0, row1, col0, col1, ch};
    t.min = ImVec2(origin_.x + CellLeft(col0) * scale_, origin_.y + (kGridY + float(row0) * kPitchY) * scale_);
    t.max = ImVec2(origin_.x + CellRight(col1) * scale_, origin_.y + (kGridY + float(row1) * kPitchY + kKeyH) * scale_);
    targets_.push_back(t);
  };
  add(Action::kLeft, 0, 1, 0, 0);
  add(Action::kNextPage, 2, 3, 0, 0);
  add(Action::kCaps, 4, 5, 0, 0);
  add(Action::kRight, 0, 1, 11, 11);
  add(Action::kPrevPage, 2, 3, 11, 11);
  add(Action::kDone, 4, 5, 11, 11);
  for (int row = 0; row <= 4; ++row) {
    for (int col = 1; col <= 10; ++col) {
      if (const char16_t ch = KeyChar(row, col)) add(Action::kKey, row, row, col, col, ch);
    }
  }
  add(Action::kBackspace, 5, 5, 1, 5);
  add(Action::kSpace, 5, 5, 6, 10);
  // Pages without a fifth key row: focus moves up.
  while (!TargetAt(cursor_) && cursor_.row > 0) --cursor_.row;
}

const Keyboard::Target* Keyboard::TargetAt(Cell cell) const {
  for (const Target& t : targets_) {
    if (cell.row >= t.row0 && cell.row <= t.row1 && cell.col >= t.col0 && cell.col <= t.col1) return &t;
  }
  return nullptr;
}

// Steps cell by cell, wrapping, to the next different button (across wide
// buttons and empty rows).
void Keyboard::Move(int d_row, int d_col) {
  const Target* from = TargetAt(cursor_);
  Cell cell = cursor_;
  for (int i = 0; i < kRows * kCols; ++i) {
    cell.row = (cell.row + d_row + kRows) % kRows;
    cell.col = (cell.col + d_col + kCols) % kCols;
    const Target* t = TargetAt(cell);
    if (t && t != from) {
      cursor_ = cell;
      audio::Play(Sound::kMove);
      return;
    }
  }
}

void Keyboard::Insert(char16_t ch) {
  if (text_.size() >= request_.max_length) return;
  text_.insert(text_.begin() + ptrdiff_t(caret_), ch);
  ++caret_;
}

void Keyboard::Backspace() {
  if (caret_ == 0) return;
  text_.erase(caret_ - 1, 1);
  --caret_;
}

void Keyboard::MoveCaret(int delta) {
  caret_ = size_t(std::clamp<long long>((long long)caret_ + delta, 0, (long long)text_.size()));
}

void Keyboard::Activate(const Target& target) {
  flash_target_ = int(&target - targets_.data());
  flash_time_ = kFlashTime;
  switch (target.action) {
    case Action::kKey: Insert(target.ch); break;
    case Action::kLeft: MoveCaret(-1); break;
    case Action::kRight: MoveCaret(1); break;
    case Action::kNextPage: page_ = Page((int(page_) + 1) % 3); break;
    case Action::kPrevPage: page_ = Page((int(page_) + 2) % 3); break;
    case Action::kCaps:
      if (CapsEnabled()) caps_ = !caps_;
      break;
    case Action::kBackspace: Backspace(); break;
    case Action::kSpace: Insert(u' '); break;
    case Action::kDone: Finish(true); break;
  }
}

// ---------------------------------------------------------------- input --

void Keyboard::Update(const NavInput& in, ImVec2 display, float dt) {
  if (!active_) return;
  ImGuiIO& io = ImGui::GetIO();
  // Typed characters, as an ImGui text box asks for them: rexglue's SDL
  // windows only send them while ImGui wants text input (that SDK's
  // ImGuiDrawer turns SDL text input on from this), whatever mnk_mode is.
  ImGuiPlatformImeData& ime = ImGui::GetCurrentContext()->PlatformImeData;
  ime.WantTextInput = true;
  ime.ViewportId = ImGui::GetMainViewport()->ID;
  opened_for_ += dt;
  flash_time_ -= dt;
  style_ = in.pad_style;
  Layout(display);
  BuildTargets();
  PollPad(in, dt);
  if (active_) PollKeys(io);
  if (active_) PollPointer(io);
  if (active_ && cancel_clicked_) Finish(false);
  if (active_) BuildTargets();  // a page or caps change relabels the keys
}

void Keyboard::PollPad(const NavInput& in, float dt) {
  const PadState now = in.pad;
  const PadState prev = pad_read_ ? pad_ : now;
  pad_ = now;
  pad_read_ = true;

  bool any = now.lt > 0.25f || now.rt > 0.25f || std::fabs(now.lx) > 0.25f || std::fabs(now.ly) > 0.25f;
  for (bool b : now.buttons) any = any || b;
  // Nothing until everything held at open is let go.
  if (!pad_armed_) {
    pad_armed_ = !any;
    if (!pad_read_ || opened_for_ <= dt) pad_device_ = in.pad_used;
    return;
  }
  if (opened_for_ < kOpenGrace) return;

  auto pressed = [&](SDL_GamepadButton b) { return now.buttons[b] && !prev.buttons[b]; };
  bool any_pressed = (now.lt > kStick && prev.lt <= kStick) || (now.rt > kStick && prev.rt <= kStick) ||
                     (std::fabs(now.lx) > kStick && std::fabs(prev.lx) <= kStick) ||
                     (std::fabs(now.ly) > kStick && std::fabs(prev.ly) <= kStick);
  for (int b = 0; b < 32; ++b) any_pressed = any_pressed || (now.buttons[b] && !prev.buttons[b]);
  if (any_pressed) pad_device_ = true;

  auto flash = [&](Action action) {
    for (size_t i = 0; i < targets_.size(); ++i) {
      if (targets_[i].action == action) {
        flash_target_ = int(i);
        flash_time_ = kFlashTime;
        return;
      }
    }
  };

  if (repeat_up_.Tick(now.buttons[SDL_GAMEPAD_BUTTON_DPAD_UP] || now.ly < -kStick, dt)) Move(-1, 0);
  if (repeat_down_.Tick(now.buttons[SDL_GAMEPAD_BUTTON_DPAD_DOWN] || now.ly > kStick, dt)) Move(1, 0);
  if (repeat_left_.Tick(now.buttons[SDL_GAMEPAD_BUTTON_DPAD_LEFT] || now.lx < -kStick, dt)) Move(0, -1);
  if (repeat_right_.Tick(now.buttons[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] || now.lx > kStick, dt)) Move(0, 1);

  if (repeat_backspace_.Tick(now.buttons[SDL_GAMEPAD_BUTTON_WEST], dt)) {
    Backspace();
    flash(Action::kBackspace);
  }
  if (pressed(SDL_GAMEPAD_BUTTON_NORTH)) {
    Insert(u' ');
    flash(Action::kSpace);
  }
  if (repeat_caret_left_.Tick(now.buttons[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER], dt)) {
    MoveCaret(-1);
    flash(Action::kLeft);
  }
  if (repeat_caret_right_.Tick(now.buttons[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER], dt)) {
    MoveCaret(1);
    flash(Action::kRight);
  }
  if (now.lt > kStick && prev.lt <= kStick) {
    page_ = Page((int(page_) + 1) % 3);
    flash(Action::kNextPage);
  }
  if (now.rt > kStick && prev.rt <= kStick) {
    page_ = Page((int(page_) + 2) % 3);
    flash(Action::kPrevPage);
  }
  if (pressed(SDL_GAMEPAD_BUTTON_LEFT_STICK) && CapsEnabled()) {
    caps_ = !caps_;
    flash(Action::kCaps);
  }
  if (pressed(SDL_GAMEPAD_BUTTON_SOUTH)) {
    if (const Target* t = TargetAt(cursor_)) Activate(*t);
    if (!active_) return;
  }
  if (pressed(SDL_GAMEPAD_BUTTON_START)) return Finish(true);
  if (pressed(SDL_GAMEPAD_BUTTON_EAST)) Finish(false);
}

void Keyboard::PollKeys(ImGuiIO& io) {
  if (opened_for_ < kOpenGrace) return;
  bool typed = false;
  for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
    const ImWchar c = io.InputQueueCharacters[i];
    if (c < 0x20 || c == 0x7F || (c >= 0xD800 && c < 0xE000) || c > 0xFFFF) continue;
    Insert(char16_t(c));
    typed = true;
  }
  auto key = [](ImGuiKey k, bool repeat) { return ImGui::IsKeyPressed(k, repeat); };
  if (key(ImGuiKey_Backspace, true)) Backspace(), typed = true;
  if (key(ImGuiKey_Delete, true) && caret_ < text_.size()) text_.erase(caret_, 1), typed = true;
  if (key(ImGuiKey_LeftArrow, true)) MoveCaret(-1), typed = true;
  if (key(ImGuiKey_RightArrow, true)) MoveCaret(1), typed = true;
  if (key(ImGuiKey_Tab, false)) page_ = Page((int(page_) + (io.KeyShift ? 2 : 1)) % 3), typed = true;
  if (key(ImGuiKey_CapsLock, false) && CapsEnabled()) caps_ = !caps_, typed = true;
  if (typed) pad_device_ = false;
  if (key(ImGuiKey_Enter, false) || key(ImGuiKey_KeypadEnter, false)) return Finish(true);
  if (key(ImGuiKey_Escape, false)) Finish(false);
}

void Keyboard::PollPointer(ImGuiIO& io) {
  if (!ImGui::IsMousePosValid()) return;
  const bool moved = std::fabs(io.MouseDelta.x) + std::fabs(io.MouseDelta.y) > 2.0f && opened_for_ > 0.3f;
  const bool clicked = ImGui::IsMouseClicked(0);
  if (moved || clicked) pad_device_ = false;

  int hovered = -1;
  for (size_t i = 0; i < targets_.size(); ++i) {
    if (ImGui::IsMouseHoveringRect(targets_[i].min, targets_[i].max, false)) {
      hovered = int(i);
      break;
    }
  }
  if (hovered >= 0 && (moved || clicked)) cursor_ = Cell{targets_[size_t(hovered)].row0, targets_[size_t(hovered)].col0};
  if (opened_for_ < kOpenGrace) return;

  if (clicked) {
    pressed_target_ = hovered;
    cancel_clicked_ = ImGui::IsMouseHoveringRect(back_hit_.first, back_hit_.second, false);
    // A click in the field puts the caret at the nearest character.
    const ImVec2 field_min(origin_.x + (kFieldX + kFieldPad) * scale_, origin_.y + kFieldY * scale_);
    const ImVec2 field_max(origin_.x + (kFieldX + kFieldW) * scale_, origin_.y + (kFieldY + kFieldH) * scale_);
    if (ImGui::IsMouseHoveringRect(ImVec2(field_min.x - kFieldPad * scale_, field_min.y), field_max, false)) {
      std::vector<size_t> offsets;
      const std::string utf8 = ToUtf8(Shown(), &offsets);
      const float size = 32.0f * scale_;
      const float x = io.MousePos.x - field_min.x + scroll_;
      size_t best = 0;
      float best_distance = FLT_MAX;
      for (size_t i = 0; i <= text_.size(); ++i) {
        const float w = Measure(Regular(), size, std::string_view(utf8).substr(0, offsets[i])).x;
        if (std::fabs(w - x) < best_distance) {
          best_distance = std::fabs(w - x);
          best = i;
        }
      }
      caret_ = best;
    }
  }
  if (ImGui::IsMouseReleased(0)) {
    if (pressed_target_ >= 0 && pressed_target_ == hovered) Activate(targets_[size_t(pressed_target_)]);
    pressed_target_ = -1;
  }
}

// -------------------------------------------------------------- prompts --

Keyboard::Prompt Keyboard::PromptFor(Action action) const {
  // Physical keys on the keyboard, the pad's buttons on a pad.
  if (!pad_device_) {
    switch (action) {
      case Action::kLeft: return {{"keyboard_arrow_left"}};
      case Action::kRight: return {{"keyboard_arrow_right"}};
      case Action::kNextPage: return {{"keyboard_tab_icon"}};
      case Action::kPrevPage: return {{"keyboard_shift_icon", "keyboard_tab_icon"}};
      case Action::kCaps: return {{"keyboard_capslock_icon"}};
      case Action::kBackspace: return {{"keyboard_backspace_icon"}};
      case Action::kSpace: return {{"keyboard_space_icon"}};
      case Action::kDone: return {{"keyboard_enter"}};
      default: return {};
    }
  }
  const PadNames names = NamesFor(style_);
  bool colored = false;
  switch (action) {
    case Action::kLeft: return {{names.lb}};
    case Action::kRight: return {{names.rb}};
    case Action::kNextPage: return {{names.lt}};
    case Action::kPrevPage: return {{names.rt}};
    case Action::kCaps: return {{names.stick_press}};
    case Action::kBackspace: {
      std::string icon = FaceIcon(style_, 2, &colored);
      return {{icon}, colored};
    }
    case Action::kSpace: {
      std::string icon = FaceIcon(style_, 3, &colored);
      return {{icon}, colored};
    }
    case Action::kDone: return {{names.start}};
    default: return {};
  }
}

Keyboard::Prompt Keyboard::SelectPrompt() const {
  if (!pad_device_) return {{"mouse_left"}};
  bool colored = false;
  std::string icon = FaceIcon(style_, 0, &colored);
  return {{icon}, colored};
}

Keyboard::Prompt Keyboard::BackPrompt() const {
  if (!pad_device_) return {{"keyboard_escape"}};
  bool colored = false;
  std::string icon = FaceIcon(style_, 1, &colored);
  return {{icon}, colored};
}

bool Keyboard::DrawPrompt(ImDrawList* dl, Textures& textures, const Prompt& prompt, ImVec2 pos, float size, ImU32 tint,
                          bool measure_only) {
  if (prompt.names.empty()) return false;
  std::vector<ImTextureID> icons;
  for (const std::string& name : prompt.names) {
    const ImTextureID tex = textures.KeyIcon(name);
    if (!tex) return false;
    icons.push_back(tex);
  }
  if (measure_only) return true;
  const ImU32 white = IM_COL32(255, 255, 255, (tint >> IM_COL32_A_SHIFT) & 0xFF);
  for (const ImTextureID tex : icons) {
    dl->AddImage(tex, pos, ImVec2(pos.x + size, pos.y + size), ImVec2(0, 0), ImVec2(1, 1), prompt.colored ? white : tint);
    pos.x += size;
  }
  return true;
}

// -------------------------------------------------------------- drawing --

void Keyboard::Draw(ImDrawList* dl, Textures& textures, ImVec2 display, bool dim,
                    const std::function<void(ImDrawList*, ImVec2, ImVec2)>& header_background) {
  if (!active_) return;
  const float s = scale_;
  ImFont* font = Regular();
  auto P = [&](float x, float y) { return ImVec2(origin_.x + x * s, origin_.y + y * s); };
  auto measure = [&](float size, const std::string& str) { return Measure(font, size * s, str); };
  auto text = [&](ImVec2 pos, float size, ImU32 col, const std::string& str) { Text(dl, font, size * s, pos, col, str); };

  // Theme colours: keys and panels from the keyboard's own, focus from the
  // accent; the shading of each worked out from its colour.
  const ImU32 accent = color::kGreen;
  const ImU32 key = color::kKey, key_ink = color::kKeyLabel;
  const ImU32 header = color::kKeyboardHeader, body = color::kKeyboardBody;
  if (dim) dl->AddRectFilled(ImVec2(0.0f, 0.0f), display, Rgb(0, 0, 0, 110));

  const float header_y = -58.0f;
  if (!request_.title.empty()) {
    text(P(45.0f, header_y + 2.0f), 34.0f, Rgb(0, 0, 0, 160), request_.title);
    text(P(43.0f, header_y), 34.0f, color::kText, request_.title);
  }
  {
    const std::string clock = Clock();
    const ImVec2 size = measure(34.0f, clock);
    text(ImVec2(P(kPanelW - 48.0f, 0.0f).x - size.x + 2.0f * s, P(0.0f, header_y + 2.0f).y), 34.0f, Rgb(0, 0, 0, 160),
         clock);
    text(ImVec2(P(kPanelW - 48.0f, 0.0f).x - size.x, P(0.0f, header_y).y), 34.0f, color::kText, clock);
  }

  // The darker upper part holds the prompt and the text; keys below.
  // The panel's shadow, fainter under a see-through theme's panel (it would
  // show through as a dark block).
  const float panel_alpha = float((body >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
  dl->AddRectFilled(P(6.0f, 8.0f), P(kPanelW + 6.0f, kPanelH + 8.0f), WithAlpha(Rgb(0, 0, 0, 90), panel_alpha * panel_alpha));
  dl->AddRectFilledMultiColor(P(0.0f, 0.0f), P(kPanelW, kUpperH), header, header, Shade(header, 0.62f),
                              Shade(header, 0.62f));
  if (header_background) header_background(dl, P(0.0f, 0.0f), P(kPanelW, kUpperH));
  dl->AddRectFilled(P(0.0f, kUpperH), P(kPanelW, kPanelH), body);
  dl->AddLine(P(0.0f, kUpperH), P(kPanelW, kUpperH), Mix(body, Rgb(255, 255, 255), 0.6f), std::max(1.0f, 2.0f * s));

  if (!request_.description.empty()) {
    dl->AddText(font, 30.0f * s, P(50.0f, 46.0f), color::kText, request_.description.c_str(), nullptr, 1250.0f * s);
  }

  // The text field, scrolled to keep the caret in view.
  {
    const ImVec2 field_min = P(kFieldX, kFieldY);
    const ImVec2 field_max = P(kFieldX + kFieldW, kFieldY + kFieldH);
    dl->AddRectFilled(field_min, field_max, color::kKeyboardField);
    std::vector<size_t> offsets;
    const std::string utf8 = ToUtf8(Shown(), &offsets);
    const float size = 32.0f * s;
    const float caret_x = Measure(font, size, std::string_view(utf8).substr(0, offsets[caret_])).x;
    const float inner_w = (kFieldW - kFieldPad * 2.0f) * s;
    if (caret_x - scroll_ > inner_w) scroll_ = caret_x - inner_w;
    if (caret_x < scroll_) scroll_ = caret_x;
    const float text_x = field_min.x + kFieldPad * s - scroll_;
    const float text_y = (field_min.y + field_max.y - size) * 0.5f;
    dl->PushClipRect(field_min, field_max, true);
    Text(dl, font, size, ImVec2(text_x, text_y), key_ink, utf8);
    const float caret_w = std::max(2.0f, 3.0f * s);
    dl->AddRectFilled(ImVec2(text_x + caret_x, text_y - 2.0f * s),
                      ImVec2(text_x + caret_x + caret_w, text_y + size + 2.0f * s), accent);
    dl->PopClipRect();
  }

  const Target* focused = TargetAt(cursor_);
  const ImU32 white = Rgb(255, 255, 255);
  for (size_t i = 0; i < targets_.size(); ++i) {
    const Target& t = targets_[i];
    const bool is_focused = focused == &t;
    const bool held =
        int(i) == pressed_target_ && ImGui::IsMouseDown(0) && ImGui::IsMouseHoveringRect(t.min, t.max, false);
    const bool pressed = held || (int(i) == flash_target_ && flash_time_ > 0.0f);
    const bool disabled = t.action == Action::kCaps && !CapsEnabled();

    ImU32 top = key, bottom = Shade(key, 0.93f), border = Shade(key, 0.69f);
    if (is_focused) {
      top = Mix(accent, white, 0.15f);
      bottom = Shade(accent, 0.8f);
      border = Shade(accent, 0.55f);
    }
    if (pressed) {
      top = is_focused ? Shade(accent, 0.62f) : Shade(key, 0.83f);
      bottom = is_focused ? Shade(accent, 0.55f) : Shade(key, 0.78f);
    }
    dl->AddRectFilledMultiColor(t.min, t.max, top, top, bottom, bottom);
    dl->AddRect(t.min, t.max, border, 0.0f, 0, std::max(1.0f, 1.5f * s));

    const ImU32 ink = disabled ? WithAlpha(key_ink, 0.45f) : is_focused ? white : key_ink;
    const ImU32 icon_tint = disabled ? WithAlpha(key_ink, 0.35f) : is_focused ? white : key_ink;
    const float w = t.max.x - t.min.x;
    const float h = t.max.y - t.min.y;

    if (t.action == Action::kKey) {
      const char16_t ch[2] = {t.ch, 0};
      const std::string label = ToUtf8(ch, nullptr);
      const ImVec2 size = measure(30.0f, label);
      text(ImVec2(t.min.x + (w - size.x) * 0.5f, t.min.y + (h - size.y) * 0.5f), 30.0f, ink, label);
      continue;
    }

    const char* label = "";
    switch (t.action) {
      case Action::kLeft: label = "Left"; break;
      case Action::kRight: label = "Right"; break;
      case Action::kNextPage: label = kPageNames[(int(page_) + 1) % 3]; break;
      case Action::kPrevPage: label = kPageNames[(int(page_) + 2) % 3]; break;
      case Action::kCaps: label = "Caps"; break;
      case Action::kBackspace: label = "Backspace"; break;
      case Action::kSpace: label = "Space"; break;
      case Action::kDone: label = "Done"; break;
      default: break;
    }
    const Prompt prompt = PromptFor(t.action);
    const bool wide = t.action == Action::kBackspace || t.action == Action::kSpace;
    const ImVec2 label_size = measure(wide ? 30.0f : 28.0f, label);

    if (wide) {
      const float icon_size = 40.0f * s;
      float x = t.min.x + 16.0f * s;
      if (DrawPrompt(dl, textures, prompt, ImVec2(x, t.min.y + (h - icon_size) * 0.5f), icon_size, icon_tint)) {
        x += icon_size * float(prompt.names.size()) + 12.0f * s;
      } else {
        x = t.min.x + 22.0f * s;
      }
      text(ImVec2(x, t.min.y + (h - label_size.y) * 0.5f), 30.0f, ink, label);
      continue;
    }

    // Side buttons: icons (and the caret arrow) above the label.
    const float icon_size = 46.0f * s;
    const bool arrow = t.action == Action::kLeft || t.action == Action::kRight;
    const float arrow_w = arrow ? 30.0f * s : 0.0f;
    const bool has_icons = DrawPrompt(dl, textures, prompt, ImVec2(), icon_size, icon_tint, true);
    const float row_w = (has_icons ? icon_size * float(prompt.names.size()) : 0.0f) + arrow_w;
    const bool stacked = has_icons || arrow;
    const float row_y = t.min.y + 14.0f * s;
    float x = t.min.x + (w - row_w) * 0.5f;
    auto draw_arrow = [&](float left, bool points_left) {
      const float cy = row_y + icon_size * 0.5f;
      const float x0 = left + 2.0f * s, x1 = left + arrow_w - 6.0f * s;
      const float thick = std::max(1.5f, 3.0f * s);
      const float head = 9.0f * s;
      if (points_left) {
        dl->AddLine(ImVec2(x0 + head * 0.5f, cy), ImVec2(x1, cy), icon_tint, thick);
        dl->AddTriangleFilled(ImVec2(x0, cy), ImVec2(x0 + head, cy - head), ImVec2(x0 + head, cy + head), icon_tint);
      } else {
        dl->AddLine(ImVec2(x0, cy), ImVec2(x1 - head * 0.5f, cy), icon_tint, thick);
        dl->AddTriangleFilled(ImVec2(x1, cy), ImVec2(x1 - head, cy - head), ImVec2(x1 - head, cy + head), icon_tint);
      }
    };
    if (t.action == Action::kLeft) {
      draw_arrow(x, true);
      x += arrow_w;
    }
    if (has_icons) {
      DrawPrompt(dl, textures, prompt, ImVec2(x, row_y), icon_size, icon_tint);
      x += icon_size * float(prompt.names.size());
    }
    if (t.action == Action::kRight) draw_arrow(x, false);

    const float label_y = stacked ? row_y + icon_size + 10.0f * s : t.min.y + (h - label_size.y) * 0.5f;
    text(ImVec2(t.min.x + (w - label_size.x) * 0.5f, label_y), 28.0f, ink, label);
    if (t.action == Action::kCaps && caps_ && CapsEnabled()) {
      const float bar_y = label_y + label_size.y + 4.0f * s;
      dl->AddRectFilled(ImVec2(t.min.x + (w - label_size.x) * 0.5f, bar_y),
                        ImVec2(t.min.x + (w + label_size.x) * 0.5f, bar_y + 4.0f * s),
                        is_focused ? white : accent);
    }
  }

  // The legend; Back takes a click too.
  {
    const float icon_size = 40.0f * s;
    float x = P(43.0f, 0.0f).x;
    const float y = P(0.0f, kFooterY).y;
    auto item = [&](const Prompt& prompt, const char* label) {
      const float left = x;
      if (DrawPrompt(dl, textures, prompt, ImVec2(x, y), icon_size, color::kText)) {
        x += icon_size * float(prompt.names.size()) + 8.0f * s;
      }
      const ImVec2 size = measure(30.0f, label);
      const ImVec2 at(x, y + (icon_size - size.y) * 0.5f);
      text(ImVec2(at.x + 2.0f * s, at.y + 2.0f * s), 30.0f, Rgb(0, 0, 0, 160), label);
      text(at, 30.0f, color::kText, label);
      x += size.x + 34.0f * s;
      return std::make_pair(ImVec2(left, y), ImVec2(x - 26.0f * s, y + icon_size));
    };
    item(SelectPrompt(), "Select");
    back_hit_ = item(BackPrompt(), "Back");
  }
}

}  // namespace rexglue_guide
