#pragma once
// The Xbox 360 on-screen keyboard, for the Guide's own text entry and the
// title's (XamShowKeyboardUI, xam_keyboard.cpp). Pad: A presses the key, X
// backspace, Y space, LB/RB move the caret, LT/RT change page (alphabet,
// symbols, accents), LS caps, Start done, B cancel. Physical keyboards type
// directly; the mouse and touch press keys. Button icons follow the last
// device used.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"
#include "input.h"
#include "ui.h"

namespace rexglue_guide {

struct KeyboardRequest {
  std::string title;
  std::string description;
  std::string text;  // starting text
  bool masked = false;     // passwords: shown as *
  size_t max_length = 32;  // characters
  // Once, on the Guide's drawing thread: the text, or nullopt if cancelled.
  std::function<void(std::optional<std::string>)> done;
};

class Keyboard {
 public:
  void Open(KeyboardRequest request);
  bool active() const { return active_; }
  // Cancels it (the Guide is going away).
  void Cancel();
  // Once a frame while active, before Draw: the pad from `in`, the keyboard
  // and mouse from ImGui.
  void Update(const NavInput& in, ImVec2 display, float dt);
  // In the theme's colours. `dim`: darken what's behind it (the Guide, when
  // the keyboard is over that). `header`, when set, draws over the top part's
  // colour [min, max] (the theme's picture), under its text.
  void Draw(ImDrawList* dl, ui::Textures& textures, ImVec2 display, bool dim,
            const std::function<void(ImDrawList*, ImVec2, ImVec2)>& header = nullptr);

 private:
  enum class Page { kAlphabet, kSymbols, kAccents };
  enum class Action { kKey, kLeft, kRight, kNextPage, kPrevPage, kCaps, kBackspace, kSpace, kDone };
  struct Cell {
    int row = 1;
    int col = 1;
  };
  struct Target {
    Action action;
    int row0, row1, col0, col1;  // grid cells covered, inclusive
    char16_t ch = 0;
    ImVec2 min, max;
  };
  // A button's icons, drawn side by side; `colored` ones aren't tinted.
  struct Prompt {
    std::vector<std::string> names;
    bool colored = false;
  };
  struct Repeat {
    float held = -1.0f;
    float next = 0.0f;
    bool Tick(bool down, float dt);
  };

  void Finish(bool accepted);
  void Layout(ImVec2 display);
  void BuildTargets();
  const Target* TargetAt(Cell cell) const;
  void Move(int d_row, int d_col);
  void Activate(const Target& target);
  void Insert(char16_t ch);
  void Backspace();
  void MoveCaret(int delta);
  char16_t KeyChar(int row, int col) const;
  bool CapsEnabled() const { return page_ != Page::kSymbols; }
  // The text as drawn: one * per character when masked.
  std::u16string Shown() const { return request_.masked ? std::u16string(text_.size(), u'*') : text_; }

  void PollPad(const NavInput& in, float dt);
  void PollKeys(ImGuiIO& io);
  void PollPointer(ImGuiIO& io);

  Prompt PromptFor(Action action) const;
  Prompt SelectPrompt() const;
  Prompt BackPrompt() const;
  // Draws a prompt's icons from `pos`; false (nothing drawn) if any is missing.
  bool DrawPrompt(ImDrawList* dl, ui::Textures& textures, const Prompt& prompt, ImVec2 pos, float size, ImU32 tint,
                  bool measure_only = false);

  KeyboardRequest request_;
  bool active_ = false;
  std::u16string text_;
  size_t caret_ = 0;
  float scroll_ = 0.0f;
  Page page_ = Page::kAlphabet;
  bool caps_ = false;
  Cell cursor_;
  std::vector<Target> targets_;
  int pressed_target_ = -1;  // mouse/touch press in progress
  int flash_target_ = -1;    // briefly drawn pressed after activation
  float flash_time_ = 0.0f;
  float opened_for_ = 0.0f;
  bool cancel_clicked_ = false;  // Back in the legend, from the last draw
  std::pair<ImVec2, ImVec2> back_hit_{};

  bool pad_device_ = false;  // icons for the pad (else keyboard and mouse)
  PadStyle style_;
  PadState pad_;
  bool pad_read_ = false;
  bool pad_armed_ = false;  // buttons held at open have been released
  Repeat repeat_up_, repeat_down_, repeat_left_, repeat_right_, repeat_backspace_, repeat_caret_left_,
      repeat_caret_right_;

  // Reference-unit point p is drawn at origin_ + p * scale_.
  ImVec2 origin_;
  float scale_ = 1.0f;
};

// UTF-8 to UTF-16 and back.
std::u16string Utf8ToUtf16(std::string_view s);
std::string Utf16ToUtf8(std::u16string_view s);

// The title's text entry (XamShowKeyboardUI): shown over the game, or over
// the Guide when it's up. False when the Guide isn't installed.
bool ShowTitleKeyboard(KeyboardRequest request);

}  // namespace rexglue_guide
