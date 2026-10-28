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
  if (keyboard_.active()) {
    keyboard_.Update(in, dt);
    return;
  }

  Page& page = Current();
  if (page.kind == PageKind::kThemes) return HandleGalleryInput(in, page);
  if (page.kind == PageKind::kGamerpics) {
    constexpr int kCols = 5;
    const int count = int(backend_->gamerpic_choices().size());
    int sel = std::clamp(page.selected, 0, std::max(0, count - 1));
    if (in.left) sel = std::max(0, sel - 1);
    if (in.right) sel = std::min(count - 1, sel + 1);
    if (in.up && sel >= kCols) sel -= kCols;
    if (in.down) sel = std::min(count - 1, sel + kCols);
    if (ImGui::GetIO().MouseWheel != 0.0f) sel = std::clamp(sel - int(ImGui::GetIO().MouseWheel) * kCols, 0, count - 1);
    if (sel != page.selected) audio::Play(Sound::kMove);
    page.selected = sel;
    for (size_t i = 0; i < grid_cells_.size(); ++i) {
      if (ImGui::IsMouseHoveringRect(grid_cells_[i].first, grid_cells_[i].second, false)) {
        page.selected = int(i);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          audio::Play(Sound::kSelect);
          ChooseGamerpic(int(i));
          return;
        }
      }
    }
    if (in.accept) {
      audio::Play(Sound::kSelect);
      ChooseGamerpic(page.selected);
    } else if (in.back || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      Pop();
    }
    return;
  }

  const int count = int(rows_.size());
  if (count > 0) {
    int sel = std::clamp(page.selected, 0, count - 1);
    if (in.up && sel > 0) --sel;
    if (in.down && sel + 1 < count) ++sel;
    if (sel != page.selected) audio::Play(Sound::kMove);
    page.selected = sel;
  }
  // A slider row takes left and right (the bumpers still switch blades), and
  // the mouse drags it.
  const ImGuiIO& io = ImGui::GetIO();
  const Row* selected = page.selected >= 0 && page.selected < count ? &rows_[page.selected] : nullptr;
  const bool slide = selected && selected->slider && selected->set_slider && (in.left || in.right);
  if (slide) {
    const float step = in.right ? selected->slider_step : -selected->slider_step;
    selected->set_slider(std::clamp(*selected->slider + step, 0.0f, 1.0f));
    audio::Play(Sound::kMove);
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging_slider_ = -1;
  for (size_t i = 0; i < slider_hits_.size() && i < rows_.size(); ++i) {
    const auto& [s0, s1] = slider_hits_[i];
    if (s1.x <= s0.x || !rows_[i].set_slider) continue;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(s0, s1, false)) dragging_slider_ = int(i);
  }
  if (dragging_slider_ >= 0 && dragging_slider_ < count && slider_hits_.size() > size_t(dragging_slider_)) {
    // The bar is inset from the hit area by its knob.
    const auto& [s0, s1] = slider_hits_[dragging_slider_];
    const float inset = Px(12);
    page.selected = dragging_slider_;
    rows_[dragging_slider_].set_slider(std::clamp((io.MousePos.x - s0.x - inset) / (s1.x - s0.x - 2 * inset), 0.0f, 1.0f));
    return;
  }
  if (screen_ == Screen::kGuide) {
    int delta = 0;
    if (in.lb || (in.left && !slide)) delta = -1;
    if (in.rb || (in.right && !slide)) delta = 1;
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
  // Mouse over rows.
  for (size_t i = 0; i < row_hits_.size() && i < rows_.size(); ++i) {
    if (!ImGui::IsMouseHoveringRect(row_hits_[i].first, row_hits_[i].second, false)) continue;
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) page.selected = int(i);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      page.selected = int(i);
      Activate(rows_[i]);
      return;
    }
  }
  if (io.MouseWheel != 0.0f && count > 0) {
    page.selected = std::clamp(page.selected - int(io.MouseWheel), 0, count - 1);
  }
  if (in.accept && page.selected < count) {
    Activate(rows_[page.selected]);
  } else if (in.back || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    Pop();
  }
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
