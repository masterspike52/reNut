/**
 * @file        rex/system_report.h
 * @brief       Host hardware and OS inventory for logs and crash reports
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rexcrash {

struct GpuReport {
  std::string name;              ///< Adapter description, e.g. "NVIDIA GeForce RTX 4070".
  std::string driver_version;    ///< Empty when the platform does not expose it.
  uint64_t dedicated_vram = 0;   ///< Bytes on the adapter itself.
  uint64_t shared_memory = 0;    ///< Bytes borrowed from system RAM.
};

/**
 * A snapshot of the machine the game is running on.
 *
 * Gathered once at startup, logged at INFO so it heads every log file, and
 * mirrored into crash reports as structured contexts. Everything here is
 * best-effort: fields the platform will not tell us stay empty rather than
 * failing the query.
 */
struct SystemReport {
  std::string os;              ///< "Windows 11 Pro 24H2 (build 26200.1234)"
  std::string kernel;          ///< Raw version string, when it differs from `os`.
  std::string compatibility;   ///< Wine/Proton version, empty on native runs.
  std::string cpu;             ///< Brand string.
  uint32_t cpu_cores = 0;      ///< Physical cores, 0 when unknown.
  uint32_t cpu_threads = 0;    ///< Logical processors.
  uint64_t ram_total = 0;      ///< Bytes.
  uint64_t ram_available = 0;  ///< Bytes free at startup.
  std::vector<GpuReport> gpus;
  std::string locale;
};

/// Query the host. Cheap enough for startup; call once and keep the result.
SystemReport QuerySystemReport();

/// The process-wide report, queried on first use.
const SystemReport& GetSystemReport();

/// Human-readable byte count, e.g. "31.9 GiB".
std::string FormatBytes(uint64_t bytes);

/**
 * Write the report to the log as a block of INFO lines.
 *
 * INFO so it lands in the in-memory crash buffer (and any log file) even when
 * the player has turned the log level down; a report without the specs on it
 * is most of the way to useless.
 */
void LogSystemReport(const SystemReport& report);

}  // namespace rexcrash
