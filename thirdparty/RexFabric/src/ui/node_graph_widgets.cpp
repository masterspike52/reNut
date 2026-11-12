#include "node_graph_widgets.h"

#include <algorithm>
#include <cctype>

#include <imgui_node_editor.h>

namespace ed = ax::NodeEditor;

namespace rexfabric::ui::node_widgets {
namespace {

// Space between the input and output columns, in multiples of the font size.
constexpr float kOutputColumnGap = 2.0f;

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

void PinIcon(bool linked, ImU32 color, PinShape shape) {
  const float size = ImGui::GetTextLineHeight();
  const ImVec2 position = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(size, size));

  const ImVec2 center(position.x + size * 0.5f, position.y + size * 0.5f);
  ed::PinPivotRect(center, center);

  ImDrawList* draw = ImGui::GetWindowDrawList();
  if (shape == PinShape::kCircle) {
    if (linked) draw->AddCircleFilled(center, size * 0.28f, color);
    else draw->AddCircle(center, size * 0.28f, color, 12, 1.5f);
    return;
  }

  if (shape == PinShape::kSquare) {
    const float half = size * 0.24f;
    const ImVec2 min(center.x - half, center.y - half);
    const ImVec2 max(center.x + half, center.y + half);
    if (linked) draw->AddRectFilled(min, max, color, 1.5f);
    else draw->AddRect(min, max, color, 1.5f, 0, 1.5f);
    return;
  }

  // A right pointing triangle, as execution pins usually are.
  const float half = size * 0.3f;
  const ImVec2 a(center.x - half, center.y - half);
  const ImVec2 b(center.x + half, center.y);
  const ImVec2 c(center.x - half, center.y + half);
  if (linked) draw->AddTriangleFilled(a, b, c, color);
  else draw->AddTriangle(a, b, c, color, 1.5f);
}

float OutputRowWidth(std::string_view label) {
  const float icon = ImGui::GetTextLineHeight();
  if (label.empty()) return icon;
  return ImGui::CalcTextSize(label.data(), label.data() + label.size()).x + ImGui::GetStyle().ItemSpacing.x + icon;
}

float PlaceOutputColumn(float content_x, float header_width, float inputs_width, float column_width) {
  const float side_by_side = inputs_width > 0.0f ? inputs_width + ImGui::GetFontSize() * kOutputColumnGap + column_width : column_width;
  const float column_x = content_x + std::max(header_width, side_by_side) - column_width;
  if (inputs_width > 0.0f) ImGui::SameLine();
  ImGui::SetCursorPosX(column_x);
  return column_x;
}

void AlignOutputRow(float column_x, float column_width, std::string_view label) { ImGui::SetCursorPosX(column_x + column_width - OutputRowWidth(label)); }

bool ContainsIgnoringCase(std::string_view text, std::string_view pattern) {
  const auto equal = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
  return std::search(text.begin(), text.end(), pattern.begin(), pattern.end(), equal) != text.end();
}

ClipboardRequest TakeClipboardRequest() {
  ClipboardRequest request;
  if (!ed::BeginShortcut()) return request;
  if (ed::AcceptCopy()) request.action = ClipboardAction::kCopy;
  else if (ed::AcceptCut()) request.action = ClipboardAction::kCut;
  else if (ed::AcceptPaste()) request.action = ClipboardAction::kPaste;
  else if (ed::AcceptDuplicate()) request.action = ClipboardAction::kDuplicate;

  if (request.action != ClipboardAction::kNone) {
    std::vector<ed::NodeId> nodes(static_cast<size_t>(std::max(ed::GetActionContextSize(), 0)));
    nodes.resize(static_cast<size_t>(ed::GetActionContextNodes(nodes.data(), static_cast<int>(nodes.size()))));
    for (const ed::NodeId node : nodes) request.nodes.push_back(static_cast<int>(node.Get()));
  }
  ed::EndShortcut();
  return request;
}

void SelectNodes(std::span<const int> nodes) {
  ed::ClearSelection();
  for (int node : nodes) ed::SelectNode(ed::NodeId(static_cast<size_t>(node)), true);
}

bool InputString(const char* label, std::string& text, ImGuiInputTextFlags flags, const char* hint) {
  flags |= ImGuiInputTextFlags_CallbackResize;
  if (hint) return ImGui::InputTextWithHint(label, hint, text.data(), text.capacity() + 1, flags, ResizeCallback, &text);
  return ImGui::InputText(label, text.data(), text.capacity() + 1, flags, ResizeCallback, &text);
}

bool InputStringMultiline(const char* label, std::string& text, const ImVec2& size, ImGuiInputTextFlags flags) {
  return ImGui::InputTextMultiline(label, text.data(), text.capacity() + 1, size, flags | ImGuiInputTextFlags_CallbackResize, ResizeCallback, &text);
}

}  // namespace rexfabric::ui::node_widgets
