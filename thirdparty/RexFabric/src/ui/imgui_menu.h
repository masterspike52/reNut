// The mods menu as plain ImGui windows, for games without RexLiveless: the mods with their load order, and
// the Thunderstore browser beside it when the game has a community.

#pragma once

#include <functional>
#include <string>

#include <rex/ui/ui_system.h>

#include "menu_model.h"

struct ImVec2;

namespace rexfabric::ui {

class ModsWindow : public rex::ui::DialogBase {
 public:
  // `closed` runs as the window goes, whether its own close button or the menu key closed it.
  ModsWindow(MenuModel& model, std::function<void()> closed);
  ~ModsWindow() override;

  void Hide() { closing_ = true; }

  bool Draw(ImGuiIO& io) override;

 private:
  void OnDraw(ImGuiIO& io);

  void DrawMods();
  void DrawColumn(const char* id, bool enabled, float width, float height);
  void DrawRow(size_t index, bool enabled_column);

  void DrawBrowser();
  void DrawToolbar();
  void DrawGrid(float height, int& pages);
  void DrawCard(const net::Package& package, const ImVec2& size);
  void DrawPager(int pages);

  MenuModel& model_;
  std::function<void()> closed_;
  // The UI drops the window on the next Draw.
  bool closing_ = false;
  // One change per frame, made once the frame's rows are drawn.
  std::function<void()> pending_;

  bool browser_open_ = false;
  char search_[128] = {};
  double search_typed_at_ = 0.0;  // 0: nothing typed since the search last applied
  int page_ = 0;                  // from 0, a grid of packages each
  int page_input_ = 1;
};

}  // namespace rexfabric::ui
