/**
 * @file        reporter/ui_imgui.h
 * @brief       The crash reporter window's contents, shared by the platform
 *              hosts that put them on screen.
 */

#pragma once

#include <string>

namespace reporter {

/// Set when the player closes the window, or chooses Restart.
extern bool g_quit;
extern bool g_restart;

std::string WindowTitle();
/// Once the ImGui context exists.
void ApplyStyle();
/// One frame of the window's contents, between ImGui::NewFrame and Render.
void DrawWindow();
/// Once the window has closed: an unsent crash report is deleted, and the game
/// started again if the player asked.
void FinishWindow();

}  // namespace reporter
