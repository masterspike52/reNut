/**
 * @file    gpu/backend/video.cpp
 * @brief   Device creation, bindless tables, resource retirement and command
 *          list management for the native renderer.
 */
#include "renut_engine/gpu/backend/video.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <share.h>
#endif

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>

#include "renut_engine/perf/frame_stats.h"
#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/command_recorder.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/native_window.h"
#include "renut_engine/gpu/render/pipeline.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/backend/video_internal.h"

#include "host_shaders/depth_resolve_ms_ps.spirv.h"
#include "host_shaders/depth_resolve_ps.spirv.h"
#include "host_shaders/present_ps.spirv.h"
#include "host_shaders/present_vs.spirv.h"
#include "host_shaders/rectangle_list_gs.spirv.h"
#include "host_shaders/point_sprite_gs.spirv.h"
#include "host_shaders/stream_convert_cs.spirv.h"
#include "host_shaders/target_readback_cs.spirv.h"
#include "host_shaders/texture_mip_ps.spirv.h"
#if defined(_WIN32)
#include "host_shaders/depth_resolve_ms_ps.dxil.h"
#include "host_shaders/depth_resolve_ps.dxil.h"
#include "host_shaders/present_ps.dxil.h"
#include "host_shaders/present_vs.dxil.h"
#include "host_shaders/rectangle_list_gs.dxil.h"
#include "host_shaders/point_sprite_gs.dxil.h"
#include "host_shaders/stream_convert_cs.dxil.h"
#include "host_shaders/target_readback_cs.dxil.h"
#include "host_shaders/texture_mip_ps.dxil.h"
#endif
#if defined(__ANDROID__)
#include <SDL3/SDL_events.h>
#include "renut_engine/app/android_driver.h"
#endif

