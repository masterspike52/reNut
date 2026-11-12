// Crash and bug reports through Sentry, with a window that asks the player before anything is sent.
//
// A crash is caught out of process by Sentry's crash daemon, which survives a wrecked heap or stack. It hands the
// report to the crash reporter window (rexcrashreporter, staged beside the game), which shows the player the log
// and what went wrong, and sends only when they press Send. Bug reports open the same window from a running
// game. Reports carry the machine, the mods, the frame rate and the log kept in memory.
//
// The app wires it in at three points:
//
//   rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override { REX_TRY_VOID(rexcrash::Configure(settings)); ... }
//   rex::Result<void> OnInit() override { ...; rexcrash::Start({.app_name = "retip", .database_path = ..., .mods = ...}); }
//   void OnShutdown() override { rexcrash::Stop(); }
//
// and calls rexcrash::OnPresent() once a frame for the frame rate. Start once the mods are loaded: everything
// logged up to then becomes the part of the log every report keeps.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/result.h>

namespace rex {
class SettingsRegistry;
}  // namespace rex

namespace rexcrash {

struct Mod {
  std::string name;
  std::string version;
  bool enabled = false;
};

struct Config {
  // The Sentry project's ingest URL. Empty takes the one rexcrash_attach baked in (its DSN argument, or the
  // REXCRASH_SENTRY_DSN cache or environment variable); with neither, reporting stays off.
  std::string dsn;
  // What reports are grouped by, such as "mygame@1.4.2". Empty: "<app_name>@<rexglue version>".
  std::string release;
  // "production", "beta" and so on. Empty: the build configuration.
  std::string environment;
  std::string app_name;
  // Where crash data waits between runs. Empty: .sentry-native beside the executable.
  std::filesystem::path database_path;
  // The game's config file, so the reporter window can turn on automatic sending when the player opts in.
  std::filesystem::path config_path;
  // The installed mods, asked for whenever a report is made, as they can change while the game runs.
  std::function<std::vector<Mod>()> mods;
  // Sentry's crash daemon and the reporter window. Empty: beside the executable, where rexcrash_attach stages them.
  std::filesystem::path handler_path;
  std::filesystem::path reporter_path;
};

// For App::OnConfigure: registers the settings (scope "rexcrash", old names as aliases) and adds the in-memory log.
// The log gets the logger's kept startup lines, so reports carry it from the very first line.
[[nodiscard]] rex::Result<void> Configure(rex::SettingsRegistry& settings);

// Starts reporting. It starts when there is a DSN and either sentry_enabled (send without asking)
// or sentry_crash_dialog (ask through the reporter window) is on; false otherwise.
bool Start(const Config& config);
// Sends what's pending and shuts down. Safe when not started.
void Stop();
bool IsStarted();

// Once a frame, from the present, for the frame rate in reports.
void OnPresent();

// A searchable key and value on every later report.
void SetTag(std::string_view key, std::string_view value);
// A step in the trail sent with the next report. Cheap enough for state changes, not for every frame.
void AddBreadcrumb(std::string_view category, std::string_view message);

// Opens the reporter window for the player to describe a problem, then sends it from here. Also the console
// command rexcrash.bugreport.
void OpenBugReport();
// Sends a bug report with this description straight away: rexcrash.bugreport <description>.
void CaptureBugReport(std::string_view description);
// A report of an error the game survived.
void CaptureError(std::string_view message);
// A report of an error the game won't survive; waits for it to be sent.
void CaptureFatal(std::string_view message);

// The user's home directory replaced with a placeholder, as reports do with sentry_scrub_paths on.
std::string ScrubPaths(std::string_view text);

}  // namespace rexcrash
