#include "keyboard.h"

#include <algorithm>
#include <cmath>

#include "audio.h"
#include "imgui_internal.h"

namespace rexglue_guide {
namespace {

constexpr const char* kLetters[4] = {"abcdefghij", "klmnopqrst", "uvwxyz-_.@", "1234567890"};
constexpr const char* kSymbols[4] = {"!?#$%&*()+", "=/\\:;'\",<>", "[]{}^`~|@.", "1234567890"};

size_t CodepointCount(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

std::string Utf8(unsigned int cp) {
  std::string out;
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
  return out;
}

}  // namespace

void Keyboard::Open(KeyboardRequest request) {
  request_ = std::move(request);
  text_ = request_.text;
  caret_ = text_.size();
  row_ = col_ = 0;
  caps_ = symbols_ = false;
  active_ = true;
  blink_ = 0.0f;
}

std::string Keyboard::KeyAt(int row, int col) const {
  const char* const* rows = symbols_ ? kSymbols : kLetters;
  char c = rows[row][col];
  if (caps_ && c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  return std::string(1, c);
}

void Keyboard::Insert(const std::string& utf8) {
  if (CodepointCount(text_) >= request_.max_length) return;
  text_.insert(caret_, utf8);
  caret_ += utf8.size();
  blink_ = 0.0f;
}

void Keyboard::Backspace() {
  if (caret_ == 0) return;
  size_t start = caret_ - 1;
  while (start > 0 && (static_cast<unsigned char>(text_[start]) & 0xC0) == 0x80) --start;
  text_.erase(start, caret_ - start);
  caret_ = start;
  blink_ = 0.0f;
}

void Keyboard::MoveCaret(int delta) {
  while (delta < 0 && caret_ > 0) {
    --caret_;
    while (caret_ > 0 && (static_cast<unsigned char>(text_[caret_]) & 0xC0) == 0x80) --caret_;
    ++delta;
  }
  while (delta > 0 && caret_ < text_.size()) {
    ++caret_;
    while (caret_ < text_.size() && (static_cast<unsigned char>(text_[caret_]) & 0xC0) == 0x80) {
      ++caret_;
    }
    --delta;
  }
  blink_ = 0.0f;
}

void Keyboard::Finish(bool accept) {
  active_ = false;
  audio::Play(accept ? assets::Sound::kSelect : assets::Sound::kBack);
  auto done = std::move(request_.done);
  request_ = {};
  if (done) done(accept ? std::optional<std::string>(text_) : std::nullopt);
}

void Keyboard::Activate(const Cell& cell) {
  switch (cell.action) {
    case Action::kKey: Insert(cell.label); break;
    case Action::kSpace: Insert(" "); break;
    case Action::kBackspace: Backspace(); break;
    case Action::kDone: Finish(true); break;
  }
}

void Keyboard::Update(const NavInput& in, float dt) {
  if (!active_) return;
  blink_ += dt;
  pad_ = in.pad_used;
  ImGuiIO& io = ImGui::GetIO();

  // Physical keyboard. Its characters only come while the window takes text
  // input, which the drawer turns on when ImGui reports a widget wanting text
  // (Platform_SetImeDataFn at EndFrame); this keyboard is not an ImGui widget,
  // so say so for it. NewFrame clears the flag: set it every frame it is up.
  ImGui::GetCurrentContext()->PlatformImeData.WantTextInput = true;
  for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
    const unsigned int cp = io.InputQueueCharacters[i];
    if (cp >= 0x20 && cp != 0x7F) Insert(Utf8(cp));
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) Backspace();
  if (ImGui::IsKeyPressed(ImGuiKey_Delete) && caret_ < text_.size()) {
    MoveCaret(1);
    Backspace();
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Home)) caret_ = 0;
  if (ImGui::IsKeyPressed(ImGuiKey_End)) caret_ = text_.size();
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
    Finish(true);
    return;
  }

  // Pad.
  if (in.back) {
    Finish(false);
    return;
  }
  if (in.start) {
    Finish(true);
    return;
  }
  if (in.x) Backspace();
  if (in.y) Insert(" ");
  if (in.lb) MoveCaret(-1);
  if (in.rb) MoveCaret(1);
  if (in.lt) symbols_ = !symbols_;
  if (in.ls) caps_ = !caps_;

  const int old_row = row_, old_col = col_;
  if (in.up) row_ = (row_ + RowCount() - 1) % RowCount();
  if (in.down) row_ = (row_ + 1) % RowCount();
  if (row_ != old_row) {
    // Moving between the 10-key rows and the 3 wide action keys.
    if (row_ == 4) col_ = std::min(col_ * 3 / 10, 2);
    else if (old_row == 4) col_ = std::min(col_ * 10 / 3 + 1, 9);
  }
  if (in.left) col_ = (col_ + ColCount(row_) - 1) % ColCount(row_);
  if (in.right) col_ = (col_ + 1) % ColCount(row_);
  if (row_ != old_row || col_ != old_col) audio::Play(assets::Sound::kMove);

  if (in.accept) {
    for (const auto& [cell, pos] : cells_) {
      if (pos.first == row_ && pos.second == col_) {
        audio::Play(assets::Sound::kMove);
        Activate(cell);
        break;
      }
    }
  }

  // Mouse.
  if (active_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    for (const auto& [cell, pos] : cells_) {
      if (ImGui::IsMouseHoveringRect(cell.min, cell.max, false)) {
        row_ = pos.first;
        col_ = pos.second;
        Activate(cell);
        break;
      }
    }
  }
  if (active_ && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) Finish(false);
}

