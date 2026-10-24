/**
 * @file    gpu/render/pipeline.cpp
 */
#include "renut_engine/gpu/render/pipeline.h"

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <plume_render_interface_builders.h>
#include <rex/cvar.h>
#include <rex/logging/api.h>
#include <xxhash.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/guest/resources.h"
#include "renut_engine/gpu/render/shader_library.h"

REXCVAR_DEFINE_STRING(renut_pipeline_list, "pipeline_list.bin", "Nuts&Bolts/Graphics",
                      "The pipelines the renderer has built, which it builds ahead of use at the "
                      "next start (delete it to start over)");

REXCVAR_DEFINE_BOOL(shader_compile_overlay, true, "Nuts&Bolts/Performance",
                    "A note at the bottom right while shaders (pipelines) are being compiled");

namespace renut::gpu {

namespace {

struct CachedPipeline {
  PipelineKey key;
  std::unique_ptr<plume::RenderPipeline> pipeline;
};

// Pipelines being built right now (render thread and prebuild workers) and
// built this session, for the compile overlay.
std::atomic<u32> g_compiling{0};
std::atomic<u32> g_compiled{0};

// Render thread (State().mutex).
std::unordered_map<uint64_t, std::vector<CachedPipeline>> g_pipelines;
// The last lookup's: most draws use the pipeline the draw before them did,
// which a compare of the key finds without hashing it.
PipelineKey g_last_key{};
plume::RenderPipeline *g_last_pipeline = nullptr;

// Built pipelines by key hash, with their keys, in front of g_pipelines: the
// map's bucket, node and vector were three cache misses a draw before the key
// compare (pipelines are never erased). Two ways to a set, the newer first.
struct PipelineMemo {
  uint64_t hash = 0;
  plume::RenderPipeline *pipeline = nullptr;
  PipelineKey key{};
};
constexpr uint32_t kPipelineMemoBits = 10;
PipelineMemo g_pipeline_memo[2u << kPipelineMemoBits];

PipelineMemo *PipelineMemoSet(uint64_t hash) {
  return &g_pipeline_memo[uint32_t((hash * 0x9E3779B97F4A7C15ull) >> (64 - kPipelineMemoBits)) * 2];
}

void RememberPipeline(uint64_t hash, const PipelineKey &key, plume::RenderPipeline *pipeline) {
  PipelineMemo *set = PipelineMemoSet(hash);
  set[1] = set[0];
  set[0].hash = hash;
  set[0].pipeline = pipeline;
  set[0].key = key;
}

// Every draw looks its pipeline up: a byte-at-a-time hash of the whole key
// cost as much as the rest of the lookup.
uint64_t HashKey(const PipelineKey &key) {
  return XXH3_64bits(&key, sizeof(key));
}

plume::RenderBlendDesc BlendDesc(u32 control, u32 write_mask) {
  // RB_BLENDCONTROL: color src 0-4, op 5-7, dst 8-12; alpha src 16-20,
  // op 21-23, dst 24-28. Xenos has no enable bit: ONE/ZERO/ADD is no blending.
  plume::RenderBlendDesc desc;
  const u32 color_src = control & 0x1F, color_op = (control >> 5) & 7,
            color_dst = (control >> 8) & 0x1F;
  const u32 alpha_src = (control >> 16) & 0x1F, alpha_op = (control >> 21) & 7,
            alpha_dst = (control >> 24) & 0x1F;
  desc.srcBlend = ConvertBlendFactor(color_src);
  desc.dstBlend = ConvertBlendFactor(color_dst);
  desc.blendOp = ConvertBlendOp(color_op);
  desc.srcBlendAlpha = ConvertBlendFactor(alpha_src);
  desc.dstBlendAlpha = ConvertBlendFactor(alpha_dst);
  desc.blendOpAlpha = ConvertBlendOp(alpha_op);
  const bool color_passthrough = color_src == 1 && color_dst == 0 && color_op == 0;
  const bool alpha_passthrough = alpha_src == 1 && alpha_dst == 0 && alpha_op == 0;
  desc.blendEnabled = !(color_passthrough && alpha_passthrough);
  // Alpha factors cannot reference color; D3D12 rejects them.
  auto alpha_only = [](plume::RenderBlend b) {
    switch (b) {
    case plume::RenderBlend::SRC_COLOR:
      return plume::RenderBlend::SRC_ALPHA;
    case plume::RenderBlend::INV_SRC_COLOR:
      return plume::RenderBlend::INV_SRC_ALPHA;
    case plume::RenderBlend::DEST_COLOR:
      return plume::RenderBlend::DEST_ALPHA;
    case plume::RenderBlend::INV_DEST_COLOR:
      return plume::RenderBlend::INV_DEST_ALPHA;
    default:
      return b;
    }
  };
  desc.srcBlendAlpha = alpha_only(desc.srcBlendAlpha);
  desc.dstBlendAlpha = alpha_only(desc.dstBlendAlpha);
  desc.renderTargetWriteMask = uint8_t(write_mask & 0xF);
  return desc;
}

void CreatePipeline(plume::RenderDevice *device, const plume::RenderGraphicsPipelineDesc &desc,
                    std::unique_ptr<plume::RenderPipeline> *out) {
  *out = device->createGraphicsPipeline(desc);
}

#if defined(_WIN32)
// A driver's shader compiler that faults on a pipeline takes the process
// down; the log then names the pipeline (the crash dump goes on as before).
LONG LogPipelineFault(const PipelineKey &key, DWORD code) {
  const auto *vs = static_cast<const ShaderCacheEntry *>(key.vertex_shader);
  const auto *ps = static_cast<const ShaderCacheEntry *>(key.pixel_shader);
  RGPU_ERROR("The driver faulted ({:08X}) compiling pipeline vs_{:016x} ps_{:016x} ({} samples, "
             "color format {}, depth format {})",
             uint32_t(code), vs ? vs->hash : 0, ps ? ps->hash : 0, key.samples,
             uint32_t(key.color_formats[0]), uint32_t(key.depth_format));
  rex::FlushLogging();
  return EXCEPTION_CONTINUE_SEARCH;
}

// No objects with destructors in here: __try forbids unwinding them.
void CreatePipelineLogged(plume::RenderDevice *device, const plume::RenderGraphicsPipelineDesc &desc,
                          const PipelineKey &key, std::unique_ptr<plume::RenderPipeline> *out) {
  __try {
    CreatePipeline(device, desc, out);
  } __except (LogPipelineFault(key, GetExceptionCode())) {
  }
}
#endif

// Any thread: reads only what device creation set up.
std::unique_ptr<plume::RenderPipeline> Build(const PipelineKey &key, const PipelineInputs &inputs) {
  auto &s = State();
  const HostVertexLayout &layout = *inputs.layout;

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.draw_layout.get();
  desc.vertexShader = inputs.vertex_shader;
  desc.pixelShader = inputs.pixel_shader;
  if (key.rectangle_list)
    desc.geometryShader = s.rectangle_list_gs.get();
  if (key.point_sprite)
    desc.geometryShader = s.point_sprite_gs.get();
  desc.primitiveTopology = key.topology;

  // PA_SU_SC_MODE_CNTL: cull front 0, cull back 1, front face clockwise 2.
  // Points have no facing; their sprites are never culled.
  const bool cull_front = !key.point_sprite && (key.mode_control & 1);
  const bool cull_back = !key.point_sprite && (key.mode_control & 2);
  desc.cullMode = cull_front && !cull_back   ? plume::RenderCullMode::FRONT
                  : cull_back && !cull_front ? plume::RenderCullMode::BACK
                                             : plume::RenderCullMode::NONE;
  desc.frontFace = (key.mode_control & 4) ? plume::RenderFrontFace::CLOCKWISE
                                          : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  desc.depthClipEnabled = true;

  // RB_DEPTHCONTROL.
  const u32 dc = key.depth_control;
  if (key.depth_format != plume::RenderFormat::UNKNOWN) {
    desc.depthEnabled = dc & 2;
    desc.depthWriteEnabled = dc & 4;
    desc.depthFunction = ConvertCompareFunction((dc >> 4) & 7);
    desc.stencilEnabled = dc & 1;
    desc.stencilReference = key.stencil_ref_mask & 0xFF;
    desc.stencilReadMask = (key.stencil_ref_mask >> 8) & 0xFF;
    desc.stencilWriteMask = (key.stencil_ref_mask >> 16) & 0xFF;
    desc.stencilFrontFace.compareFunction = ConvertCompareFunction((dc >> 8) & 7);
    desc.stencilFrontFace.failOp = ConvertStencilOp((dc >> 11) & 7);
    desc.stencilFrontFace.passOp = ConvertStencilOp((dc >> 14) & 7);
    desc.stencilFrontFace.depthFailOp = ConvertStencilOp((dc >> 17) & 7);
    if (dc & 0x80) {
      desc.stencilBackFace.compareFunction = ConvertCompareFunction((dc >> 20) & 7);
      desc.stencilBackFace.failOp = ConvertStencilOp((dc >> 23) & 7);
      desc.stencilBackFace.passOp = ConvertStencilOp((dc >> 26) & 7);
      desc.stencilBackFace.depthFailOp = ConvertStencilOp((dc >> 29) & 7);
    } else {
      desc.stencilBackFace = desc.stencilFrontFace;
    }
  }
  desc.depthTargetFormat = key.depth_format;

  desc.renderTargetCount = key.color_count;
  for (uint32_t i = 0; i < key.color_count; ++i) {
    desc.renderTargetFormat[i] = key.color_formats[i];
    desc.renderTargetBlend[i] = BlendDesc(key.blend_control[i], key.color_mask >> (i * 4));
  }
  desc.multisampling = plume::RenderMultisampling(plume::RenderSampleCounts(key.samples));
  desc.alphaToCoverageEnabled = key.alpha_to_coverage != 0;

  plume::RenderInputSlot slots[kMaxVertexStreams + 1];
  uint32_t slot_count = 0;
  for (uint32_t stream = 0; stream < kMaxVertexStreams; ++stream) {
    const HostVertexStream &hs = layout.streams[stream];
    if (hs.used) {
      slots[slot_count++] = plume::RenderInputSlot(
          stream, hs.host_stride,
          key.per_instance_streams ? plume::RenderInputSlotClassification::PER_INSTANCE_DATA
                                   : plume::RenderInputSlotClassification::PER_VERTEX_DATA);
    }
  }
  slots[slot_count++] = plume::RenderInputSlot(kZeroStreamSlot, 0);
  desc.inputSlots = slots;
  desc.inputSlotsCount = slot_count;
  desc.inputElements = layout.input_elements.data();
  desc.inputElementsCount = uint32_t(layout.input_elements.size());

  std::unique_ptr<plume::RenderPipeline> pipeline;
  g_compiling.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
  CreatePipelineLogged(s.device.get(), desc, key, &pipeline);
#else
  CreatePipeline(s.device.get(), desc, &pipeline);
#endif
  g_compiling.fetch_sub(1, std::memory_order_relaxed);
  g_compiled.fetch_add(1, std::memory_order_relaxed);
  if (GraphicsPipelineCreated(pipeline.get())) {
    // Names the guest shaders in captures.
    const auto *vs = static_cast<const ShaderCacheEntry *>(key.vertex_shader);
    const auto *ps = static_cast<const ShaderCacheEntry *>(key.pixel_shader);
    pipeline->setName(fmt::format("vs_{:016x} ps_{:016x}", vs ? vs->hash : 0, ps ? ps->hash : 0));
    return pipeline;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// The pipeline list file
// ---------------------------------------------------------------------------
//
// A header, then records: u32 byte size of the rest of the record, the key
// with the two shader pointers replaced by the shaders' cache hashes, the
// layout's streams (u8 used, u32 host stride, 16 of them), u32 input element
// count, and each element (u8 name length, the name, u32 semantic index,
// location, format, slot, offset). A build whose PipelineKey or layout differs
// bumps kListVersion; a file of another version is started over.

constexpr char kListMagic[4] = {'R', 'N', 'P', 'L'};
constexpr u32 kListVersion = 1;

struct ListHeader {
  char magic[4];
  u32 version;
  u32 key_size;
  u32 streams;
};

class ByteWriter {
public:
  template <typename T>
  void Put(const T &value) {
    const auto *p = reinterpret_cast<const char *>(&value);
    bytes_.insert(bytes_.end(), p, p + sizeof(T));
  }
  void PutBytes(const void *data, size_t size) {
    const auto *p = static_cast<const char *>(data);
    bytes_.insert(bytes_.end(), p, p + size);
  }
  const std::vector<char> &Bytes() const { return bytes_; }

private:
  std::vector<char> bytes_;
};

class ByteReader {
public:
  ByteReader(const char *data, size_t size) : data_(data), size_(size) {}
  template <typename T>
  bool Get(T &value) {
    if (size_ - at_ < sizeof(T))
      return false;
    std::memcpy(&value, data_ + at_, sizeof(T));
    at_ += sizeof(T);
    return true;
  }
  bool GetBytes(void *out, size_t size) {
    if (size_ - at_ < size)
      return false;
    std::memcpy(out, data_ + at_, size);
    at_ += size;
    return true;
  }

private:
  const char *data_;
  size_t size_;
  size_t at_ = 0;
};

// The key as stored: the shader pointers as their cache hashes.
PipelineKey StoredKey(const PipelineKey &key) {
  PipelineKey stored = key;
  const auto *vs = static_cast<const ShaderCacheEntry *>(key.vertex_shader);
  const auto *ps = static_cast<const ShaderCacheEntry *>(key.pixel_shader);
  const uint64_t vs_hash = vs ? vs->hash : 0, ps_hash = ps ? ps->hash : 0;
  static_assert(sizeof(stored.vertex_shader) == sizeof(uint64_t));
  std::memcpy(&stored.vertex_shader, &vs_hash, sizeof(vs_hash));
  std::memcpy(&stored.pixel_shader, &ps_hash, sizeof(ps_hash));
  return stored;
}

std::vector<char> EncodeRecord(const PipelineKey &key, const HostVertexLayout &layout) {
  ByteWriter w;
  w.Put(StoredKey(key));
  for (u32 s = 0; s < kMaxVertexStreams; ++s) {
    w.Put(u8(layout.streams[s].used ? 1 : 0));
    w.Put(u32(layout.streams[s].host_stride));
  }
  w.Put(u32(layout.input_elements.size()));
  for (const plume::RenderInputElement &e : layout.input_elements) {
    const std::string name = e.semanticName ? e.semanticName : "";
    w.Put(u8(name.size()));
    w.PutBytes(name.data(), name.size());
    w.Put(u32(e.semanticIndex));
    w.Put(u32(e.location));
    w.Put(u32(e.format));
    w.Put(u32(e.slotIndex));
    w.Put(u32(e.alignedByteOffset));
  }
  return w.Bytes();
}

// A listed pipeline, as the worker threads build it.
struct Job {
  PipelineKey key{};
  uint64_t hash = 0;
  HostVertexLayout layout;
  std::deque<std::string> names;  // the elements' semantic names point here
  enum class Stage { Pending, Building, Done } stage = Stage::Pending;
  std::unique_ptr<plume::RenderPipeline> pipeline;  // Done: nullptr if taken or failed
  // Its shaders the game has not made yet (PipelineShaderLoaded); queued at 0.
  u32 shaders_missing = 0;
};

bool DecodeRecord(const char *data, size_t size, Job &job) {
  ByteReader r(data, size);
  PipelineKey stored;
  if (!r.Get(stored))
    return false;
  uint64_t vs_hash, ps_hash;
  std::memcpy(&vs_hash, &stored.vertex_shader, sizeof(vs_hash));
  std::memcpy(&ps_hash, &stored.pixel_shader, sizeof(ps_hash));
  const ShaderCacheEntry *vs = FindShaderEntry(vs_hash);
  const ShaderCacheEntry *ps = ps_hash ? FindShaderEntry(ps_hash) : nullptr;
  if (!vs || (ps_hash && !ps))
    return false;  // a shader this build lacks
  job.key = stored;
  job.key.vertex_shader = vs;
  job.key.pixel_shader = ps;
  job.hash = HashKey(job.key);
  for (u32 s = 0; s < kMaxVertexStreams; ++s) {
    u8 used;
    u32 stride;
    if (!r.Get(used) || !r.Get(stride))
      return false;
    job.layout.streams[s].used = used != 0;
    job.layout.streams[s].host_stride = stride;
  }
  u32 count;
  if (!r.Get(count) || count > 64)
    return false;
  for (u32 i = 0; i < count; ++i) {
    u8 length;
    std::string name;
    u32 index, location, format, slot, offset;
    if (!r.Get(length))
      return false;
    name.resize(length);
    if (!r.GetBytes(name.data(), length) || !r.Get(index) || !r.Get(location) || !r.Get(format) ||
        !r.Get(slot) || !r.Get(offset)) {
      return false;
    }
    job.names.push_back(std::move(name));
    job.layout.input_elements.emplace_back(job.names.back().c_str(), index, location,
                                           plume::RenderFormat(format), slot, offset);
  }
  return true;
}

// The list's records, and the file the new ones are appended to. Render
// thread, except the load at the start.
struct PipelineList {
  std::ofstream out;
  bool failed = false;
  std::unordered_set<uint64_t> listed;  // key hashes in the file

  void Append(const PipelineKey &key, uint64_t hash, const HostVertexLayout &layout) {
    if (failed || !listed.insert(hash).second)
      return;
    if (!out.is_open()) {
      const std::filesystem::path path(REXCVAR_GET(renut_pipeline_list));
      std::error_code ec;
      const bool fresh = !std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) == 0;
      out.open(path, std::ios::binary | std::ios::app);
      if (!out) {
        failed = true;
        RGPU_WARN("Pipeline list {} cannot be written; pipelines are not built ahead next time",
                  path.string());
        return;
      }
      if (fresh) {
        ListHeader header{};
        std::memcpy(header.magic, kListMagic, 4);
        header.version = kListVersion;
        header.key_size = sizeof(PipelineKey);
        header.streams = kMaxVertexStreams;
        out.write(reinterpret_cast<const char *>(&header), sizeof(header));
      }
    }
    const std::vector<char> record = EncodeRecord(key, layout);
    const u32 size = u32(record.size());
    out.write(reinterpret_cast<const char *>(&size), sizeof(size));
    out.write(record.data(), std::streamsize(record.size()));
    out.flush();
  }
};
PipelineList g_list;

// The records of the list file; empty (and the file started over) when it
// is missing or from another version.
std::vector<std::unique_ptr<Job>> LoadList() {
  std::vector<std::unique_ptr<Job>> jobs;
  const std::filesystem::path path(REXCVAR_GET(renut_pipeline_list));
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return jobs;
  const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  ListHeader header{};
  if (bytes.size() >= sizeof(header))
    std::memcpy(&header, bytes.data(), sizeof(header));
  const bool valid = bytes.size() >= sizeof(header) && std::memcmp(header.magic, kListMagic, 4) == 0 &&
                     header.version == kListVersion && header.key_size == sizeof(PipelineKey) &&
                     header.streams == kMaxVertexStreams;
  if (!valid) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return jobs;
  }
  std::unordered_set<uint64_t> seen;
  size_t at = sizeof(header);
  while (bytes.size() - at >= sizeof(u32)) {
    u32 size;
    std::memcpy(&size, bytes.data() + at, sizeof(size));
    at += sizeof(size);
    if (bytes.size() - at < size)
      break;  // cut short (the game closed while writing)
    auto job = std::make_unique<Job>();
    if (DecodeRecord(bytes.data() + at, size, *job) && seen.insert(job->hash).second)
      jobs.push_back(std::move(job));
    at += size;
  }
  // Records of shaders this build lacks stay out of 'listed': a later build
  // that has them lists the pipelines again.
  g_list.listed = std::move(seen);
  return jobs;
}

// ---------------------------------------------------------------------------
// Building the listed pipelines ahead
// ---------------------------------------------------------------------------
//
// A listed pipeline is built once the game has made both of its shaders (a
// level's shaders come with its load), not at the start: the list holds every
// level ever played, and the driver keeps each built pipeline's shaders and
// code in host memory. What is built stays built.

struct Prebuild {
  std::mutex mutex;
  std::condition_variable work;  // a job queued, or stopping
  std::condition_variable done;  // a job reached Done, or a worker ended
  std::vector<std::unique_ptr<Job>> jobs;
  std::unordered_map<uint64_t, std::vector<Job *>> by_hash;
  std::deque<Job *> queue;       // shaders all made; may have been claimed since
  // The shaders the game has made, and the jobs waiting for each of the others.
  std::unordered_set<const ShaderCacheEntry *> loaded;
  std::unordered_map<const ShaderCacheEntry *, std::vector<Job *>> waiting;
  u32 workers = 0;               // still running
  u32 building = 0;              // workers between taking a job and finishing it
  bool stop = false;
  // Host shaders for the queued jobs, shared by the workers; freed whenever the
  // queue runs dry (the pipelines keep their own compiled copies).
  std::unordered_map<const ShaderCacheEntry *, std::unique_ptr<plume::RenderShader>> shaders;
};
Prebuild g_prebuild;

plume::RenderShader *PrebuildShader(const ShaderCacheEntry *entry) {
  if (!entry)
    return nullptr;
  auto &slot = g_prebuild.shaders[entry];
  if (!slot)
    slot = CreateCachedShader(State().device.get(), *entry);
  return slot.get();
}

void PrebuildWorker() {
#if defined(_WIN32)
  SetThreadDescription(GetCurrentThread(), L"renut pipeline prebuild");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  std::unique_lock lock(g_prebuild.mutex);
  for (;;) {
    g_prebuild.work.wait(lock, [] { return g_prebuild.stop || !g_prebuild.queue.empty(); });
    if (g_prebuild.stop)
      break;
    Job &job = *g_prebuild.queue.front();
    g_prebuild.queue.pop_front();
    if (job.stage != Job::Stage::Pending)
      continue;  // the render thread claimed it (TakePrebuilt)
    job.stage = Job::Stage::Building;
    ++g_prebuild.building;
    PipelineInputs inputs;
    inputs.vertex_shader = PrebuildShader(static_cast<const ShaderCacheEntry *>(job.key.vertex_shader));
    inputs.pixel_shader = PrebuildShader(static_cast<const ShaderCacheEntry *>(job.key.pixel_shader));
    inputs.layout = &job.layout;
    const bool shaders = inputs.vertex_shader && (!job.key.pixel_shader || inputs.pixel_shader);
    lock.unlock();
    std::unique_ptr<plume::RenderPipeline> pipeline = shaders ? Build(job.key, inputs) : nullptr;
    lock.lock();
    job.pipeline = std::move(pipeline);
    job.stage = Job::Stage::Done;
    if (--g_prebuild.building == 0 && g_prebuild.queue.empty())
      g_prebuild.shaders.clear();
    g_prebuild.done.notify_all();
  }
  if (--g_prebuild.workers == 0)
    g_prebuild.shaders.clear();
  g_prebuild.done.notify_all();
}

// Requires g_prebuild.mutex. Queues the job once the game has made its shaders,
// or waits for the ones it has not.
void QueueOrWait(Job &job) {
  for (const void *shader : {job.key.vertex_shader, job.key.pixel_shader}) {
    const auto *entry = static_cast<const ShaderCacheEntry *>(shader);
    if (entry && !g_prebuild.loaded.contains(entry)) {
      ++job.shaders_missing;
      g_prebuild.waiting[entry].push_back(&job);
    }
  }
  if (job.shaders_missing == 0)
    g_prebuild.queue.push_back(&job);
}

// The listed pipeline for the key, when the list has it: built by a worker
// (waited for when one is building it), or nullptr with 'claimed' set when no
// worker has started it, which the caller then builds itself. Render thread.
plume::RenderPipeline *TakePrebuilt(const PipelineKey &key, uint64_t hash,
                                    std::unique_ptr<plume::RenderPipeline> &out, bool &claimed) {
  claimed = false;
  std::unique_lock lock(g_prebuild.mutex);
  auto it = g_prebuild.by_hash.find(hash);
  if (it == g_prebuild.by_hash.end())
    return nullptr;
  Job *job = nullptr;
  for (Job *j : it->second) {
    if (std::memcmp(&j->key, &key, sizeof(key)) == 0)
      job = j;
  }
  if (!job)
    return nullptr;
  if (job->stage == Job::Stage::Pending) {
    job->stage = Job::Stage::Done;  // the render thread's now
    claimed = true;
    return nullptr;
  }
  g_prebuild.done.wait(lock, [job] { return job->stage == Job::Stage::Done; });
  out = std::move(job->pipeline);
  return out.get();
}

} // namespace

bool BuildDrawLayout(VideoState &s) {
  plume::RenderDescriptorSetBuilder textures;
  textures.begin();
  textures.addTexture(0, kTextureDescriptorCount);
  textures.end(true, kTextureDescriptorCount);

  plume::RenderDescriptorSetBuilder samplers;
  samplers.begin();
  samplers.addSampler(0, kSamplerDescriptorCount);
  samplers.end(true, kSamplerDescriptorCount);

  plume::RenderPipelineLayoutBuilder layout;
  layout.begin(false, true);
  layout.addDescriptorSet(textures);  // space0 Texture2D[]
  layout.addDescriptorSet(textures);  // space1 Texture3D[]
  layout.addDescriptorSet(textures);  // space2 TextureCube[]
  layout.addDescriptorSet(samplers);  // space3 SamplerState[]
  layout.addDescriptorSet(textures);  // space4 Texture2DArray[] (stacked textures)
  if (IsVulkan()) {
    // Geometry too: point_sprite_gs reads SharedConstants.
    layout.addPushConstant(0, 4, sizeof(DrawPushConstants),
                           plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::GEOMETRY |
                               plume::RenderShaderStageFlag::PIXEL);
  } else {
    layout.addRootDescriptor(0, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
    layout.addRootDescriptor(1, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
    layout.addRootDescriptor(2, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
    layout.addRootDescriptor(0, 5, plume::RenderRootDescriptorType::SHADER_RESOURCE);
    layout.addRootDescriptor(0, 6, plume::RenderRootDescriptorType::UNORDERED_ACCESS);
  }
  layout.end();
  s.draw_layout = layout.create(s.device.get());
  if (!s.draw_layout) {
    RGPU_ERROR("Draw pipeline layout creation failed");
    return false;
  }
  return true;
}

plume::RenderPipeline *GetPipelineLocked(const PipelineKey &key, const PipelineInputs &inputs) {
  if (g_last_pipeline && std::memcmp(&g_last_key, &key, sizeof(key)) == 0)
    return g_last_pipeline;
  const uint64_t hash = HashKey(key);
  PipelineMemo *set = PipelineMemoSet(hash);
  for (uint32_t way = 0; way < 2; ++way) {
    const PipelineMemo &m = set[way];
    if (m.pipeline && m.hash == hash && std::memcmp(&m.key, &key, sizeof(key)) == 0) {
      g_last_key = key;
      g_last_pipeline = m.pipeline;
      return m.pipeline;
    }
  }
  auto &bucket = g_pipelines[hash];
  for (auto &cached : bucket) {
    if (std::memcmp(&cached.key, &key, sizeof(key)) == 0) {
      if (cached.pipeline) {
        g_last_key = key;
        g_last_pipeline = cached.pipeline.get();
        RememberPipeline(hash, key, g_last_pipeline);
      }
      return cached.pipeline.get();
    }
  }
  std::unique_ptr<plume::RenderPipeline> pipeline;
  bool claimed = false;
  if (!TakePrebuilt(key, hash, pipeline, claimed)) {
    pipeline = Build(key, inputs);
    if (pipeline && !claimed)
      g_list.Append(key, hash, *inputs.layout);
  }
  if (!pipeline) {
    static uint32_t logged = 0;
    if (!ReportDeviceLost(State().device.get()) && logged++ < 32) {
      const auto *vs = static_cast<const ShaderCacheEntry *>(key.vertex_shader);
      const auto *ps = static_cast<const ShaderCacheEntry *>(key.pixel_shader);
      RGPU_ERROR("Pipeline creation failed (key {:016X}): VS {:016X} (spec constants {:X}), "
                 "PS {:016X} (spec constants {:X}), {} targets [{} {} {} {}], depth format {}, "
                 "{} samples, {} input elements, topology {}",
                 hash, vs ? vs->hash : 0, vs ? vs->specConstantsMask : 0, ps ? ps->hash : 0,
                 ps ? ps->specConstantsMask : 0, key.color_count, u32(key.color_formats[0]),
                 u32(key.color_formats[1]), u32(key.color_formats[2]), u32(key.color_formats[3]),
                 u32(key.depth_format), key.samples, inputs.layout->input_elements.size(),
                 u32(key.topology));
    }
  }
  bucket.push_back({key, std::move(pipeline)});
  plume::RenderPipeline *result = bucket.back().pipeline.get();
  if (result) {
    g_last_key = key;
    g_last_pipeline = result;
    RememberPipeline(hash, key, result);
  }
  return result;
}

void StartPipelinePrebuild() {
  std::vector<std::unique_ptr<Job>> jobs = LoadList();
  if (jobs.empty())
    return;
  std::lock_guard lock(g_prebuild.mutex);
  g_prebuild.jobs = std::move(jobs);
  for (const auto &job : g_prebuild.jobs) {
    g_prebuild.by_hash[job->hash].push_back(job.get());
    QueueOrWait(*job);
  }
  // Leave the render and game threads their cores.
  const u32 cores = std::max(1u, std::thread::hardware_concurrency());
  const u32 workers = std::clamp(cores / 2, 1u, 4u);
  g_prebuild.workers = workers;
  for (u32 i = 0; i < workers; ++i)
    std::thread(PrebuildWorker).detach();
}

void StopPipelinePrebuild() {
  // The workers finish the pipeline each is building (they use the device).
  std::unique_lock lock(g_prebuild.mutex);
  g_prebuild.stop = true;
  g_prebuild.work.notify_all();
  g_prebuild.done.wait(lock, [] { return g_prebuild.workers == 0; });
}

void PipelineShaderLoaded(const ShaderCacheEntry *entry) {
  if (!entry)
    return;
  std::lock_guard lock(g_prebuild.mutex);
  if (!g_prebuild.loaded.insert(entry).second)
    return;
  auto it = g_prebuild.waiting.find(entry);
  if (it == g_prebuild.waiting.end())
    return;
  bool queued = false;
  for (Job *job : it->second) {
    // A job the render thread claimed meanwhile is done with.
    if (--job->shaders_missing == 0 && job->stage == Job::Stage::Pending) {
      g_prebuild.queue.push_back(job);
      queued = true;
    }
  }
  g_prebuild.waiting.erase(it);
  if (queued)
    g_prebuild.work.notify_all();
}

PipelineCompileStatus GetPipelineCompileStatus() {
  PipelineCompileStatus status;
  status.compiling = g_compiling.load(std::memory_order_relaxed);
  status.compiled = g_compiled.load(std::memory_order_relaxed);
  std::lock_guard lock(g_prebuild.mutex);
  for (const Job *job : g_prebuild.queue)
    status.queued += job->stage == Job::Stage::Pending ? 1 : 0;
  return status;
}

PipelineCounts PipelineCountsLocked() {
  PipelineCounts counts;
  for (const auto &[hash, bucket] : g_pipelines) {
    for (const CachedPipeline &cached : bucket)
      counts.built += cached.pipeline ? 1 : 0;
  }
  std::lock_guard lock(g_prebuild.mutex);
  counts.listed = g_prebuild.jobs.size();
  for (const auto &job : g_prebuild.jobs) {
    counts.prebuilt_unused += job->pipeline ? 1 : 0;
    counts.waiting += job->stage == Job::Stage::Pending && job->shaders_missing ? 1 : 0;
  }
  return counts;
}

} // namespace renut::gpu
