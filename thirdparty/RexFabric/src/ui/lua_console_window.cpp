#include "lua_console_window.h"

#include <algorithm>
#include <cfloat>
#include <cstring>

#include <imgui.h>

#include "../lua/lua_scripts.h"
#include "glass.h"
#include "widgets.h"

namespace rexfabric::ui {
namespace {

constexpr ImVec4 kCodeColor{0.62f, 0.70f, 0.88f, 1.0f};
constexpr ImVec4 kWaitingColor{0.6f, 0.6f, 0.65f, 1.0f};
constexpr size_t kMostLines = 400;
constexpr size_t kMostTyped = 100;

}  // namespace

LuaConsoleWindow::LuaConsoleWindow(std::function<void()> closed) : closed_(std::move(closed)) {}

LuaConsoleWindow::~LuaConsoleWindow() {
  if (closed_) closed_();
}

bool LuaConsoleWindow::Draw(ImGuiIO& io) {
  if (closing_) return false;
  OnDraw(io);
  return !closing_;
}

void LuaConsoleWindow::Submit() {
  std::string code = input_;
  input_[0] = '\0';
  typed_at_ = -1;
  if (code.find_first_not_of(" \t") == std::string::npos) return;
  if (typed_.empty() || typed_.back() != code) typed_.push_back(code);
  if (typed_.size() > kMostTyped) typed_.erase(typed_.begin());
  lines_.push_back({.id = lua::QueueConsole(code), .code = std::move(code)});
  if (lines_.size() > kMostLines) lines_.erase(lines_.begin());
  scroll_ = true;
}

int LuaConsoleWindow::History(ImGuiInputTextCallbackData* data) {
  auto* self = static_cast<LuaConsoleWindow*>(data->UserData);
  if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || self->typed_.empty()) return 0;
  const int count = static_cast<int>(self->typed_.size());
  if (data->EventKey == ImGuiKey_UpArrow) self->typed_at_ = self->typed_at_ < 0 ? count - 1 : std::max(0, self->typed_at_ - 1);
  else if (data->EventKey == ImGuiKey_DownArrow) self->typed_at_ = self->typed_at_ < 0 || self->typed_at_ + 1 >= count ? -1 : self->typed_at_ + 1;
  data->DeleteChars(0, data->BufTextLen);
  if (self->typed_at_ >= 0) data->InsertChars(0, self->typed_[static_cast<size_t>(self->typed_at_)].c_str());
  return 0;
}

void LuaConsoleWindow::OnDraw(ImGuiIO& io) {
  // What came back from the frame thread.
  for (lua::ConsoleReply& reply : lua::TakeConsoleReplies()) {
    const auto line = std::find_if(lines_.begin(), lines_.end(), [&reply](const Line& held) { return held.id == reply.id; });
    if (line == lines_.end()) continue;
    line->id = 0;
    line->reply = std::move(reply.text);
    line->error = reply.error;
    scroll_ = true;
  }

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.75f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(820.0f, 360.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 200.0f), ImVec2(FLT_MAX, FLT_MAX));
  bool open = true;
  if (glass::BeginWindow("Lua Console##rexfabric", &open, ImGuiWindowFlags_NoCollapse)) {
    const float input_height = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##lines", ImVec2(0.0f, -input_height), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    if (lines_.empty()) ImGui::TextDisabled("Lua typed here runs in the mods' VM on the next frame, where the game's API works: #tip.objects(\"entityAvatarPinataSignpost\"), tip.class_name(2), x = 1 ...");
    for (const Line& line : lines_) {
      ImGui::TextColored(kCodeColor, "> %s", line.code.c_str());
      if (line.id != 0) ImGui::TextColored(kWaitingColor, "...");
      else if (line.error) ImGui::TextColored(widgets::kErrorColor, "%s", line.reply.c_str());
      else ImGui::TextUnformatted(line.reply.c_str());
    }
    if (scroll_) ImGui::SetScrollHereY(1.0f);
    scroll_ = false;
    ImGui::EndChild();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (focus_) {
      ImGui::SetKeyboardFocusHere();
      focus_ = false;
    }
    if (ImGui::InputTextWithHint("##input", "Lua, then Enter (Escape closes)", input_, sizeof(input_), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, History, this)) {
      Submit();
      focus_ = true;
    }
    // While its line has the keyboard the hotkeys don't reach it, so Escape closes it.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) open = false;
  }
  ImGui::End();
  if (!open) Hide();
}

}  // namespace rexfabric::ui
