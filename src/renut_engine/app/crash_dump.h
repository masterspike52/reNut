#pragma once

// Writes a minidump to <exe dir>/crashdumps/ when the process crashes:
// unhandled exceptions, abort(), std::terminate and CRT invalid-parameter /
// pure-call failures. Open one with `cdb -z <file> -y <exe dir>` (renut.pdb
// maps host frames to the recompiled guest functions). Windows only; a no-op
// elsewhere.
namespace renut::crash {

// Safe to call more than once: a later call re-installs the unhandled
// exception filter in case something replaced it since.
void InstallCrashDumps();

} // namespace renut::crash
