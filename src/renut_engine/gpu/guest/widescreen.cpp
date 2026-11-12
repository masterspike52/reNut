#include "renut_engine/gpu/guest/widescreen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>

#include <rex/logging/log_category.h>
#include <rex/ppc/hook.h>
#include <rex/setting.h>
#include <rex/types.h>

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/gpu/backend/native_window.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/guest/guest_memory.h"

namespace {
struct WidescreenSettings final : rex::SettingOwner {
  WidescreenSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
  REX_SETTING(std::string, renut_aspect_ratio, "Auto",
              "Aspect ratio the game renders at. Auto = the game window's when the game "
              "starts (the monitor's in fullscreen). Narrower than 16:9 stays 16:9. "
              "Takes effect on restart.",
              rex::Allowed("Auto", "16:9", "21:9", "32:9"));
};
WidescreenSettings &g_widescreen_settings = *new WidescreenSettings();  // never freed, registered by RenutApp::OnConfigure
} // namespace

namespace renut::gpu {

namespace {

constexpr uint32_t kNativeWidth = 1280;
constexpr uint32_t kNativeHeight = 720;
constexpr double kNativeAspect = 16.0 / 9.0;
constexpr double kMaxAspect = 32.0 / 9.0;

// Camera projection aspect modes (sub_82208C50): the active camera's mode at
// +0x6C, where 3 means the console's (0 = 4:3, 1 = 16:9, 2 = 2.35:1).
constexpr u32 kActiveCameraVa = 0x82E1B5F0;
constexpr u32 kCameraAspectModeOffset = 0x6C;
constexpr u32 kConsoleAspectModeVa = 0x82E1B6B8;
constexpr u32 kAspectModeConsole = 3;
constexpr u32 kAspectMode4x3 = 0;

// sub_822E8660's buffers: pools of 1280x736 32bpp texels (0x398000 bytes) and
// a 0x730000-byte one, created once (kPoolsCreatedVa), with their recorded sizes.
constexpr u32 kPoolsCreatedVa = 0x82DF0748;
constexpr u32 kScreenPoolBytes = 0x398000;
constexpr u32 kImagePoolBytes = 0x730000;
constexpr u32 kScratchPoolSizeVa = 0x82DF0710;
constexpr u32 kScratchPoolVa = 0x82DF0714;
constexpr u32 kCopyPoolSizeVa = 0x82DF0718;
constexpr u32 kDepthPoolSizeVa = 0x82DF0730;
// Scratch textures over the scratch pool sized for 1280: the whole screen at its
// start, the split-screen halves at its end.
constexpr u32 kScreenScratchVa = 0x82DF04CC;      // 1280x720
constexpr u32 kHalfWidthScratchVa = 0x82DF0500;   // 640x720
constexpr u32 kTopScratchVa = 0x82DF0534;         // 1280x352
constexpr u32 kBottomScratchVa = 0x82DF0568;      // 1280x368

// sub_823912A8's depth-only quad over L.O.G.'s TV screen: 4 clip-space vertices
// (x, y, z, pad) sized for 16:9, narrowed for wider screens.
constexpr u32 kTvMaskVerticesVa = 0x82DA2960;
constexpr u32 kTvMaskVertexCount = 4;
constexpr u32 kTvMaskVertexStride = 16;

std::atomic<uint32_t> g_screen_width{0};
std::atomic<bool> g_tv_mask_narrowed{false};

// sub_822E8660 is creating its pools on this thread.
thread_local bool t_sizing_pools = false;

u32 LoadU32(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  return p ? p->Get() : 0;
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
  const std::string setting = g_widescreen_settings.renut_aspect_ratio.Get();
  if (setting == "16:9")
    return kNativeAspect;
  if (setting == "21:9")
    return 64.0 / 27.0;
  if (setting == "32:9")
    return 32.0 / 9.0;
  // No game window size yet: 16:9.
  uint32_t width = 0, height = 0;
  if (!GetRenderWindowPixelSize(width, height) || !width || !height)
    return kNativeAspect;
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
// (64-texel rows): what each of sub_822E8660's 1280x736 pools holds.
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
  fetch.dword[0] = (fetch.dword[0].Get() & ~(0x1FFu << 22)) | ((AlignUp(width, 32) >> 5) << 22);
  fetch.dword[1] = (fetch.dword[1].Get() & 0xFFFu) | (base_address & ~0xFFFu);
  fetch.dword[2] = (fetch.dword[2].Get() & ~0x3FFFFFFu) | ((height - 1) << 13) | (width - 1);
}

// After sub_822E8660 made its pools (at the sizes PoolAllocationBytes gave):
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
}

// sub_822E8660's pool allocations, sized for the wider screen; the image pool
// only grows past its recorded size (sub_82558A80 places a texture by it).
u32 PoolAllocationBytes(u32 bytes) {
  if (bytes == kScreenPoolBytes)
    return ScreenPoolBytes();
  if (bytes == kImagePoolBytes)
    return bytes + ScreenPoolBytes() - kScreenPoolBytes;
  return bytes;
}

// Widens the projection aspect for 'mode' by the screen's extra width (Hor+);
// 4:3 keeps its own.
void WidenAspect(PPCContext &ctx, u32 mode) {
  if (mode == kAspectMode4x3 || !Widened())
    return;
  const float scale = float(GuestScreenWidth()) / float(kNativeWidth);
  ctx.f1.f64 = double(float(ctx.f1.f64) * scale);
}

// The TV's quad narrowed as WidenAspect narrows the world on screen: once,
// as the screen width is kept until restart.
void NarrowTvMask() {
  if (!Widened() || g_tv_mask_narrowed.exchange(true))
    return;
  const float scale = float(kNativeWidth) / float(GuestScreenWidth());
  for (u32 i = 0; i < kTvMaskVertexCount; ++i)
    if (auto *x = GuestPtr<be_f32>(kTvMaskVerticesVa + i * kTvMaskVertexStride))
      *x = x->Get() * scale;
}

u32 CameraAspectMode() {
  const u32 camera = LoadU32(kActiveCameraVa);
  const u32 mode = camera ? LoadU32(camera + kCameraAspectModeOffset) : kAspectModeConsole;
  return mode == kAspectModeConsole ? LoadU32(kConsoleAspectModeVa) : mode;
}

// How far right XUI's 1280-wide space sits on the wider screen, in screen
// pixels: in the middle.
float XuiOffset() {
  return Widened() ? float(GuestScreenWidth() - kNativeWidth) * 0.5f : 0.0f;
}

// Keeps XUI's view transform at the vertical scale on both axes and moves it
// right by XuiOffset, so its 1280x720 layout is centred on a wider screen.
void CenterXuiView(u32 matrix_va) {
  if (!Widened())
    return;
  auto *m = GuestPtr<be_f32>(matrix_va);
  if (!m)
    return;
  const float target_width = m[0].Get() * float(kNativeWidth);
  const float target_height = m[5].Get() * float(kNativeHeight);
  const u32 width = GuestScreenWidth();
  if (target_width > float(width) - 1.0f && target_width < float(AlignUp(width, 32)) + 1.0f &&
      std::fabs(target_height - float(kNativeHeight)) < 1.0f) {
    const float scale = m[5].Get();
    m[0] = scale;
    m[12] = m[12].Get() + (float(width) - float(kNativeWidth) * scale) * 0.5f;
  }
}

// TEMP (the blink on opening a save): sub_82239C18's renderer matrices, view at +42208, projection at
// +42336 and their product at +42528; logged while any holds a NaN, and once when they're whole again.
constexpr u32 kRendererView = 42208;
constexpr u32 kRendererProjection = 42336;
constexpr u32 kRendererViewProjection = 42528;
constexpr u32 kFieldOfViewVa = 0x82E1B6B4;
constexpr u32 kCameraNear = 104;
constexpr u32 kCameraFar = 100;

float LoadFloat(u32 va) {
  const auto *p = GuestPtr<const be_f32>(va);
  return p ? p->Get() : 0.0f;
}

bool HasNan(u32 va) {
  for (u32 i = 0; i < 16; ++i)
    if (std::isnan(LoadFloat(va + i * 4)))
      return true;
  return false;
}

std::string Floats(u32 va) {
  std::string text;
  for (u32 i = 0; i < 16; ++i)
    text += (i ? " " : "") + std::to_string(LoadFloat(va + i * 4));
  return text;
}

std::atomic<int> g_nan_logged{0};
std::atomic<bool> g_nan_seen{false};

void LogCameraNan(u32 renderer, u32 caller) {
  const bool view = HasNan(renderer + kRendererView);
  const bool projection = HasNan(renderer + kRendererProjection);
  const bool product = HasNan(renderer + kRendererViewProjection);
  const u32 camera = LoadU32(kActiveCameraVa);
  if (!view && !projection && !product) {
    if (g_nan_seen.exchange(false))
      REXLOG_WARN("[camera nan] whole again: camera {:08X} near {} far {} fov {}", camera,
                  camera ? LoadFloat(camera + kCameraNear) : 0.0f, camera ? LoadFloat(camera + kCameraFar) : 0.0f,
                  LoadFloat(kFieldOfViewVa));
    return;
  }
  g_nan_seen = true;
  if (g_nan_logged.fetch_add(1) >= 24)
    return;
  REXLOG_WARN("[camera nan] from {:08X}: view {} projection {} view-projection {}; camera {:08X} near {} far {} fov {} mode {}",
              caller, view, projection, product, camera, camera ? LoadFloat(camera + kCameraNear) : 0.0f,
              camera ? LoadFloat(camera + kCameraFar) : 0.0f, LoadFloat(kFieldOfViewVa), CameraAspectMode());
  REXLOG_WARN("[camera nan] view [{}]", Floats(renderer + kRendererView));
  REXLOG_WARN("[camera nan] projection [{}]", Floats(renderer + kRendererProjection));
}

// sub_823B7338 (XUI start) is sizing XUI's root on this thread, and the root.
thread_local bool t_sizing_xui_root = false;
std::atomic<u32> g_xui_root{0};
// A marker (sub_825CA958, sub_825AB958: a world position projected to screen
// pixels by sub_823A8438, kept in the safe area) is placing its XUI element.
thread_local bool t_placing_marker = false;

} // namespace

