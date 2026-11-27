#include "widgets.h"

#include <array>

namespace rexfabric::ui::widgets {
namespace {

// Grows the string as ImGui types into it.
int ResizeCallback(ImGuiInputTextCallbackData* data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* text = static_cast<std::string*>(data->UserData);
    text->resize(static_cast<size_t>(data->BufTextLen));
    data->Buf = text->data();
  }
  return 0;
}

}  // namespace

bool InputString(const char* label, std::string& text, ImGuiInputTextFlags flags, const char* hint) {
  flags |= ImGuiInputTextFlags_CallbackResize;
  if (hint) return ImGui::InputTextWithHint(label, hint, text.data(), text.capacity() + 1, flags, ResizeCallback, &text);
  return ImGui::InputText(label, text.data(), text.capacity() + 1, flags, ResizeCallback, &text);
}

bool InputStringMultiline(const char* label, std::string& text, const ImVec2& size, ImGuiInputTextFlags flags) {
  return ImGui::InputTextMultiline(label, text.data(), text.capacity() + 1, size, flags | ImGuiInputTextFlags_CallbackResize, ResizeCallback, &text);
}

void SyncModifiers(ImGuiIO& io) {
  constexpr std::array<std::array<ImGuiKey, 3>, 3> kModifiers = {{
      {ImGuiMod_Ctrl, ImGuiKey_LeftCtrl, ImGuiKey_RightCtrl},
      {ImGuiMod_Shift, ImGuiKey_LeftShift, ImGuiKey_RightShift},
      {ImGuiMod_Alt, ImGuiKey_LeftAlt, ImGuiKey_RightAlt},
  }};
  for (const auto& [modifier, left, right] : kModifiers) {
    const bool held = ImGui::IsKeyDown(left) || ImGui::IsKeyDown(right);
    if (held != ImGui::IsKeyDown(modifier)) io.AddKeyEvent(modifier, held);
  }
}

}  // namespace rexfabric::ui::widgets
