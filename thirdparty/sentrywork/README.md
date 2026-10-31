# SentryWork

Crash reports for ReXGlue recomps, sent to the recomp owner's [Sentry](https://sentry.io) project.

When the game crashes, SentryWork sends a report with a minidump, the game's current log file, the last warnings and errors as breadcrumbs, and tags for the release, the title ID and the ReXGlue version. Fatal asserts, `abort()` and `std::terminate` count as crashes too. Players can turn reports off in the game's `.toml`.

It is built on the [Sentry Native SDK](https://github.com/getsentry/sentry-native) 0.17.3 (MIT), vendored in `sentry-native/`, using its `native` backend. That backend writes up and sends a crash from a small helper process, `sentry-crash`, so a crash that has wrecked the game's own process still gets reported.

## Add it to a recomp

1. Copy this folder to `thirdparty/sentrywork/`.
2. Make a project on sentry.io (platform **Native**) and copy its DSN from *Settings → Client Keys*. A DSN only allows sending events, so it is safe to build into a game.
3. Add this to `CMakeLists.txt`, after `rexglue_setup_target(<target>)`:

   ```cmake
   include(thirdparty/sentrywork/CMakeLists.txt)
   sentrywork_attach(my_game
       DSN "https://<key>@o<org>.ingest.us.sentry.io/<project>"
       RELEASE "my_game@1.2.0"               # optional: defaults to <target>@<PROJECT_VERSION>, or <target>@dev
       ORG my-org PROJECT my-game)           # optional: adds the my_game_upload_symbols target
   ```

4. Call it from the app class:

   ```cpp
   #include <sentrywork/sentrywork.h>

   void OnPostInitLogging() override { sentrywork::Init({.app_name = "my_game"}); }
   void OnPostSetup() override { sentrywork::OnRuntimeReady(runtime()); }
   void OnShutdown() override { sentrywork::Shutdown(); }
   ```

   `Init` has to run in `OnPostInitLogging`. By then the game's `.toml` has been read, so `crash_reports = false` is respected. The runtime doesn't exist yet, and its exception handler must be installed after Sentry's. `Options` also takes `database_dir`, the folder where reports wait to be sent (give a writable one when the executable's folder isn't, as in an AppImage), plus `log_file` and extra `tags`.

5. Build, then crash on purpose to check that reports arrive:

   ```
   SENTRYWORK_TEST_CRASH=1 ./my_game
   ```

   The game writes through a null pointer once it's loaded. The report shows up in Sentry's *Issues* within a minute or so.

## Readable stack traces

Sentry names the functions in a crash from the build's debug files. With `SENTRYWORK_DEBUG_INFO` (on by default), every configuration builds with line tables, and on Windows writes a `.pdb` next to the executable. The executable itself barely changes.

Upload the debug files for each build you hand out, **before** players run it. You'll need [`sentry-cli`](https://docs.sentry.io/cli/) (`npm install -g @sentry/cli`) and an organization auth token (Sentry → *Settings → Developer Settings → Organization Tokens*), set in your shell:

```powershell
# PowerShell
$env:SENTRY_AUTH_TOKEN = "<token>"
cmake --build --preset <preset> --target my_game_upload_symbols
```

```sh
# bash
SENTRY_AUTH_TOKEN=<token> cmake --build --preset <preset> --target my_game_upload_symbols
```

Keep the token out of files and chat. If it gets pasted anywhere, revoke it and make a new one.

This sends the executable, its `.pdb` and the ReXGlue runtime. Debug files contain function names and source file paths, not source code. A build whose files were never uploaded still reports crashes, but its frames show only addresses. Sentry can reprocess those events after a late upload.

## What to ship

| Ship to players | Keep out |
|---|---|
| the game executable | the `.pdb` (upload it, don't ship it) |
| `sentry-crash` / `sentry-crash.exe` (next to the executable) | the `sentry/` folder (this machine's queued reports) |
| what the game already needs (`rexruntime.dll`, …) | |

Without `sentry-crash` next to the executable, the game still runs, but crashes go unreported.

## Settings

Players can change these in the game's `.toml`:

| cvar | default | |
|---|---|---|
| `crash_reports` | `true` | Send crash reports at all. |
| `crash_report_log_level` | `"warn"` | Log lines sent as breadcrumbs: `error`, `warn`, `info` or `off`. Each one is written to disk, so `info` costs more. |

CMake options:

| option | default | |
|---|---|---|
| `SENTRYWORK_DISABLE` | `OFF` | Build with no DSN compiled in, for forks and local builds that shouldn't report to your project. |
| `SENTRYWORK_DEBUG_INFO` | `ON` | Line tables and a `.pdb` for every configuration. |
| `SENTRYWORK_BACKEND` | `native` | `breakpad` or `inproc` send from inside the game instead, with no `sentry-crash` to ship. (Crashpad, Sentry's usual backend, doesn't build with clang++ on Windows, so it is left out of `sentry-native/`.) |

## Notes

- **Linux** sends reports over libcurl, so install `libcurl4-openssl-dev` before building. The runtime's signal handler replaces Sentry's and never passes on a fault none of its handlers claims. SentryWork adds itself as the runtime's last handler and hands real crashes back to Sentry (`src/fault_fallback_posix.cpp`). An AppImage needs `sentry-crash` copied next to the game in `usr/bin`.
- **Windows**: Sentry's unhandled exception filter ends the process once the crash is written up, so a game's own minidump writer that it replaced no longer runs. Crashes that skip exception handling altogether (`__fastfail`, stack buffer overruns) aren't caught.
- The DSN can be overridden for one run with the `SENTRY_DSN` environment variable. `SENTRY_DEBUG=1` prints the SDK's own log.
