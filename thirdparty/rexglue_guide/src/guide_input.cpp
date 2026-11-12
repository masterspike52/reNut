// Navigation input and text entry.

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "audio.h"
#include "guide_dialog.h"
#include "imgui.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

void GuideDialog::HandleInput(const NavInput& in, float dt) {
  if (busy_) return;
  if (message_) {
    Prompt& box = *message_;
    const int count = int(box.buttons.size());
    if (in.up) box.selected = (box.selected + count - 1) % count;
    if (in.down) box.selected = (box.selected + 1) % count;
    if (in.up || in.down) audio::Play(Sound::kMove);
    int chosen = -2;
    if (in.accept) chosen = box.selected;
    if (in.back || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) chosen = -1;
    for (size_t i = 0; i < message_buttons_.size(); ++i) {
      if (ImGui::IsMouseHoveringRect(message_buttons_[i].first, message_buttons_[i].second, false)) {
        if (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0) box.selected = int(i);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) chosen = int(i);
      }
    }
    if (chosen != -2) {
      audio::Play(chosen >= 0 ? Sound::kSelect : Sound::kBack);
      auto done = std::move(box.done);
      message_.reset();
      if (done) done(chosen);
    }
    return;
  }
  Page& page = Current();
  if (page.kind == PageKind::kThemes) return HandleGalleryInput(in, page);
  if (page.kind == PageKind::kGamerpics || page.kind == PageKind::kGamerpicGame) {
    constexpr int kCols = 5;
    const int count = GridCount(page);
    int sel = std::clamp(page.selected, 0, std::max(0, count - 1));
    if (in.left) sel = std::max(0, sel - 1);
    if (in.right) sel = std::min(count - 1, sel + 1);
    if (in.up && sel >= kCols) sel -= kCols;
    if (in.down) sel = std::min(count - 1, sel + kCols);
    if (in.left || in.right || in.up || in.down) page.free_scroll = false;
    if (ImGui::GetIO().MouseWheel != 0.0f) {
      HoldScroll(page);
      page.scroll_goal -= ImGui::GetIO().MouseWheel;  // a row a notch
    }
    if (sel != page.selected) audio::Play(Sound::kMove);
    page.selected = sel;
    const bool moved = ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0;
    for (size_t i = 0; i < grid_cells_.size(); ++i) {
      if (ImGui::IsMouseHoveringRect(grid_cells_[i].first, grid_cells_[i].second, false)) {
        if (moved || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          HoldScroll(page);
          page.selected = int(i);
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          audio::Play(Sound::kSelect);
          ChooseGridPicture(page, int(i));
          return;
        }
      }
    }
    if (in.accept) {
      audio::Play(Sound::kSelect);
      ChooseGridPicture(page, page.selected);
    } else if (in.back || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      Pop();
    }
    return;
  }

  if (screen_ == Screen::kGuide) {
    // The bumpers, and the side blades clicked.
    int delta = in.lb ? -1 : in.rb ? 1 : 0;
    for (const auto& [blade, rect] : blade_hits_) {
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
          ImGui::IsMouseHoveringRect(rect.first, rect.second, false)) {
        delta = blade - blade_;
      }
    }
    if (delta != 0) {
      SwitchBlade(delta);
      return;
    }
    if (in.x && page.kind == PageKind::kHome && backend_->signed_in()) {
      ConfirmSignOut();
      return;
    }
  }
  const ListInput result = HandleList(in, list_, page, true);
  // Left and right switch blades, unless the row took them.
  if (result.sideways != 0 && screen_ == Screen::kGuide) SwitchBlade(result.sideways);
  else if (result.back) Pop();
}