namespace renut::gpu {

VideoState &State() {
  static VideoState s;
  return s;
}

namespace {

#if defined(__ANDROID__)
// The format every Android surface has.
constexpr plume::RenderFormat kSwapChainFormat = plume::RenderFormat::R8G8B8A8_UNORM;
#else
constexpr plume::RenderFormat kSwapChainFormat = plume::RenderFormat::B8G8R8A8_UNORM;
#endif

bool BuildUtilityLayout(VideoState &s) {
  plume::RenderDescriptorSetBuilder textures;
  textures.begin();
  textures.addTexture(0, kTextureDescriptorCount);
  textures.end(true, kTextureDescriptorCount);

  plume::RenderDescriptorSetBuilder samplers;
  samplers.begin();
  samplers.addSampler(0, kSamplerDescriptorCount);
  samplers.end(true, kSamplerDescriptorCount);

  // The two bindless tables are created once and shared by every layout that
  // includes them (plume's D3D12 backend draws sets from one global heap).
  s.texture_set = textures.create(s.device.get());
  s.sampler_set = samplers.create(s.device.get());
  if (!s.texture_set || !s.sampler_set) {
    RGPU_ERROR("Bindless descriptor set creation failed");
    return false;
  }
  s.texture_slot_used.assign(kTextureDescriptorCount, false);
  s.sampler_slot_used.assign(kSamplerDescriptorCount, false);
  s.texture_slot_used[kNullTextureSlot] = true;
  s.sampler_slot_used[kLinearClampSamplerSlot] = true;
  s.sampler_slot_used[kPointClampSamplerSlot] = true;

  plume::RenderPipelineLayoutBuilder layout;
  layout.begin(false, true);
  layout.addDescriptorSet(textures);  // set 0 -> t0, space0
  layout.addDescriptorSet(samplers);  // set 1 -> s0, space1
  layout.addPushConstant(0, 2, sizeof(UtilityPush),
                         plume::RenderShaderStageFlag::VERTEX |
                             plume::RenderShaderStageFlag::PIXEL);  // b0, space2
  layout.end();
  s.utility_layout = layout.create(s.device.get());
  if (!s.utility_layout) {
    RGPU_ERROR("Utility pipeline layout creation failed");
    return false;
  }
  return true;
}

bool BuildSamplers(VideoState &s) {
  plume::RenderSamplerDesc linear;
  linear.minFilter = plume::RenderFilter::LINEAR;
  linear.magFilter = plume::RenderFilter::LINEAR;
  linear.mipmapMode = plume::RenderMipmapMode::LINEAR;
  linear.addressU = linear.addressV = linear.addressW = plume::RenderTextureAddressMode::CLAMP;
  s.linear_clamp_sampler = s.device->createSampler(linear);

  plume::RenderSamplerDesc point = linear;
  point.minFilter = plume::RenderFilter::NEAREST;
  point.magFilter = plume::RenderFilter::NEAREST;
  point.mipmapMode = plume::RenderMipmapMode::NEAREST;
  s.point_clamp_sampler = s.device->createSampler(point);

  if (!s.linear_clamp_sampler || !s.point_clamp_sampler) {
    RGPU_ERROR("Sampler creation failed");
    return false;
  }
  s.sampler_set->setSampler(kLinearClampSamplerSlot, s.linear_clamp_sampler.get());
  s.sampler_set->setSampler(kPointClampSamplerSlot, s.point_clamp_sampler.get());
  return true;
}

bool BuildNullTexture(VideoState &s) {
  constexpr auto kFormat = plume::RenderFormat::R8G8B8A8_UNORM;
  s.null_texture = s.device->createTexture(plume::RenderTextureDesc::Texture2D(1, 1, 1, kFormat));
  s.null_texture_3d =
      s.device->createTexture(plume::RenderTextureDesc::Texture3D(1, 1, 1, 1, kFormat));
  s.null_texture_cube = s.device->createTexture(plume::RenderTextureDesc::Texture(
      plume::RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1, 6, kFormat,
      plume::RenderTextureFlag::CUBE));
  if (!s.null_texture || !s.null_texture_3d || !s.null_texture_cube) {
    RGPU_ERROR("Null texture creation failed");
    return false;
  }
  s.null_texture->setName("renut null texture");
  s.null_texture_3d->setName("renut null texture 3d");
  s.null_texture_cube->setName("renut null texture cube");
  s.null_texture_view =
      s.null_texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(kFormat));
  s.null_texture_3d_view =
      s.null_texture_3d->createTextureView(plume::RenderTextureViewDesc::Texture3D(kFormat));
  s.null_texture_cube_view =
      s.null_texture_cube->createTextureView(plume::RenderTextureViewDesc::TextureCube(kFormat));
  if (!s.null_texture_view || !s.null_texture_3d_view || !s.null_texture_cube_view)
    return false;
  // Every slot starts pointing at the 2D one, so a stale index still reads a
  // live (zero) descriptor. Contents are uploaded on the first command list.
  for (uint32_t i = 0; i < kTextureDescriptorCount; ++i) {
    s.texture_set->setTexture(i, s.null_texture.get(), plume::RenderTextureLayout::SHADER_READ,
                              s.null_texture_view.get());
  }
  s.texture_set->setTexture(kNullTexture3DSlot, s.null_texture_3d.get(),
                            plume::RenderTextureLayout::SHADER_READ, s.null_texture_3d_view.get());
  s.texture_set->setTexture(kNullTextureCubeSlot, s.null_texture_cube.get(),
                            plume::RenderTextureLayout::SHADER_READ,
                            s.null_texture_cube_view.get());
  s.texture_slot_used[kNullTexture3DSlot] = true;
  s.texture_slot_used[kNullTextureCubeSlot] = true;
  return true;
}

// Zero-fills the null textures (every subresource is a single texel).
bool InitializeNullTextures(VideoState &s) {
  UploadAllocation zero = s.uploads[s.frame].Allocate(256, 512);
  if (!zero)
    return false;
  std::memset(zero.cpu, 0, 256);
  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderTexture *textures[] = {s.null_texture.get(), s.null_texture_3d.get(),
                                      s.null_texture_cube.get()};
  const uint32_t slices[] = {1, 1, 6};
  for (uint32_t t = 0; t < 3; ++t) {
    cmd->barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderTextureBarrier(textures[t], plume::RenderTextureLayout::COPY_DEST));
    for (uint32_t slice = 0; slice < slices[t]; ++slice) {
      cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(textures[t], 0, slice),
                             plume::RenderTextureCopyLocation::PlacedFootprint(
                                 zero.ref.ref, plume::RenderFormat::R8G8B8A8_UNORM, 1, 1, 1, 64,
                                 zero.ref.offset));
    }
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderTextureBarrier(textures[t], plume::RenderTextureLayout::SHADER_READ));
  }
  return true;
}

