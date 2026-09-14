#pragma once

// F6 overlay for the keyboard/mouse controls. Everything here is just a view
// onto the "Nuts&Bolts/Controls" cvars -- the same values the F4 settings
// overlay and the in-game reNut Settings section edit, and the same ones that
// persist to renut.toml. The only thing this adds is press-a-key rebinding,
// which the F4 overlay only offers for its own "Input/Keybinds" category.

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "imgui.h"
#include "renut_engine/mnk_controls.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

// cvar_menu.cpp: merge the current cvar values into renut.toml.
void RenutSaveConfig();

class MnkControlsDialog : public rex::ui::ImGuiDialog {
 public:
  explicit MnkControlsDialog(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {
    rex::ui::RegisterBind("bind_mnk_controls", "F6", "Toggle keyboard/mouse controls overlay",
                          [this] { SetVisible(!visible_); });
  }

  ~MnkControlsDialog() override {
    rex::ui::UnregisterBind("bind_mnk_controls");
    renut::mnk::SetOverlayOpen(false);
  }

  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!visible_) {
      return;
    }

    // A rebind that was armed and then had the overlay closed under it would
    // leave the listener swallowing keys, so land any pending capture first.
    std::string captured;
    if (renut::mnk::TakeCaptured(captured) && !capturing_.empty()) {
      rex::cvar::SetFlagByName(capturing_, captured);
      capturing_.clear();
      RenutSaveConfig();
    } else if (!capturing_.empty() && !renut::mnk::IsCapturing()) {
      capturing_.clear();  // cancelled with Escape
    }

    ImGui::SetNextWindowSize(ImVec2(460.0f, 560.0f), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("reNut - Keyboard & Mouse", &open)) {
      DrawSettings();
      ImGui::Separator();
      DrawBinds();
    }
    ImGui::End();

    if (!open) {
      SetVisible(false);
    }
  }

 private:
  void SetVisible(bool visible) {
    visible_ = visible;
    if (!visible) {
      renut::mnk::CancelCapture();
      capturing_.clear();
    }
    renut::mnk::SetOverlayOpen(visible);
  }

  static bool GetBool(const char* name) { return rex::cvar::GetFlagByName(name) == "true"; }
  static int GetInt(const char* name) {
    return std::atoi(rex::cvar::GetFlagByName(name).c_str());
  }

  void SetAndSave(const char* name, const std::string& value) {
    rex::cvar::SetFlagByName(name, value);
    RenutSaveConfig();
  }

  void DrawSettings() {
    bool enabled = GetBool("mnk_controls");
    if (ImGui::Checkbox("Keyboard & mouse controls", &enabled)) {
      SetAndSave("mnk_controls", enabled ? "true" : "false");
    }
    ImGui::TextDisabled("Movement and camera are written straight into the game's");
    ImGui::TextDisabled("own input, not through an emulated pad. A controller keeps");
    ImGui::TextDisabled("working alongside it.");

    ImGui::Spacing();

    int sensitivity = GetInt("mnk_look_sensitivity");
    if (ImGui::SliderInt("Look sensitivity", &sensitivity, 1, 400, "%d%%")) {
      SetAndSave("mnk_look_sensitivity", std::to_string(sensitivity));
    }

    bool invert = GetBool("mnk_invert_look");
    if (ImGui::Checkbox("Invert look Y", &invert)) {
      SetAndSave("mnk_invert_look", invert ? "true" : "false");
    }

    int walk = GetInt("mnk_walk_scale");
    if (ImGui::SliderInt("Walk speed", &walk, 5, 100, "%d%%")) {
      SetAndSave("mnk_walk_scale", std::to_string(walk));
    }

    bool lock = GetBool("mnk_lock_cursor");
    if (ImGui::Checkbox("Lock cursor to window", &lock)) {
      SetAndSave("mnk_lock_cursor", lock ? "true" : "false");
    }

    int slot = GetInt("mnk_pad_slot");
    if (ImGui::SliderInt("Player slot", &slot, 0, 3)) {
      SetAndSave("mnk_pad_slot", std::to_string(slot));
    }
  }

  void DrawBinds() {
    std::size_t count = 0;
    const renut::mnk::BindInfo* binds = renut::mnk::BindList(count);

    const char* section = nullptr;
    for (std::size_t i = 0; i < count; ++i) {
      const auto& bind = binds[i];
      if (!section || std::strcmp(section, bind.section) != 0) {
        section = bind.section;
        ImGui::Spacing();
        ImGui::TextDisabled("%s", section);
      }

      ImGui::PushID(static_cast<int>(i));
      ImGui::Text("%s", bind.label);
      ImGui::SameLine(180.0f);

      if (capturing_ == bind.cvar) {
        ImGui::Button("Press any key...", ImVec2(150.0f, 0.0f));
        ImGui::SameLine();
        ImGui::TextDisabled("(Esc cancels)");
      } else {
        const std::string value = rex::cvar::GetFlagByName(bind.cvar);
        if (ImGui::Button(value.empty() ? "unbound##rebind" : (value + "##rebind").c_str(),
                          ImVec2(150.0f, 0.0f))) {
          capturing_ = bind.cvar;
          renut::mnk::BeginCapture();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
          SetAndSave(bind.cvar, "");
        }
      }
      ImGui::PopID();
    }
  }

  bool visible_ = false;
  std::string capturing_;  // cvar name currently awaiting a key, empty if none
};