#pragma once

#include <cstdint>

#include <plume_render_interface.h>

#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/backend/video.h"

struct ShaderCacheEntry;

namespace renut::gpu {

constexpr uint32_t kDrawRootVertexConstants = 0;
constexpr uint32_t kDrawRootPixelConstants = 1;
constexpr uint32_t kDrawRootSharedConstants = 2;
constexpr uint32_t kDrawRootVertexFetchData = 3;
constexpr uint32_t kDrawRootMemExport = 4;  // u0 space6, the memexport buffer

struct DrawPushConstants {
  uint64_t vertex_constants;
  uint64_t pixel_constants;
  uint64_t shared_constants;
};

bool BuildDrawLayout(VideoState &s);

// Everything a pipeline is built from. Compared and hashed as raw bytes, so
// it is always zero-initialised before filling.
struct PipelineKey {
  const void *vertex_shader;  // ShaderCacheEntry (static, unlike the host shader objects)
  const void *pixel_shader;   // ShaderCacheEntry, nullptr without a pixel shader
  uint64_t layout_hash;       // the declaration's host input layout
  plume::RenderPrimitiveTopology topology;
  plume::RenderFormat color_formats[d3d::kMaxRenderTargets];
  plume::RenderFormat depth_format;
  uint32_t color_count;
  uint32_t samples;
  uint32_t blend_control[d3d::kMaxRenderTargets];
  uint32_t color_mask;
  uint32_t depth_control;
  uint32_t stencil_ref_mask;
  uint32_t stencil_ref_mask_bf;
  uint32_t mode_control;
  int32_t depth_bias;             // host DepthBias and DepthBiasClamp for the
  float depth_bias_clamp;         // guest's polygon offset (MakeKey)
  uint32_t alpha_to_coverage;
  uint32_t per_instance_streams;  // tessellated draws: vertex streams are per instance
  uint32_t rectangle_list;        // triangle list expanded by rectangle_list_gs
  uint32_t point_sprite;          // point list expanded by point_sprite_gs
};

struct HostVertexLayout;

// The live objects a key was made from, used only when the pipeline is built.
struct PipelineInputs {
  plume::RenderShader *vertex_shader = nullptr;
  plume::RenderShader *pixel_shader = nullptr;
  const HostVertexLayout *layout = nullptr;  // the declaration's (key.layout_hash)
};

// Built pipeline for the key, compiling it on first use unless the built-in
// pipeline list (below) built it ahead. nullptr on failure (kept for the key).
plume::RenderPipeline *GetPipelineLocked(const PipelineKey &key, const PipelineInputs &inputs);

// Start/stop the workers that build the built-in list's pipelines
// (pipeline_cache_data.inc) ahead, each once the game has made its shaders.
void StartPipelinePrebuild();
void StopPipelinePrebuild();

// The game made a shader object of this cache entry (any thread, any time).
void PipelineShaderLoaded(const ShaderCacheEntry *entry);

// Normalizes state a pipeline never uses (unbound targets, stencil or depth test
// off), so draws differing only there share one pipeline.
void SanitizePipelineKey(PipelineKey &key);

} // namespace renut::gpu
