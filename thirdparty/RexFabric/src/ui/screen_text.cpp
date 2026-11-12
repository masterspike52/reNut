#include "screen_text.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>

#include <imgui.h>
#include <imgui_internal.h>

#include <rex/ui/ui_system.h>

namespace rexfabric::ui {
namespace {

using Clock = std::chrono::steady_clock;

// Older lines than this many go, so a print in every frame doesn't fill the screen.
constexpr size_t kMaxLines = 40;
// The text's size, in multiples of the UI's font size, so it reads over the game from across the room.
constexpr float kTextScale = 1.6f;
// From the screen's top right corner, and between lines, in multiples of the text's size.
constexpr float kMargin = 1.0f;
constexpr float kLineGap = 0.2f;
// Lines fade out over their last moments.
constexpr double kFadeSeconds = 0.4;

struct Line {
  std::string text;
  Clock::time_point until;
  uint32_t rgb = 0;
  float size = 0.0f;  // pixels; 0 is kTextScale times the UI's font
  std::string key;    // empty: none
};

// The largest a line can be, in pixels, so a typo doesn't fill the screen.
constexpr float kMaxTextSize = 200.0f;

// A drawn shape and when it goes (Clock::time_point::max(): when it's removed).
struct Shape {
  ScreenShape shape;
  Clock::time_point until;
};

// So shapes drawn every frame without a key can't pile up without end.
constexpr size_t kMaxShapes = 4096;
// An unkeyed shape given no time stays this long, so one drawn every tick stays up.
constexpr double kShapeTick = 0.1;

struct Screen {
  std::mutex mutex;
  std::deque<Line> lines;  // newest first
  std::deque<Shape> shapes;  // oldest first, drawn in order
  // Made with the menus, on the UI thread, and kept: prints come from wherever the scripts run.
  bool overlay = false;
};

Screen& Get() {
  static Screen screen;
  return screen;
}

// Draws the lines over everything; nothing while there are none.
class Overlay final : public rex::ui::DialogBase {
 public:
  bool Draw(ImGuiIO& io) override {
    OnDraw(io);
    return true;
  }

 private:
  void OnDraw(ImGuiIO& io) {
    Screen& screen = Get();
    std::lock_guard lock(screen.mutex);
    const Clock::time_point now = Clock::now();
    std::erase_if(screen.lines, [now](const Line& line) { return line.until <= now; });
    std::erase_if(screen.shapes, [now](const Shape& shape) { return shape.until <= now; });
    if (screen.lines.empty() && screen.shapes.empty()) return;

    // In a window of its own over the whole screen, which takes no input and is kept in front: a game drawing the
    // UI itself may not draw the foreground list of a dialog without windows.
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    const bool shown = ImGui::Begin("##rexfabric_screen_text", nullptr, kFlags);
    ImGui::PopStyleVar(2);
    if (shown) {
      //ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
      // Through GetCurrentContext: the runtime exports ImGui's functions but not GImGui, which GetCurrentWindow reads.
      ImGui::BringWindowToDisplayFront(ImGui::GetCurrentContext()->CurrentWindow);
      DrawShapes(screen, io);
      DrawLines(screen, io, now);
    }
    ImGui::End();
  }

  static ImU32 Color(const ScreenShape& shape, float alpha = 1.0f) {
    const int a = static_cast<int>(255.0f * std::clamp(shape.alpha * alpha, 0.0f, 1.0f));
    return IM_COL32((shape.rgb >> 16) & 0xFF, (shape.rgb >> 8) & 0xFF, shape.rgb & 0xFF, a);
  }

  static void DrawShapes(const Screen& screen, const ImGuiIO& io) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImFont* face = ImGui::GetFont();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    for (const Shape& entry : screen.shapes) {
      const ScreenShape& s = entry.shape;
      const ImVec2 at(s.x * w, s.y * h);
      switch (s.kind) {
        case ScreenShape::Kind::kText: {
          const float font = std::clamp(s.size * h, 1.0f, kMaxTextSize);
          const ImVec2 size = face->CalcTextSizeA(font, FLT_MAX, 0.0f, s.text.c_str());
          const ImVec2 p(at.x - (s.align < 0 ? 0.0f : s.align > 0 ? size.x : size.x * 0.5f), at.y - size.y * 0.5f);
          if (s.background) {
            const float pad = font * 0.3f;
            draw->AddRectFilled(ImVec2(p.x - pad, p.y - pad * 0.5f), ImVec2(p.x + size.x + pad, p.y + size.y + pad * 0.5f),
                                IM_COL32(0, 0, 0, static_cast<int>(160.0f * std::clamp(s.alpha, 0.0f, 1.0f))), pad);
          }
          draw->AddText(face, font, ImVec2(p.x + 2.0f, p.y + 2.0f), IM_COL32(0, 0, 0, static_cast<int>(200.0f * std::clamp(s.alpha, 0.0f, 1.0f))), s.text.c_str());
          draw->AddText(face, font, p, Color(s), s.text.c_str());
          break;
        }
        case ScreenShape::Kind::kRect: {
          const ImVec2 end(at.x + s.x2 * w, at.y + s.y2 * h);
          if (s.filled) draw->AddRectFilled(at, end, Color(s), s.rounding * h);
          else draw->AddRect(at, end, Color(s), s.rounding * h, 0, s.thickness);
          break;
        }
        case ScreenShape::Kind::kLine:
          draw->AddLine(at, ImVec2(s.x2 * w, s.y2 * h), Color(s), s.thickness);
          break;
        case ScreenShape::Kind::kCircle:
          if (s.filled) draw->AddCircleFilled(at, s.size * h, Color(s));
          else draw->AddCircle(at, s.size * h, Color(s), 0, s.thickness);
          break;
      }
    }
  }

