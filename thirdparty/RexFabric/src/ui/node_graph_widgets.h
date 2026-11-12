// Small pieces of the node editor's canvas, ported from Toon's node graph widgets: pin icons, the output column's
// layout, the clipboard keys, and text input into a std::string.

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

namespace rexfabric::ui::node_widgets {

enum class PinShape {
  kCircle,  // values
  kArrow,   // execution order
  kSquare,  // an event handed on, a delegate
};

// A pin icon a text line high, filled when linked and hollow when not. Call between ed::BeginPin and ed::EndPin:
// it sets the pin's pivot to the icon's center, so links end on the icon rather than on the label.
void PinIcon(bool linked, ImU32 color, PinShape shape);

// The width of an output row: its label, if any, then the pin icon.
float OutputRowWidth(std::string_view label);

// Places the output column so it ends on the node's right edge, which the header or the inputs and outputs side
// by side set, whichever is wider. `content_x` is the node's content start (the cursor before the header);
// `inputs_width` is 0 for a node without inputs, otherwise the column goes on the inputs' line. Returns the
// column's x; right align each row with AlignOutputRow before its ed::BeginPin.
float PlaceOutputColumn(float content_x, float header_width, float inputs_width, float column_width);
void AlignOutputRow(float column_x, float column_width, std::string_view label);

// Case insensitive search, for the add node menu.
bool ContainsIgnoringCase(std::string_view text, std::string_view pattern);

// The clipboard keys (Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+D), as imgui-node-editor reports them while its canvas has
// focus.
enum class ClipboardAction { kNone, kCopy, kCut, kPaste, kDuplicate };

struct ClipboardRequest {
  ClipboardAction action = ClipboardAction::kNone;
  std::vector<int> nodes;  // the selected nodes, or the one under the mouse
};

// Which clipboard key was pressed this frame, if any. Call between ed::Begin and ed::End.
ClipboardRequest TakeClipboardRequest();
// Selects exactly these nodes, as after pasting.
void SelectNodes(std::span<const int> nodes);

// ImGui text input into a std::string, which grows as it's typed into.
bool InputString(const char* label, std::string& text, ImGuiInputTextFlags flags = 0, const char* hint = nullptr);
bool InputStringMultiline(const char* label, std::string& text, const ImVec2& size, ImGuiInputTextFlags flags = 0);

}  // namespace rexfabric::ui::node_widgets
