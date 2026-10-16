/**
 * @file    gpu/guest/widescreen.cpp
 */
#include "renut_engine/gpu/guest/widescreen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "renut_engine/gpu/backend/native_window.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"

REXCVAR_DEFINE_STRING(renut_aspect_ratio, "Auto", "Nuts&Bolts/Graphics",
                      "Aspect ratio the game renders at. Auto = the game window's when the game "
                      "starts (the monitor's in fullscreen). Narrower than 16:9 stays 16:9. "
                      "Takes effect on restart.")
    .allowed({"Auto", "16:9", "21:9", "32:9"});

namespace renut::gpu {

namespace {

constexpr uint32_t kNativeWidth = 1280;
constexpr uint32_t kNativeHeight = 720;
constexpr double kNativeAspect = 16.0 / 9.0;
constexpr double kMaxAspect = 32.0 / 9.0;

// Camera projection aspect modes (sub_82239C40): the active camera's mode at
// +0x6C, where 3 means the console's (0 = 4:3, 1 = 16:9, 2 = 2.35:1).
constexpr u32 kActiveCameraVa = 0x82FADD20;
constexpr u32 kCameraAspectModeOffset = 0x6C;
constexpr u32 kConsoleAspectModeVa = 0x82FADDE8;
constexpr u32 kAspectModeConsole = 3;
constexpr u32 kAspectMode4x3 = 0;

// sub_823191B8's buffers: pools of 1280x736 32bpp texels (0x398000 bytes)
// and a 0x730000-byte one, created once (kPoolsCreatedVa) and freed by
// sub_823ED2A8. Their sizes are kept for placing textures at a pool's end.
constexpr u32 kPoolsCreatedVa = 0x82F835E8;
constexpr u32 kScreenPoolBytes = 0x398000;
constexpr u32 kImagePoolBytes = 0x730000;
constexpr u32 kScratchPoolSizeVa = 0x82F835B0;
constexpr u32 kScratchPoolVa = 0x82F835B4;
constexpr u32 kCopyPoolSizeVa = 0x82F835B8;
constexpr u32 kDepthPoolSizeVa = 0x82F835D0;
// Scratch textures over the scratch pool sized for 1280 (sub_82319650 /
// sub_82230E18 pick them by the camera viewport): the whole screen at its
// start, the split-screen halves at its end.
constexpr u32 kScreenScratchVa = 0x82F8336C;      // 1280x720
constexpr u32 kHalfWidthScratchVa = 0x82F833A0;   // 640x720
constexpr u32 kTopScratchVa = 0x82F833D4;         // 1280x352
constexpr u32 kBottomScratchVa = 0x82F83408;      // 1280x368

std::atomic<uint32_t> g_screen_width{0};

// sub_823191B8 is creating its pools on this thread.
thread_local bool t_sizing_pools = false;

u32 LoadU32(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  return p ? p->get() : 0;
}

void StoreU32(u32 va, u32 value) {
  if (auto *p = GuestPtr<be_u32>(va))
    *p = value;
}

constexpr u32 AlignUp(u32 value, u32 alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

// A 1-level tiled 32bpp 2D texture's size (XGSetTextureHeader): 32x32 tiles,
// 4 KB pages.
constexpr u32 Tiled32bppBytes(u32 width, u32 height) {
  return AlignUp(AlignUp(width, 32) * AlignUp(height, 32) * 4, 0x1000);
}

double ConfiguredAspect() {
  const std::string &setting = REXCVAR_GET(renut_aspect_ratio);
  if (setting == "16:9")
    return kNativeAspect;
  if (setting == "21:9")
    return 64.0 / 27.0;
  if (setting == "32:9")
    return 32.0 / 9.0;
  uint32_t width = 0, height = 0;
  if (!GetRenderWindowPixelSize(width, height) || !width || !height) {
    RGPU_WARN("Widescreen: no game window size yet, staying 16:9");
    return kNativeAspect;
  }
  return double(width) / double(height);
}

// Wider screens keep the height; the width is a multiple of 16, so its
// halves and quarters (split screen, downsamples) stay whole.
uint32_t WidthForAspect(double aspect) {
  if (aspect <= kNativeAspect + 0.01)
    return kNativeWidth;
  const double width = double(kNativeHeight) * std::min(aspect, kMaxAspect);
  return std::max(uint32_t(std::lround(width / 16.0)) * 16, kNativeWidth);
}

bool Widened() {
  return GuestScreenWidth() > kNativeWidth;
}

// Room for one screen of 32bpp texels, tiled (32-texel rows) or linear
// (64-texel rows): what each of sub_823191B8's 1280x736 pools holds.
u32 ScreenPoolBytes() {
  const u32 bytes = AlignUp(AlignUp(GuestScreenWidth(), 64) * AlignUp(kNativeHeight, 32) * 4, 0x1000);
  return std::max(bytes, kScreenPoolBytes);
}

// The texture header at 'header_va', as XGSetTextureHeader and
// XGOffsetResourceAddress make a 1-level 2D texture: pitch, size and base.
void SetTexture2D(u32 header_va, u32 width, u32 height, u32 base_address) {
  auto *texture = GuestPtr<d3d::BaseTexture>(header_va);
  if (!texture)
    return;
  d3d::TextureFetchConstant &fetch = texture->Format;
  fetch.dword[0] = (fetch.dword[0].get() & ~(0x1FFu << 22)) | ((AlignUp(width, 32) >> 5) << 22);
  fetch.dword[1] = (fetch.dword[1].get() & 0xFFFu) | (base_address & ~0xFFFu);
  fetch.dword[2] = (fetch.dword[2].get() & ~0x3FFFFFFu) | ((height - 1) << 13) | (width - 1);
}

// After sub_823191B8 made its pools (at the sizes PoolAllocationBytes gave):
// their recorded sizes, and the scratch textures it sized for 1280.
void WidenScratchTextures() {
  const u32 width = GuestScreenWidth();
  const u32 pool_bytes = ScreenPoolBytes();
  StoreU32(kScratchPoolSizeVa, pool_bytes);
  StoreU32(kCopyPoolSizeVa, pool_bytes);
  StoreU32(kDepthPoolSizeVa, pool_bytes);

  const u32 pool = LoadU32(kScratchPoolVa);
  if (!pool)
    return;
  const u32 pool_end = pool + pool_bytes;
  SetTexture2D(kScreenScratchVa, width, kNativeHeight, pool);
  SetTexture2D(kHalfWidthScratchVa, width / 2, kNativeHeight,
               pool_end - Tiled32bppBytes(width / 2, kNativeHeight));
  SetTexture2D(kTopScratchVa, width, 352, pool_end - Tiled32bppBytes(width, 352));
  SetTexture2D(kBottomScratchVa, width, 368, pool_end - Tiled32bppBytes(width, 368));
  RGPU_INFO("Widescreen: screen pools {:#x} bytes", pool_bytes);
}

// sub_823191B8's pool allocations, sized for the wider screen. The image
// pool keeps its recorded size: sub_82575010 puts a screen texture at its
// recorded size less one 1280 screen, so only the room past it grows.
u32 PoolAllocationBytes(u32 bytes) {
  if (bytes == kScreenPoolBytes)
    return ScreenPoolBytes();
  if (bytes == kImagePoolBytes)
    return bytes + ScreenPoolBytes() - kScreenPoolBytes;
  return bytes;
}

// The projection aspect for 'mode' (a camera's, 3 resolved), widened by the
// screen's extra width: the viewports the game derives from the screen width
// widen by as much. 4:3 viewports are the game's own and keep theirs.
void WidenAspect(PPCContext &ctx, u32 mode) {
  if (mode == kAspectMode4x3 || !Widened())
    return;
  const float scale = float(GuestScreenWidth()) / float(kNativeWidth);
  ctx.f1.f64 = double(float(ctx.f1.f64) * scale);
}

u32 CameraAspectMode() {
  const u32 camera = LoadU32(kActiveCameraVa);
  const u32 mode = camera ? LoadU32(camera + kCameraAspectModeOffset) : kAspectModeConsole;
  return mode == kAspectModeConsole ? LoadU32(kConsoleAspectModeVa) : mode;
}

// XUI's root is the back buffer's size (sub_823E92E8), so its units are
// screen pixels; sub_82208338 still scales its view by the render target
// over 1280x720, which on a wider screen would stretch XUI sideways. That
// view keeps the vertical scale on both axes. The target is the screen, or
// the tiling target (its width rounded up to 32).
void UnstretchXuiView(u32 matrix_va) {
  if (!Widened())
    return;
  auto *m = GuestPtr<be_f32>(matrix_va);
  if (!m)
    return;
  const float target_width = m[0].get() * float(kNativeWidth);
  const float target_height = m[5].get() * float(kNativeHeight);
  const u32 width = GuestScreenWidth();
  if (target_width > float(width) - 1.0f && target_width < float(AlignUp(width, 32)) + 1.0f &&
      std::fabs(target_height - float(kNativeHeight)) < 1.0f)
    m[0] = m[5].get();
}

} // namespace

uint32_t GuestScreenWidth() {
  uint32_t width = g_screen_width.load(std::memory_order_acquire);
  if (width)
    return width;
  const double aspect = ConfiguredAspect();
  width = WidthForAspect(aspect);
  uint32_t expected = 0;
  if (g_screen_width.compare_exchange_strong(expected, width, std::memory_order_acq_rel)) {
    RGPU_INFO("Widescreen: game screen {}x{} for aspect {:.3f} ({})", width, kNativeHeight, aspect,
              REXCVAR_GET(renut_aspect_ratio));
    return width;
  }
  return expected;
}

} // namespace renut::gpu

using namespace renut::gpu;

// void sub_823ED828(int query, int *mode, DWORD *width, DWORD *height,
//                   BOOL *hidef): the screen size for the video mode, 1280x720
// for every HD mode. Widened here.
REX_EXTERN(__imp__sub_823ED828);
REX_HOOK_RAW(sub_823ED828) {
  const u32 width_va = ctx.r5.u32;
  const u32 height_va = ctx.r6.u32;
  __imp__sub_823ED828(ctx, base);
  if (width_va && height_va && LoadU32(width_va) == kNativeWidth &&
      LoadU32(height_va) == kNativeHeight)
    StoreU32(width_va, GuestScreenWidth());
}

// void sub_823191B8(): the screen pools and the textures over them, once.
REX_EXTERN(__imp__sub_823191B8);
REX_HOOK_RAW(sub_823191B8) {
  const bool creates = LoadU32(kPoolsCreatedVa) == 0;
  t_sizing_pools = creates && Widened();
  __imp__sub_823191B8(ctx, base);
  if (t_sizing_pools)
    WidenScratchTextures();
  t_sizing_pools = false;
}

// LPVOID sub_82364C78(SIZE_T size, int alignment, DWORD protect): the game's
// XPhysicalAlloc.
REX_EXTERN(__imp__sub_82364C78);
REX_HOOK_RAW(sub_82364C78) {
  if (t_sizing_pools)
    ctx.r3.u64 = PoolAllocationBytes(ctx.r3.u32);
  __imp__sub_82364C78(ctx, base);
}

// float sub_82239C40(): the active camera's projection aspect.
REX_EXTERN(__imp__sub_82239C40);
REX_HOOK_RAW(sub_82239C40) {
  __imp__sub_82239C40(ctx, base);
  WidenAspect(ctx, CameraAspectMode());
}

// float sub_82236148(): the projection aspect for the console's mode.
REX_EXTERN(__imp__sub_82236148);
REX_HOOK_RAW(sub_82236148) {
  __imp__sub_82236148(ctx, base);
  WidenAspect(ctx, LoadU32(kConsoleAspectModeVa));
}

// HRESULT XuiRenderSetViewTransform(HXUIDC, const D3DXMATRIX *) [0x82205C88]
REX_EXTERN(__imp__sub_82205C88);
REX_HOOK_RAW(sub_82205C88) {
  UnstretchXuiView(ctx.r4.u32);
  __imp__sub_82205C88(ctx, base);
}
