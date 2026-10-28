/**
 * @file    gpu/render/pipeline.cpp
 */
#include "renut_engine/gpu/render/pipeline.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
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

#include "renut_engine/app/app_paths.h"
#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/guest/resources.h"
#include "renut_engine/gpu/render/shader_library.h"

REXCVAR_DEFINE_BOOL(renut_pipeline_capture, false, "Nuts&Bolts/Debug",
                    "Writes each pipeline a draw had to build itself (none of the built-in "
                    "list's) to pipeline_capture_<date>.txt next to renut, for "
                    "tools/pipeline_cache.py to add to the built-in list");

namespace renut::gpu {

namespace {

struct CachedPipeline {
  PipelineKey key;
  std::unique_ptr<plume::RenderPipeline> pipeline;
};

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
    desc.depthBias = key.depth_bias;
    desc.depthBiasClamp = key.depth_bias_clamp;
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
#if defined(_WIN32)
  CreatePipelineLogged(s.device.get(), desc, key, &pipeline);
#else
  CreatePipeline(s.device.get(), desc, &pipeline);
#endif
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
// The built-in pipeline list
// ---------------------------------------------------------------------------
//
// pipeline_cache_data.inc, made by tools/pipeline_cache.py from pipeline
// captures (below): the pipelines with their shaders as cache hashes, and the
// vertex layouts they share. Each array ends in an empty entry.

struct BuiltInElement {
  const char *name;
  u32 semantic_index;
  u32 location;
  u32 format;
  u32 slot;
  u32 offset;
};

struct BuiltInLayout {
  u32 first_element;
  u32 element_count;
  uint64_t hash;      // PipelineKey::layout_hash
  u32 used_streams;   // a bit each
  u32 strides[kMaxVertexStreams];
};

// The PipelineKey's fields, the shaders as cache hashes (pixel 0: none).
struct BuiltInPipeline {
  uint64_t vertex_shader;
  uint64_t pixel_shader;
  u32 layout;
  u32 topology;
  u32 color_formats[d3d::kMaxRenderTargets];
  u32 depth_format;
  u32 color_count;
  u32 samples;
  u32 blend_control[d3d::kMaxRenderTargets];
  u32 color_mask;
  u32 depth_control;
  u32 stencil_ref_mask;
  u32 stencil_ref_mask_bf;
  u32 mode_control;
  int32_t depth_bias;
  float depth_bias_clamp;
  u32 alpha_to_coverage;
  u32 per_instance_streams;
  u32 rectangle_list;
  u32 point_sprite;
};

#include "renut_engine/gpu/render/pipeline_cache_data.inc"

// ---------------------------------------------------------------------------
// Pipeline captures
// ---------------------------------------------------------------------------
//
// With renut_pipeline_capture on, each pipeline a draw builds itself (none of
// the built-in list's) goes to a file of this session, a line each of
// name=value fields: the shaders as cache hashes, the layout written out, the
// rest of the key. tools/pipeline_cache.py adds them to the built-in list.

std::filesystem::path CapturePath() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char name[64];
  std::strftime(name, sizeof(name), "pipeline_capture_%Y%m%d-%H%M%S.txt", &local);
  return renut::paths::WritableFolder() / name;
}

std::string CaptureLine(const PipelineKey &key, const HostVertexLayout &layout) {
  const auto *vs = static_cast<const ShaderCacheEntry *>(key.vertex_shader);
  const auto *ps = static_cast<const ShaderCacheEntry *>(key.pixel_shader);
  u32 used = 0;
  std::string strides, elements;
  for (u32 s = 0; s < kMaxVertexStreams; ++s) {
    used |= layout.streams[s].used ? 1u << s : 0u;
    strides += fmt::format("{}{}", s ? "," : "", layout.streams[s].host_stride);
  }
  for (const plume::RenderInputElement &e : layout.input_elements) {
    elements += fmt::format("{}{}:{}:{}:{}:{}:{}", elements.empty() ? "" : ",",
                            e.semanticName ? e.semanticName : "", e.semanticIndex, e.location,
                            u32(e.format), e.slotIndex, e.alignedByteOffset);
  }
  auto four = [](const auto &values) {
    return fmt::format("{},{},{},{}", u32(values[0]), u32(values[1]), u32(values[2]),
                       u32(values[3]));
  };
  static_assert(d3d::kMaxRenderTargets == 4);
  return fmt::format(
      "vs=0x{:016X} ps=0x{:016X} layout=0x{:016X} used=0x{:04X} strides={} elements={} "
      "topology={} color_formats={} depth_format={} color_count={} samples={} blend_control={} "
      "color_mask={} depth_control={} stencil_ref_mask={} stencil_ref_mask_bf={} mode_control={} "
      "depth_bias={} depth_bias_clamp={:.9g} alpha_to_coverage={} per_instance_streams={} "
      "rectangle_list={} point_sprite={}",
      vs ? vs->hash : 0, ps ? ps->hash : 0, key.layout_hash, used, strides, elements,
      u32(key.topology), four(key.color_formats), u32(key.depth_format), key.color_count,
      key.samples, four(key.blend_control), key.color_mask, key.depth_control,
      key.stencil_ref_mask, key.stencil_ref_mask_bf, key.mode_control, key.depth_bias,
      key.depth_bias_clamp, key.alpha_to_coverage, key.per_instance_streams, key.rectangle_list,
      key.point_sprite);
}

// Render thread. Each key is built once (GetPipelineLocked keeps even a
// failed build), so each is written once.
struct PipelineCapture {
  std::ofstream out;
  bool failed = false;

