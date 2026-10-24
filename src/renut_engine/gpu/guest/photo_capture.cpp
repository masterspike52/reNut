/**
 * @file    gpu/guest/photo_capture.cpp
 * @brief   N&B's frame capture for photos, native.
 *
 * sub_825160B0 (BOOL (D3DTexture *photo)) copies the frame into a CPU
 * texture: photo mode's shot (sub_82269D20, state 3) and the pause menu's
 * backdrop (sub_8255B428, sub_82514080), all into stru_82DF06C4, the
 * screen-sized D3DFMT_LIN_X8R8G8B8 texture sub_822E8660 sets up. The game
 * then shows it and saves it to the album from guest memory (sub_82516250).
 * The original waits for the GPU to be idle, resolves render target 0 (the
 * size of the device's automatic back buffer, +0x39F0) into an X8R8G8B8
 * header over scratch memory (sub_821B7FC0: dword_82DF070C), locks both and
 * untiles the resolve into the photo with XGRAPHICS::UntileSurface. The
 * renderer's resolves stay on the host GPU, so that untile copied memory
 * nothing wrote: a black photo.
 *
 * Here the render thread reads render target 0 back (ReadRenderTargetLocked,
 * after the work queued before the call) and the words go into the photo's
 * rows, which is what the untile leaves there. Left out: the scratch header
 * and its resolve's device work (fences on a header that lives for the call),
 * and the cache flushes of both ranges (sub_8263D360).
 */
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <rex/hook.h>
#include <rex/types.h>

#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/guest/callbacks.h"
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/render/texture_layout.h"

using namespace renut::gpu;

namespace {

// dword_82E19AC8: N&B's D3DDevice.
constexpr u32 kDevicePointer = 0x82E19AC8;

u32 LoadU32(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  return p ? u32(*p) : 0;
}

// BOOL sub_825160B0(D3DTexture *photo) [0x825160B0]
u32 PhotoCapture_hook(u32 photo) {
  const u32 device = LoadU32(kDevicePointer);
  const u32 back_buffer = device ? LoadU32(device + native::off::AutoBackBuffer) : 0;
  if (!back_buffer)
    return 0;
  const auto *surface = GuestPtr<const d3d::Surface>(back_buffer);
  const auto *header = GuestPtr<const d3d::BaseTexture>(photo);
  const auto *device_bytes = GuestPtr<const u8>(device);
  auto &s = State();
  if (!surface || !header || !device_bytes || !s.host_ready ||
      s.shutting_down.load(std::memory_order_acquire) || IsRecordingCommandBuffer(device)) {
    return 0;
  }
  // GetTextureDesc of the back buffer: the resolve's size.
  const u32 size = surface->word[3];
  u32 width = d3d::SurfaceWidth(size);
  u32 height = d3d::SurfaceHeight(size);

  // The photo's base level: linear rows of pitch_texels words.
  const TextureFetch fetch = TextureFetch::FromHeader(header->Format);
  if (FormatBitsPerPixel(fetch.Format()) != 32)
    return 0;
  u32 photo_width = 0, photo_height = 0, photo_depth = 0;
  fetch.Size(photo_width, photo_height, photo_depth);
  const TextureLevelLayout layout = GetTextureLevelLayout(fetch, 0);
  auto *dest = GuestPhysicalPtr<u8>((layout.page << 12) + layout.offset);
  if (!dest)
    return 0;

  // After the draws queued before the call, as the resolve would be.
  FlushDeferredVertices(device);
  RunReadyCallbacks();
  struct Readback {
    std::vector<u8> state;
    u32 width = 0, height = 0;
    std::vector<u32> words;
    bool done = false;
  };
  auto readback = std::make_shared<Readback>();
  readback->state.assign(device_bytes, device_bytes + d3d::kDeviceSnapshotSize);
  readback->width = width;
  readback->height = height;
  render_thread::Queue([readback] {
    readback->done = ReadRenderTargetLocked(readback->state.data(), 0, readback->width,
                                            readback->height, readback->words);
  });
  render_thread::Drain();
  if (!readback->done) {
    RGPU_WARN("Photo capture: render target 0 of {:08X} was not read back", device);
    return 0;
  }

  width = std::min(readback->width, photo_width);
  height = std::min(readback->height, photo_height);
  const u32 pitch = layout.pitch_texels * 4;
  for (u32 y = 0; y < height; ++y) {
    std::memcpy(dest + size_t(y) * pitch, readback->words.data() + size_t(y) * readback->width,
                size_t(width) * 4);
  }
  // No D3D::UnlockResource, as in the original: the photo's content check
  // sees the new words. (An unlock would make it lock-driven, and the album's
  // XGCopySurface into it, sub_822E9970, writes without one.)
  return 1;
}

} // namespace

REX_HOOK(sub_825160B0, PhotoCapture_hook);
