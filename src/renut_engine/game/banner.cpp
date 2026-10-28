/**
 * @file    game/banner.cpp
 * @brief   A line of text shown a few seconds at the top of the screen
 *          (banner.h): an overlay window that takes no input.
 */
#include "renut_engine/game/banner.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

#include "imgui.h"

#include "renut_engine/gpu/overlay/overlay_ui.h"

namespace {

using Clock = std::chrono::steady_clock;

class Banner : public renut::gpu::overlay::Dialog {
public:
  void Show(std::string text) {
    std::lock_guard lock(mutex_);
    text_ = std::move(text);
    until_ = Clock::now() + std::chrono::seconds(3);
    if (!added_) {
      added_ = true;
      renut::gpu::overlay::AddDialog(this);
    }
  }

  void Draw() override {
    std::string text;
    {
      std::lock_guard lock(mutex_);
      if (Clock::now() >= until_)
        return;
      text = text_;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float size = std::max(16.0f, display.y * 0.026f);
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.06f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, size * 0.4f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(size * 0.8f, size * 0.4f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##renut_banner", nullptr, flags)) {
      ImGui::PushFont(nullptr, size);
      ImGui::TextUnformatted(text.c_str());
      ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }

private:
  std::mutex mutex_;
  std::string text_;
  Clock::time_point until_{};
  bool added_ = false;
};

Banner g_banner;

}  // namespace

namespace renut::game {

void ShowBanner(std::string text) {
  g_banner.Show(std::move(text));
}

}  // namespace renut::game