  void Add(const PipelineKey &key, const HostVertexLayout &layout) {
    if (failed || !REXCVAR_GET(renut_pipeline_capture))
      return;
    if (!out.is_open()) {
      const std::filesystem::path path = CapturePath();
      out.open(path, std::ios::binary | std::ios::trunc);
      if (!out) {
        failed = true;
        RGPU_WARN("Pipeline capture {} cannot be written", path.string());
        return;
      }
      RGPU_INFO("Pipeline capture: the pipelines draws build themselves go to {}", path.string());
      out << "# reNut pipeline capture 1 (tools/pipeline_cache.py)\n";
    }
    out << CaptureLine(key, layout) << '\n';
    out.flush();
  }
};
PipelineCapture g_capture;

// A listed pipeline, as the worker threads build it.
struct Job {
  PipelineKey key{};
  uint64_t hash = 0;
  const HostVertexLayout *layout = nullptr;  // g_built_in_layouts
  enum class Stage { Pending, Building, Done } stage = Stage::Pending;
  std::unique_ptr<plume::RenderPipeline> pipeline;  // Done: nullptr if taken or failed
  // Its shaders the game has not made yet (PipelineShaderLoaded); queued at 0.
  u32 shaders_missing = 0;
};

// The built-in list's layouts, made at the start.
std::vector<HostVertexLayout> g_built_in_layouts;

// The built-in list's pipelines, but those of shaders this build lacks.
std::vector<std::unique_ptr<Job>> BuiltInJobs() {
  const size_t layouts = std::size(kBuiltInLayouts) - 1;
  g_built_in_layouts.resize(layouts);
  for (size_t i = 0; i < layouts; ++i) {
    const BuiltInLayout &in = kBuiltInLayouts[i];
    HostVertexLayout &layout = g_built_in_layouts[i];
    for (u32 s = 0; s < kMaxVertexStreams; ++s) {
      layout.streams[s].used = (in.used_streams >> s) & 1;
      layout.streams[s].host_stride = in.strides[s];
    }
    for (u32 e = 0; e < in.element_count; ++e) {
      const BuiltInElement &element = kBuiltInElements[in.first_element + e];
      layout.input_elements.emplace_back(element.name, element.semantic_index, element.location,
                                         plume::RenderFormat(element.format), element.slot,
                                         element.offset);
    }
  }
  std::vector<std::unique_ptr<Job>> jobs;
  std::unordered_set<uint64_t> seen;
  for (size_t i = 0; i + 1 < std::size(kBuiltInPipelines); ++i) {
    const BuiltInPipeline &p = kBuiltInPipelines[i];
    const ShaderCacheEntry *vs = FindShaderEntry(p.vertex_shader);
    const ShaderCacheEntry *ps = p.pixel_shader ? FindShaderEntry(p.pixel_shader) : nullptr;
    if (!vs || (p.pixel_shader && !ps) || p.layout >= layouts)
      continue;
    auto job = std::make_unique<Job>();
    PipelineKey &key = job->key;
    std::memset(&key, 0, sizeof(key));
    key.vertex_shader = vs;
    key.pixel_shader = ps;
    key.layout_hash = kBuiltInLayouts[p.layout].hash;
    key.topology = plume::RenderPrimitiveTopology(p.topology);
    for (u32 t = 0; t < d3d::kMaxRenderTargets; ++t) {
      key.color_formats[t] = plume::RenderFormat(p.color_formats[t]);
      key.blend_control[t] = p.blend_control[t];
    }
    key.depth_format = plume::RenderFormat(p.depth_format);
    key.color_count = p.color_count;
    key.samples = p.samples;
    key.color_mask = p.color_mask;
    key.depth_control = p.depth_control;
    key.stencil_ref_mask = p.stencil_ref_mask;
    key.stencil_ref_mask_bf = p.stencil_ref_mask_bf;
    key.mode_control = p.mode_control;
    key.depth_bias = p.depth_bias;
    key.depth_bias_clamp = p.depth_bias_clamp;
    key.alpha_to_coverage = p.alpha_to_coverage;
    key.per_instance_streams = p.per_instance_streams;
    key.rectangle_list = p.rectangle_list;
    key.point_sprite = p.point_sprite;
    job->layout = &g_built_in_layouts[p.layout];
    job->hash = HashKey(key);
    if (seen.insert(job->hash).second)
      jobs.push_back(std::move(job));
  }
  return jobs;
}

// ---------------------------------------------------------------------------
// Building the listed pipelines ahead
// ---------------------------------------------------------------------------
//
// A listed pipeline is built once the game has made both of its shaders (a
// level's shaders come with its load), not at the start: the list holds every
// level's, and the driver keeps each built pipeline's shaders and code in host
// memory. What is built stays built.

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
    inputs.layout = job.layout;
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
      g_capture.Add(key, *inputs.layout);
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
  std::vector<std::unique_ptr<Job>> jobs = BuiltInJobs();
  RGPU_INFO("Pipelines: {} of the built-in list's {} are built ahead as the game makes their "
            "shaders", jobs.size(), std::size(kBuiltInPipelines) - 1);
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