bool BuildPresentPipeline(VideoState &s) {
  s.present_vs = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(present_vs), "present_vs");
  s.present_ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(present_ps), "present_ps");
  if (!s.present_vs || !s.present_ps)
    return false;

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.utility_layout.get();
  desc.vertexShader = s.present_vs.get();
  desc.pixelShader = s.present_ps.get();
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = kSwapChainFormat;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  s.present_pipeline = s.device->createGraphicsPipeline(desc);
  if (!s.present_pipeline) {
    RGPU_ERROR("Present pipeline creation failed");
    return false;
  }
  s.present_pipeline->setName("renut present");
  return true;
}

// See depth_resolve.hlsli; present_vs draws the fullscreen triangle.
bool BuildDepthResolvePipelines(VideoState &s) {
  s.depth_resolve_ps[0] =
      CreateHostShader(s.device.get(), RENUT_HOST_SHADER(depth_resolve_ps), "depth_resolve_ps");
  s.depth_resolve_ps[1] = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(depth_resolve_ms_ps),
                                           "depth_resolve_ms_ps");
  for (uint32_t i = 0; i < 2; ++i) {
    if (!s.depth_resolve_ps[i])
      return false;
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.utility_layout.get();
    desc.vertexShader = s.present_vs.get();
    desc.pixelShader = s.depth_resolve_ps[i].get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 2;
    desc.renderTargetFormat[0] = kDepthResolveFormat;
    desc.renderTargetFormat[1] = kDepthResolveWordsFormat;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.renderTargetBlend[1] = plume::RenderBlendDesc::Copy();
    desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
    s.depth_resolve_pipeline[i] = s.device->createGraphicsPipeline(desc);
    if (!s.depth_resolve_pipeline[i]) {
      RGPU_ERROR("Depth resolve pipeline creation failed");
      return false;
    }
    s.depth_resolve_pipeline[i]->setName(i ? "renut depth resolve (msaa)" : "renut depth resolve");
  }
  return true;
}

// Optional: without geometry shaders rectangle lists stay triangle lists and
// point sprites single pixels.
void BuildRectangleListShader(VideoState &s) {
  if (!s.device->getCapabilities().geometryShader) {
    RGPU_WARN("No geometry shader support; rectangle lists are drawn as triangle lists and "
              "point sprites as points");
    return;
  }
  s.rectangle_list_gs =
      CreateHostShader(s.device.get(), RENUT_HOST_SHADER(rectangle_list_gs), "rectangle_list_gs");
  s.point_sprite_gs =
      CreateHostShader(s.device.get(), RENUT_HOST_SHADER(point_sprite_gs), "point_sprite_gs");
}

// See shaders/host/stream_convert_cs.hlsl.
plume::RenderDescriptorSetBuilder StreamConvertSetBuilder() {
  plume::RenderDescriptorSetBuilder buffers;
  buffers.begin();
  buffers.addReadWriteByteAddressBuffer(0, kStreamConvertBuffers);  // u0, space0
  buffers.end();
  return buffers;
}

// Optional: without it, draws that read memexport results as vertices wait
// for them on the CPU.
void BuildStreamConvertPipeline(VideoState &s) {
  plume::RenderPipelineLayoutBuilder layout;
  layout.begin(false, false);
  layout.addDescriptorSet(StreamConvertSetBuilder());
  layout.addPushConstant(0, 1, sizeof(StreamConvertPush), plume::RenderShaderStageFlag::COMPUTE);
  layout.end();
  s.stream_convert_layout = layout.create(s.device.get());
  s.stream_convert_cs =
      CreateHostShader(s.device.get(), RENUT_HOST_SHADER(stream_convert_cs), "stream_convert_cs");
  if (s.stream_convert_layout && s.stream_convert_cs) {
    s.stream_convert_pipeline = s.device->createComputePipeline(plume::RenderComputePipelineDesc(
        s.stream_convert_layout.get(), s.stream_convert_cs.get(), 64, 1, 1));
  }
  if (!s.stream_convert_pipeline) {
    RGPU_WARN("Stream conversion pipeline creation failed; draws reading memexport results as "
              "vertices wait for them");
    return;
  }
  s.stream_convert_pipeline->setName("renut stream convert");
}

