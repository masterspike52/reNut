/**
 * @file        reporter/main.cpp
 * @brief       Crash reporter entry point and shared actions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Launched by sentry-native's crash daemon after the game has
 *              died, with the path to the crash envelope as its only argument
 *              (see sentry_options_set_external_crash_reporter_path). While a
 *              reporter is configured the daemon does not upload anything
 *              itself, so nothing reaches Sentry unless the player presses
 *              Send here.
 *
 *              Also launched by the game with --bug-report <path> to collect a
 *              description while it is still running; in that mode the window
 *              writes the text to <path> and the game does the sending, since
 *              the game is what knows the mod list and frame timings.
 */

#include "util.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include <sentry.h>

#if defined(_WIN32)
#include <shellapi.h>
#else
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

#include "reporter.h"

namespace reporter {

Session g_session;

namespace {

// The daemon hands us only the report path, so everything else comes from an
// ini the game writes into its crash database directory. The report sits a
// couple of levels below that, so walk up looking for it.
std::string FindSessionIni(const std::string& anchor) {
  std::string directory = DirectoryOf(anchor);
  for (int depth = 0; depth < 6 && !directory.empty(); ++depth) {
    const std::string candidate = directory + "/crash-reporter.ini";
    if (FileExists(candidate)) {
      return candidate;
    }
    const std::string parent = DirectoryOf(directory);
    if (parent == directory) {
      break;
    }
    directory = parent;
  }
  return {};
}

void LoadSessionIni(const std::string& path) {
  std::istringstream file(ReadTextFile(path));
  std::string line;
  while (std::getline(file, line)) {
    const size_t equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    const std::string key = Trim(line.substr(0, equals));
    const std::string value = Trim(line.substr(equals + 1));
    if (key == "app_name") {
      g_session.app_name = value;
    } else if (key == "game_exe") {
      g_session.game_exe = value;
    } else if (key == "config_path") {
      g_session.config_path = value;
    } else if (key == "log_path") {
      g_session.log_path = value;
    } else if (key == "summary_path") {
      g_session.summary_path = value;
    } else if (key == "database_path") {
      g_session.database_path = value;
    } else if (key == "dsn") {
      g_session.dsn = value;
    } else if (key == "reporting_enabled") {
      g_session.reporting_enabled = (value == "1" || value == "true");
    }
  }
}

std::string OwnExecutablePath() {
#if defined(_WIN32)
  std::wstring buffer(MAX_PATH, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  buffer.resize(length);
  return Narrow(buffer);
#else
  std::error_code ec;
  return FromPath(std::filesystem::read_symlink("/proc/self/exe", ec));
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

bool SendReport(const std::string& message) {
  if (!g_session.can_send()) {
    return false;
  }

  sentry_options_t* options = sentry_options_new();
  sentry_options_set_dsn(options, g_session.dsn.c_str());
  if (!g_session.database_path.empty()) {
    // The narrow path setters take UTF-8 on every platform; sentry-native
    // canonicalizes to UTF-8 internally and only widens at the Win32 boundary.
    sentry_options_set_database_path(options, g_session.database_path.c_str());
  }
  // This process is the reporter; it must not arm a crash handler of its own,
  // nor start a session of its own accord.
  sentry_options_set_auto_session_tracking(options, 0);

  if (sentry_init(options) != 0) {
    return false;
  }

  bool ok = false;
  if (sentry_envelope_t* envelope =
          sentry_envelope_read_from_file(g_session.envelope_path.c_str())) {
    // Read the id before capture, which takes ownership of the envelope.
    sentry_uuid_t event_id = sentry_uuid_nil();
    const sentry_value_t header = sentry_envelope_get_header(envelope, "event_id");
    if (const char* text = sentry_value_as_string(header)) {
      event_id = sentry_uuid_from_string(text);
    }

    sentry_capture_envelope(envelope);

    if (!message.empty()) {
      sentry_value_t feedback = sentry_value_new_feedback(
          message.c_str(), nullptr, nullptr, sentry_uuid_is_nil(&event_id) ? nullptr : &event_id);
      sentry_capture_feedback(feedback);
    }
    ok = true;
  }

  sentry_flush(10000);
  sentry_close();
  return ok;
}

// A well-behaved reporter does not leave crash reports lying around, whether or
// not the player chose to send this one.
void DiscardReport() {
  if (!g_session.envelope_path.empty()) {
    std::error_code ec;
    std::filesystem::remove(ToPath(g_session.envelope_path), ec);
  }
}

// The config is TOML with one table per settings scope, where a repeated key fails to load. So the value goes in
// [rexcrash], and any earlier one there or at the root (the old flat name) is dropped.
void EnableReportingInConfig() {
  if (g_session.config_path.empty()) {
    return;
  }
  std::string text;
  if (std::ifstream file(ToPath(g_session.config_path), std::ios::binary); file) {
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }

  std::istringstream in(text);
  std::string out;
  std::string table;
  bool written = false;
  for (std::string line; std::getline(in, line);) {
    const std::string trimmed = Trim(line);
    if (trimmed.size() > 1 && trimmed[0] == '[' && trimmed[1] != '[') {
      const size_t close = trimmed.find(']');
      table = Trim(trimmed.substr(1, close == std::string::npos ? std::string::npos : close - 1));
      out += line + '\n';
      if (table == "rexcrash" && !written) {
        out += "sentry_enabled = true\n";
        written = true;
      }
      continue;
    }
    if (const size_t equals = trimmed.find('='); equals != std::string::npos && trimmed[0] != '#') {
      const std::string key = Trim(trimmed.substr(0, equals));
      const bool in_rexcrash = table == "rexcrash" && key == "sentry_enabled";
      const bool at_root = table.empty() && (key == "sentry_enabled" || key == "rexcrash.sentry_enabled");
      if (in_rexcrash || at_root) {
        continue;
      }
    }
    out += line + '\n';
  }
  if (!written) {
    out += "\n[rexcrash]\nsentry_enabled = true\n";
  }
  WriteTextFile(g_session.config_path, out);
}

void RestartGame() {
  if (g_session.game_exe.empty()) {
    return;
  }
#if defined(_WIN32)
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  std::wstring command = L"\"" + Widen(g_session.game_exe) + L"\"";
  const std::wstring directory = Widen(DirectoryOf(g_session.game_exe));
  if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                     directory.c_str(), &startup, &process)) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
#else
  // posix_spawn rather than fork/exec: the child must outlive this process, and
  // there is nothing to do between the two calls that would need a real fork.
  pid_t pid = 0;
  char* argv[] = {const_cast<char*>(g_session.game_exe.c_str()), nullptr};
  const std::string directory = DirectoryOf(g_session.game_exe);
  if (!directory.empty()) {
    std::error_code ec;
    std::filesystem::current_path(ToPath(directory), ec);
  }
  posix_spawn(&pid, g_session.game_exe.c_str(), nullptr, nullptr, argv, environ);
#endif
}

bool WriteMessageForGame(const std::string& message) {
  return WriteTextFile(g_session.message_out_path, message);
}

}  // namespace reporter

namespace {

// The arguments, as UTF-8: crash data lives under the player's own folder, whose name may not fit the ANSI code page.
int Run(const std::vector<std::string>& args) {
  using namespace reporter;

  // Crash: the daemon passes the report path as the only argument.
  // Bug report: the game passes --bug-report <file to write the message to>.
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--bug-report" && i + 1 < args.size()) {
      g_session.bug_report = true;
      g_session.message_out_path = args[++i];
    } else if (g_session.envelope_path.empty()) {
      g_session.envelope_path = args[i];
    }
  }

