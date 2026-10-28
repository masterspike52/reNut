/**
 * @file    gpu/overlay/overlay_drawer.cpp
 */
#include "renut_engine/gpu/overlay/overlay_drawer.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include <plume_render_interface.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/video.h"

#include "host_shaders/overlay_ps.spirv.h"
#include "host_shaders/overlay_vs.spirv.h"
#if defined(_WIN32)
#include "host_shaders/overlay_ps.dxil.h"
#include "host_shaders/overlay_vs.dxil.h"
#endif

namespace renut::gpu {

// What the SDK drew in one ImGuiDrawer::Draw, in render target pixels of
// width x height.
struct OverlayDrawer::Frame {
  struct Batch {
    uint32_t first_vertex = 0;
    uint32_t vertex_count = 0;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
  };
  struct DrawCall {
    uint32_t batch = 0;
    bool lines = false;
    uint32_t count = 0;
    uint32_t index_offset = 0;
    int32_t base_vertex = 0;
    plume::RenderRect scissor;
    std::shared_ptr<OverlayTextureData> texture;
  };

  uint32_t width = 0;
  uint32_t height = 0;
  float scale[2] = {};      // coordinate space -> clip space, y down
  float translate[2] = {};
  std::vector<rex::ui::ImmediateVertex> vertices;
  std::vector<uint16_t> indices;
  std::vector<Batch> batches;
  std::vector<DrawCall> draws;
};

namespace {

const plume::RenderInputSlot kVertexSlot(0, sizeof(rex::ui::ImmediateVertex),
                                         plume::RenderInputSlotClassification::PER_VERTEX_DATA);

// The latest recorded frame (UI thread writes, presenting thread reads).
std::mutex g_frame_mutex;
std::shared_ptr<const OverlayDrawer::Frame> g_latest;
uint64_t g_frames_recorded = 0;  // UI thread

// Host textures of textures gone, for the presenting thread to release.
struct Released {
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  uint32_t slot = kInvalidDescriptor;
};
std::mutex g_release_mutex;
std::vector<Released> g_released;

// Pipelines and the white texel, made on the presenting thread on first use.
// Kept to the end of the process: the device outlives every frame drawn.
struct Resources {
  bool ready = false;
  bool failed = false;
  std::unique_ptr<plume::RenderShader> vs;
  std::unique_ptr<plume::RenderShader> ps;
  std::unique_ptr<plume::RenderPipeline> triangles;
  std::unique_ptr<plume::RenderPipeline> lines;
  std::unique_ptr<plume::RenderTexture> white;
  std::unique_ptr<plume::RenderTextureView> white_view;
  uint32_t white_slot = kInvalidDescriptor;
};
Resources &GetResources() {
  static Resources *resources = new Resources;
  return *resources;
}

std::unique_ptr<plume::RenderPipeline> CreateOverlayPipeline(plume::RenderDevice *device,
                                                             plume::RenderShader *vs,
                                                             plume::RenderShader *ps,
                                                             plume::RenderPrimitiveTopology topology) {
  const plume::RenderInputElement elements[] = {
      plume::RenderInputElement("POSITION", 0, 0, plume::RenderFormat::R32G32_FLOAT, 0, 0),
      plume::RenderInputElement("TEXCOORD", 0, 1, plume::RenderFormat::R32G32_FLOAT, 0, 8),
      plume::RenderInputElement("COLOR", 0, 2, plume::RenderFormat::R8G8B8A8_UNORM, 0, 16),
  };
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = State().utility_layout.get();
  desc.vertexShader = vs;
  desc.pixelShader = ps;
  desc.primitiveTopology = topology;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::AlphaBlend();
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  desc.inputSlots = &kVertexSlot;
  desc.inputSlotsCount = 1;
  desc.inputElements = elements;
  desc.inputElementsCount = 3;
  return device->createGraphicsPipeline(desc);
}

// Creates a sampleable RGBA8 texture, records its upload and binds a slot.
bool CreateUploadedTexture(uint32_t width, uint32_t height, const uint8_t *rgba,
                           std::unique_ptr<plume::RenderTexture> &out_texture,
                           std::unique_ptr<plume::RenderTextureView> &out_view,
                           uint32_t &out_slot) {
  plume::RenderDevice *device = Video::Device();
  auto texture = device->createTexture(
      plume::RenderTextureDesc::Texture2D(width, height, 1, plume::RenderFormat::R8G8B8A8_UNORM));
  if (!texture)
    return false;
  texture->setName("renut overlay texture");
  auto view = texture->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM));
  if (!view)
    return false;
  if (!Video::UploadRGBA8Locked(texture.get(), width, height, rgba))
    return false;
  const uint32_t slot = Video::AllocateTextureSlotLocked();
  if (slot == kInvalidDescriptor)
    return false;
  Video::SetTextureSlotLocked(slot, texture.get(), view.get());
  out_texture = std::move(texture);
  out_view = std::move(view);
  out_slot = slot;
  return true;
}

