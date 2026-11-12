/**
 * @file        crashreporter/reporter.h
 * @brief       Shared state and actions for the crash reporter UI
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

namespace reporter {

/// Everything the reporter knows, assembled before any UI is created.
/// All strings are UTF-8.
struct Session {
  std::string envelope_path;  ///< Empty when launched without a crash report.
  std::string app_name = "The game";
  std::string game_exe;
  std::string config_path;
  std::string log_path;
  std::string summary_path;
  std::string dsn;
  std::string database_path;
  bool reporting_enabled = false;  ///< Whether automatic sending is already on.

  std::string window_error;  ///< Why the ImGui window couldn't start, when the fallback is used.
  std::string details;  ///< Crash summary + host stack, ready to display.
  std::string log;      ///< Tail of the in-memory log.
  bool sent = false;

  /// Bug report rather than crash: the game is still running and asked for a
  /// window to collect a description. There is no crash to explain, so no
  /// stack trace and no restart: just the log and what the player types,
  /// which is written to message_out_path for the game to pick up and send.
  bool bug_report = false;
  std::string message_out_path;

  /// True when there is actually something that could be sent.
  bool can_send() const {
    return bug_report ? !message_out_path.empty() : (!envelope_path.empty() && !dsn.empty());
  }
};

extern Session g_session;

// --- Actions ---------------------------------------------------------------

/// Submits the envelope the daemon left on disk, then attaches `message` as
/// Sentry User Feedback against the same event. Returns false if nothing was
/// sent.
bool SendReport(const std::string& message);

/// Removes the crash report from disk. Called when the player declines, so
/// reports are not left lying around.
void DiscardReport();

/// Sets `sentry_enabled = true` in the [rexcrash] table of the game's config.
void EnableReportingInConfig();

/// Relaunches the game.
void RestartGame();

/// Bug report mode: hands the typed description back to the game, which owns
/// the Sentry connection and all the context that goes with a report.
bool WriteMessageForGame(const std::string& message);

// --- UIs --------------------------------------------------------------------

/**
 * The ImGui window, on a renderer with a software fallback, as the driver may
 * be what broke: Direct3D 11 on the GPU or else on WARP (Windows), SDL's
 * renderer on the GPU or else in software (Linux).
 *
 * Returns false, with the reason in g_session.window_error, if it could not
 * start.
 */
bool RunImGuiUi();

#if defined(_WIN32)
/// Plain Win32 dialog, no graphics device of any kind. Last resort for when
/// even the software renderer cannot start.
void RunWin32Ui();
#endif

}  // namespace reporter
