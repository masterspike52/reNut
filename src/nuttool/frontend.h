#pragma once

namespace renut::nuttool {

// Once at startup: the main menu's NUTTOOL icon (a row of reNut's own), which opens the studio, and NutTool's window.
void Declare();

// Whether the studio was opened from the main menu and NutTool hasn't been left since.
bool Opened();
void SetOpened(bool open);

} // namespace renut::nuttool