bool EnsureResourcesLocked() {
  Resources &r = GetResources();
  if (r.ready || r.failed)
    return r.ready;
  plume::RenderDevice *device = Video::Device();
  if (!device || !State().utility_layout)
    return false;
  r.failed = true;
  r.vs = CreateHostShader(device, RENUT_HOST_SHADER(overlay_vs), "overlay_vs");
  r.ps = CreateHostShader(device, RENUT_HOST_SHADER(overlay_ps), "overlay_ps");
  if (!r.vs || !r.ps)
    return false;
  r.triangles = CreateOverlayPipeline(device, r.vs.get(), r.ps.get(),
                                      plume::RenderPrimitiveTopology::TRIANGLE_LIST);
  r.lines = CreateOverlayPipeline(device, r.vs.get(), r.ps.get(),
                                  plume::RenderPrimitiveTopology::LINE_LIST);
  if (!r.triangles || !r.lines) {
    RGPU_ERROR("Overlay pipeline creation failed");
    return false;
  }
  // Untextured ImGui geometry samples a white texel.
  static const uint8_t kWhite[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  if (!CreateUploadedTexture(1, 1, kWhite, r.white, r.white_view, r.white_slot)) {
    RGPU_ERROR("Overlay white texture creation failed");
    return false;
  }
  r.failed = false;
  r.ready = true;
  return true;
}

void ReleaseTexturesLocked() {
  std::vector<Released> released;
  {
    std::lock_guard lock(g_release_mutex);
    released.swap(g_released);
  }
  for (Released &r : released) {
    // Frame lists in flight may still sample it: retire rather than free.
    if (r.slot != kInvalidDescriptor)
      Video::FreeTextureSlotLocked(r.slot);
    Video::RetireLocked(std::move(r.view));
    Video::RetireLocked(std::move(r.texture));
  }
}

// The frame just scanned out (SetGameFrameLocked), for GameFrameTexture.
std::mutex g_game_frame_mutex;
uint32_t g_game_frame_slot = kInvalidDescriptor;
float g_game_frame_uv[4] = {};

// The host texture of 'data', made from its pixels the first time; the
// white texel's slot when there is none.
uint32_t TextureSlotLocked(OverlayTextureData *data) {
  if (!data)
    return GetResources().white_slot;
  if (data->game_frame) {
    std::lock_guard lock(g_game_frame_mutex);
    return g_game_frame_slot != kInvalidDescriptor ? g_game_frame_slot : GetResources().white_slot;
  }
  if (!data->texture && !data->failed) {
    data->failed = !CreateUploadedTexture(data->width, data->height, data->rgba.data(),
                                          data->texture, data->view, data->slot);
    data->rgba.clear();
    data->rgba.shrink_to_fit();
  }
  return data->failed ? GetResources().white_slot : data->slot;
}

} // namespace

OverlayTextureData::OverlayTextureData(uint32_t width, uint32_t height, uint32_t sampler_slot,
                                       const uint8_t *pixels)
    : width(width), height(height), sampler_slot(sampler_slot),
      rgba(pixels, pixels + size_t(width) * height * 4), slot(kInvalidDescriptor) {}

OverlayTextureData::~OverlayTextureData() {
  if (!texture && !view && slot == kInvalidDescriptor)
    return;
  std::lock_guard lock(g_release_mutex);
  g_released.push_back({std::move(texture), std::move(view), slot});
}

OverlayTexture::OverlayTexture(std::shared_ptr<OverlayTextureData> data)
    : rex::ui::ImmediateTexture(data->width, data->height), data_(std::move(data)) {}