  static void DrawLines(const Screen& screen, const ImGuiIO& io, Clock::time_point now) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImFont* face = ImGui::GetFont();
    const float standard = ImGui::GetFontSize() * kTextScale;
    // The margins are the default size's, whatever each line's.
    const float right = io.DisplaySize.x - standard * kMargin;
    float y = standard * kMargin;
    for (const Line& line : screen.lines) {
      const float font = line.size > 0.0f ? line.size : standard;
      const double left = std::chrono::duration<double>(line.until - now).count();
      const float alpha = static_cast<float>(std::clamp(left / kFadeSeconds, 0.0, 1.0));
      const ImVec2 size = face->CalcTextSizeA(font, FLT_MAX, 0.0f, line.text.c_str());
      const ImVec2 at(right - size.x, y);
      const ImU32 color = IM_COL32((line.rgb >> 16) & 0xFF, (line.rgb >> 8) & 0xFF, line.rgb & 0xFF, static_cast<int>(255.0f * alpha));
      // A shadow, so the text reads over a bright game.
      draw->AddText(face, font, ImVec2(at.x + 2.0f, at.y + 2.0f), IM_COL32(0, 0, 0, static_cast<int>(200.0f * alpha)), line.text.c_str());
      draw->AddText(face, font, at, color, line.text.c_str());
      y += size.y + font * kLineGap;
      if (y > io.DisplaySize.y) break;
    }
  }
};

}  // namespace

void PrintToScreen(std::string text, double seconds, uint32_t rgb, float size, std::string key) {
  Screen& screen = Get();
  std::lock_guard lock(screen.mutex);
  if (!screen.overlay || seconds <= 0.0) return;
  Line line{std::move(text), Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds)), rgb & 0xFFFFFF,
            std::isfinite(size) ? std::clamp(size, 0.0f, kMaxTextSize) : 0.0f, std::move(key)};
  // A keyed line takes the place of the one with its key, so a value printed every frame is one line that changes.
  if (!line.key.empty()) {
    const auto same = std::find_if(screen.lines.begin(), screen.lines.end(), [&line](const Line& shown) { return shown.key == line.key; });
    if (same != screen.lines.end()) {
      *same = std::move(line);
      return;
    }
  }
  screen.lines.push_front(std::move(line));
  if (screen.lines.size() > kMaxLines) screen.lines.pop_back();
}

void DrawOnScreen(ScreenShape shape) {
  Screen& screen = Get();
  std::lock_guard lock(screen.mutex);
  if (!screen.overlay) return;
  const double seconds = std::isfinite(shape.seconds) && shape.seconds > 0.0 ? shape.seconds : shape.key.empty() ? kShapeTick : 0.0;
  const Clock::time_point until =
      seconds > 0.0 ? Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds)) : Clock::time_point::max();
  if (!std::isfinite(shape.thickness) || shape.thickness <= 0.0f) shape.thickness = 2.0f;
  shape.thickness = std::min(shape.thickness, 64.0f);
  if (!shape.key.empty()) {
    const auto same = std::find_if(screen.shapes.begin(), screen.shapes.end(), [&shape](const Shape& shown) { return shown.shape.key == shape.key; });
    if (same != screen.shapes.end()) {
      *same = Shape{std::move(shape), until};
      return;
    }
  }
  screen.shapes.push_back(Shape{std::move(shape), until});
  if (screen.shapes.size() > kMaxShapes) screen.shapes.pop_front();
}

void RemoveFromScreen(const std::string& key) {
  Screen& screen = Get();
  std::lock_guard lock(screen.mutex);
  std::erase_if(screen.shapes, [&key](const Shape& shape) { return shape.shape.key == key; });
}

void ClearScreen(const std::string& prefix) {
  Screen& screen = Get();
  std::lock_guard lock(screen.mutex);
  std::erase_if(screen.shapes, [&prefix](const Shape& shape) { return shape.shape.key.compare(0, prefix.size(), prefix) == 0; });
}

void SetScreenTextUI(rex::ui::IUISystem& ui) {
  Screen& screen = Get();
  {
    std::lock_guard lock(screen.mutex);
    if (screen.overlay) return;
    screen.overlay = true;
  }
  // CreateMenus runs on the UI thread, where dialogs are made. Unlocked: a dialog may draw as it's made.
  ui.AddDialog(std::make_unique<Overlay>());
}

}  // namespace rexfabric::ui