// See shaders/host/target_readback_cs.hlsl.
plume::RenderDescriptorSetBuilder TargetReadbackSetBuilder() {
  plume::RenderDescriptorSetBuilder buffers;
  buffers.begin();
  buffers.addReadWriteByteAddressBuffer(0, 1);  // u0, space1
  buffers.end();
  return buffers;
}

// Optional: without it, render targets the game reads back on the CPU (photo
// mode's photos, guest/photo_capture.cpp) are not read back.
void BuildTargetReadbackPipeline(VideoState &s) {
  plume::RenderDescriptorSetBuilder textures;  // the bindless table, as BuildUtilityLayout's
  textures.begin();
  textures.addTexture(0, kTextureDescriptorCount);
  textures.end(true, kTextureDescriptorCount);

  plume::RenderPipelineLayoutBuilder layout;
  layout.begin(false, false);
  layout.addDescriptorSet(textures);                    // set 0 -> t0, space0
  layout.addDescriptorSet(TargetReadbackSetBuilder());  // set 1 -> u0, space1
  layout.addPushConstant(0, 2, sizeof(TargetReadbackPush),
                         plume::RenderShaderStageFlag::COMPUTE);  // b0, space2
  layout.end();
  s.target_readback_layout = layout.create(s.device.get());
  s.target_readback_cs = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(target_readback_cs),
                                          "target_readback_cs");
  if (s.target_readback_layout && s.target_readback_cs) {
    s.target_readback_pipeline = s.device->createComputePipeline(plume::RenderComputePipelineDesc(
        s.target_readback_layout.get(), s.target_readback_cs.get(), 8, 8, 1));
  }
  if (!s.target_readback_pipeline) {
    RGPU_WARN("Render target readback pipeline creation failed; photos stay black");
    return;
  }
  s.target_readback_pipeline->setName("renut target readback");
}

} // namespace

namespace detail {

std::unique_ptr<plume::RenderDescriptorSet> CreateStreamConvertSet(VideoState &s) {
  return StreamConvertSetBuilder().create(s.device.get());
}

std::unique_ptr<plume::RenderDescriptorSet> CreateTargetReadbackSet(VideoState &s) {
  return TargetReadbackSetBuilder().create(s.device.get());
}

} // namespace detail

