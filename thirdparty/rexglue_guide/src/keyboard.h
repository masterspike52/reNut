#pragma once
// 360-style on-screen keyboard. Pad: A types, X backspace, Y space, LB/RB move
// the cursor, LT symbols, LS caps, Start done, B cancel. Physical keyboards
// type directly.

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"
#include "input.h"
#include "ui.h"

namespace rexglue_guide {

struct KeyboardRequest {
  std::string title;
  std::string description;
  std::string text;
  bool masked = false;
  size_t max_length = 32;  // characters
  std::function<void(std::optional<std::string>)> done;
};

class Keyboard {
 public:
  void Open(KeyboardRequest request);
  bool active() const { return active_; }
  void Update(const NavInput& in, float dt);
  void Draw(ImDrawList* dl, ui::Textures& textures, ImVec2 display, bool pad, float time);

 private:
  enum class Action { kKey, kSpace, kBackspace, kDone };
  struct Cell {
    Action action = Action::kKey;
    std::string label;
    ImVec2 min, max;
  };

  void Insert(const std::string& utf8);
  void Backspace();
  void MoveCaret(int delta);
  void Finish(bool accept);
  void Activate(const Cell& cell);
  std::string KeyAt(int row, int col) const;
  int RowCount() const { return 5; }
  int ColCount(int row) const { return row == 4 ? 3 : 10; }

  KeyboardRequest request_;
  bool active_ = false;
  std::string text_;
  size_t caret_ = 0;  // byte offset
  int row_ = 0, col_ = 0;
  bool caps_ = false;
  bool symbols_ = false;
  bool pad_ = true;
  float blink_ = 0.0f;
  std::vector<std::pair<Cell, std::pair<int, int>>> cells_;  // from the last draw, for the mouse
};

}  // namespace rexglue_guide