uint32_t GuestScreenWidth() {
  uint32_t width = g_screen_width.load(std::memory_order_acquire);
  if (width)
    return width;
  width = WidthForAspect(ConfiguredAspect());
  uint32_t expected = 0;
  if (g_screen_width.compare_exchange_strong(expected, width, std::memory_order_acq_rel))
    return width;
  return expected;
}

} // namespace renut::gpu

using namespace renut::gpu;

// void sub_823BB7C0(int query, int *mode, DWORD *width, DWORD *height, BOOL *hidef):
// the screen size for the video mode (1280x720 for HD), widened here.
REX_EXTERN(__imp__sub_823BB7C0);
REX_HOOK_RAW(sub_823BB7C0) {
  const u32 width_va = ctx.r5.u32;
  const u32 height_va = ctx.r6.u32;
  __imp__sub_823BB7C0(ctx, base);
  if (width_va && height_va && LoadU32(width_va) == kNativeWidth &&
      LoadU32(height_va) == kNativeHeight)
    StoreU32(width_va, GuestScreenWidth());
}

// void sub_822E8660(): the screen pools and the textures over them, once.
REX_EXTERN(__imp__sub_822E8660);
REX_HOOK_RAW(sub_822E8660) {
  const bool creates = LoadU32(kPoolsCreatedVa) == 0;
  t_sizing_pools = creates && Widened();
  __imp__sub_822E8660(ctx, base);
  if (t_sizing_pools)
    WidenScratchTextures();
  t_sizing_pools = false;
}