namespace detail {

bool CreateSwapChain(VideoState &s, plume::RenderWindow window) {
  // One image beyond the frames in flight, so acquiring never waits on scanout.
  plume::RenderSwapChainDesc swap_desc(window, kSwapChainFormat, kFramesInFlight + 1, false,
                                       kFramesInFlight);
  s.swap_chain = s.queue->createSwapChain(swap_desc);
  // plume's Vulkan swapchain is created lazily by resize().
  if (s.swap_chain && IsVulkan())
    s.swap_chain->resize();
  if (!s.swap_chain || s.swap_chain->isEmpty()) {
    RGPU_ERROR("Swapchain creation failed");
    s.swap_chain.reset();
    return false;
  }
  return BuildSwapChainTargets(s);
}

bool BuildSwapChainTargets(VideoState &s) {
  s.swap_framebuffers.clear();
  s.present_semaphores.clear();
  const uint32_t count = s.swap_chain->getTextureCount();
  for (uint32_t i = 0; i < count; ++i) {
    const plume::RenderTexture *texture = s.swap_chain->getTexture(i);
    plume::RenderFramebufferDesc desc(&texture, 1);
    auto framebuffer = s.device->createFramebuffer(desc);
    if (!framebuffer) {
      RGPU_ERROR("Swapchain framebuffer {} creation failed", i);
      s.swap_framebuffers.clear();
      return false;
    }
    s.swap_framebuffers.push_back(std::move(framebuffer));
    s.present_semaphores.push_back(s.device->createCommandSemaphore());
  }
  return count != 0;
}

void DrainSlot(VideoState &s, uint32_t slot) {
  s.retired_framebuffers[slot].clear();
  s.retired_views[slot].clear();
  s.retired_textures[slot].clear();
  s.retired_buffers[slot].clear();
  s.retired_sets[slot].clear();
  ReleaseCacheCopiesLocked(slot);
  s.exported_streams_used[slot] = 0;
  s.gpu_fetch_data_used[slot] = 0;
  s.memexport_scratch_used[slot] = 0;
  for (uint32_t index : s.retired_texture_slots[slot]) {
    if (index >= kFirstDynamicTextureSlot && index < kTextureDescriptorCount) {
      s.texture_set->setTexture(index, s.null_texture.get(), plume::RenderTextureLayout::SHADER_READ,
                                s.null_texture_view.get());
      if (s.texture_slot_used[index] && s.texture_slots_in_use)
        --s.texture_slots_in_use;
      s.texture_slot_used[index] = false;
    }
  }
  s.retired_texture_slots[slot].clear();
  s.uploads[slot].Reset();
}

namespace {

// After the slot's fence has been waited: its frame's GPU time, the sum of its
// lists' spans (so the gaps where the GPU waited on the CPU are left out).
void CollectGpuTime(VideoState &s, uint32_t slot) {
  const uint32_t lists = s.timed_lists[slot];
  s.timed_lists[slot] = 0;
  plume::RenderQueryPool *pool = s.timestamps[slot].get();
  if (!pool || !lists)
    return;
  pool->queryResults();
  const uint64_t *ns = pool->getResults();
  uint64_t total = 0;
  for (uint32_t i = 0; i < lists; ++i) {
    if (ns[2 * i + 1] > ns[2 * i])
      total += ns[2 * i + 1] - ns[2 * i];
  }
  frame_stats::OnGpuFrame(double(total) / 1e6);
}

} // namespace

void BeginListTiming(VideoState &s) {
  gpu_profile::BeginList(s);
  s.timing_open = false;
  plume::RenderQueryPool *pool = s.timestamps[s.frame].get();
  uint32_t &lists = s.timed_lists[s.frame];
  if (!pool || lists >= VideoState::kTimedLists)
    return;
  // Vulkan queries are reset before they are written again, outside any
  // render pass: at the frame's first list, all of them.
  if (lists == 0)
    s.command_list->resetQueryPool(pool, 0, 2 * VideoState::kTimedLists);
  s.command_list->writeTimestamp(pool, 2 * lists);
  s.timing_open = true;
}

void EndListTiming(VideoState &s, bool last_in_frame) {
  gpu_profile::EndList(s, last_in_frame);
  plume::RenderQueryPool *pool = s.timestamps[s.frame].get();
  if (!pool || !s.timing_open)
    return;
  uint32_t &lists = s.timed_lists[s.frame];
  s.command_list->writeTimestamp(pool, 2 * lists + 1);
  ++lists;
  s.timing_open = false;
  // Vulkan returns no results while any query of the pool is unwritten.
  if (last_in_frame && IsVulkan()) {
    for (uint32_t i = 2 * lists; i < 2 * VideoState::kTimedLists; ++i)
      s.command_list->writeTimestamp(pool, i);
  }
}

void WaitSlotLocked(VideoState &s, uint32_t slot) {
  if (!s.submitted[slot])
    return;
  {
    render_thread::WaitScope wait;
    // Submitted by the recording thread: once it has, the fence means it.
    CommandRecorder::WaitForCall(s.submit_calls[slot]);
    s.queue->waitForCommandFence(s.fences[slot].get());
  }
  s.submit_calls[slot] = 0;
  s.submitted[slot] = false;
  CollectGpuTime(s, slot);
  gpu_profile::Collect(s, slot);
}

void WaitAllSubmitted(VideoState &s) {
  // Oldest slot first, so its memexport results land before newer ones.
  for (uint32_t k = 1; k <= kFramesInFlight; ++k) {
    const uint32_t i = (s.frame + k) % kFramesInFlight;
    if (s.submitted[i]) {
      WaitSlotLocked(s, i);
      CompleteMemExportsLocked(i);
      if (i != s.frame)
        DrainSlot(s, i);
    }
  }
}

// Ends the slot's list: with a recorder, once the recording thread has made
// all of its calls.
void EndCommandList(VideoState &s, uint32_t slot) {
  if (CommandRecorder *recorder = s.recorders[slot]) {
    recorder->end();
    render_thread::WaitScope wait;
    recorder->Finish();
  } else {
    s.command_lists[slot]->end();
  }
}

void SubmitAndWaitLocked(VideoState &s) {
  if (!s.list_open)
    return;
  const uint32_t cur = s.frame;
  FinishRecordingLocked(s);
  EndListTiming(s, false);
  EndCommandList(s, cur);
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, s.fences[cur].get());
  {
    render_thread::WaitScope wait;
    s.queue->waitForCommandFence(s.fences[cur].get());
  }
  s.submitted[cur] = false;
  // The slot's upload ring and retired objects stay: the rest of the frame
  // records into the same slot.
  s.command_list->begin();
  ++s.list_generation;
  BeginListTiming(s);
}