ListInput GuideDialog::HandleList(const NavInput& nav, ListView& v, Page& page, bool mouse) {
  const ImGuiIO& io = ImGui::GetIO();
  NavInput in = nav;  // less what a Bar or Lists row takes
  const int count = int(v.rows.size());
  auto selectable = [&](int i) { return i >= 0 && i < count && v.rows[size_t(i)].Selectable(); };
  if (count > 0) {
    int sel = std::clamp(page.selected, 0, count - 1);
    // Off a row that can't be picked (the page changed under it): the nearest
    // that can, below first.
    for (int i = sel; !selectable(sel) && i < count; ++i) {
      if (selectable(i)) sel = i;
    }
    for (int i = sel; !selectable(sel) && i >= 0; --i) {
      if (selectable(i)) sel = i;
    }
    if (selectable(sel) && v.rows[size_t(sel)].Inner()) {
      const Row& row = v.rows[size_t(sel)];
      InnerInput(in, row, page.inner[row.key]);
    }
    const int was = sel;
    if (in.up) {
      for (int i = sel - 1; i >= 0; --i) {
        if (selectable(i)) {
          sel = i;
          break;
        }
      }
    }
    if (in.down) {
      for (int i = sel + 1; i < count; ++i) {
        if (selectable(i)) {
          sel = i;
          break;
        }
      }
    }
    if (in.up || in.down) page.free_scroll = false;
    if (sel != was) audio::Play(Sound::kMove);
    page.selected = sel;
  }
  // A slider or stepper row takes left and right (the bumpers still switch
  // blades), and the mouse drags a slider.
  const Row* selected = selectable(page.selected) ? &v.rows[size_t(page.selected)] : nullptr;
  bool taken = false;
  if (selected && (in.left || in.right)) {
    if (selected->slider && selected->set_slider) {
      const float step = in.right ? selected->slider_step : -selected->slider_step;
      selected->set_slider(std::clamp(*selected->slider + step, 0.0f, 1.0f));
      taken = true;
    } else if (selected->step && !selected->disabled) {
      selected->step(in.right ? 1 : -1);
      taken = true;
    }
    if (taken) audio::Play(Sound::kMove);
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) v.dragging_slider = -1;
  for (size_t i = 0; mouse && i < v.slider_hits.size() && i < v.rows.size(); ++i) {
    const auto& [s0, s1] = v.slider_hits[i];
    if (s1.x <= s0.x || !v.rows[i].set_slider) continue;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(s0, s1, false)) v.dragging_slider = int(i);
  }
  if (v.dragging_slider >= 0 && v.dragging_slider < count && v.slider_hits.size() > size_t(v.dragging_slider)) {
    // The bar is inset from the hit area by its knob.
    const auto& [s0, s1] = v.slider_hits[size_t(v.dragging_slider)];
    const float inset = Px(12);
    page.selected = v.dragging_slider;
    v.rows[size_t(v.dragging_slider)].set_slider(
        std::clamp((io.MousePos.x - s0.x - inset) / (s1.x - s0.x - 2 * inset), 0.0f, 1.0f));
    return {};
  }
  if (!taken && (in.left || in.right)) return {.sideways = in.right ? 1 : -1};
  // The scroll bar's knob: dragged, or the track clicked to bring it there.
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) v.dragging_scroll = false;
  if (mouse && v.scroll_bar.shown && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      ImGui::IsMouseHoveringRect(v.scroll_bar.hit0, v.scroll_bar.hit1, false)) {
    v.dragging_scroll = true;
    const float on_knob = io.MousePos.y - v.scroll_bar.knob_y;
    v.drag_grab = on_knob >= 0.0f && on_knob <= v.scroll_bar.knob_h ? on_knob : v.scroll_bar.knob_h * 0.5f;
  }
  if (v.dragging_scroll && v.scroll_bar.shown) {
    HoldScroll(page);
    const float travel = v.scroll_bar.bottom - v.scroll_bar.top - v.scroll_bar.knob_h;
    const float t = travel > 0.0f ? std::clamp((io.MousePos.y - v.drag_grab - v.scroll_bar.top) / travel, 0.0f, 1.0f) : 0.0f;
    // Straight there, not eased: the list keeps up with the knob.
    page.scroll_goal = page.scroll = t * v.scroll_bar.max;
    return {};
  }
  // Inside Bar and Lists rows, then over rows.
  bool wheel_taken = false;
  if (mouse) {
    const InnerMouse inner = InnerMouseInput(v, page);
    if (inner.clicked) return {};
    wheel_taken = inner.wheel;
  }
  for (size_t i = 0; mouse && i < v.row_hits.size() && i < v.rows.size(); ++i) {
    if (!selectable(int(i)) || !ImGui::IsMouseHoveringRect(v.row_hits[i].first, v.row_hits[i].second, false)) continue;
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) {
      HoldScroll(page);
      page.selected = int(i);
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      HoldScroll(page);
      page.selected = int(i);
      Activate(v.rows[i]);
      return {};
    }
  }
  if (mouse && io.MouseWheel != 0.0f && count > 0 && !wheel_taken) {
    HoldScroll(page);
    page.scroll_goal -= io.MouseWheel * Px(112);  // two rows a notch
  }
  if (in.accept && selectable(page.selected)) {
    Activate(v.rows[size_t(page.selected)]);
  } else if (in.back || (mouse && ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
    return {.back = true};
  }
  return {};
}

void GuideDialog::Activate(const Row& row) {
  if (row.disabled || !row.activate) return;
  auto activate = row.activate;  // the call may rebuild the rows
  audio::Play(Sound::kSelect);
  activate();
}

void GuideDialog::OpenKeyboard(KeyboardRequest request) {
  if (!config_.show_keyboard) return keyboard_.Open(std::move(request));
  external_keyboard_open_ = true;
  const TextInput input{request.title, request.description, request.text, request.masked, request.max_length};
  config_.show_keyboard(input, [results = external_results_, done = std::move(request.done)](
                                   std::optional<std::string> text) mutable {
    std::lock_guard lock(results->mutex);
    results->done.push_back({std::move(done), std::move(text)});
  });
}

void GuideDialog::PumpExternalKeyboard() {
  std::vector<std::pair<std::function<void(std::optional<std::string>)>, std::optional<std::string>>> done;
  {
    std::lock_guard lock(external_results_->mutex);
    done.swap(external_results_->done);
  }
  for (auto& [callback, text] : done) {
    external_keyboard_open_ = false;  // the callback may open the next one
    if (callback) callback(std::move(text));
  }
}

}  // namespace rexglue_guide
