#include "sentrywork/sentrywork.h"

#include "fault_fallback.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/thread.h>
#include <rex/version.h>

#include <spdlog/sinks/base_sink.h>

#include <sentry.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <system_error>

REXCVAR_DEFINE_BOOL(crash_reports, true, "Crash Reports",
                    "Send crash reports to the game's developers (through Sentry)");
REXCVAR_DEFINE_STRING(crash_report_log_level, "warn", "Crash Reports",
                      "The log lines sent with a crash report: error, warn, info or off");

namespace sentrywork {

namespace {

#ifdef _WIN32
constexpr const char *kHelperName = "sentry-crash.exe";
#else
constexpr const char *kHelperName = "sentry-crash";
#endif

bool g_enabled = false;

// Sentry's path options take wide strings on Windows, so folders with
// non-ASCII names (a player's user name) still work.
#ifdef _WIN32
void SetDatabasePath(sentry_options_t *o, const std::filesystem::path &p) {
  sentry_options_set_database_pathw(o, p.c_str());
}
void SetHandlerPath(sentry_options_t *o, const std::filesystem::path &p) {
  sentry_options_set_handler_pathw(o, p.c_str());
}
void AddAttachment(sentry_options_t *o, const std::filesystem::path &p) {
  sentry_options_add_attachmentw(o, p.c_str());
}
#else
void SetDatabasePath(sentry_options_t *o, const std::filesystem::path &p) {
  sentry_options_set_database_path(o, p.c_str());
}
void SetHandlerPath(sentry_options_t *o, const std::filesystem::path &p) {
  sentry_options_set_handler_path(o, p.c_str());
}
void AddAttachment(sentry_options_t *o, const std::filesystem::path &p) {
  sentry_options_add_attachment(o, p.c_str());
}
#endif

#ifndef _WIN32
// Reports go out over libcurl, and an AppImage carries the libcurl of the
// distribution it was built on, which looks for root certificates where that
// distribution keeps them. Point it at this system's (or SSL_CERT_FILE).
std::string FindCaCertificates() {
  if (const char *file = std::getenv("SSL_CERT_FILE"); file && *file)
    return file;
  static constexpr const char *kBundles[] = {
      "/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch, Gentoo
      "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL
      "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // CentOS, RHEL 7
      "/etc/ssl/ca-bundle.pem",                            // openSUSE
      "/etc/ssl/cert.pem",                                 // Alpine, macOS
  };
  std::error_code ec;
  for (const char *bundle : kBundles) {
    if (std::filesystem::exists(bundle, ec))
      return bundle;
  }
  return {};
}
#endif

// The SDK names its log logs/<app>_NNN.log next to the executable, counting
// up each run, unless the log_file cvar names one; the newest is this run's.
std::filesystem::path FindLogFile(const Options &options) {
  if (!options.log_file.empty())
    return options.log_file;
  if (const std::string &cvar = REXCVAR_GET(log_file); !cvar.empty())
    return cvar;
  if (options.app_name.empty())
    return {};
  const std::string prefix = options.app_name + "_";
  std::filesystem::path newest;
  std::filesystem::file_time_type newest_time;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(
           rex::filesystem::GetExecutableFolder() / "logs", ec)) {
    const std::string name = entry.path().filename().string();
    if (!name.starts_with(prefix) || entry.path().extension() != ".log")
      continue;
    const auto time = entry.last_write_time(ec);
    if (!ec && (newest.empty() || time > newest_time)) {
      newest = entry.path();
      newest_time = time;
    }
  }
  return newest;
}

const char *SentryLevel(spdlog::level::level_enum level) {
  switch (level) {
  case spdlog::level::critical:
    return "fatal";
  case spdlog::level::err:
    return "error";
  case spdlog::level::warn:
    return "warning";
  case spdlog::level::info:
    return "info";
  default:
    return "debug";
  }
}

// Log lines become breadcrumbs: the last ones before a crash come with it.
// Each breadcrumb is written to disk for the crash daemon, so only the levels
// crash_report_log_level asks for are passed in.
class BreadcrumbSink : public spdlog::sinks::base_sink<std::mutex> {
protected:
  void sink_it_(const spdlog::details::log_msg &msg) override {
    sentry_value_t crumb = sentry_value_new_breadcrumb("default", nullptr);
    sentry_value_set_by_key(crumb, "message",
                            sentry_value_new_string_n(msg.payload.data(), msg.payload.size()));
    sentry_value_set_by_key(crumb, "category",
                            sentry_value_new_string_n(msg.logger_name.data(), msg.logger_name.size()));
    sentry_value_set_by_key(crumb, "level", sentry_value_new_string(SentryLevel(msg.level)));
    sentry_add_breadcrumb(crumb);
  }
  void flush_() override {}
};

std::shared_ptr<BreadcrumbSink> g_breadcrumbs;

// SENTRYWORK_TEST_CRASH=1 crashes the game once it is loaded, the way a game
// bug does (a write through a null pointer), to check that reports arrive.
[[gnu::noinline]] void TestCrash() {
  REXLOG_WARN("SentryWork: crashing on purpose (SENTRYWORK_TEST_CRASH=1)");
  int *volatile null = nullptr;
  *null = 0x5E27;
}

} // namespace