void SubmitAndAdvance(VideoState &s, plume::RenderCommandSemaphore *wait,
                      plume::RenderCommandSemaphore *signal) {
  const uint32_t cur = s.frame;
  if (s.list_open) {
    FinishRecordingLocked(s);
    EndListTiming(s, true);
    s.list_open = false;
    s.command_list = nullptr;
    const auto submit = [queue = s.queue.get(), list = s.command_lists[cur].get(),
                         fence = s.fences[cur].get(), wait, signal] {
      const plume::RenderCommandList *lists[] = {list};
      plume::RenderCommandSemaphore *waits[] = {wait};
      plume::RenderCommandSemaphore *signals[] = {signal};
      queue->executeCommandLists(lists, 1, wait ? waits : nullptr, wait ? 1 : 0,
                                 signal ? signals : nullptr, signal ? 1 : 0, fence);
    };
    if (CommandRecorder *recorder = s.recorders[cur]) {
      // The recording thread ends and submits the list after the rest of its
      // calls; the render thread goes on to the next frame meanwhile.
      recorder->end();
      s.submit_calls[cur] = recorder->RecordCall(submit);
    } else {
      s.command_lists[cur]->end();
      submit();
    }
    s.submitted[cur] = true;
  }

  // Record into the other slot next; the GPU keeps working on this one.
  s.frame = (cur + 1) % kFramesInFlight;
  WaitSlotLocked(s, s.frame);
  CompleteMemExportsLocked(s.frame);
  s.timed_lists[s.frame] = 0;
  DrainSlot(s, s.frame);
}

} // namespace detail

#if defined(__ANDROID__)
namespace {
// SDL sends these from the activity's thread, before the surface goes and
// after the app is back.
bool SDLCALL WatchAppLifecycle(void *, SDL_Event *event) {
  if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND)
    Video::SetSuspended(true);
  else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND)
    Video::SetSuspended(false);
  return true;
}
} // namespace
#endif

