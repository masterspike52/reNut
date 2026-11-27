// The Lua console window (docs/lua_mods.md, The console): a line of Lua typed and run in the mods' VM on the next
// frame, each line and what it gave back kept above. Up and Down go through the lines typed before.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <rex/ui/ui_system.h>

struct ImGuiInputTextCallbackData;

namespace rexfabric::ui {

class LuaConsoleWindow : public rex::ui::DialogBase {
 public:
  // `closed` runs as the window goes.
  explicit LuaConsoleWindow(std::function<void()> closed);
  ~LuaConsoleWindow() override;

  void Hide() { closing_ = true; }
  bool Draw(ImGuiIO& io) override;

 private:
  struct Line {
    uint64_t id = 0;     // the run it waits for; 0 once answered
    std::string code;
    std::string reply;
    bool error = false;
  };

  void OnDraw(ImGuiIO& io);
  void Submit();
  // Up and Down through the lines typed before.
  static int History(ImGuiInputTextCallbackData* data);

  std::function<void()> closed_;
  bool closing_ = false;
  std::vector<Line> lines_;
  std::vector<std::string> typed_;
  int typed_at_ = -1;  // -1: a new line, not one from the history
  char input_[1024] = {};
  bool scroll_ = false;
  bool focus_ = true;
};

}  // namespace rexfabric::ui
