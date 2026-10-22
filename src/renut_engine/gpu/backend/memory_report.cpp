/**
 * @file    gpu/backend/memory_report.cpp
 */
#include "renut_engine/gpu/backend/memory_report.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/pipeline.h"
#include "renut_engine/gpu/render/shader_library.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/backend/video.h"

REXCVAR_DEFINE_DOUBLE(renut_memory_report, 30.0, "Nuts&Bolts/Debug",
                      "Seconds between memory reports in the log (process, GPU and the "
                      "renderer's pools); 0 turns them off.");

namespace renut::gpu {

namespace {

// Private: memory only this process has (what Task Manager's "Memory" column
// shows on Windows); resident: what of it, and of shared mappings, is in RAM.
struct ProcessMemory {
  bool valid = false;
  uint64_t private_bytes = 0;
  uint64_t resident_bytes = 0;
};

ProcessMemory QueryProcessMemory() {
  ProcessMemory memory;
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                           sizeof(counters))) {
    memory.valid = true;
    memory.private_bytes = counters.PrivateUsage;
    memory.resident_bytes = counters.WorkingSetSize;
  }
#elif defined(__linux__)
  // RssAnon: resident anonymous memory, the closest to Windows' private bytes.
  if (FILE *status = std::fopen("/proc/self/status", "r")) {
    char line[256];
    while (std::fgets(line, sizeof(line), status)) {
      unsigned long long kb = 0;
      if (std::sscanf(line, "RssAnon: %llu kB", &kb) == 1)
        memory.private_bytes = kb << 10;
      else if (std::sscanf(line, "VmRSS: %llu kB", &kb) == 1)
        memory.resident_bytes = kb << 10;
    }
    std::fclose(status);
    memory.valid = memory.resident_bytes != 0;
  }
#endif
  return memory;
}

double Mb(uint64_t bytes) {
  return double(bytes) / double(1u << 20);
}

} // namespace

void MaybeReportMemory() {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last = Clock::now();
  const double interval = REXCVAR_GET(renut_memory_report);
  const Clock::time_point now = Clock::now();
  if (interval <= 0.0 || std::chrono::duration<double>(now - last).count() < interval)
    return;
  last = now;

  // Outside State().mutex: the recording lock is never taken inside it.
  const CommandBufferMemory commands = CommandBufferMemoryUsage();

  auto &s = State();
  std::lock_guard lock(s.mutex);
  if (!s.host_ready)
    return;
  const ProcessMemory process = QueryProcessMemory();
  const GpuMemoryUsage gpu = QueryGpuMemory(s.device.get());
  size_t targets = 0;
  uint64_t target_bytes = 0;
  SurfaceMemoryLocked(targets, target_bytes);
  const TextureMemory textures = TextureMemoryLocked();
  const StreamCacheMemory streams = StreamCacheMemoryLocked();
  const PipelineCounts pipelines = PipelineCountsLocked();
  uint64_t uploads = 0, memexport = 0, readback = 0;
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    uploads += s.uploads[i].Bytes();
    memexport += s.memexport_capacity[i] + s.memexport_scratch_capacity[i] +
                 s.exported_streams_capacity[i] + s.gpu_fetch_data_capacity[i];
    readback += s.memexport_readback_capacity[i];
  }

  RGPU_INFO("Memory: process {:.0f} MB private, {:.0f} MB resident; GPU {} (local {:.0f}/{:.0f} "
            "MB, shared {:.0f}/{:.0f} MB)",
            Mb(process.private_bytes), Mb(process.resident_bytes), gpu.valid ? "used/budget" : "n/a",
            Mb(gpu.local_used), Mb(gpu.local_budget), Mb(gpu.shared_used), Mb(gpu.shared_budget));
  RGPU_INFO("Memory: GPU: {} render targets {:.0f} MB (x{:.3g} scale), {} textures {:.0f} MB (of "
            "which {} resolve targets {:.0f} MB), memexport {:.0f} MB, stream cache {:.0f} MB in "
            "{:.0f} MB of pools ({} streams, {} index ranges)",
            targets, Mb(target_bytes), ResolutionScale().Factor(), textures.textures, Mb(textures.bytes),
            textures.resolve_targets, Mb(textures.resolve_bytes), Mb(memexport),
            Mb(streams.cached_bytes), Mb(streams.pool_bytes), streams.streams, streams.index_ranges);
  RGPU_INFO("Memory: host: upload rings {:.0f} MB, memexport readback {:.0f} MB, command buffers "
            "{:.0f} MB in {} ({} live) + {:.0f} MB in {} spare, shader cache {:.0f} MB, "
            "pipelines {} built + {} prebuilt unused (of {} listed, {} waiting for their shaders)",
            Mb(uploads), Mb(readback), Mb(commands.recorded_bytes), commands.recorded,
            commands.live, Mb(commands.spare_bytes), commands.spare, Mb(ShaderCacheHostBytes()),
            pipelines.built, pipelines.prebuilt_unused, pipelines.listed, pipelines.waiting);
}

} // namespace renut::gpu
