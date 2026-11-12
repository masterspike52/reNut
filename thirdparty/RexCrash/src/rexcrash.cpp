/**
 * @file        rexcrash.cpp
 * @brief       Crash and bug reporting via Sentry
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rexcrash/rexcrash.h>

#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <thread>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/console_command.h>
#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>
#include <rex/platform/file.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/version.h>

#include "frame_stats.h"
#include "log_buffer.h"
#include "system_report.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <sentry.h>

// rexglue_setup_target defines it; a build without it says so.
#ifndef REXGLUE_BUILD_CONFIG
#define REXGLUE_BUILD_CONFIG "unknown"
#endif

namespace rexcrash {

namespace {

rex::Result<void> RunBugReport(std::string_view args) {
  // No description: open the reporter window so it can be typed there.
  // With one: send straight away, which keeps the command scriptable.
  if (args.empty()) {
    OpenBugReport();
  } else {
    CaptureBugReport(args);
  }
  return rex::Ok();
}

struct Settings final : rex::SettingOwner {
  Settings() : rex::SettingOwner("rexcrash") {}
  REX_SETTING(bool, sentry_enabled, false, "Send reports automatically, without asking. Off by default. Crashes are unaffected: those always go through the crash reporter window, which sends nothing unless you press Send.");
  REX_SETTING(std::string, sentry_dsn, "", "Sentry ingest URL. Overrides the DSN the game was built with; normally left empty.");
  REX_SETTING(std::string, sentry_environment, "", "Environment reports are filed under (production, beta, ...). Empty uses the build configuration.");
  REX_SETTING(std::string, sentry_user, "", "Optional handle attached to reports so you can be contacted about them. Empty reports anonymously.");
  REX_SETTING(bool, sentry_attach_log, true, "Attach the in-memory log (see crash_log_kb) to reports.");
  REX_SETTING(bool, sentry_scrub_paths, true, "Replace your home directory with a placeholder in report text, so the account name in file paths is not uploaded.");
  REX_SETTING(bool, sentry_debug, false, "Log what the Sentry SDK is doing. For diagnosing reporting itself.");
  REX_SETTING(int32_t, crash_log_kb, 1024, "How much of the most recent log reports carry, in KB, besides the startup lines", rex::Range{64, 65536});
  REX_SETTING(bool, sentry_crash_dialog, true, "On a crash, show the crash reporter window with the log, the crash details and a choice of sending the report, restarting or quitting. Windows only.");
  REX_COMMAND(bugreport, "Open the bug report window, or send one straight away when given a description",
              RunBugReport);
};

Settings& GetSettings() {
  static Settings* settings = new Settings();
  return *settings;
}

// The flat cvar names of the old SDK, which old config files and command lines still use.
constexpr std::string_view kOldNames[] = {
    "sentry_enabled", "sentry_dsn",   "sentry_environment", "sentry_user",      "sentry_attach_log",
    "sentry_scrub_paths", "sentry_debug", "crash_log_kb",   "sentry_crash_dialog", "fps_log_interval",
    "fps_hitch_ms",
};

std::mutex g_mutex;
bool g_started = false;
std::string g_app_name;
std::filesystem::path g_crash_log_path;
std::filesystem::path g_crash_summary_path;
std::filesystem::path g_reporter_path;
// Never freed: the logger may still write to its sinks while the process exits.
LogBuffer* g_log_buffer = nullptr;
rex::ScopeGuard* g_log_buffer_guard = nullptr;
rex::ScopeGuard* g_log_budget_watch = nullptr;
rex::ScopeGuard* g_breadcrumb_guard = nullptr;
std::function<std::vector<Mod>()> g_mods;
thread_local bool t_in_breadcrumb = false;

std::optional<std::string> GetEnv(const char* name) {
#if defined(_WIN32)
  const std::wstring wide_name(name, name + std::strlen(name));
  const DWORD needed = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
  if (needed == 0)
    return std::nullopt;
  std::wstring value(needed, L'\0');
  const DWORD length = GetEnvironmentVariableW(wide_name.c_str(), value.data(), needed);
  value.resize(length);
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0,
                                       nullptr, nullptr);
  std::string out(size > 0 ? static_cast<size_t>(size) : 0, '\0');
  if (size > 0)
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr,
                        nullptr);
  return out;
#else
  const char* value = std::getenv(name);
  return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

std::string HomeDirectory() {
#if defined(_WIN32)
  if (auto value = GetEnv("USERPROFILE"); value && !value->empty())
    return *value;
#endif
  if (auto value = GetEnv("HOME"); value && !value->empty())
    return *value;
  return {};
}

// Feeds warnings and errors to Sentry as breadcrumbs, so a report arrives with
// the trail that led to it even when the log attachment is turned off.
class BreadcrumbSink final : public rex::log::ILogSink {
 public:
  // Set around AddSink, which replays the kept lines: only lines from then on become breadcrumbs.
  bool replaying = false;

  void Write(const rex::log::Message& message) override {
    if (replaying || message.level < rex::log::Level::kWarn)
      return;
    std::string category(message.category);
    std::string text(message.text);
    if (GetSettings().sentry_scrub_paths.Get())
      text = ScrubPaths(text);

    t_in_breadcrumb = true;
    sentry_value_t crumb =
        sentry_value_new_breadcrumb("default", text.c_str());
    sentry_value_set_by_key(crumb, "category", sentry_value_new_string(category.c_str()));
    sentry_value_set_by_key(crumb, "level",
                            sentry_value_new_string(message.level >= rex::log::Level::kError ? "error"
                                                                                            : "warning"));
    sentry_add_breadcrumb(crumb);
    t_in_breadcrumb = false;
  }

  void Flush() override {}
};

BreadcrumbSink& GetBreadcrumbSink() {
  static BreadcrumbSink* sink = new BreadcrumbSink();
  return *sink;
}

void SentryLogger(sentry_level_t level, const char* message, va_list args, void*) {
  // Sentry logging from inside the breadcrumb sink would re-enter the logger, which holds its sink lock.
  if (t_in_breadcrumb)
    return;
  char buffer[1024];
  std::vsnprintf(buffer, sizeof(buffer), message, args);
  const char* text = buffer;
  if (level >= SENTRY_LEVEL_ERROR) {
    REXLOG_ERROR("[sentry] {}", text);
  } else if (level == SENTRY_LEVEL_WARNING) {
    REXLOG_WARN("[sentry] {}", text);
  } else {
    REXLOG_DEBUG("[sentry] {}", text);
  }
}

#if defined(_WIN32)
// Resolves an address to "module+offset" without dbghelp. Symbol lookup wants
// SymInitialize, which is far too much machinery to start up inside a crashed
// process; Sentry symbolizes the minidump server-side anyway. This trace exists
// so the player has something concrete to look at in the reporter window.
void DescribeAddress(std::FILE* file, void* address) {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                             | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(address), &module)
      && module) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    const uintptr_t offset =
        reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module);
    std::fprintf(file, "  %-28ls +0x%llx  (0x%p)\n", name,
                 static_cast<unsigned long long>(offset), address);
  } else {
    std::fprintf(file, "  %-28s        (0x%p)\n", "<unknown module>", address);
  }
}

const char* ExceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
      return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:
      return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION:
      return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
      return "EXCEPTION_IN_PAGE_ERROR";
    default:
      return "EXCEPTION";
  }
}

// A short, human-readable account of the crash for the reporter window to show.
// The authoritative version is the minidump; this is the part a player can read.
void WriteCrashSummary(const sentry_ucontext_t* uctx) {
  if (g_crash_summary_path.empty()) {
    return;
  }
  std::FILE* file = nullptr;
  _wfopen_s(&file, g_crash_summary_path.c_str(), L"wb");
  if (!file) {
    return;
  }

  const EXCEPTION_RECORD* record =
      uctx ? uctx->exception_ptrs.ExceptionRecord : nullptr;
  if (record) {
    std::fprintf(file, "%s (0x%08lx)\n", ExceptionName(record->ExceptionCode),
                 static_cast<unsigned long>(record->ExceptionCode));
    std::fprintf(file, "Faulting address: 0x%p\n",
                 reinterpret_cast<void*>(record->ExceptionAddress));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
        && record->NumberParameters >= 2) {
      const char* operation = record->ExceptionInformation[0] == 0   ? "reading"
                              : record->ExceptionInformation[0] == 1 ? "writing"
                                                                     : "accessing";
      std::fprintf(file, "Faulted while %s 0x%p\n", operation,
                   reinterpret_cast<void*>(record->ExceptionInformation[1]));
    }
  } else {
    std::fprintf(file, "Crash details were not available.\n");
  }
  std::fprintf(file, "Thread: %lu\n\n", GetCurrentThreadId());

  std::fprintf(file, "Stack (host):\n");
  void* frames[62] = {};
  const USHORT count = CaptureStackBackTrace(0, 62, frames, nullptr);
  for (USHORT i = 0; i < count; ++i) {
    DescribeAddress(file, frames[i]);
  }
  if (count == 0) {
    std::fprintf(file, "  <no frames captured>\n");
  }

  std::fclose(file);
}
#endif  // _WIN32

// Runs in the crashing process, before the out-of-process daemon assembles the
// envelope, so the file is on disk by the time the daemon reads the attachment
// list. The process is already in trouble here: no allocation, no logging, and
// TryWriteTo gives up rather than waiting if the dying thread happened to hold
// the buffer lock.
sentry_value_t OnCrash(const sentry_ucontext_t* uctx, sentry_value_t event, void*) {
#if defined(_WIN32)
  // Written before the daemon is signalled, so the reporter window it launches
  // finds both files already in place.
  WriteCrashSummary(uctx);
#else
  (void)uctx;
#endif

  if (!GetSettings().sentry_attach_log.Get() || g_crash_log_path.empty())
    return event;

  auto buffer = g_log_buffer;
  if (!buffer)
    return event;

  std::FILE* file = nullptr;
#if defined(_WIN32)
  _wfopen_s(&file, g_crash_log_path.c_str(), L"wb");
#else
  file = std::fopen(g_crash_log_path.c_str(), "wb");
#endif
  if (file) {
    buffer->TryWriteTo(file);
    std::fclose(file);
  }
  return event;
}

void SetExternalReporter(sentry_options_t* options, const std::filesystem::path& path) {
#if defined(_WIN32)
  sentry_options_set_external_crash_reporter_pathw(options, path.c_str());
#else
  sentry_options_set_external_crash_reporter_path(options, path.c_str());
#endif
}

// The daemon passes the reporter only the crash report path, so everything else
// it needs goes in a file beside the crash data, which it walks up to find.
void WriteReporterInfo(const std::filesystem::path& database_path, const Config& config, const std::string& dsn, bool automatic) {
  std::ofstream info(database_path / "crash-reporter.ini", std::ios::binary | std::ios::trunc);
  if (!info) {
    REXLOG_WARN("Could not write the crash reporter info file in {}", database_path.string());
    return;
  }
  info << "app_name=" << g_app_name << "\n"
       << "game_exe=" << rex::platform::GetExecutablePath().string() << "\n"
       << "database_path=" << database_path.string() << "\n"
       << "log_path=" << g_crash_log_path.string() << "\n"
       << "summary_path=" << g_crash_summary_path.string() << "\n"
       << "config_path=" << config.config_path.string() << "\n"
       << "reporting_enabled=" << (automatic ? "1" : "0") << "\n"
       << "dsn=" << dsn << "\n";
}

sentry_value_t StringList(const std::vector<std::string>& values) {
  sentry_value_t list = sentry_value_new_list();
  for (const auto& value : values)
    sentry_value_append(list, sentry_value_new_string(value.c_str()));
  return list;
}

// Everything the SDK knows about the machine, mirrored into report contexts so
// it is visible in the Sentry UI without opening the log attachment.
void PublishStaticContexts() {
  const auto& system = GetSystemReport();

  sentry_value_t host = sentry_value_new_object();
  sentry_value_set_by_key(host, "os", sentry_value_new_string(system.os.c_str()));
  sentry_value_set_by_key(host, "kernel", sentry_value_new_string(system.kernel.c_str()));
  sentry_value_set_by_key(host, "cpu", sentry_value_new_string(system.cpu.c_str()));
  sentry_value_set_by_key(host, "cpu_cores",
                          sentry_value_new_int32(static_cast<int32_t>(system.cpu_cores)));
  sentry_value_set_by_key(host, "cpu_threads",
                          sentry_value_new_int32(static_cast<int32_t>(system.cpu_threads)));
  sentry_value_set_by_key(host, "memory",
                          sentry_value_new_string(FormatBytes(system.ram_total).c_str()));
  sentry_value_set_by_key(host, "locale", sentry_value_new_string(system.locale.c_str()));
  if (!system.compatibility.empty()) {
    sentry_value_set_by_key(host, "compatibility_layer",
                            sentry_value_new_string(system.compatibility.c_str()));
  }
  sentry_set_context("host", host);

  std::vector<std::string> adapters;
  adapters.reserve(system.gpus.size());
  for (const auto& gpu : system.gpus) {
    std::string line = gpu.name;
    if (gpu.dedicated_vram)
      line += " (" + FormatBytes(gpu.dedicated_vram) + ")";
    if (!gpu.driver_version.empty())
      line += " driver " + gpu.driver_version;
    adapters.push_back(std::move(line));
  }
  sentry_value_t gpu_context = sentry_value_new_object();
  sentry_value_set_by_key(gpu_context, "adapters", StringList(adapters));
  sentry_set_context("gpu", gpu_context);

  if (!system.gpus.empty()) {
    sentry_set_tag("gpu", system.gpus.front().name.c_str());
    if (!system.gpus.front().driver_version.empty())
      sentry_set_tag("gpu.driver", system.gpus.front().driver_version.c_str());
  }
  if (!system.cpu.empty())
    sentry_set_tag("cpu", system.cpu.c_str());
  if (!system.compatibility.empty())
    sentry_set_tag("compatibility_layer", system.compatibility.c_str());

  sentry_value_t sdk = sentry_value_new_object();
  sentry_value_set_by_key(sdk, "version", sentry_value_new_string(REXGLUE_VERSION_STRING));
  sentry_value_set_by_key(sdk, "build", sentry_value_new_string(rex::GetBuildStamp()));
  sentry_value_set_by_key(sdk, "platform", sentry_value_new_string(REXGLUE_BUILD_PLATFORM));
  sentry_set_context("rexglue", sdk);
  sentry_set_tag("rexglue", REXGLUE_VERSION_STRING);
}

// Refreshed rather than pinned at startup: mods can be toggled mid-session.
void PublishModContext() {
  const std::vector<Mod> mods = g_mods ? g_mods() : std::vector<Mod>{};

  std::vector<std::string> enabled;
  std::vector<std::string> installed;
  for (const auto& mod : mods) {
    std::string label = mod.version.empty() ? mod.name : mod.name + " v" + mod.version;
    if (mod.enabled)
      enabled.push_back(label);
    installed.push_back(std::move(label));
  }

  sentry_value_t context = sentry_value_new_object();
  sentry_value_set_by_key(context, "installed_count",
                          sentry_value_new_int32(static_cast<int32_t>(mods.size())));
  sentry_value_set_by_key(context, "enabled_count",
                          sentry_value_new_int32(static_cast<int32_t>(enabled.size())));
  sentry_value_set_by_key(context, "enabled", StringList(enabled));
  sentry_value_set_by_key(context, "installed", StringList(installed));
  sentry_set_context("mods", context);

  sentry_set_tag("mods.enabled", std::to_string(enabled.size()).c_str());
  sentry_set_tag("modded", enabled.empty() ? "no" : "yes");
}

void PublishFrameStats(const FrameStats::Sample& sample) {
  sentry_value_t context = sentry_value_new_object();
  sentry_value_set_by_key(context, "average_fps", sentry_value_new_double(sample.average_fps));
  sentry_value_set_by_key(context, "average_frame_ms", sentry_value_new_double(sample.average_ms));
  sentry_value_set_by_key(context, "worst_frame_ms", sentry_value_new_double(sample.worst_ms));
  sentry_value_set_by_key(context, "hitches",
                          sentry_value_new_int32(static_cast<int32_t>(sample.hitches)));
  sentry_value_set_by_key(context, "frames",
                          sentry_value_new_int32(static_cast<int32_t>(sample.total_frames)));
  sentry_value_set_by_key(context, "uptime_seconds",
                          sentry_value_new_double(sample.uptime_seconds));
  sentry_set_context("performance", context);
  sentry_set_tag("fps", std::format("{:.0f}", sample.average_fps).c_str());
}

// Attaches the in-memory log for the duration of one capture. Attaching per
// report rather than once at startup keeps the text current; the attachment is
// removed again so it is not re-uploaded with every later event.
class ScopedLogAttachment {
 public:
  ScopedLogAttachment() {
    if (!GetSettings().sentry_attach_log.Get())
      return;
    auto buffer = g_log_buffer;
    if (!buffer)
      return;

    text_ = buffer->Snapshot();
    if (GetSettings().sentry_scrub_paths.Get())
      text_ = ScrubPaths(text_);
    if (text_.empty())
      return;
    attachment_ = sentry_attach_bytes(text_.data(), text_.size(), "rexglue.log");
  }

  ~ScopedLogAttachment() {
    if (attachment_)
      sentry_remove_attachment(attachment_);
  }

  ScopedLogAttachment(const ScopedLogAttachment&) = delete;
  ScopedLogAttachment& operator=(const ScopedLogAttachment&) = delete;

 private:
  std::string text_;
  sentry_attachment_t* attachment_ = nullptr;
};

void Capture(sentry_level_t level, const char* logger, std::string_view message) {
  PublishModContext();
  ScopedLogAttachment attachment;

  std::string text(message);
  if (GetSettings().sentry_scrub_paths.Get())
    text = ScrubPaths(text);

  sentry_capture_event(sentry_value_new_message_event(level, logger, text.c_str()));
}

}  // namespace

std::string ScrubPaths(std::string_view text) {
  const std::string home = HomeDirectory();
  if (home.empty())
    return std::string(text);

  // Windows paths turn up with either separator depending on who built them.
  std::string alternate = home;
  for (char& c : alternate) {
    if (c == '\\')
      c = '/';
  }

  std::string out(text);
  for (const auto& needle : {home, alternate}) {
    if (needle.empty())
      continue;
    size_t pos = 0;
    while ((pos = out.find(needle, pos)) != std::string::npos) {
      out.replace(pos, needle.size(), "<home>");
      pos += 6;
    }
  }
  return out;
}

rex::Result<void> Configure(rex::SettingsRegistry& settings) {
  if (g_log_buffer)
    return rex::Ok();
  for (const std::string_view name : kOldNames)
    settings.AddAlias(name, std::format("rexcrash.{}", name));
  REX_TRY_VOID(settings.Register(GetSettings()));
  REX_TRY_VOID(settings.Register(FrameStatsSettings()));

  // Sized from the default here; the config file is read later, and the watch applies its crash_log_kb.
  g_log_buffer = new LogBuffer(static_cast<size_t>(GetSettings().crash_log_kb.Get()) * 1024);
  g_log_budget_watch = new rex::ScopeGuard(GetSettings().crash_log_kb.OnChange(
      [](const int32_t& kb) { g_log_buffer->SetRingBudget(static_cast<size_t>(kb) * 1024); }));
  g_log_buffer_guard = new rex::ScopeGuard(REX_LOGGER().AddSink(*g_log_buffer));
  return rex::Ok();
}

void OnPresent() { FrameStats::Global().OnPresent(); }

bool Start(const Config& config) {
  std::lock_guard lock(g_mutex);
  if (g_started)
    return true;

  // Two separate things: sentry_enabled governs sending anything on its own
  // initiative, while sentry_crash_dialog governs the crash reporter window.
  // Starting up for the reporter alone is not a consent problem: with an
  // external reporter configured the daemon uploads nothing by itself, so a
  // crash report only leaves the machine when the player presses Send.
  const bool automatic = GetSettings().sentry_enabled.Get();
  const bool crash_reporter = GetSettings().sentry_crash_dialog.Get();
  if (!automatic && !crash_reporter) {
    REXLOG_INFO("Crash reporting is off (set sentry_enabled or sentry_crash_dialog)");
    return false;
  }

  // Asking needs the window. Without it the daemon would send every crash on its own, so with sending without
  // asking off, nothing starts.
  auto reporter_path = config.reporter_path;
  if (reporter_path.empty()) {
#if defined(_WIN32)
    reporter_path = rex::platform::GetExecutableFolder() / "rexcrashreporter.exe";
#else
    reporter_path = rex::platform::GetExecutableFolder() / "rexcrashreporter";
#endif
  }
  const bool ask = crash_reporter && std::filesystem::exists(reporter_path);
  if (crash_reporter && !ask) {
    if (!automatic) {
      REXLOG_INFO("Crash reporting is off: there is no crash reporter window at {} to ask the player, and sentry_enabled is off", reporter_path.string());
      return false;
    }
    REXLOG_WARN("Crash reporter window not found at {}; crashes are sent without asking, as sentry_enabled is on", reporter_path.string());
  }

  std::string dsn = GetSettings().sentry_dsn.Get();
  if (dsn.empty())
    dsn = config.dsn;
#ifdef REXCRASH_SENTRY_DSN
  if (dsn.empty())
    dsn = REXCRASH_SENTRY_DSN;
#endif
  if (dsn.empty()) {
    REXLOG_WARN("Crash reporting is on but no Sentry DSN was configured; reports will not be sent");
    return false;
  }

  g_app_name = config.app_name.empty() ? "rexglue" : config.app_name;
  g_mods = config.mods;

  // The machine and the mods head every report's log, however long the game runs.
  LogSystemReport(GetSystemReport());
  if (g_mods) {
    for (const Mod& mod : g_mods()) REXLOG_INFO("Mod: {}{}{}", mod.name, mod.version.empty() ? "" : " v" + mod.version, mod.enabled ? "" : " (off)");
  }
  if (g_log_buffer) g_log_buffer->PinPrefix();

  auto database_path = config.database_path;
  if (database_path.empty())
    database_path = rex::platform::GetExecutableFolder() / ".sentry-native";

  auto handler_path = config.handler_path;
  if (handler_path.empty()) {
#if defined(_WIN32)
    handler_path = rex::platform::GetExecutableFolder() / "sentry-crash.exe";
#else
    handler_path = rex::platform::GetExecutableFolder() / "sentry-crash";
#endif
  }
  if (!std::filesystem::exists(handler_path)) {
    REXLOG_WARN("The crash daemon is missing from {}; hard crashes will not be reported. rexcrash_attach stages it beside the executable.", handler_path.string());
  }

  std::string release = config.release;
#ifdef REXCRASH_SENTRY_RELEASE
  if (release.empty())
    release = REXCRASH_SENTRY_RELEASE;
#endif
  if (release.empty())
    release = std::format("{}@{}", g_app_name, REXGLUE_VERSION_STRING);

  std::string environment = GetSettings().sentry_environment.Get();
  if (environment.empty())
    environment = config.environment;
  if (environment.empty())
    environment = REXGLUE_BUILD_CONFIG;

  // The handler writes the log here from OnCrash. Clear last run's copy: it has
  // already been folded into that run's report by the crash handler.
  std::error_code ec;
  std::filesystem::create_directories(database_path, ec);
  g_crash_log_path = database_path / "rexglue-crash.log";
  g_crash_summary_path = database_path / "rexglue-crash-summary.txt";
  std::filesystem::remove(g_crash_log_path, ec);
  std::filesystem::remove(g_crash_summary_path, ec);

  sentry_options_t* options = sentry_options_new();
  sentry_options_set_dsn(options, dsn.c_str());
  sentry_options_set_release(options, release.c_str());
  sentry_options_set_environment(options, environment.c_str());
  sentry_options_set_debug(options, GetSettings().sentry_debug.Get() ? 1 : 0);
  sentry_options_set_logger(options, SentryLogger, nullptr);
  sentry_options_set_on_crash(options, OnCrash, nullptr);
  sentry_options_set_max_breadcrumbs(options, 100);
  // Sessions would otherwise go out on their own the moment the SDK starts,
  // which is exactly what "not automatic" has to rule out.
  sentry_options_set_auto_session_tracking(options, automatic ? 1 : 0);

  if (ask) {
    // Hands crash envelopes to our own window instead of uploading them. The
    // daemon writes the report to disk, launches this, and sends nothing.
    SetExternalReporter(options, reporter_path);
    // The DSN in use, wherever it came from: the cvar, the config or the one baked in.
    WriteReporterInfo(database_path, config, dsn, automatic);
    g_reporter_path = reporter_path;
    REXLOG_INFO("Crash reporter: {}", reporter_path.string());
  }

  // The wide overloads avoid a round trip through the ANSI code page, which
  // mangles paths under a non-ASCII account name.
#if defined(_WIN32)
  sentry_options_set_database_pathw(options, database_path.c_str());
  sentry_options_set_handler_pathw(options, handler_path.c_str());
  // Registered up front so the out-of-process handler knows to pick it up; the
  // file itself only appears when OnCrash writes it.
  sentry_options_add_attachmentw(options, g_crash_log_path.c_str());
#else
  sentry_options_set_database_path(options, database_path.c_str());
  sentry_options_set_handler_path(options, handler_path.c_str());
  sentry_options_add_attachment(options, g_crash_log_path.c_str());
#endif

  if (sentry_init(options) != 0) {
    REXLOG_ERROR("Sentry failed to initialize; crash reporting is unavailable");
    g_crash_log_path.clear();
    return false;
  }

  if (const std::string user = GetSettings().sentry_user.Get(); !user.empty()) {
    sentry_value_t value = sentry_value_new_object();
    sentry_value_set_by_key(value, "username", sentry_value_new_string(user.c_str()));
    sentry_set_user(value);
  }

  PublishStaticContexts();
  PublishModContext();

  FrameStats::Global().SetSummaryCallback(
      [](const FrameStats::Sample& sample) { PublishFrameStats(sample); });

  BreadcrumbSink& breadcrumbs = GetBreadcrumbSink();
  breadcrumbs.replaying = true;
  g_breadcrumb_guard = new rex::ScopeGuard(REX_LOGGER().AddSink(breadcrumbs));
  breadcrumbs.replaying = false;

  g_started = true;
  REXLOG_INFO("Crash reporting ready: release {}, environment {}, automatic sending {}",
              release, environment, automatic ? "on" : "off");
  return true;
}

void Stop() {
  std::lock_guard lock(g_mutex);
  if (!g_started)
    return;

  FrameStats::Global().SetSummaryCallback({});
  delete g_breadcrumb_guard;
  g_breadcrumb_guard = nullptr;

  sentry_close();
  g_started = false;
}

bool IsStarted() {
  std::lock_guard lock(g_mutex);
  return g_started;
}

void SetTag(std::string_view key, std::string_view value) {
  if (!IsStarted())
    return;
  sentry_set_tag(std::string(key).c_str(), std::string(value).c_str());
}

void AddBreadcrumb(std::string_view category, std::string_view message) {
  if (!IsStarted())
    return;
  sentry_value_t crumb =
      sentry_value_new_breadcrumb("default", std::string(message).c_str());
  sentry_value_set_by_key(crumb, "category",
                          sentry_value_new_string(std::string(category).c_str()));
  sentry_add_breadcrumb(crumb);
}

namespace {

bool HasReporterWindow() {
  return !g_reporter_path.empty() && std::filesystem::exists(g_reporter_path);
}

// Dumps the in-memory log so the reporter window has something to show. Unlike
// the crash path this runs in a healthy process, so it can take the ordinary
// route.
bool WriteLogSnapshot() {
  auto buffer = g_log_buffer;
  if (!buffer || g_crash_log_path.empty()) {
    return false;
  }
  std::ofstream out(g_crash_log_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  const std::string text = buffer->Snapshot();
  out << (GetSettings().sentry_scrub_paths.Get() ? ScrubPaths(text) : text);
  return true;
}

// Opens the reporter in bug report mode and waits for it on a worker thread,
// then sends from here rather than from the reporter: this process is the one
// that knows the mods, the frame timings and the rest of the context.
}  // namespace

void OpenBugReport() {
  if (!HasReporterWindow()) {
    REXLOG_INFO("Usage: rexcrash.bugreport <what went wrong>");
    return;
  }

  WriteLogSnapshot();
  FrameStats::Global().FlushSummary();

  std::thread([] {
    std::error_code ec;
    const auto message_path =
        std::filesystem::temp_directory_path(ec) / "rexglue-bugreport.txt";
    std::filesystem::remove(message_path, ec);

    const std::string command = std::format("\"{}\" --bug-report \"{}\"",
                                            g_reporter_path.string(), message_path.string());
    if (std::system(command.c_str()) != 0) {
      REXLOG_DEBUG("Bug report window exited without sending");
    }

    std::ifstream file(message_path, std::ios::binary);
    if (!file) {
      REXLOG_INFO("Bug report cancelled");
      return;
    }
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    std::filesystem::remove(message_path, ec);

    if (text.empty()) {
      REXLOG_INFO("Bug report cancelled");
      return;
    }

    // Start() may have been torn down while the window was open; Stop() holds
    // the same lock across sentry_close, so this either runs before it or sees
    // that there is nothing left to send through.
    std::lock_guard lock(g_mutex);
    if (!g_started) {
      REXLOG_WARN("Bug report discarded: reporting shut down while the window was open");
      return;
    }
    FrameStats::Global().FlushSummary();
    Capture(SENTRY_LEVEL_INFO, "bugreport", text);
    REXLOG_INFO("Bug report sent");
  }).detach();
}

void CaptureBugReport(std::string_view description) {
  if (!IsStarted() || !GetSettings().sentry_enabled.Get()) {
    REXLOG_WARN("Cannot send a bug report: automatic reporting is off (set sentry_enabled)");
    return;
  }
  // Publish a fresh frame rate summary so the report reflects how the game was
  // running when the player hit the problem, not the last interval boundary.
  FrameStats::Global().FlushSummary();
  Capture(SENTRY_LEVEL_INFO, "bugreport", description);
  REXLOG_INFO("Bug report sent ({} bytes of log attached)",
              g_log_buffer ? g_log_buffer->ByteSize() : 0);
}

void CaptureError(std::string_view message) {
  if (!IsStarted() || !GetSettings().sentry_enabled.Get())
    return;
  Capture(SENTRY_LEVEL_ERROR, "rexglue", message);
}

void CaptureFatal(std::string_view message) {
  if (!IsStarted() || !GetSettings().sentry_enabled.Get())
    return;
  Capture(SENTRY_LEVEL_FATAL, "rexglue", message);
  // The process is about to go away, so wait for the upload rather than
  // discarding the report on exit.
  sentry_flush(5000);
}

}  // namespace rexcrash
