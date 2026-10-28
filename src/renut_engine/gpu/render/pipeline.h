/**
 * @file    gpu/render/pipeline.h
 * @brief   The pipeline layout of the recompiled guest shaders and the
 *          pipeline cache.
 *
 * Binding contract (shader_common.h, RENUT_RECOMP):
 *   set 0/1/2  bindless Texture2D / Texture3D / TextureCube (one shared table)
 *   set 3      bindless samplers
 *   set 4      the texture table again, as Texture2DArray (stacked textures)
 *   D3D12      b0/b1/b2 space4 root CBVs: VS constants, PS constants,
 *              SharedConstants; t0 space5 root SRV: this draw's vertex fetch
 *              data (ByteAddressBuffer)
 *   Vulkan     push constants: the three constant buffers' device addresses;
 *              vertex fetch data is read through its address in
 *              SharedConstants
 */
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
// pipeline list (below) built it ahead. nullptr on failure (logged once per
// key).
plume::RenderPipeline *GetPipelineLocked(const PipelineKey &key, const PipelineInputs &inputs);

// The built-in pipeline list (pipeline_cache_data.inc, from
// tools/pipeline_cache.py): worker threads build the listed pipelines before
// the game draws with them, so a scene costs no compile on the render thread.
// A listed pipeline is built once the game has made its shaders
// (PipelineShaderLoaded), so only what the levels played this session use is
// built. The pipelines draws build themselves can be captured for the list
// (renut_pipeline_capture). Start once the draw layout and host shaders exist;
// Stop at shutdown.
void StartPipelinePrebuild();
void StopPipelinePrebuild();

// The game made a shader object of this cache entry (any thread, any time).
void PipelineShaderLoaded(const ShaderCacheEntry *entry);

// For the memory report: pipelines the draws use, pipelines built ahead from
// the list that no draw has asked for yet (all of them stay built), listed
// pipelines whose shaders the game has not made, and the listed pipelines of
// shaders this build has.
struct PipelineCounts {
  size_t built = 0;
  size_t prebuilt_unused = 0;
  size_t waiting = 0;
  size_t listed = 0;
};
PipelineCounts PipelineCountsLocked();

} // namespace renut::gpu