void SetGameFrameLocked(uint32_t slot, const float uv[4]) {
  std::lock_guard lock(g_game_frame_mutex);
  g_game_frame_slot = slot;
  if (uv)
    std::copy(uv, uv + 4, g_game_frame_uv);
}

rex::ui::ImmediateTexture *GameFrameTexture(float uv[4]) {
  static const uint8_t kTexel[4] = {};
  static OverlayTexture texture([] {
    auto data = std::make_shared<OverlayTextureData>(1, 1, kLinearClampSamplerSlot, kTexel);
    data->game_frame = true;
    return data;
  }());
  std::lock_guard lock(g_game_frame_mutex);
  if (g_game_frame_slot == kInvalidDescriptor)
    return nullptr;
  std::copy(g_game_frame_uv, g_game_frame_uv + 4, uv);
  return &texture;
}

OverlayDrawer::OverlayDrawer() = default;

OverlayDrawer::~OverlayDrawer() {
  std::lock_guard lock(g_frame_mutex);
  g_latest.reset();
}

std::unique_ptr<rex::ui::ImmediateTexture>
OverlayDrawer::CreateTexture(uint32_t width, uint32_t height, rex::ui::ImmediateTextureFilter filter,
                             bool is_repeated, const uint8_t *data) {
  (void)is_repeated;
  if (!data || width == 0 || height == 0)
    return nullptr;
  const uint32_t sampler = filter == rex::ui::ImmediateTextureFilter::kNearest
                               ? kPointClampSamplerSlot
                               : kLinearClampSamplerSlot;
  return std::make_unique<OverlayTexture>(
      std::make_shared<OverlayTextureData>(width, height, sampler, data));
}

void OverlayDrawer::Begin(rex::ui::UIDrawContext &ctx, float coord_width, float coord_height) {
  // The base resolves non-positive sizes; read them back through its accessors.
  rex::ui::ImmediateDrawer::Begin(ctx, coord_width, coord_height);
  recording_ = std::make_unique<Frame>();
  recording_->width = ctx.render_target_width();
  recording_->height = ctx.render_target_height();
  recording_->scale[0] = 2.0f / coordinate_space_width();
  recording_->scale[1] = -2.0f / coordinate_space_height();
  recording_->translate[0] = -1.0f;
  recording_->translate[1] = 1.0f;
}

void OverlayDrawer::BeginDrawBatch(const rex::ui::ImmediateDrawBatch &batch) {
  if (!recording_)
    return;
  Frame::Batch b;
  b.first_vertex = uint32_t(recording_->vertices.size());
  b.first_index = uint32_t(recording_->indices.size());
  if (batch.vertices && batch.vertex_count > 0) {
    recording_->vertices.insert(recording_->vertices.end(), batch.vertices,
                                batch.vertices + batch.vertex_count);
    b.vertex_count = uint32_t(batch.vertex_count);
  }
  if (batch.indices && batch.index_count > 0) {
    recording_->indices.insert(recording_->indices.end(), batch.indices,
                               batch.indices + batch.index_count);
    b.index_count = uint32_t(batch.index_count);
  }
  recording_->batches.push_back(b);
}

void OverlayDrawer::Draw(const rex::ui::ImmediateDraw &draw) {
  if (!recording_ || recording_->batches.empty() || draw.count <= 0 ||
      !recording_->batches.back().vertex_count)
    return;
  uint32_t left, top, width, height;
  if (!ScissorToRenderTarget(draw, left, top, width, height))
    return;
  Frame::DrawCall call;
  call.batch = uint32_t(recording_->batches.size() - 1);
  call.lines = draw.primitive_type == rex::ui::ImmediatePrimitiveType::kLines;
  call.count = uint32_t(draw.count);
  call.index_offset = uint32_t(draw.index_offset);
  call.base_vertex = draw.base_vertex;
  call.scissor = plume::RenderRect(int32_t(left), int32_t(top), int32_t(left + width),
                                   int32_t(top + height));
  if (draw.texture)
    call.texture = static_cast<OverlayTexture *>(draw.texture)->data();
  recording_->draws.push_back(std::move(call));
}

void OverlayDrawer::EndDrawBatch() {}