  // In bug report mode there is no envelope to walk up from, so start the
  // search at this executable instead, which sits beside the game.
  const std::string anchor =
      g_session.envelope_path.empty() ? OwnExecutablePath() : g_session.envelope_path;
  const std::string ini = FindSessionIni(anchor);
  if (!ini.empty()) {
    LoadSessionIni(ini);
    if (g_session.database_path.empty()) {
      g_session.database_path = DirectoryOf(ini);
    }
  }

  if (!g_session.bug_report) {
    g_session.details = ReadTextFile(g_session.summary_path);
    if (g_session.details.empty()) {
      g_session.details = "No crash details were captured.";
    }
  }
  g_session.log = ReadTextFile(g_session.log_path);
  if (g_session.log.empty()) {
    g_session.log = "No log was captured.";
  }

  if (!RunImGuiUi()) {
    // Beside the crash data, so whoever looks into it knows why there was no window, or only the plain one.
    std::ofstream(ToPath(g_session.database_path) / "crash-reporter.log", std::ios::app) << "ImGui window unavailable: " << g_session.window_error << "\n";
#if defined(_WIN32)
    RunWin32Ui();
#else
    // No window at all, so no one to ask: the report is deleted unsent.
    DiscardReport();
#endif
  }
  return 0;
}

}  // namespace

#if defined(_WIN32)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int count = 0;
  wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
  std::vector<std::string> args;
  for (int i = 1; i < count; ++i) args.push_back(reporter::Narrow(wide[i]));
  LocalFree(wide);
  return Run(args);
}
#else
int main(int argc, char* argv[]) { return Run(std::vector<std::string>(argv + 1, argv + argc)); }
#endif
