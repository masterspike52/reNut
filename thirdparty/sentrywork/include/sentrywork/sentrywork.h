#pragma once

// SentryWork - Sentry crash reports for ReXGlue recomps.
//
// A crash (or a fatal assert, abort or std::terminate) is sent to the Sentry
// project whose DSN the game passed to sentrywork_attach() in CMake, with the
// game's log file, the last warnings and errors as breadcrumbs, and the title
// it was running. Players can turn reports off with crash_reports = false in
// the game's .toml.
//
//   class MyApp : public rex::ReXApp {
//     void OnPostInitLogging() override { sentrywork::Init({.app_name = "my_app"}); }
//     void OnPostSetup() override { sentrywork::OnRuntimeReady(runtime()); }
//     void OnShutdown() override { sentrywork::Shutdown(); }
//   };

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace rex {
class Runtime;
}

namespace sentrywork {

struct Options {
  // The name the app gives rex::ReXApp: its logs are logs/<app_name>_NNN.log.
  std::string app_name;
  // Where reports wait until they are sent (a crash's is sent by the
  // sentry-crash helper, or on the next run). Default: "sentry" next to the
  // executable; give a writable folder when that one is not (an AppImage).
  std::filesystem::path database_dir;
  // The log attached to reports. Default: the log_file cvar, or the newest
  // logs/<app_name>_NNN.log next to the executable.
  std::filesystem::path log_file;
  // Shown on every report, e.g. {"game", "Banjo-Kazooie: Nuts & Bolts"}.
  std::vector<std::pair<std::string, std::string>> tags;
};

// Starts crash reporting. Call from ReXApp::OnPostInitLogging: the game's
// .toml and the log are loaded then, and the runtime, whose exception handler
// has to come after Sentry's, is not yet built.
void Init(const Options& options);

// Adds the title to reports and makes sure crashes the runtime's exception
// handler passes over still reach Sentry. Call from ReXApp::OnPostSetup.
void OnRuntimeReady(rex::Runtime* runtime);

// Sends what is still queued and stops. Call from ReXApp::OnShutdown.
void Shutdown();

// Whether reports are being sent: a DSN was built in and the player allows it.
bool Enabled();

// Reports a problem the game survived, as a Sentry issue with a stack trace.
void CaptureMessage(const std::string& message);

} // namespace sentrywork