bool Video::CreateHostDevice() {
  auto &s = State();
  std::lock_guard lock(s.mutex);
  if (s.host_ready)
    return true;
#if defined(__ANDROID__)
  static const bool watching = SDL_AddEventWatch(WatchAppLifecycle, nullptr);
  (void)watching;
#endif

  plume::RenderWindow render_window{};
  if (!GetNativeRenderWindow(render_window))
    return false;

#if defined(_WIN32)
  // plume reports its failures (vkQueueSubmit, swapchain creation, ...) on
  // stderr, which a Windows GUI process without a console discards.
  // Opened shared (freopen_s denies other readers), so it can be read while
  // the game runs.
  if (_fileno(stderr) < 0) {
    std::error_code ec;
    std::filesystem::create_directories("logs", ec);
    FILE *file = nullptr;
    if (freopen_s(&file, "NUL", "w", stderr) == 0) {
      if (FILE *log = _fsopen("logs/plume_stderr.log", "w", _SH_DENYNO)) {
        _dup2(_fileno(log), _fileno(stderr));
        setvbuf(stderr, nullptr, _IONBF, 0);
        RGPU_INFO("plume's error messages go to logs/plume_stderr.log");
      }
    }
  }
#endif

  s.render_interface = CreateRenderInterface(render_window);
  if (!s.render_interface) {
    RGPU_ERROR("{} render interface creation failed", BackendName(ActiveBackend()));
    return false;
  }
  s.device = s.render_interface->createDevice();
  if (!s.device) {
    RGPU_ERROR("{} device creation failed", BackendName(ActiveBackend()));
#if defined(__ANDROID__)
    // The custom driver has no usable device: start over on the system one.
    if (!renut::android::LoadedDriver().empty()) {
      renut::android::SelectDriver({});
      renut::android::RestartApp();
    }
#endif
    return false;
  }
#if defined(__ANDROID__)
  renut::android::MarkDriverWorking();
#endif
  s.backend_description =
      std::string(BackendName(ActiveBackend())) + " on " + s.device->getDescription().name;
  RGPU_INFO("Renderer: {}", s.backend_description);

  s.queue = s.device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  if (!s.queue) {
    RGPU_ERROR("Command queue creation failed");
    return false;
  }
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    s.command_lists[i] = s.queue->createCommandList();
    s.fences[i] = s.device->createCommandFence();
    s.acquire_semaphores[i] = s.device->createCommandSemaphore();
    if (!s.command_lists[i] || !s.fences[i] || !s.acquire_semaphores[i]) {
      RGPU_ERROR("Frame {} command objects creation failed", i);
      return false;
    }
    if (!s.uploads[i].Init(s.device.get(), i == 0 ? "upload ring 0" : "upload ring 1"))
      return false;
    if (s.device->getCapabilities().queryPools)
      s.timestamps[i] = s.device->createQueryPool(2 * VideoState::kTimedLists);
    if (ThreadedRecordingEnabled())
      s.recorders[i] = new CommandRecorder(s.command_lists[i].get());
  }

  if (!detail::CreateSwapChain(s, render_window))
    return false;

  if (!BuildUtilityLayout(s) || !BuildSamplers(s) || !BuildNullTexture(s) ||
      !BuildPresentPipeline(s) || !BuildDepthResolvePipelines(s) || !BuildDrawLayout(s)) {
    return false;
  }
  BuildRectangleListShader(s);
  BuildStreamConvertPipeline(s);
  BuildTargetReadbackPipeline(s);
  // Optional: without it, scaled resolve destinations keep one level.
  s.texture_mip_ps =
      CreateHostShader(s.device.get(), RENUT_HOST_SHADER(texture_mip_ps), "texture_mip_ps");
  StartPipelinePrebuild();

  s.host_ready = true;
  return true;
}

bool Video::HostReady() {
  auto &s = State();
  std::lock_guard lock(s.mutex);
  return s.host_ready;
}

void Video::BeginShutdown() {
  auto &s = State();
  s.shutting_down.store(true, std::memory_order_release);
  StopPipelinePrebuild();
  render_thread::Stop();
  std::lock_guard lock(s.mutex);
  // The recording thread is done with the lists before they go at exit.
  for (CommandRecorder *recorder : s.recorders) {
    if (recorder)
      recorder->Finish();
  }
  if (s.queue)
    detail::WaitAllSubmitted(s);
}

plume::RenderDevice *Video::Device() {
  return State().device.get();
}

void Video::SetGuestDevice(uint32_t device_va) {
  auto &s = State();
  std::lock_guard lock(s.mutex);
  s.guest_device.store(device_va, std::memory_order_release);
  s.guest_ready = device_va != 0;
}

uint32_t Video::GuestDevice() {
  return State().guest_device.load(std::memory_order_acquire);
}

void Video::RequestResize() {
  State().resize_requested.store(true, std::memory_order_release);
}

void Video::SetOverlayHook(OverlayHook hook) {
  auto &s = State();
  std::lock_guard lock(s.mutex);
  s.overlay_hook = std::move(hook);
}