void Init(const Options &options) {
  if (g_enabled)
    return;
  if (SENTRYWORK_DSN[0] == '\0') {
    REXLOG_INFO("SentryWork: built without a DSN, so crash reports are off");
    return;
  }
  if (!REXCVAR_GET(crash_reports)) {
    REXLOG_INFO("SentryWork: crash reports are off (crash_reports = false)");
    return;
  }

  const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
  sentry_options_t *o = sentry_options_new();
  sentry_options_set_dsn(o, SENTRYWORK_DSN);
  sentry_options_set_release(o, SENTRYWORK_RELEASE);
  sentry_options_set_environment(o, SENTRYWORK_ENVIRONMENT);
  SetDatabasePath(o, options.database_dir.empty() ? exe_dir / "sentry" : options.database_dir);
  SetHandlerPath(o, exe_dir / kHelperName);
  // Sentry walks the stack from the minidump, with the debug files uploaded
  // for the build: the daemon's own walk loses the crashed thread's frames.
  sentry_options_set_crash_reporting_mode(o, SENTRY_CRASH_REPORTING_MODE_MINIDUMP);
#ifndef _WIN32
  if (const std::string certs = FindCaCertificates(); !certs.empty())
    sentry_options_set_ca_certs(o, certs.c_str());
#endif
  const std::filesystem::path log = FindLogFile(options);
  if (!log.empty())
    AddAttachment(o, log);
  if (sentry_init(o) != 0) {
    REXLOG_WARN("SentryWork: Sentry failed to start, so crash reports are off");
    return;
  }
  g_enabled = true;
#ifndef _WIN32
  detail::CaptureSentrySignalHandlers();
#endif

  sentry_set_tag("rexglue", REXGLUE_VERSION_STRING);
  for (const auto &[key, value] : options.tags)
    sentry_set_tag(key.c_str(), value.c_str());

  const auto level = spdlog::level::from_str(REXCVAR_GET(crash_report_log_level));
  if (level != spdlog::level::off) {
    g_breadcrumbs = std::make_shared<BreadcrumbSink>();
    g_breadcrumbs->set_level(level);
    rex::AddSink(g_breadcrumbs);
  }
  REXLOG_INFO("SentryWork: crash reports on ({}, {})", SENTRYWORK_RELEASE, SENTRYWORK_ENVIRONMENT);
}

void OnRuntimeReady(rex::Runtime *runtime) {
  if (!g_enabled)
    return;
  if (runtime && runtime->kernel_state()) {
    char title_id[16];
    std::snprintf(title_id, sizeof(title_id), "%08X", runtime->kernel_state()->title_id());
    sentry_set_tag("title_id", title_id);
  }
#ifdef _WIN32
  // Runtime setup can replace the unhandled exception filter; take it back.
  // Sentry's ends the process once the crash is written up, so filters it
  // replaced (a game's own minidump writer) no longer run.
  sentry_reinstall_backend();
#else
  detail::InstallFaultFallback();
#endif
  if (const char *test = std::getenv("SENTRYWORK_TEST_CRASH"); test && test[0] == '1')
    TestCrash();
}

void Shutdown() {
  if (!g_enabled)
    return;
  if (g_breadcrumbs) {
    rex::RemoveSink(g_breadcrumbs);
    g_breadcrumbs.reset();
  }
  sentry_close();
  g_enabled = false;
}

bool Enabled() {
  return g_enabled;
}

void CaptureMessage(const std::string &message) {
  if (!g_enabled)
    return;
  sentry_value_t event =
      sentry_value_new_message_event(SENTRY_LEVEL_WARNING, "sentrywork", message.c_str());
  sentry_value_t thread = sentry_value_new_thread(rex::thread::current_thread_system_id(), nullptr);
  sentry_value_set_by_key(thread, "stacktrace", sentry_value_new_stacktrace(nullptr, 0));
  sentry_event_add_thread(event, thread);
  sentry_capture_event(event);
}

} // namespace sentrywork