void Keyboard::Draw(ImDrawList* dl, ui::Textures& textures, ImVec2 display, bool pad, float time) {
  if (!active_) return;
  using namespace ui;
  dl->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 120));

  const float w = Px(860), h = Px(560);
  const ImVec2 p0(std::floor((display.x - w) * 0.5f), std::floor((display.y - h) * 0.5f));
  const ImVec2 p1(p0.x + w, p0.y + h);
  Shadow(dl, p0, p1, Px(14), Px(40), 1.0f);
  dl->AddRectFilled(p0, p1, color::kBlade, Px(14));

  float y = p0.y + Px(24);
  Text(dl, Semibold(), Px(30), ImVec2(p0.x + Px(32), y), color::kText, request_.title);
  y += Px(42);
  if (!request_.description.empty()) {
    Text(dl, Regular(), Px(20), ImVec2(p0.x + Px(32), y), color::kTextDim, request_.description);
  }
  y += Px(36);

  // Text field.
  const ImVec2 f0(p0.x + Px(32), y), f1(p1.x - Px(32), y + Px(58));
  dl->AddRectFilled(f0, f1, IM_COL32(235, 238, 243, 255), Px(6));
  std::string shown = request_.masked ? std::string() : text_;
  size_t caret_shown = caret_;
  if (request_.masked) {
    const size_t before = CodepointCount(text_.substr(0, caret_));
    for (size_t i = 0; i < CodepointCount(text_); ++i) shown += "\xE2\x80\xA2";  // bullet
    caret_shown = before * 3;
  }
  const float text_size = Px(30);
  const ImVec2 tp(f0.x + Px(16), f0.y + (f1.y - f0.y - text_size) * 0.5f - Px(2));
  Text(dl, Regular(), text_size, tp, IM_COL32(28, 32, 38, 255), shown);
  if (std::fmod(blink_, 1.0f) < 0.6f) {
    const float cx = tp.x + Measure(Regular(), text_size, std::string_view(shown).substr(0, caret_shown)).x;
    dl->AddRectFilled(ImVec2(cx, tp.y + Px(3)), ImVec2(cx + Px(2), tp.y + text_size), IM_COL32(28, 32, 38, 255));
  }
  y = f1.y + Px(24);

  // Keys.
  cells_.clear();
  const float gap = Px(8);
  const float key_w = (f1.x - f0.x - gap * 9) / 10.0f, key_h = Px(54);
  for (int r = 0; r < RowCount(); ++r) {
    for (int c = 0; c < ColCount(r); ++c) {
      Cell cell;
      if (r < 4) {
        cell.label = KeyAt(r, c);
        cell.min = ImVec2(f0.x + c * (key_w + gap), y);
        cell.max = ImVec2(cell.min.x + key_w, y + key_h);
      } else {
        static constexpr Action kActions[3] = {Action::kSpace, Action::kBackspace, Action::kDone};
        static constexpr const char* kLabels[3] = {"Space", "Backspace", "Done"};
        cell.action = kActions[c];
        cell.label = kLabels[c];
        const float wide = (f1.x - f0.x - gap * 2) / 3.0f;
        cell.min = ImVec2(f0.x + c * (wide + gap), y);
        cell.max = ImVec2(cell.min.x + wide, y + key_h);
      }
      const bool selected = (r == row_ && c == col_) && pad;
      const bool hovered = !pad && ImGui::IsMouseHoveringRect(cell.min, cell.max, false);
      dl->AddRectFilled(cell.min, cell.max,
                        selected ? color::kGreen : hovered ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 18),
                        Px(5));
      const float size = r < 4 ? Px(28) : Px(22);
      const ImVec2 extent = Measure(r < 4 ? Regular() : Semibold(), size, cell.label);
      Text(dl, r < 4 ? Regular() : Semibold(), size,
           ImVec2((cell.min.x + cell.max.x - extent.x) * 0.5f, (cell.min.y + cell.max.y - extent.y) * 0.5f),
           color::kText, cell.label);
      cells_.push_back({cell, {r, c}});
    }
    y += key_h + gap;
  }

  // Legend.
  y = p1.y - Px(40);
  float x = p0.x + Px(32);
  const float icon = Px(30);
  auto icon_label = [&](assets::Icon i, const char* label) {
    Image(dl, textures.Icon(i), ImVec2(x, y - icon * 0.5f), ImVec2(x + icon, y + icon * 0.5f));
    x += icon + Px(6);
    Caps(dl, Semibold(), Px(13), ImVec2(x, y - Px(8)), color::kTextDim, label, Px(1));
    x += CapsWidth(Semibold(), Px(13), label, Px(1)) + Px(18);
  };
  auto face_label = [&](char b, const char* label) {
    FaceButton(dl, ImVec2(x + Px(11), y), Px(11), b);
    x += Px(28);
    Caps(dl, Semibold(), Px(13), ImVec2(x, y - Px(8)), color::kTextDim, label, Px(1));
    x += CapsWidth(Semibold(), Px(13), label, Px(1)) + Px(18);
  };
  if (pad) {
    face_label('X', "Backspace");
    face_label('Y', "Space");
    icon_label(assets::Icon::kKeyLt, symbols_ ? "Letters" : "Symbols");
    icon_label(assets::Icon::kKeyLs, caps_ ? "Lowercase" : "Caps");
    icon_label(assets::Icon::kKeyCursorLb, "");
    x -= Px(18);
    icon_label(assets::Icon::kKeyCursorRb, "Cursor");
    icon_label(assets::Icon::kKeyStart, "Done");
  } else {
    Caps(dl, Semibold(), Px(13), ImVec2(x, y - Px(8)), color::kTextDim,
         "Type to enter text   -   Enter: done   -   Esc: cancel", Px(1));
  }
  (void)time;
}

}  // namespace rexglue_guide