bool Video::EnsureCommandListLocked() {
  auto &s = State();
  if (s.list_open)
    return true;
  if (!s.host_ready || s.shutting_down.load(std::memory_order_acquire))
    return false;

  s.command_list = s.recorders[s.frame] ? static_cast<plume::RenderCommandList *>(s.recorders[s.frame])
                                        : s.command_lists[s.frame].get();
  s.command_list->begin();
  s.list_open = true;
  ++s.list_generation;
  detail::BeginListTiming(s);

  if (!s.null_texture_initialized)
    s.null_texture_initialized = InitializeNullTextures(s);
  return true;
}

plume::RenderCommandList *Video::CommandListLocked() {
  return State().command_list;
}

UploadRing &Video::UploadsLocked() {
  auto &s = State();
  return s.uploads[s.frame];
}

uint32_t Video::AllocateTextureSlotLocked() {
  auto &s = State();
  for (uint32_t i = kFirstDynamicTextureSlot; i < kTextureDescriptorCount; ++i) {
    if (!s.texture_slot_used[i]) {
      s.texture_slot_used[i] = true;
      ++s.texture_slots_in_use;
      return i;
    }
  }
  RGPU_ERROR("Bindless texture table full ({} slots)", kTextureDescriptorCount);
  return kInvalidDescriptor;
}

void Video::SetTextureSlotLocked(uint32_t slot, plume::RenderTexture *texture,
                                 plume::RenderTextureView *view) {
  if (slot >= kTextureDescriptorCount)
    return;
  State().texture_set->setTexture(slot, texture, plume::RenderTextureLayout::SHADER_READ, view);
}

uint32_t Video::TextureSlotsInUseLocked() {
  return State().texture_slots_in_use;
}

void Video::FreeTextureSlotLocked(uint32_t slot) {
  auto &s = State();
  if (slot >= kFirstDynamicTextureSlot && slot < kTextureDescriptorCount)
    s.retired_texture_slots[s.frame].push_back(slot);
}

void Video::RetireLocked(std::unique_ptr<plume::RenderTexture> texture) {
  if (texture) {
    auto &s = State();
    s.retired_textures[s.frame].push_back(std::move(texture));
  }
}

void Video::RetireLocked(std::unique_ptr<plume::RenderTextureView> view) {
  if (view) {
    auto &s = State();
    s.retired_views[s.frame].push_back(std::move(view));
  }
}

void Video::RetireLocked(std::unique_ptr<plume::RenderBuffer> buffer) {
  if (buffer) {
    auto &s = State();
    s.retired_buffers[s.frame].push_back(std::move(buffer));
  }
}

void Video::RetireLocked(std::unique_ptr<plume::RenderFramebuffer> framebuffer) {
  if (framebuffer) {
    auto &s = State();
    s.retired_framebuffers[s.frame].push_back(std::move(framebuffer));
  }
}

void Video::RetireLocked(std::unique_ptr<plume::RenderDescriptorSet> set) {
  if (set) {
    auto &s = State();
    s.retired_sets[s.frame].push_back(std::move(set));
  }
}

bool Video::UploadRGBA8Locked(plume::RenderTexture *texture, uint32_t width, uint32_t height,
                              const uint8_t *rgba) {
  auto &s = State();
  if (!s.list_open || !texture || !rgba || width == 0 || height == 0)
    return false;

  // D3D12 requires 256-byte aligned row pitches for buffer->texture copies.
  const uint32_t row_bytes = width * 4;
  const uint32_t row_pitch = (row_bytes + 255u) & ~255u;
  UploadAllocation staging = s.uploads[s.frame].Allocate(uint64_t(row_pitch) * height, 512);
  if (!staging)
    return false;
  auto *dst = static_cast<uint8_t *>(staging.cpu);
  for (uint32_t y = 0; y < height; ++y)
    std::memcpy(dst + size_t(y) * row_pitch, rgba + size_t(y) * row_bytes, row_bytes);

  s.command_list->barriers(plume::RenderBarrierStage::COPY,
                           plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::COPY_DEST));
  s.command_list->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(texture, 0),
      plume::RenderTextureCopyLocation::PlacedFootprint(staging.ref.ref,
                                                        plume::RenderFormat::R8G8B8A8_UNORM, width,
                                                        height, 1, row_pitch / 4,
                                                        staging.ref.offset));
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::SHADER_READ));
  return true;
}

} // namespace renut::gpu