// LPVOID sub_82333708(SIZE_T size, int alignment, DWORD protect): the game's
// XPhysicalAlloc.
REX_EXTERN(__imp__sub_82333708);
REX_HOOK_RAW(sub_82333708) {
  if (t_sizing_pools)
    ctx.r3.u64 = PoolAllocationBytes(ctx.r3.u32);
  __imp__sub_82333708(ctx, base);
}

// float sub_82208C50(): the active camera's projection aspect.
REX_EXTERN(__imp__sub_82208C50);
REX_HOOK_RAW(sub_82208C50) {
  __imp__sub_82208C50(ctx, base);
  WidenAspect(ctx, CameraAspectMode());
}

// float sub_822050D0(): the projection aspect for the console's mode.
REX_EXTERN(__imp__sub_822050D0);
REX_HOOK_RAW(sub_822050D0) {
  __imp__sub_822050D0(ctx, base);
  WidenAspect(ctx, LoadU32(kConsoleAspectModeVa));
}

// TEMP: sub_82239C18(renderer, projection): stores the projection and its product with the view.
REX_EXTERN(__imp__sub_82239C18);
REX_HOOK_RAW(sub_82239C18) {
  const u32 renderer = ctx.r3.u32;
  const u32 caller = u32(ctx.lr);
  __imp__sub_82239C18(ctx, base);
  LogCameraNan(renderer, caller);
}

