/**
 * @file        reporter/ui_imgui.cpp
 * @brief       The crash reporter window's contents, drawn the same by each
 *              platform's host: Direct3D 11 on Windows, SDL's renderer on
 *              Linux.
 */

#include "ui_imgui.h"

#include <string>
#include <vector>

#include <imgui.h>

#include "reporter.h"

namespace reporter {

bool g_quit = false;
bool g_restart = false;

namespace {

void ApplyStyleOnce() {
  ImGuiStyle& style = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  style.WindowRounding = 0.0f;
  style.FrameRounding = 3.0f;
  style.GrabRounding = 3.0f;
  style.ScrollbarRounding = 3.0f;
  style.WindowPadding = ImVec2(16, 14);
  style.FramePadding = ImVec2(8, 5);
  style.ItemSpacing = ImVec2(8, 8);
}

void DrawReadOnly(const char* id, const std::string& text, float height) {
  // InputTextMultiline rather than TextUnformatted so the player can select and
  // copy any part of it.
  ImGui::InputTextMultiline(id, const_cast<char*>(text.c_str()), text.size() + 1,
                            ImVec2(-FLT_MIN, height), ImGuiInputTextFlags_ReadOnly);
}

void DrawUi(const std::string& headline, std::vector<char>& message, bool& auto_send, std::string& status) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("##crash", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                   | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

  // A bug report is not a failure, so it does not get the alarming red.
  const ImVec4 headline_colour = g_session.bug_report ? ImVec4(0.80f, 0.85f, 0.95f, 1.0f)
                                                      : ImVec4(1.00f, 0.42f, 0.38f, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, headline_colour);
  ImGui::TextUnformatted(headline.c_str());
  ImGui::PopStyleColor();

  // Says what is true right now rather than what was true on arrival: telling
  // someone nothing has been sent immediately after they sent it reads like the
  // send failed.
  if (g_session.sent) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 0.55f, 1.0f));
    ImGui::TextUnformatted(g_session.bug_report ? "Thank you, your report is on its way."
                                                : "Your report has been sent. Thank you.");
    ImGui::PopStyleColor();
  } else if (!g_session.can_send()) {
    ImGui::TextDisabled("No crash report was captured, so there is nothing to send.");
  } else if (g_session.bug_report) {
    ImGui::TextDisabled("Describe the problem below. The log is attached as it stands now.");
  } else {
    ImGui::TextDisabled("Nothing has been sent. Look over what would be reported below.");
  }
  ImGui::Spacing();

  const float footer = ImGui::GetFrameHeightWithSpacing() * 5.0f + 40.0f;
  if (ImGui::BeginTabBar("##views")) {
    // Nothing crashed in bug report mode, so there is no exception to explain.
    if (!g_session.bug_report && ImGui::BeginTabItem("Crash details")) {
      DrawReadOnly("##details", g_session.details, -footer);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Log")) {
      DrawReadOnly("##log", g_session.log, -footer);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  ImGui::Spacing();
  ImGui::BeginDisabled(g_session.sent);
  ImGui::TextUnformatted(g_session.bug_report
                             ? "What went wrong?"
                             : "What were you doing when it crashed? (optional)");
  ImGui::InputTextMultiline("##message", message.data(), message.size(),
                            ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 2.2f));

  if (!g_session.reporting_enabled && !g_session.bug_report) {
    ImGui::Checkbox("Send future crash reports automatically", &auto_send);
  }
  ImGui::EndDisabled();

  ImGui::Spacing();

  const std::string typed(message.data());
  const bool empty_message = typed.empty();
  // A bug report with nothing written in it is just a log dump. The crash path
  // has no such requirement, because there the crash itself is the content.
  const bool sendable =
      g_session.can_send() && !g_session.sent && !(g_session.bug_report && empty_message);

  ImGui::BeginDisabled(!sendable);
  if (ImGui::Button("Send report", ImVec2(150, 34))) {
    if (g_session.bug_report) {
      if (WriteMessageForGame(typed)) {
        g_session.sent = true;
        g_quit = true;  // The game does the sending; nothing left to show here.
      } else {
        status = "Could not hand the report back to the game.";
      }
    } else if (SendReport(typed)) {
      g_session.sent = true;
      if (auto_send) {
        EnableReportingInConfig();
      }
      status.clear();  // The subtitle now carries the outcome.
    } else {
      status = "Could not send the report.";
    }
  }
  ImGui::EndDisabled();

  if (!g_session.bug_report) {
    ImGui::SameLine();
    ImGui::BeginDisabled(g_session.game_exe.empty());
    if (ImGui::Button("Restart", ImVec2(120, 34))) {
      g_restart = true;
      g_quit = true;
    }
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  if (ImGui::Button(g_session.bug_report ? "Cancel" : "Quit", ImVec2(120, 34))) {
    g_quit = true;
  }

  ImGui::SameLine();
  if (ImGui::Button("Copy details", ImVec2(130, 34))) {
    const std::string all =
        g_session.bug_report ? g_session.log : g_session.details + "\n\n" + g_session.log;
    ImGui::SetClipboardText(all.c_str());
    status = "Copied to the clipboard.";
  }

  if (g_session.bug_report && empty_message && !g_session.sent) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Type a description to enable sending.");
  }

  if (!status.empty()) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", status.c_str());
  }

  ImGui::End();
}

}  // namespace

std::string WindowTitle() {
  return g_session.app_name + (g_session.bug_report ? " - bug report" : " - crash report");
}

void ApplyStyle() { ApplyStyleOnce(); }

void DrawWindow() {
  static const std::string headline = g_session.bug_report ? std::string("Send a bug report") : g_session.app_name + " has crashed.";
  static std::vector<char> message(4096, '\0');
  static bool auto_send = false;
  static std::string status;
  DrawUi(headline, message, auto_send, status);
}

void FinishWindow() {
  // Only a crash leaves a report on disk to clean up.
  if (!g_session.sent && !g_session.bug_report) {
    DiscardReport();
  }
  if (g_restart) {
    RestartGame();
  }
}

}  // namespace reporter