void OverlayDrawer::End() {
  std::shared_ptr<const Frame> frame(std::move(recording_));
  {
    std::lock_guard lock(g_frame_mutex);
    g_latest.swap(frame);
  }
  // The frame replaced (and any texture only it held) goes here, unlocked.
  frame.reset();
  ++g_frames_recorded;
  rex::ui::ImmediateDrawer::End();
}

uint64_t OverlayDrawer::FramesRecorded() {
  return g_frames_recorded;
}

void OverlayDrawer::ClearLatest() {
  std::shared_ptr<const Frame> frame;
  {
    std::lock_guard lock(g_frame_mutex);
    g_latest.swap(frame);
  }
}

void OverlayDrawer::DrawLatestLocked(plume::RenderCommandList *cmd,
                                     plume::RenderFramebuffer *framebuffer, uint32_t width,
                                     uint32_t height) {
  ReleaseTexturesLocked();
  std::shared_ptr<const Frame> frame;
  {
    std::lock_guard lock(g_frame_mutex);
    frame = g_latest;
  }
  if (!cmd || !frame || frame->draws.empty() || !frame->width || !frame->height || !width ||
      !height || !EnsureResourcesLocked())
    return;
  Resources &r = GetResources();

  cmd->setFramebuffer(framebuffer);
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  cmd->setGraphicsPipelineLayout(State().utility_layout.get());
  cmd->setGraphicsDescriptorSet(State().texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(State().sampler_set.get(), 1);

  // Scissors are in the pixels of the size the frame was recorded at; the
  // window may have been resized since.
  const float to_x = float(width) / float(frame->width);
  const float to_y = float(height) / float(frame->height);
  UploadRing &uploads = Video::UploadsLocked();
  uint32_t bound_batch = UINT32_MAX;
  bool batch_ok = false;
  bool indexed = false;
  for (const Frame::DrawCall &call : frame->draws) {
    if (call.batch != bound_batch) {
      bound_batch = call.batch;
      const Frame::Batch &b = frame->batches[call.batch];
      batch_ok = false;
      indexed = b.index_count > 0;
      const UploadAllocation vertices =
          uploads.Upload(frame->vertices.data() + b.first_vertex,
                         sizeof(rex::ui::ImmediateVertex) * uint64_t(b.vertex_count), 16);
      if (!vertices)
        continue;
      const plume::RenderVertexBufferView vertex_view(vertices.ref, vertices.size);
      cmd->setVertexBuffers(0, &vertex_view, 1, &kVertexSlot);
      if (indexed) {
        const UploadAllocation indices = uploads.Upload(
            frame->indices.data() + b.first_index, sizeof(uint16_t) * uint64_t(b.index_count), 16);
        if (!indices)
          continue;
        const plume::RenderIndexBufferView index_view(indices.ref, indices.size,
                                                      plume::RenderFormat::R16_UINT);
        cmd->setIndexBuffer(&index_view);
      }
      batch_ok = true;
    }
    if (!batch_ok)
      continue;

    const int32_t left = std::clamp(int32_t(float(call.scissor.left) * to_x), 0, int32_t(width));
    const int32_t top = std::clamp(int32_t(float(call.scissor.top) * to_y), 0, int32_t(height));
    const int32_t right =
        std::clamp(int32_t(float(call.scissor.right) * to_x + 0.5f), left, int32_t(width));
    const int32_t bottom =
        std::clamp(int32_t(float(call.scissor.bottom) * to_y + 0.5f), top, int32_t(height));
    if (right <= left || bottom <= top)
      continue;
    cmd->setScissors(plume::RenderRect(left, top, right, bottom));
    cmd->setPipeline(call.lines ? r.lines.get() : r.triangles.get());

    UtilityPush push{};
    push.scale[0] = frame->scale[0];
    push.scale[1] = frame->scale[1];
    push.translate[0] = frame->translate[0];
    push.translate[1] = frame->translate[1];
    push.texture_index = TextureSlotLocked(call.texture.get());
    push.sampler_index = call.texture ? call.texture->sampler_slot : kLinearClampSamplerSlot;
    push.param0 = call.texture && call.texture->game_frame ? 1.0f : 0.0f;
    cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    if (indexed)
      cmd->drawIndexedInstanced(call.count, 1, call.index_offset, call.base_vertex, 0);
    else
      cmd->drawInstanced(call.count, 1, uint32_t(call.base_vertex), 0);
  }
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
}

} // namespace renut::gpu
