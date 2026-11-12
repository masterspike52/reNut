#include "renut_engine/gpu/overlay/overlay_drawer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include <imgui.h>

#include <plume_render_interface.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/frame_blur.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#include "renut_engine/gpu/backend/video.h"

#include "host_shaders/overlay_ps.spirv.h"
#include "host_shaders/overlay_vs.spirv.h"
#if defined(_WIN32)
#include "host_shaders/overlay_ps.dxil.h"
#include "host_shaders/overlay_vs.dxil.h"
#endif

namespace renut::gpu {

// What the SDK drew in one IUISystem::DrawOverlays, in render target pixels of
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
  std::vector<ImDrawVert> vertices;
  std::vector<ImDrawIdx> indices;
  std::vector<Batch> batches;
  std::vector<DrawCall> draws;
};

namespace {

const plume::RenderInputSlot kVertexSlot(0, sizeof(ImDrawVert),
                                         plume::RenderInputSlotClassification::PER_VERTEX_DATA);

// The latest recorded frame (UI thread writes, presenting thread reads).
std::mutex g_frame_mutex;
std::shared_ptr<const OverlayDrawer::Frame> g_latest;
uint64_t g_frames_recorded = 0;  // UI thread

// The textures the SDK's ImGui names by ImTextureID: its own (font atlas, user
// textures), CreateOverlayTexture's, and the game frame and screen stand-ins.
std::mutex g_textures_mutex;
std::unordered_map<ImTextureID, std::shared_ptr<OverlayTextureData>> g_textures;
// Apart from reNut's own overlay's ids (overlay_ui.cpp), so one used in the
// wrong context draws nothing.
ImTextureID g_next_texture = ImTextureID(1) << 63;

ImTextureID RegisterTexture(std::shared_ptr<OverlayTextureData> data) {
  std::lock_guard lock(g_textures_mutex);
  const ImTextureID id = g_next_texture++;
  g_textures.emplace(id, std::move(data));
  return id;
}

std::shared_ptr<OverlayTextureData> FindTexture(ImTextureID id) {
  std::lock_guard lock(g_textures_mutex);
  auto it = g_textures.find(id);
  return it != g_textures.end() ? it->second : nullptr;
}

// A stand-in texture (game frame, blurred frame, screen), drawn from what the
// presenting thread has rather than from pixels.
ImTextureID StandInTexture(bool game_frame, bool blurred_frame, uint8_t screen) {
  static const uint8_t kTexel[4] = {};
  auto data = std::make_shared<OverlayTextureData>(1, 1, kLinearClampSamplerSlot, kTexel);
  data->game_frame = game_frame;
  data->blurred_frame = blurred_frame;
  data->screen = screen;
  return RegisterTexture(std::move(data));
}

// Honours one of ImGui's texture requests (UI thread): the pixels are copied
// for the presenting thread, so ImGui may consider them done.
void TakeTextureRequest(ImTextureData &texture) {
  if (texture.Status == ImTextureStatus_WantDestroy) {
    {
      std::lock_guard lock(g_textures_mutex);
      g_textures.erase(texture.GetTexID());
    }
    texture.SetTexID(ImTextureID_Invalid);
    texture.SetStatus(ImTextureStatus_Destroyed);
    return;
  }
  if (texture.Status != ImTextureStatus_WantCreate && texture.Status != ImTextureStatus_WantUpdates)
    return;
  // An update takes the whole texture again: the font atlas changes rarely.
  const uint32_t width = uint32_t(texture.Width);
  const uint32_t height = uint32_t(texture.Height);
  const size_t texels = size_t(width) * height;
  std::vector<uint8_t> rgba(texels * 4);
  const auto *pixels = static_cast<const uint8_t *>(texture.GetPixels());
  if (texture.Format == ImTextureFormat_RGBA32) {
    std::memcpy(rgba.data(), pixels, texels * 4);
  } else {  // Alpha8: white with the coverage as alpha
    for (size_t i = 0; i < texels; ++i) {
      rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 0xFF;
      rgba[i * 4 + 3] = pixels[i];
    }
  }
  auto data = std::make_shared<OverlayTextureData>(width, height, kLinearClampSamplerSlot, rgba.data());
  if (texture.Status == ImTextureStatus_WantCreate) {
    texture.SetTexID(RegisterTexture(std::move(data)));
  } else {
    // Frames already recorded keep the old pixels.
    std::lock_guard lock(g_textures_mutex);
    g_textures[texture.GetTexID()] = std::move(data);
  }
  texture.SetStatus(ImTextureStatus_OK);
}

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
  if (!r.triangles || !r.lines)
    return false;
  // Untextured ImGui geometry samples a white texel.
  static const uint8_t kWhite[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  if (!CreateUploadedTexture(1, 1, kWhite, r.white, r.white_view, r.white_slot))
    return false;
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
uint32_t g_blurred_frame_slot = kInvalidDescriptor;  // SetBlurredFrameLocked

// The host texture of 'data', made from its pixels the first time; the
// white texel's slot when there is none.
uint32_t TextureSlotLocked(OverlayTextureData *data) {
  if (!data)
    return GetResources().white_slot;
  if (data->screen) {
    if (const uint32_t slot = ScreenSlotLocked(data->screen == 2); slot != kInvalidDescriptor)
      return slot;
  }
  if (data->game_frame || data->blurred_frame || data->screen) {
    std::lock_guard lock(g_game_frame_mutex);
    // A blurred frame (or screen) that stopped being made shows the plain one rather than white.
    if ((data->blurred_frame || data->screen) && g_blurred_frame_slot != kInvalidDescriptor)
      return g_blurred_frame_slot;
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

void SetGameFrameLocked(uint32_t slot, const float uv[4]) {
  std::lock_guard lock(g_game_frame_mutex);
  g_game_frame_slot = slot;
  if (uv)
    std::copy(uv, uv + 4, g_game_frame_uv);
}

ImTextureID GameFrameTexture(float uv[4]) {
  static const ImTextureID texture = StandInTexture(true, false, 0);
  std::lock_guard lock(g_game_frame_mutex);
  if (g_game_frame_slot == kInvalidDescriptor)
    return ImTextureID_Invalid;
  std::copy(g_game_frame_uv, g_game_frame_uv + 4, uv);
  return texture;
}

void SetBlurredFrameLocked(uint32_t slot) {
  std::lock_guard lock(g_game_frame_mutex);
  g_blurred_frame_slot = slot;
}

ImTextureID BlurredGameFrameTexture(float sigma, float uv[4]) {
  static const ImTextureID texture = StandInTexture(false, true, 0);
  // Asking is what keeps the blur running (frame_blur.cpp), so it's asked for even before there's one.
  WantFrameBlur(sigma);
  std::lock_guard lock(g_game_frame_mutex);
  if (g_blurred_frame_slot == kInvalidDescriptor)
    return ImTextureID_Invalid;
  // The blurred copy is only the shown part.
  uv[0] = uv[1] = 0.0f;
  uv[2] = uv[3] = 1.0f;
  return texture;
}

ImTextureID ScreenTexture(float sigma, float uv[4]) {
  static const ImTextureID sharp = StandInTexture(false, false, 1);
  static const ImTextureID blurred = StandInTexture(false, false, 2);
  // Asking keeps the UI drawn where it can be copied (frame_blur.cpp), so it's asked for even before it's ready.
  WantScreen(sigma);
  if (!ScreenReady())
    return ImTextureID_Invalid;
  // The copies are the window's size: the whole screen.
  uv[0] = uv[1] = 0.0f;
  uv[2] = uv[3] = 1.0f;
  return sigma > 0.0f ? blurred : sharp;
}

ImTextureID CreateOverlayTexture(uint32_t width, uint32_t height, const uint8_t *rgba) {
  if (!rgba || width == 0 || height == 0)
    return ImTextureID_Invalid;
  return RegisterTexture(std::make_shared<OverlayTextureData>(width, height, kLinearClampSamplerSlot, rgba));
}

void DestroyOverlayTexture(ImTextureID texture) {
  std::shared_ptr<OverlayTextureData> data;
  {
    std::lock_guard lock(g_textures_mutex);
    auto it = g_textures.find(texture);
    if (it == g_textures.end())
      return;
    data = std::move(it->second);
    g_textures.erase(it);
  }
  // Released here, unlocked, unless a recorded frame still holds it.
  data.reset();
}

OverlayDrawer::OverlayDrawer() = default;

OverlayDrawer::~OverlayDrawer() {
  std::lock_guard lock(g_frame_mutex);
  g_latest.reset();
}

void OverlayDrawer::RenderOverlays(ImDrawData &draw_data) {
  // Texture ids first: the commands name their textures by them.
  if (draw_data.Textures) {
    for (ImTextureData *texture : *draw_data.Textures)
      TakeTextureRequest(*texture);
  }

  auto recording = std::make_unique<Frame>();
  const ImVec2 display = draw_data.DisplaySize;
  const ImVec2 origin = draw_data.DisplayPos;
  const ImVec2 to_pixels = draw_data.FramebufferScale;
  recording->width = uint32_t(std::max(display.x * to_pixels.x + 0.5f, 0.0f));
  recording->height = uint32_t(std::max(display.y * to_pixels.y + 0.5f, 0.0f));
  if (display.x > 0.0f && display.y > 0.0f) {
    recording->scale[0] = 2.0f / display.x;
    recording->scale[1] = -2.0f / display.y;
    recording->translate[0] = -1.0f - origin.x * recording->scale[0];
    recording->translate[1] = 1.0f - origin.y * recording->scale[1];
  }
  for (const ImDrawList *list : draw_data.CmdLists) {
    Frame::Batch b;
    b.first_vertex = uint32_t(recording->vertices.size());
    b.first_index = uint32_t(recording->indices.size());
    recording->vertices.insert(recording->vertices.end(), list->VtxBuffer.begin(), list->VtxBuffer.end());
    recording->indices.insert(recording->indices.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
    b.vertex_count = uint32_t(list->VtxBuffer.Size);
    b.index_count = uint32_t(list->IdxBuffer.Size);
    recording->batches.push_back(b);
    if (!b.vertex_count || !b.index_count)
      continue;
    for (const ImDrawCmd &draw : list->CmdBuffer) {
      if (draw.UserCallback || !draw.ElemCount)
        continue;
      // The clip rectangle in render target pixels, on it.
      const float max_x = float(recording->width), max_y = float(recording->height);
      const float left = std::clamp((draw.ClipRect.x - origin.x) * to_pixels.x, 0.0f, max_x);
      const float top = std::clamp((draw.ClipRect.y - origin.y) * to_pixels.y, 0.0f, max_y);
      const float right = std::clamp((draw.ClipRect.z - origin.x) * to_pixels.x, left, max_x);
      const float bottom = std::clamp((draw.ClipRect.w - origin.y) * to_pixels.y, top, max_y);
      if (right <= left || bottom <= top)
        continue;
      Frame::DrawCall call;
      if (const ImTextureID id = draw.GetTexID(); id != ImTextureID_Invalid) {
        // A texture already gone draws nothing rather than white.
        call.texture = FindTexture(id);
        if (!call.texture)
          continue;
      }
      call.batch = uint32_t(recording->batches.size() - 1);
      call.count = draw.ElemCount;
      call.index_offset = draw.IdxOffset;
      call.base_vertex = int32_t(draw.VtxOffset);
      call.scissor = plume::RenderRect(int32_t(left), int32_t(top), int32_t(std::ceil(right)),
                                       int32_t(std::ceil(bottom)));
      recording->draws.push_back(std::move(call));
    }
  }

  std::shared_ptr<const Frame> frame(std::move(recording));
  {
    std::lock_guard lock(g_frame_mutex);
    g_latest.swap(frame);
  }
  // The frame replaced (and any texture only it held) goes here, unlocked.
  frame.reset();
  ++g_frames_recorded;
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

namespace {

// Room around a glass panel's draw, in pixels, for its rim, which bends what's just outside it inwards.
constexpr int32_t kPanelMargin = 12;

// The pixels a run of glass draws covers and samples (their vertices' and texture coordinates' bounds; the screen's
// copies span the target), widened by kPanelMargin, on a width x height target.
bool DrawBounds(const OverlayDrawer::Frame &frame, const OverlayDrawer::Frame::DrawCall *calls, size_t count,
                uint32_t width, uint32_t height, plume::RenderRect &out) {
  float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
  const auto point = [&](float x, float y) {
    min_x = std::min(min_x, x);
    min_y = std::min(min_y, y);
    max_x = std::max(max_x, x);
    max_y = std::max(max_y, y);
  };
  // Coordinates to clip space (scale, translate) to pixels, y down.
  const auto px = [&](float x) { return (x * frame.scale[0] + frame.translate[0] + 1.0f) * 0.5f * float(width); };
  const auto py = [&](float y) { return (1.0f - (y * frame.scale[1] + frame.translate[1])) * 0.5f * float(height); };
  for (size_t c = 0; c < count; ++c) {
    const OverlayDrawer::Frame::DrawCall &call = calls[c];
    const OverlayDrawer::Frame::Batch &b = frame.batches[call.batch];
    const auto add = [&](int64_t vertex) {
      if (vertex < 0 || vertex >= int64_t(b.vertex_count))
        return;
      // Position then texture coordinate, the vertex's first four floats (CreateOverlayPipeline's layout).
      float v[4];
      std::memcpy(v, &frame.vertices[b.first_vertex + size_t(vertex)], sizeof(v));
      point(px(v[0]), py(v[1]));
      point(v[2] * float(width), v[3] * float(height));
    };
    if (b.index_count) {
      for (uint32_t i = call.index_offset; i < call.index_offset + call.count && i < b.index_count; ++i)
        add(int64_t(frame.indices[b.first_index + i]) + call.base_vertex);
    } else {
      for (uint32_t i = 0; i < call.count; ++i)
        add(int64_t(call.base_vertex) + i);
    }
  }
  if (min_x > max_x || min_y > max_y)
    return false;
  out = plume::RenderRect(int32_t(std::floor(min_x)) - kPanelMargin, int32_t(std::floor(min_y)) - kPanelMargin,
                          int32_t(std::ceil(max_x)) + kPanelMargin, int32_t(std::ceil(max_y)) + kPanelMargin);
  return true;
}

} // namespace

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
  std::optional<plume::RenderVertexBufferView> vertex_view;
  std::optional<plume::RenderIndexBufferView> index_view;
  // Something other than glass was drawn since the screen was last copied (the game, before the first draw).
  bool drawn = true;
  for (size_t index = 0; index < frame->draws.size(); ++index) {
    const Frame::DrawCall &call = frame->draws[index];
    if (call.batch != bound_batch) {
      bound_batch = call.batch;
      const Frame::Batch &b = frame->batches[call.batch];
      batch_ok = false;
      indexed = b.index_count > 0;
      const UploadAllocation vertices =
          uploads.Upload(frame->vertices.data() + b.first_vertex,
                         sizeof(ImDrawVert) * uint64_t(b.vertex_count), 16);
      if (!vertices)
        continue;
      vertex_view.emplace(vertices.ref, vertices.size);
      cmd->setVertexBuffers(0, &*vertex_view, 1, &kVertexSlot);
      if (indexed) {
        const UploadAllocation indices = uploads.Upload(
            frame->indices.data() + b.first_index, sizeof(ImDrawIdx) * uint64_t(b.index_count), 16);
        if (!indices)
          continue;
        index_view.emplace(indices.ref, indices.size,
                           sizeof(ImDrawIdx) == 2 ? plume::RenderFormat::R16_UINT : plume::RenderFormat::R32_UINT);
        cmd->setIndexBuffer(&*index_view);
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

    // Glass showing the screen: what's under it is copied first, once anything else was drawn since the last copy.
    const bool screen = call.texture && call.texture->screen;
    if (screen && drawn && ScreenActiveLocked()) {
      // The glass draws up to the next other draw all use this copy.
      size_t run = index + 1;
      while (run < frame->draws.size() && frame->draws[run].texture && frame->draws[run].texture->screen)
        ++run;
      plume::RenderRect region;
      if (DrawBounds(*frame, &frame->draws[index], run - index, width, height, region) &&
          CaptureScreenLocked(State(), cmd, region)) {
        // The copy left the pass; back in it, with this pass's state.
        cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
        cmd->setGraphicsPipelineLayout(State().utility_layout.get());
        cmd->setGraphicsDescriptorSet(State().texture_set.get(), 0);
        cmd->setGraphicsDescriptorSet(State().sampler_set.get(), 1);
        cmd->setVertexBuffers(0, &*vertex_view, 1, &kVertexSlot);
        if (indexed && index_view)
          cmd->setIndexBuffer(&*index_view);
      }
      drawn = false;
    }
    if (!screen)
      drawn = true;

    cmd->setScissors(plume::RenderRect(left, top, right, bottom));
    cmd->setPipeline(call.lines ? r.lines.get() : r.triangles.get());

    UtilityPush push{};
    push.scale[0] = frame->scale[0];
    push.scale[1] = frame->scale[1];
    push.translate[0] = frame->translate[0];
    push.translate[1] = frame->translate[1];
    push.texture_index = TextureSlotLocked(call.texture.get());
    push.sampler_index = call.texture ? call.texture->sampler_slot : kLinearClampSamplerSlot;
    push.param0 =
        call.texture && (call.texture->game_frame || call.texture->blurred_frame || call.texture->screen) ? 1.0f : 0.0f;
    cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    if (indexed)
      cmd->drawIndexedInstanced(call.count, 1, call.index_offset, call.base_vertex, 0);
    else
      cmd->drawInstanced(call.count, 1, uint32_t(call.base_vertex), 0);
  }
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
}

} // namespace renut::gpu