// void sub_823912A8(this): draws the TV's quad (kTvMaskVerticesVa).
REX_EXTERN(__imp__sub_823912A8);
REX_HOOK_RAW(sub_823912A8) {
  NarrowTvMask();
  __imp__sub_823912A8(ctx, base);
}

// HRESULT XuiRenderSetViewTransform(HXUIDC, const D3DXMATRIX *) [0x821D55E8]
REX_EXTERN(__imp__sub_821D55E8);
REX_HOOK_RAW(sub_821D55E8) {
  CenterXuiView(ctx.r4.u32);
  __imp__sub_821D55E8(ctx, base);
}

// int sub_823B7338(app): starts XUI; its root (app + 8) is sized to the back
// buffer with XuiElementSetBounds.
REX_EXTERN(__imp__sub_823B7338);
REX_HOOK_RAW(sub_823B7338) {
  t_sizing_xui_root = true;
  __imp__sub_823B7338(ctx, base);
  t_sizing_xui_root = false;
}

// HRESULT XuiElementSetBounds(HXUIOBJ, float width, float height) [0x82A27180]:
// XUI's root keeps the game's 1280 wide.
REX_EXTERN(__imp__sub_82A27180);
REX_HOOK_RAW(sub_82A27180) {
  if (Widened()) {
    const u32 object = ctx.r3.u32;
    if (t_sizing_xui_root)
      g_xui_root = object;
    if (object && object == g_xui_root.load() &&
        std::fabs(float(ctx.f1.f64) - float(GuestScreenWidth())) < 1.0f)
      ctx.f1.f64 = double(kNativeWidth);
  }
  __imp__sub_82A27180(ctx, base);
}

// A marker's screen position (screen pixels, from the screen's left) goes to
// its element in XUI's centred 1280-wide space.
REX_EXTERN(__imp__sub_825CA958);
REX_HOOK_RAW(sub_825CA958) {
  t_placing_marker = true;
  __imp__sub_825CA958(ctx, base);
  t_placing_marker = false;
}

REX_EXTERN(__imp__sub_825AB958);
REX_HOOK_RAW(sub_825AB958) {
  t_placing_marker = true;
  __imp__sub_825AB958(ctx, base);
  t_placing_marker = false;
}

// HRESULT XuiElementSetPosition(HXUIOBJ, const D3DXVECTOR3 *) [0x82235C88]
REX_EXTERN(__imp__sub_82235C88);
REX_HOOK_RAW(sub_82235C88) {
  if (t_placing_marker && Widened()) {
    if (auto *position = GuestPtr<be_f32>(ctx.r4.u32))
      position[0] = position[0].Get() - XuiOffset();
  }
  __imp__sub_82235C88(ctx, base);
}
