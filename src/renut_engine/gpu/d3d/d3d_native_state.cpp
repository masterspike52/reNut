/**
 * @file    gpu/d3d/d3d_native_state.cpp
 * @brief   N&B's D3D state-setting calls, natively (see d3d_native.h).
 *
 * The render and sampler state setters are the functions of N&B's state
 * tables (off_82E506F0, 101 render states; off_82E50BD0, 20 sampler states:
 * getter, setter, default), each ported from its disassembly. The rest are
 * D3DDevice_Set* and the CDevice internals they share. Their register
 * packets, which N&B's D3D writes into the ring, are left out.
 */
#include "renut_engine/gpu/d3d/d3d_native.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace renut::gpu::native {

namespace {

// m_Pending words.
constexpr u32 kPendingStates = 2;   // the register packets
constexpr u32 kPendingFetch = 3;    // fetch constants, tessellation, points
constexpr u32 kPendingMisc = 4;     // m_MiscPacket and friends

// Render state storage in the CDevice-private area.
constexpr u32 kBlendState = 0x2E48;       // D3D blend: src 0-4, op 5-7, dest 8-12, alpha at 16+
constexpr u32 kBlendFlags = 0x2E4C;       // 31 blend enable, 30 separate alpha, HiZ fields
constexpr u32 kScissorTestEnable = 0x2E50;
constexpr u32 kColorWriteEnable = 0x2E54;  // [4]
constexpr u32 kZEnable = 0x2E64;
constexpr u32 kStencilEnable = 0x2E68;
constexpr u32 kPointSpriteEnable = 0x2E6C;
constexpr u32 kPointSize = 0x2E74;
constexpr u32 kPointSizeMin = 0x2E78;
constexpr u32 kPointSizeMax = 0x2E7C;
constexpr u32 kHighPrecisionBlend = 0x2EFC;  // [4]
constexpr u32 kPresentInterval = 0x351C;
constexpr u32 kBehaviorFlags2 = 0x5DAC;
// Sampler state storage, a byte per sampler.
constexpr u32 kMaxAnisotropy = 0x2E94;
constexpr u32 kMaxMipLevel = 0x2EAE;
constexpr u32 kMinMipLevel = 0x2EC8;
constexpr u32 kZFilter = 0x2EE2;  // bit 0 mag, 1 min, 2 separate Z filtering
// Viewport (SetViewport's arguments, clamped) and the register image values.
constexpr u32 kViewport = 0x3168;  // x, y, width, height, min z, max z, 0
constexpr u32 kVportXScale = 0x2908;
constexpr u32 kVportZScale = 0x2918;
constexpr u32 kStreamDeclStrides = 0x2E38;  // u8[16]
constexpr u32 kVertexDeclaration = 0x2E2C;
constexpr u32 kVertexShader = 0x3198;
constexpr u32 kPixelShader = 0x3194;
constexpr u32 kGprAllocation = 0x2AA8;
constexpr u32 kScreenExtentQueryMode = 0x2AA4;

constexpr float kInv255 = 0.003921568859368563f;  // flt_82000D08

// D3DMAXANISOTROPY to the fetch constant's aniso filter (dword_82003E38).
constexpr std::array<u32, 17> kAnisoFilter = {0, 0, 2, 2, 3, 3, 3, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5};

float AsFloat(u32 bits) {
  return std::bit_cast<float>(bits);
}

constexpr u64 Bit(u32 n) {
  return u64(1) << n;
}

// The sampler's fetch constant dirty bit (D3DTag_FetchConstant).
constexpr u64 FetchBit(u32 fetch_constant) {
  return Bit(31 - fetch_constant);
}

u32 Fetch(u32 sampler, u32 dword) {
  return off::TextureFetch + sampler * 24 + dword * 4;
}

// ---------------------------------------------------------------------------
// Blend
// ---------------------------------------------------------------------------

// RB_BLENDCONTROL from the blend state: without separate alpha blending the
// alpha fields repeat the color ones.
u32 BlendControl(u32 blend, bool separate_alpha) {
  if (separate_alpha)
    return blend;
  u32 alpha = (blend & 0x1010) | ((blend & 0xFFFF) << 4);
  alpha = (alpha << 12) & 0xFFFF0000u;
  alpha &= ~0x00100000u;
  alpha &= ~0x10000000u;
  return alpha | (blend & 0xFFFF);
}

void StoreBlendControl(Device d, u32 value, bool separate_order) {
  // The two orders N&B's setters store in; the values are the same.
  if (separate_order) {
    d.SetW(off::BlendControl0, value);
    d.SetW(off::BlendControl1, value);
    d.SetW(off::BlendControl2, value);
    d.SetW(off::BlendControl3, value);
  } else {
    d.SetW(off::BlendControl2, value);
    d.SetW(off::BlendControl0, value);
    d.SetW(off::BlendControl1, value);
    d.SetW(off::BlendControl3, value);
  }
  d.Dirty(kPendingStates, 0x400 | 4 | 2 | 1);
}

// SrcBlend/DestBlend/BlendOp (color): applied while blending is enabled.
void SetBlendColorField(Device d, u32 value, u32 shift, u32 width) {
  d.SetW(kBlendState, Insert(d.W(kBlendState), value, shift, width));
  const u32 flags = d.W(kBlendFlags);
  if (!(flags & 0x80000000u))
    return;
  StoreBlendControl(d, BlendControl(d.W(kBlendState), (flags & 0x40000000u) != 0), true);
}

// The alpha fields: applied while blending with separate alpha.
void SetBlendAlphaField(Device d, u32 value, u32 shift, u32 width) {
  const u32 blend = Insert(d.W(kBlendState), value, shift, width);
  d.SetW(kBlendState, blend);
  const u32 flags = d.W(kBlendFlags);
  if (!(flags & 0x80000000u) || !(flags & 0x40000000u))
    return;
  StoreBlendControl(d, blend, true);
}

// ---------------------------------------------------------------------------
// Render states (off_82E506F0 order)
// ---------------------------------------------------------------------------

void RsNone(Device, u32) {}

void RsZEnable(Device d, u32 v) {  // 0x8264C140
  d.SetW(kZEnable, v);
  if (!d.W(off::DepthStencil))
    v = 0;
  d.SetW(off::DepthControl, Insert(d.W(off::DepthControl), v, 1, 1));
  d.Dirty(kPendingStates, 0x800 | 0x20000);
}
void RsZFunc(Device d, u32 v) {  // 0x821FB7B0
  d.SetW(off::DepthControl, Insert(d.W(off::DepthControl), v, 4, 3));
  d.Dirty(kPendingStates, 0x800 | 0x20000);
}
void RsZWriteEnable(Device d, u32 v) {  // 0x8264C180
  d.SetW(off::DepthControl, Insert(d.W(off::DepthControl), v, 2, 1));
  d.Dirty(kPendingStates, 0x800);
}
void RsFillMode(Device d, u32 v) {  // 0x8228F330
  d.SetW(off::ModeControl, Insert(d.W(off::ModeControl), v, 3, 8));
  d.Dirty(kPendingStates, 0x40);
}
void RsCullMode(Device d, u32 v) {  // 0x8228F350
  d.SetW(off::ModeControl, (d.W(off::ModeControl) & ~7u) | (v & 7));
  d.Dirty(kPendingStates, 0x40);
}
void RsAlphaBlendEnable(Device d, u32 v) {  // 0x8222A718
  const u32 flags = Insert(d.W(kBlendFlags), v, 31, 1);
  d.SetW(kBlendFlags, flags);
  u32 control = BlendControl(d.W(kBlendState), (flags & 0x40000000u) != 0);
  if (!v)
    control = 0x10001;
  StoreBlendControl(d, control, false);
}
void RsSeparateAlphaBlendEnable(Device d, u32 v) {  // 0x8222A690
  const u32 flags = Insert(d.W(kBlendFlags), v, 30, 1);
  d.SetW(kBlendFlags, flags);
  u32 control = BlendControl(d.W(kBlendState), v != 0);
  if (!(flags >> 31))
    control = 0x10001;
  StoreBlendControl(d, control, true);
}
void RsBlendFactor(Device d, u32 v) {  // 0x8226E8E0
  d.SetF(0x28E8, float(v & 0xFF) * kInv255);           // blue
  d.SetF(0x28E4, float((v >> 8) & 0xFF) * kInv255);    // green
  d.SetF(0x28E0, float((v >> 16) & 0xFF) * kInv255);   // red
  d.SetF(0x28EC, float(v >> 24) * kInv255);            // alpha
  d.Dirty(kPendingStates, u64(0xF) << 33);
}
void RsSrcBlend(Device d, u32 v) {  // 0x82230090
  SetBlendColorField(d, v, 0, 5);
}
void RsDestBlend(Device d, u32 v) {  // 0x822307B8
  SetBlendColorField(d, v, 8, 5);
}
void RsBlendOp(Device d, u32 v) {  // 0x82226380
  SetBlendColorField(d, v, 5, 3);
}
void RsSrcBlendAlpha(Device d, u32 v) {  // 0x822672B8
  SetBlendAlphaField(d, v, 16, 5);
}
void RsDestBlendAlpha(Device d, u32 v) {  // 0x822208E8
  SetBlendAlphaField(d, v, 24, 5);
}
void RsBlendOpAlpha(Device d, u32 v) {  // 0x8221F5F8
  SetBlendAlphaField(d, v, 21, 3);
}
void RsAlphaTestEnable(Device d, u32 v) {  // 0x8264BE98
  d.SetW(off::ColorControl, Insert(d.W(off::ColorControl), v, 3, 1));
  d.Dirty(kPendingStates, 0x200 | 0x40000);
}
void RsAlphaRef(Device d, u32 v) {  // 0x8264BF50
  d.SetF(0x2904, float(v) * kInv255);
  d.Dirty(kPendingStates, 0x8000000);
}
void RsAlphaFunc(Device d, u32 v) {  // 0x8264BFB8
  d.SetW(off::ColorControl, (d.W(off::ColorControl) & ~7u) | (v & 7));
  d.Dirty(kPendingStates, 0x200);
}
void RsStencilEnable(Device d, u32 v) {  // 0x8264C1C0
  d.SetW(kStencilEnable, v);
  if (!d.W(off::DepthStencil))
    v = 0;
  d.SetW(off::DepthControl, Insert(d.W(off::DepthControl), v, 0, 1));
  d.Dirty(kPendingStates, 0x800 | 0x20000);
}

template <u32 Shift, u64 Bits>
void RsDepthControlField(Device d, u32 v) {
  d.SetW(off::DepthControl, Insert(d.W(off::DepthControl), v, Shift, 3));
  d.Dirty(kPendingStates, Bits);
}

void RsTwoSidedStencilMode(Device d, u32 v) {  // 0x8264C200
  d.SetW(off::DepthControl, Insert(d.W(off::DepthControl), v, 7, 1));
  d.Dirty(kPendingStates, 0x800 | 0x20000);
}
void RsStencilRef(Device d, u32 v) {  // 0x8264C3D8
  d.SetB(0x2903, u8(v));
  d.Dirty(kPendingStates, 0x10000000);
}
void RsStencilMask(Device d, u32 v) {  // 0x8264C3F8
  d.SetB(0x2902, u8(v));
  d.Dirty(kPendingStates, 0x10000000);
}
void RsStencilWriteMask(Device d, u32 v) {  // 0x8264C418
  d.SetB(0x2901, u8(v));
  d.Dirty(kPendingStates, 0x10000000);
}
void RsCCWStencilRef(Device d, u32 v) {  // 0x8264C438
  d.SetB(0x28FF, u8(v));
  d.Dirty(kPendingStates, 0x20000000);
}
void RsCCWStencilMask(Device d, u32 v) {  // 0x8264C458
  d.SetB(0x28FE, u8(v));
  d.Dirty(kPendingStates, 0x20000000);
}
void RsCCWStencilWriteMask(Device d, u32 v) {  // 0x8264C478
  d.SetB(0x28FD, u8(v));
  d.Dirty(kPendingStates, 0x20000000);
}
void RsClipPlaneEnable(Device d, u32 v) {  // 0x8228F370
  d.SetW(off::ClipControl, (d.W(off::ClipControl) & ~0x3Fu) | v);
  d.SetW(0x28B4, v ? 0x1000u : 0u);
  d.Dirty(kPendingStates, 0x80 | Bit(44));
}
void RsPointSize(Device d, u32 v) {  // 0x8264C6C0
  d.SetW(kPointSize, v);
  const u16 size = u16(TruncateToI32(AsFloat(v) * 8.0f));
  d.SetH(0x2966, size);
  d.SetH(0x2964, size);
  d.Dirty(kPendingFetch, Bit(54));
}
void RsPointSizeMin(Device d, u32 v) {  // 0x8264C720
  d.SetW(kPointSizeMin, v);
  d.SetH(0x296A, u16(TruncateToI32(AsFloat(v) * 16.0f)));
  d.Dirty(kPendingFetch, Bit(53));
}
void RsPointSpriteEnable(Device d, u32 v) {  // 0x8264C6B0
  d.SetW(kPointSpriteEnable, v);
}
void RsPointSizeMax(Device d, u32 v) {  // 0x822687D0
  d.SetW(kPointSizeMax, v);
  d.SetH(0x2968, u16(TruncateToI32(AsFloat(v) * 16.0f)));
  d.Dirty(kPendingFetch, Bit(53));
}
void RsMultiSampleAntiAlias(Device d, u32 v) {  // 0x8264C598
  d.SetW(off::ModeControl, Insert(d.W(off::ModeControl), v, 15, 1));
  d.Dirty(kPendingStates, 0x40);
}
void RsMultiSampleMask(Device d, u32 v) {  // 0x8264C5C8
  d.SetW(0x2A00, v & 0xFFFF);
  d.Dirty(kPendingMisc, 0x80000);
}
void RsScissorTestEnable(Device d, u32 v) {  // 0x8264C4A8
  d.SetW(kScissorTestEnable, v);
  SetScissorRect(d, i32(d.W(off::ScissorRect)), i32(d.W(off::ScissorRect + 4)),
                 i32(d.W(off::ScissorRect + 8)), i32(d.W(off::ScissorRect + 12)));
}
void RsSlopeScaleDepthBias(Device d, u32 v) {  // 0x8264C4C0
  const float scale = AsFloat(v) * 16.0f;
  u32 mode = d.W(off::ModeControl);
  mode = Insert(mode, (scale != 0.0f || d.F(0x2A54) != 0.0f) ? 1 : 0, 11, 1);
  const bool back = scale != 0.0f || d.F(0x2A5C) != 0.0f;
  d.SetF(0x2A50, scale);
  mode = Insert(mode, back ? 1 : 0, 12, 1);
  d.SetF(0x2A58, scale);
  d.SetW(off::ModeControl, mode);
  d.Dirty(kPendingMisc, Bit(45) | Bit(43));
  d.Dirty(kPendingStates, 0x40);
}
void RsDepthBias(Device d, u32 v) {  // 0x821FB718
  const float bias = AsFloat(v);
  u32 mode = d.W(off::ModeControl);
  mode = Insert(mode, (d.F(0x2A50) != 0.0f || bias != 0.0f) ? 1 : 0, 11, 1);
  const bool back = d.F(0x2A58) != 0.0f || bias != 0.0f;
  d.SetF(0x2A54, bias);
  mode = Insert(mode, back ? 1 : 0, 12, 1);
  d.SetF(0x2A5C, bias);
  d.SetW(off::ModeControl, mode);
  d.Dirty(kPendingMisc, Bit(44) | Bit(42));
  d.Dirty(kPendingStates, 0x40);
}

template <u32 Target>
void RsColorWriteEnable(Device d, u32 v) {  // 0x821FB6E0, 0x8264C5F0, 0x8264C630, 0x8264C670
  d.SetW(kColorWriteEnable + Target * 4, v);
  if (!d.W(off::RenderTargets + Target * 4))
    v = 0;
  d.SetW(off::ColorMask, Insert(d.W(off::ColorMask), v, Target * 4, 4));
  d.Dirty(kPendingStates, Bit(37));
}

void RsTessellationMode(Device d, u32 v) {  // 0x8264CDC0
  d.SetW(0x2978, (d.W(0x2978) & ~3u) | (v & 3));
  d.Dirty(kPendingFetch, Bit(49));
}
void RsMinTessellationLevel(Device d, u32 v) {  // 0x8264CD50
  d.SetW(0x2980, v);
  d.Dirty(kPendingFetch, Bit(47));
}
void RsMaxTessellationLevel(Device d, u32 v) {  // 0x8264CD88
  d.SetW(0x297C, v);
  d.Dirty(kPendingFetch, Bit(48));
}

// Wrap0-7 in PA_SU_WRAPPING0 (0x292C), Wrap8-15 in WRAPPING1 (0x2930). The
// first of each word ORs the value in unmasked, as N&B's code does.
template <u32 Index>
void RsWrap(Device d, u32 v) {  // 0x8264C780 + ...
  constexpr u32 kWord = Index < 8 ? 0x292C : 0x2930;
  constexpr u32 kShift = (Index % 8) * 4;
  u32 word = d.W(kWord);
  if constexpr (kShift == 0)
    word = (word & ~0xFu) | v;
  else if constexpr (kShift == 28)
    word = Insert(word, v, 28, 4);
  else
    word = (word & ~(0xFu << kShift)) | (v << kShift);
  d.SetW(kWord, word);
  d.Dirty(kPendingStates, Index < 8 ? 0x2000 : 0x1000);
}

void RsViewportEnable(Device d, u32 v) {  // 0x82205A80
  d.SetW(0x294C, v ? 0x43Fu : 0x400u);
  d.SetW(off::ClipControl, Insert(d.W(off::ClipControl), v ? 0 : 1, 16, 1));
  d.Dirty(kPendingStates, 0x20 | 0x80);
}

// RB_COLOR*_INFO's format for high precision blending (2, 3, 10 and 12 have
// an alternative), as HighPrecisionBlendEnable and SetRenderTarget apply it.
// 'slot' is the target's register (0, 2, 3, 4: DepthInfo sits at 1).
void ApplyHighPrecisionBlend(Device d, u32 slot, u32 enable) {
  const u32 offset = off::Color0Info + slot * 4;
  const u32 info = d.W(offset);
  const u32 format = Extract(info, 16, 4);
  if (format != 2 && format != 3 && format != 10 && format != 12)
    return;
  if ((Extract(info, 19, 1) ^ enable) == 0)
    return;
  const u32 enabled = ~(enable - 1);
  const u32 low = ((format >> 1) - 3) & (enable - 1);
  u32 value = ((enabled << 16) & ((format + 3) << 17)) | (low << 16);
  value = (info & ~0xF0000u) | (value & 0xF0000u);
  d.SetW(offset, value);
  d.Dirty(kPendingStates, Bit(56 - slot));
}

template <u32 Target>
void RsHighPrecisionBlendEnable(Device d, u32 v) {  // 0x8264CAF0 + 0x98 * Target
  d.SetW(kHighPrecisionBlend + Target * 4, v);
  if (!d.W(off::RenderTargets + Target * 4))
    return;
  ApplyHighPrecisionBlend(d, Target ? Target + 1 : 0, v);
}

void RsHalfPixelOffset(Device d, u32 v) {  // 0x8264CDF8
  d.SetW(0x29C0, (d.W(0x29C0) & ~1u) | (v & 1));
  d.Dirty(kPendingMisc, Bit(35));
}
void RsPrimitiveResetEnable(Device d, u32 v) {  // 0x8264CE30
  d.SetW(off::ModeControl, Insert(d.W(off::ModeControl), v, 21, 1));
  d.Dirty(kPendingStates, 0x40);
}
void RsPrimitiveResetIndex(Device d, u32 v) {  // 0x8264CE60
  d.SetW(0x28D8, v);
  d.Dirty(kPendingStates, Bit(38));
}
void RsAlphaToMaskEnable(Device d, u32 v) {  // 0x8264CE88
  d.SetW(off::ColorControl, Insert(d.W(off::ColorControl), v, 4, 1));
  d.Dirty(kPendingStates, 0x200);
}
void RsAlphaToMaskOffsets(Device d, u32 v) {  // 0x8264CEB8
  d.SetW(off::ColorControl, Insert(d.W(off::ColorControl), v, 24, 8));
  d.Dirty(kPendingStates, 0x200);
}
void RsGuardBandX(Device d, u32 v) {  // 0x8264CEF0
  d.SetW(0x29CC, v);
  d.Dirty(kPendingMisc, Bit(32));
}
void RsGuardBandY(Device d, u32 v) {  // 0x8264CF20
  d.SetW(0x29C4, v);
  d.Dirty(kPendingMisc, Bit(34));
}
void RsDiscardBandX(Device d, u32 v) {  // 0x8264CF50
  d.SetW(0x29D0, v);
  d.Dirty(kPendingMisc, 0x80000000u);
}
void RsDiscardBandY(Device d, u32 v) {  // 0x8264CF78
  d.SetW(0x29C8, v);
  d.Dirty(kPendingMisc, Bit(33));
}
void RsHiStencilEnable(Device d, u32 v) {  // 0x8264CFE8
  d.SetW(off::HiControl, Insert(d.W(off::HiControl), v, 3, 1));
  d.Dirty(kPendingStates, 0x100);
}
void RsHiStencilWriteEnable(Device d, u32 v) {  // 0x8264D018
  d.SetW(off::HiControl, Insert(d.W(off::HiControl), v, 2, 1));
  d.Dirty(kPendingStates, 0x100);
}
void RsHiStencilFunc(Device d, u32 v) {  // 0x8264D048
  d.SetW(off::HiControl, Insert(d.W(off::HiControl), v, 5, 1));
  d.Dirty(kPendingStates, 0x100);
}
void RsHiStencilRef(Device d, u32 v) {  // 0x8264D078
  d.SetB(0x2942, u8(v));
  d.Dirty(kPendingStates, 0x100);
}
void RsPresentInterval(Device d, u32 v) {  // 0x8264D098
  d.SetW(kPresentInterval, v);
}
void RsPresentImmediateThreshold(Device d, u32 v) {  // 0x8264D0A8
  d.SetW(kBlendFlags, Insert(d.W(kBlendFlags), v, 23, 7));
}
void RsHiZEnable(Device d, u32 v) {  // 0x8264CFA8
  d.SetW(kBlendFlags, Insert(d.W(kBlendFlags), v, 20, 3));
  d.Dirty(kPendingStates, 0x20000);
}
void RsHiZWriteEnable(Device d, u32 v) {  // 0x822899D0
  const u32 flags = Insert(d.W(kBlendFlags), v, 17, 3);
  d.SetW(kBlendFlags, flags);
  u64 predicated = d.Q(off::PredicatedPending2);
  if (!(d.B(off::Flags3) & 0x20) && (flags & 0xE0000) == 0x40000)
    predicated &= ~u64(0x100);
  else
    predicated |= 0x100;
  d.SetQ(off::PredicatedPending2, predicated);
  d.Dirty(kPendingStates, 0x100);
}
void RsLastPixel(Device d, u32 v) {  // 0x8264C060
  d.SetW(0x29B8, Insert(d.W(0x29B8), v, 10, 1));
  d.Dirty(kPendingMisc, Bit(37));
}
void RsLineWidth(Device d, u32 v) {  // 0x8264C098
  d.SetH(0x296E, u16(TruncateToI64(AsFloat(v) * 8.0f)));
  d.Dirty(kPendingFetch, Bit(52));
}
void RsBuffer2Frames(Device d, u32 v) {  // 0x8264C110
  const u32 flags = d.W(kBehaviorFlags2);
  d.SetW(kBehaviorFlags2, v ? (flags | 1) : (flags & ~1u));
}

using RenderStateFn = void (*)(Device, u32);
constexpr u32 kRenderStates = 101;
constexpr RenderStateFn kRenderStateFns[kRenderStates] = {
    RsNone, RsNone, RsNone, RsNone, RsNone, RsNone, RsNone, RsNone, RsNone, RsNone,  // 0-9
    RsZEnable,                                   // 10
    RsZFunc,                                     // 11
    RsZWriteEnable,                              // 12
    RsFillMode,                                  // 13
    RsCullMode,                                  // 14
    RsAlphaBlendEnable,                          // 15
    RsSeparateAlphaBlendEnable,                  // 16
    RsBlendFactor,                               // 17
    RsSrcBlend,                                  // 18
    RsDestBlend,                                 // 19
    RsBlendOp,                                   // 20
    RsSrcBlendAlpha,                             // 21
    RsDestBlendAlpha,                            // 22
    RsBlendOpAlpha,                              // 23
    RsAlphaTestEnable,                           // 24
    RsAlphaRef,                                  // 25
    RsAlphaFunc,                                 // 26
    RsStencilEnable,                             // 27
    RsTwoSidedStencilMode,                       // 28
    RsDepthControlField<11, 0x800 | 0x20000>,    // 29 StencilFail
    RsDepthControlField<17, 0x800 | 0x20000>,    // 30 StencilZFail
    RsDepthControlField<14, 0x800>,              // 31 StencilPass
    RsDepthControlField<8, 0x800>,               // 32 StencilFunc
    RsStencilRef,                                // 33
    RsStencilMask,                               // 34
    RsStencilWriteMask,                          // 35
    RsDepthControlField<23, 0x800 | 0x20000>,    // 36 CCWStencilFail
    RsDepthControlField<29, 0x800 | 0x20000>,    // 37 CCWStencilZFail
    RsDepthControlField<26, 0x800>,              // 38 CCWStencilPass
    RsDepthControlField<20, 0x800>,              // 39 CCWStencilFunc
    RsCCWStencilRef,                             // 40
    RsCCWStencilMask,                            // 41
    RsCCWStencilWriteMask,                       // 42
    RsClipPlaneEnable,                           // 43
    RsPointSize,                                 // 44
    RsPointSizeMin,                              // 45
    RsPointSpriteEnable,                         // 46
    RsPointSizeMax,                              // 47
    RsMultiSampleAntiAlias,                      // 48
    RsMultiSampleMask,                           // 49
    RsScissorTestEnable,                         // 50
    RsSlopeScaleDepthBias,                       // 51
    RsDepthBias,                                 // 52
    RsColorWriteEnable<0>,                       // 53
    RsColorWriteEnable<1>,                       // 54
    RsColorWriteEnable<2>,                       // 55
    RsColorWriteEnable<3>,                       // 56
    RsTessellationMode,                          // 57
    RsMinTessellationLevel,                      // 58
    RsMaxTessellationLevel,                      // 59
    RsWrap<0>, RsWrap<1>, RsWrap<2>, RsWrap<3>, RsWrap<4>, RsWrap<5>, RsWrap<6>, RsWrap<7>,
    RsWrap<8>, RsWrap<9>, RsWrap<10>, RsWrap<11>, RsWrap<12>, RsWrap<13>, RsWrap<14>,
    RsWrap<15>,                                  // 60-75
    RsViewportEnable,                            // 76
    RsHighPrecisionBlendEnable<0>,               // 77
    RsHighPrecisionBlendEnable<1>,               // 78
    RsHighPrecisionBlendEnable<2>,               // 79
    RsHighPrecisionBlendEnable<3>,               // 80
    RsHalfPixelOffset,                           // 81
    RsPrimitiveResetEnable,                      // 82
    RsPrimitiveResetIndex,                       // 83
    RsAlphaToMaskEnable,                         // 84
    RsAlphaToMaskOffsets,                        // 85
    RsGuardBandX,                                // 86
    RsGuardBandY,                                // 87
    RsDiscardBandX,                              // 88
    RsDiscardBandY,                              // 89
    RsHiStencilEnable,                           // 90
    RsHiStencilWriteEnable,                      // 91
    RsHiStencilFunc,                             // 92
    RsHiStencilRef,                              // 93
    RsPresentInterval,                           // 94
    RsPresentImmediateThreshold,                 // 95
    RsHiZEnable,                                 // 96
    RsHiZWriteEnable,                            // 97
    RsLastPixel,                                 // 98
    RsLineWidth,                                 // 99
    RsBuffer2Frames,                             // 100
};

// ---------------------------------------------------------------------------
// Sampler states (off_82E50BD0 order)
// ---------------------------------------------------------------------------

// The texture's Z filter (fetch dword 4, bits 0-1) from the min/mag filters
// in dword 3, or from the separate Z filter state when that is enabled.
u32 ZFilterBits(u32 dword3, u8 z_state) {
  const u32 folded = (dword3 & 0x80080000u) | ((dword3 >> 1) & 0x7FF7FFFFu);
  const u32 separate_mask = (u32(z_state) >> 2) - 1;  // 0 when separate, ~0 when not
  return ((Extract(folded, 19, 12) & separate_mask) + (u32(z_state) & ~separate_mask)) & 3;
}

void SsAddress(Device d, u32 s, u32 v, u32 shift) {
  d.SetW(Fetch(s, 0), Insert(d.W(Fetch(s, 0)), v, shift, 3));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsAddressU(Device d, u32 s, u32 v) {  // 0x8227D2D0
  SsAddress(d, s, v, 10);
}
void SsAddressV(Device d, u32 s, u32 v) {  // 0x8220C4E0
  SsAddress(d, s, v, 13);
}
void SsAddressW(Device d, u32 s, u32 v) {  // 0x8264D630
  SsAddress(d, s, v, 16);
}
void SsBorderColor(Device d, u32 s, u32 v) {  // 0x8264D590
  d.SetW(Fetch(s, 5), (d.W(Fetch(s, 5)) & ~3u) | (v ? 1u : 0u));
  d.Dirty(kPendingFetch, FetchBit(s));
}
// MagFilter (filter at dword 3 bits 19-20, aniso flag at dword 4 bit 10) and
// MinFilter (21-22, flag at 11); the aniso filter (dword 3 bits 25-27) is set
// while either is anisotropic.
void SsFilter(Device d, u32 s, u32 v, bool mag) {  // 0x8222F1B8 / 0x8222F280
  const u32 aniso = kAnisoFilter[d.B(kMaxAnisotropy + s) % kAnisoFilter.size()];
  const u32 anisotropic = v >> 2;
  const u32 dword4 = d.W(Fetch(s, 4));
  const u32 other = Extract(dword4, mag ? 11 : 10, 1);
  const u32 new4 = Insert(dword4, anisotropic, mag ? 10 : 11, 1);
  d.SetW(Fetch(s, 4), new4);
  const u32 aniso_field = aniso & ~((other | anisotropic) - 1);
  u32 dword3 = d.W(Fetch(s, 3));
  if (mag) {
    const u32 bits = (aniso_field << 6) | anisotropic | v;
    dword3 = Insert(dword3, bits, 19, 2);
    dword3 = (dword3 & ~0x0E000000u) | ((bits << 19) & 0x0E000000u);
  } else {
    const u32 bits = (aniso_field << 4) | anisotropic | v;
    dword3 = Insert(dword3, bits, 21, 2);
    dword3 = (dword3 & ~0x0E000000u) | ((bits << 21) & 0x0E000000u);
  }
  d.SetW(Fetch(s, 3), dword3);
  d.SetW(Fetch(s, 4), (new4 & ~3u) | ZFilterBits(dword3, d.B(kZFilter + s)));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsMagFilter(Device d, u32 s, u32 v) {
  SsFilter(d, s, v, true);
}
void SsMinFilter(Device d, u32 s, u32 v) {
  SsFilter(d, s, v, false);
}
void SsMipFilter(Device d, u32 s, u32 v) {  // 0x8228F2F0
  d.SetW(Fetch(s, 3), Insert(d.W(Fetch(s, 3)), v, 23, 2));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsMipMapLodBias(Device d, u32 s, u32 v) {  // 0x8264D460
  const u32 bias = u32(TruncateToI64(AsFloat(v) * 32.0f));
  d.SetW(Fetch(s, 4), Insert(d.W(Fetch(s, 4)), bias, 12, 10));
  d.Dirty(kPendingFetch, FetchBit(s));
}
// MaxMipLevel / MinMipLevel, limited to the bound texture's own mip range.
void SsMaxMipLevel(Device d, u32 s, u32 v) {  // 0x8222F590
  if (const u32 texture = d.W(off::Textures + s * 4)) {
    const auto *format = GuestPtr<const be_u32>(texture + 0x2C);
    u32 level = format ? Extract(*format, 2, 4) : 0;
    if (level <= v)
      level = v;
    d.SetW(Fetch(s, 4), Insert(d.W(Fetch(s, 4)), level, 2, 4));
    d.Dirty(kPendingFetch, FetchBit(s));
  }
  d.SetB(kMaxMipLevel + s, u8(v));
}
void SsMinMipLevel(Device d, u32 s, u32 v) {  // 0x8264D510
  if (const u32 texture = d.W(off::Textures + s * 4)) {
    const auto *format = GuestPtr<const be_u32>(texture + 0x2C);
    u32 level = format ? Extract(*format, 6, 4) : 0;
    if (level >= v)
      level = v;
    d.SetW(Fetch(s, 4), Insert(d.W(Fetch(s, 4)), level, 6, 4));
    d.Dirty(kPendingFetch, FetchBit(s));
  }
  d.SetB(kMinMipLevel + s, u8(v));
}
void SsMaxAnisotropy(Device d, u32 s, u32 v) {  // 0x8264D340
  const u32 dword4 = d.W(Fetch(s, 4));
  if ((dword4 & 0x800) || (dword4 & 0x400)) {
    d.SetW(Fetch(s, 3), Insert(d.W(Fetch(s, 3)), kAnisoFilter[v % kAnisoFilter.size()], 25, 3));
    d.Dirty(kPendingFetch, FetchBit(s));
  }
  d.SetB(kMaxAnisotropy + s, u8(v));
}
// MagFilterZ / MinFilterZ / SeparateZFilterEnable: bits 0 / 1 / 2 of the
// sampler's Z filter state, then the fetch constant's Z filter.
void SsZFilterState(Device d, u32 s, u32 v, u32 bit) {  // 0x8264D1E0 / 0x8264D100 / 0x8264D298
  const u8 z_state = u8((d.B(kZFilter + s) & ~(1u << bit)) | (v << bit));
  const u32 dword3 = d.W(Fetch(s, 3));
  d.SetW(Fetch(s, 4), (d.W(Fetch(s, 4)) & ~3u) | ZFilterBits(dword3, z_state));
  d.SetB(kZFilter + s, z_state);
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsMagFilterZ(Device d, u32 s, u32 v) {
  SsZFilterState(d, s, v, 0);
}
void SsMinFilterZ(Device d, u32 s, u32 v) {
  SsZFilterState(d, s, v, 1);
}
void SsSeparateZFilterEnable(Device d, u32 s, u32 v) {
  SsZFilterState(d, s, v, 2);
}
void SsTrilinearThreshold(Device d, u32 s, u32 v) {  // 0x8264D680
  d.SetW(Fetch(s, 5), Insert(d.W(Fetch(s, 5)), v, 3, 2));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsAnisotropyBias(Device d, u32 s, u32 v) {  // 0x8264D3B8
  const u32 bias = u32(TruncateToI64(AsFloat(v) * -8.0f));
  d.SetW(Fetch(s, 5), Insert(d.W(Fetch(s, 5)), bias, 5, 4));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsHGradientExpBias(Device d, u32 s, u32 v) {  // 0x8264D6D8
  d.SetW(Fetch(s, 4), Insert(d.W(Fetch(s, 4)), v, 22, 5));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsVGradientExpBias(Device d, u32 s, u32 v) {  // 0x8264D730
  d.SetW(Fetch(s, 4), Insert(d.W(Fetch(s, 4)), v, 27, 5));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsWhiteBorderColorW(Device d, u32 s, u32 v) {  // 0x8264D788
  d.SetW(Fetch(s, 5), Insert(d.W(Fetch(s, 5)), v, 2, 1));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsPointBorderEnable(Device d, u32 s, u32 v) {  // 0x8264D7E0
  d.SetW(Fetch(s, 1), Insert(d.W(Fetch(s, 1)), v ? 0 : 1, 11, 1));
  d.Dirty(kPendingFetch, FetchBit(s));
}

using SamplerStateFn = void (*)(Device, u32, u32);
constexpr u32 kSamplerStates = 20;
constexpr SamplerStateFn kSamplerStateFns[kSamplerStates] = {
    SsAddressU,             // 0
    SsAddressV,             // 1
    SsAddressW,             // 2
    SsBorderColor,          // 3
    SsMagFilter,            // 4
    SsMinFilter,            // 5
    SsMipFilter,            // 6
    SsMipMapLodBias,        // 7
    SsMaxMipLevel,          // 8
    SsMaxAnisotropy,        // 9
    SsMagFilterZ,           // 10
    SsMinFilterZ,           // 11
    SsSeparateZFilterEnable,  // 12
    SsMinMipLevel,          // 13
    SsTrilinearThreshold,   // 14
    SsAnisotropyBias,       // 15
    SsHGradientExpBias,     // 16
    SsVGradientExpBias,     // 17
    SsWhiteBorderColorW,    // 18
    SsPointBorderEnable,    // 19
};

// N&B's state tables: {getter, setter, default} per state.
constexpr u32 kRenderStateTable = 0x82E506F0;
constexpr u32 kSamplerStateTable = 0x82E50BD0;
constexpr u32 kTextureSamplers = 26;

u32 TableWord(u32 table, u32 index, u32 field) {
  const auto *p = GuestPtr<const be_u32>(table + index * 12 + field * 4);
  return p ? u32(*p) : 0;
}

}  // namespace

// ---------------------------------------------------------------------------

void SetRenderState(Device device, u32 state, u32 value) {
  if (device && state < kRenderStates)
    kRenderStateFns[state](device, value);
}

void SetSamplerState(Device device, u32 sampler, u32 state, u32 value) {
  if (device && sampler < kTextureSamplers && state < kSamplerStates)
    kSamplerStateFns[state](device, sampler, value);
}

// The getters (0x8264BE78-0x8264D840), each a read of the state's storage.
// Entries 0-9 of the table share a getter returning 0.
u32 GetRenderState(Device d, u32 state) {
  if (!d)
    return 0;
  auto bits = [&d](u32 offset, u32 shift, u32 width) { return Extract(d.W(offset), shift, width); };
  // A [0, 1] float as a byte: fmadds by 255 and 0.5 (flt_821258F4, flt_82000C74), fctidz.
  auto byte = [](float v) { return u32(TruncateToI64(std::fma(v, 255.0f, 0.5f))); };
  switch (state) {
    case 10: return d.W(kZEnable);
    case 11: return bits(off::DepthControl, 4, 3);   // ZFunc
    case 12: return bits(off::DepthControl, 2, 1);   // ZWriteEnable
    case 13: return bits(off::ModeControl, 3, 8);    // FillMode
    case 14: return bits(off::ModeControl, 0, 3);    // CullMode
    case 15: return d.W(kBlendFlags) >> 31;          // AlphaBlendEnable
    case 16: return bits(kBlendFlags, 30, 1);        // SeparateAlphaBlendEnable
    case 17: {  // BlendFactor (sub_8264BFE8): ARGB from the RGBA floats
      const u32 r = byte(d.F(0x28E0)), g = byte(d.F(0x28E4)), b = byte(d.F(0x28E8));
      const u32 a = byte(d.F(0x28EC));
      return (a << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
    }
    case 18: return bits(kBlendState, 0, 5);         // SrcBlend
    case 19: return bits(kBlendState, 8, 5);         // DestBlend
    case 20: return bits(kBlendState, 5, 3);         // BlendOp
    case 21: return bits(kBlendState, 16, 5);        // SrcBlendAlpha
    case 22: return bits(kBlendState, 24, 5);        // DestBlendAlpha
    case 23: return bits(kBlendState, 21, 3);        // BlendOpAlpha
    case 24: return bits(off::ColorControl, 3, 1);   // AlphaTestEnable
    case 25: return byte(d.F(0x2904));               // AlphaRef (sub_8264BF88)
    case 26: return bits(off::ColorControl, 0, 3);   // AlphaFunc
    case 27: return d.W(kStencilEnable);
    case 28: return bits(off::DepthControl, 7, 1);   // TwoSidedStencilMode
    case 29: return bits(off::DepthControl, 11, 3);  // StencilFail
    case 30: return bits(off::DepthControl, 17, 3);  // StencilZFail
    case 31: return bits(off::DepthControl, 14, 3);  // StencilPass
    case 32: return bits(off::DepthControl, 8, 3);   // StencilFunc
    case 33: return d.B(0x2903);                     // StencilRef
    case 34: return d.B(0x2902);                     // StencilMask
    case 35: return d.B(0x2901);                     // StencilWriteMask
    case 36: return bits(off::DepthControl, 23, 3);  // CCWStencilFail
    case 37: return bits(off::DepthControl, 29, 3);  // CCWStencilZFail
    case 38: return bits(off::DepthControl, 26, 3);  // CCWStencilPass
    case 39: return bits(off::DepthControl, 20, 3);  // CCWStencilFunc
    case 40: return d.B(0x28FF);                     // CCWStencilRef
    case 41: return d.B(0x28FE);                     // CCWStencilMask
    case 42: return d.B(0x28FD);                     // CCWStencilWriteMask
    case 43: return bits(off::ClipControl, 0, 6);    // ClipPlaneEnable
    case 44: return d.W(kPointSize);
    case 45: return d.W(kPointSizeMin);
    case 46: return d.W(kPointSpriteEnable);
    case 47: return d.W(kPointSizeMax);
    case 48: return bits(off::ModeControl, 15, 1);   // MultiSampleAntiAlias
    case 49: return d.W(0x2A00);                     // MultiSampleMask
    case 50: return d.W(kScissorTestEnable);
    case 51: return std::bit_cast<u32>(d.F(0x2A50) * 0.0625f);  // SlopeScaleDepthBias (flt_82000B58)
    case 52: return d.W(0x2A54);                     // DepthBias
    case 53: case 54: case 55: case 56:              // ColorWriteEnable0-3
      return d.W(kColorWriteEnable + (state - 53) * 4);
    case 57: return bits(0x2978, 0, 2);              // TessellationMode
    case 58: return d.W(0x2980);                     // MinTessellationLevel
    case 59: return d.W(0x297C);                     // MaxTessellationLevel
    case 76: return bits(0x294C, 0, 1);              // ViewportEnable
    case 77: case 78: case 79: case 80:              // HighPrecisionBlendEnable0-3
      return d.W(kHighPrecisionBlend + (state - 77) * 4);
    case 81: return bits(0x29C0, 0, 1);              // HalfPixelOffset
    case 82: return bits(off::ModeControl, 21, 1);   // PrimitiveResetEnable
    case 83: return d.W(0x28D8);                     // PrimitiveResetIndex
    case 84: return bits(off::ColorControl, 4, 1);   // AlphaToMaskEnable
    case 85: return d.B(off::ColorControl);          // AlphaToMaskOffsets
    case 86: return d.W(0x29CC);                     // GuardBand_X
    case 87: return d.W(0x29C4);                     // GuardBand_Y
    case 88: return d.W(0x29D0);                     // DiscardBand_X
    case 89: return d.W(0x29C8);                     // DiscardBand_Y
    case 90: return bits(off::HiControl, 3, 1);      // HiStencilEnable
    case 91: return bits(off::HiControl, 2, 1);      // HiStencilWriteEnable
    case 92: return bits(off::HiControl, 5, 1);      // HiStencilFunc
    case 93: return d.B(off::HiControl + 2);         // HiStencilRef
    case 94: return d.W(kPresentInterval);
    case 95: return bits(kBlendFlags, 23, 7);        // PresentImmediateThreshold
    case 96: return bits(kBlendFlags, 20, 3);        // HiZEnable
    case 97: return bits(kBlendFlags, 17, 3);        // HiZWriteEnable
    case 98: return bits(0x29B8, 10, 1);             // LastPixel
    case 99: return std::bit_cast<u32>(float(d.H(0x296E)) * 0.125f);  // LineWidth (sub_8264C0D8)
    case 100: return bits(kBehaviorFlags2, 0, 1);    // Buffer2Frames
    default:
      if (state >= 60 && state <= 75) {  // Wrap0-15, a nibble each
        const u32 wrap = state - 60;
        return bits(0x292C + (wrap / 8) * 4, (wrap % 8) * 4, 4);
      }
      return 0;
  }
}

u32 GetSamplerState(Device d, u32 s, u32 state) {
  if (!d || s >= kTextureSamplers)
    return 0;
  auto fetch = [&](u32 dword) { return d.W(Fetch(s, dword)); };
  switch (state) {
    case 0: return Extract(fetch(0), 10, 3);         // AddressU
    case 1: return Extract(fetch(0), 13, 3);         // AddressV
    case 2: return Extract(fetch(0), 16, 3);         // AddressW
    case 3: return (fetch(5) & 3) ? ~0u : 0;         // BorderColor
    case 4:                                          // MagFilter: anisotropic, or the mag filter
      return Extract(fetch(4), 10, 1) ? 4 : Extract(fetch(3), 19, 2);
    case 5:                                          // MinFilter
      return Extract(fetch(4), 11, 1) ? 4 : Extract(fetch(3), 21, 2);
    case 6: return Extract(fetch(3), 23, 2);         // MipFilter
    case 7:                                          // MipMapLodBias: signed 10 bits, / 32
      return std::bit_cast<u32>(float(i32(fetch(4) << 10) >> 22) * 0.03125f);
    case 8: return d.B(kMaxMipLevel + s);
    case 9: return d.B(kMaxAnisotropy + s);
    case 10: return d.B(kZFilter + s) & 1;           // MagFilterZ
    case 11: return (d.B(kZFilter + s) >> 1) & 1;    // MinFilterZ
    case 12: return (d.B(kZFilter + s) >> 2) & 1;    // SeparateZFilterEnable
    case 13: return d.B(kMinMipLevel + s);
    case 14: return Extract(fetch(5), 3, 2);         // TrilinearThreshold
    case 15:                                         // AnisotropyBias (sub_8264D418): -signed 4 bits / 8
      return std::bit_cast<u32>(-(float(i32(fetch(5) << 23) >> 28) * 0.125f));
    case 16: return u32(i32(fetch(4) << 5) >> 27);   // HGradientExpBias
    case 17: return u32(i32(fetch(4)) >> 27);        // VGradientExpBias
    case 18: return Extract(fetch(5), 2, 1);         // WhiteBorderColorW
    case 19: return Extract(~fetch(1), 11, 1);       // PointBorderEnable
    default: return 0;
  }
}

// D3D::InitializeApiState (0x826597C8), its first part: each state's getter
// and setter go into the device's dispatch tables (the game's inline
// SetRenderState calls through them, and so reaches the hooked setters), and
// the state is set to its default; then every sampler's texture is unset.
void InitializeStateTables(Device device) {
  if (!device)
    return;
  for (u32 i = 0; i < kRenderStates; ++i) {
    device.SetW(off::SetRenderStateCall + i * 4, TableWord(kRenderStateTable, i, 1));
    device.SetW(off::GetRenderStateCall + i * 4, TableWord(kRenderStateTable, i, 0));
    kRenderStateFns[i](device, TableWord(kRenderStateTable, i, 2));
  }
  for (u32 sampler = 0; sampler < kTextureSamplers; ++sampler) {
    for (u32 i = 0; i < kSamplerStates; ++i) {
      device.SetW(off::SetSamplerStateCall + i * 4, TableWord(kSamplerStateTable, i, 1));
      device.SetW(off::GetSamplerStateCall + i * 4, TableWord(kSamplerStateTable, i, 0));
      kSamplerStateFns[i](device, sampler, TableWord(kSamplerStateTable, i, 2));
    }
    SetTexture(device, sampler, 0);
  }
}

void TagResourceFence(Device device, u32 resource, bool read_fence) {
  auto *header = GuestPtr<be_u32>(resource);
  if (!header)
    return;
  if (const u32 fence = device.W(off::FenceTag)) {
    header[2] = fence;  // Fence
    if (read_fence)
      header[3] = fence;  // ReadFence
  }
  // Otherwise (a device recording a command buffer) N&B's D3D adds the
  // resource to the buffer's list, which fences it when the buffer runs; the
  // buffers run on the host (command_buffers.h), so the list is left out.
}

// void D3DDevice_SetTexture(D3DDevice*, DWORD Sampler, D3DBaseTexture*,
//     UINT64 PendingMask3) [0x8222F410]. The fetch constant takes the texture's
// header, keeping the sampler state bits; its mip range is narrowed by the
// sampler's MaxMipLevel / MinMipLevel.
void SetTexture(Device device, u32 sampler, u32 texture) {
  if (!device || sampler >= kTextureSamplers)
    return;
  const u32 previous = device.W(off::Textures + sampler * 4);
  if (const auto *header = GuestPtr<const be_u32>(texture)) {
    const u32 t0 = header[7];   // +0x1C, the header's fetch constant
    const u32 t1 = header[8];
    const u32 t2 = header[9];
    const u32 t3 = header[10];
    const u32 t4 = header[11];
    const u32 t5 = header[12];
    const u32 f0 = device.W(Fetch(sampler, 0));
    const u32 f1 = device.W(Fetch(sampler, 1));
    const u32 f3 = device.W(Fetch(sampler, 3));
    const u32 f4 = device.W(Fetch(sampler, 4));
    const u32 f5 = device.W(Fetch(sampler, 5));
    const u32 mip_base = (t5 & 0x1FFFFE00u) + (((t5 >> 20) + 0x200u) & 0x1000u);
    device.SetW(Fetch(sampler, 2), t2);
    u32 dword4 = (t4 & 0x3FC) | (f4 & 0xFFFFFC03u);
    device.SetW(Fetch(sampler, 4), dword4);
    device.SetW(Fetch(sampler, 0), (t0 & ~0x3FFC00u) | (f0 & 0x3FFC00u));
    device.SetW(Fetch(sampler, 1), (GpuAddress(t1) & ~0x800u) | (f1 & 0x800u));
    device.SetW(Fetch(sampler, 3), (t3 & ~0x7FF80000u) | (f3 & 0x7FF80000u));
    device.SetW(Fetch(sampler, 5), (mip_base & ~0x1FFu) | (f5 & 0x1FFu));
    u32 max_level = device.B(kMaxMipLevel + sampler);
    if (Extract(t4, 2, 4) > max_level)
      max_level = Extract(t4, 2, 4);
    dword4 = Insert(dword4, max_level, 2, 4);
    u32 min_level = device.B(kMinMipLevel + sampler);
    const u32 texture_min = Extract(header[11], 6, 4);
    if (texture_min < min_level)
      min_level = texture_min;
    dword4 = Insert(dword4, min_level, 6, 4);
    device.SetW(Fetch(sampler, 4), dword4);
    device.Dirty(kPendingFetch, FetchBit(sampler));
  }
  device.SetW(off::Textures + sampler * 4, texture);
  if (previous)
    TagResourceFence(device, previous, false);
}

// void D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber, D3DVertexBuffer*,
//     UINT OffsetInBytes, UINT Stride, UINT64 PendingMask3) [0x8222A408]
void SetStreamSource(Device device, u32 stream, u32 vertex_buffer, u32 offset, u32 stride) {
  if (!device || stream >= 16)
    return;
  if (const auto *buffer = GuestPtr<const be_u32>(vertex_buffer)) {
    // Stream n is VertexFetch[17 - n], in fetch constant (95 - n) / 3.
    const u32 fetch = off::VertexFetch + (17 - stream) * 8;
    device.SetW(fetch, GpuAddress(u32(buffer[6]) + offset));
    device.SetW(fetch + 4, u32(buffer[7]) - offset);
    device.Dirty(kPendingFetch, FetchBit((95 - stream) / 3));
  }
  const u32 previous = device.W(off::StreamSources + stream * 4);
  if (previous)
    TagResourceFence(device, previous, false);
  device.SetW(off::StreamSources + stream * 4, vertex_buffer);
  const u32 dwords = stride >> 2;
  device.SetB(off::StreamStrides + stream, u8(dwords));
  if (dwords && dwords != device.B(kStreamDeclStrides + stream))
    device.Dirty(kPendingStates, 0x80000);
}

// void D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*) [0x8220EE38]
void SetIndices(Device device, u32 index_buffer) {
  if (!device)
    return;
  if (const u32 previous = device.W(off::IndexBuffer))
    TagResourceFence(device, previous, false);
  device.SetW(off::IndexBuffer, index_buffer);
}

// void D3DDevice_SetVertexDeclaration(D3DDevice*, D3DVertexDeclaration*) [0x82205700]
void SetVertexDeclaration(Device device, u32 declaration) {
  if (!device)
    return;
  device.SetW(kVertexDeclaration, declaration);
  device.Dirty(kPendingStates, 0x80000);
}

// N&B's SetVertexShader/SetPixelShader (0x8222A0A8 / 0x82226730) also bind
// the shader's microcode and literal constants. The renderer's shaders are
// records with recompiled code that carry their literals, so only the device
// slot (and the bound-shader flag) is kept.
void SetVertexShader(Device device, u32 shader) {
  if (!device)
    return;
  device.SetW(kVertexShader, shader);
  device.SetB(off::Flags2, device.B(off::Flags2) & 0x7F);
}

void SetPixelShader(Device device, u32 shader) {
  if (!device)
    return;
  device.SetW(kPixelShader, shader);
}

bool TilingBoundSurfaces(Device device) {
  const u8 flags = device.B(off::Flags);
  if (flags & 0x10)
    return true;
  if (!(flags & 0x20))
    return false;
  for (u32 i = 0; i < 5; ++i) {
    const u32 bound = device.W(off::RenderTargets + i * 4);  // 4 targets, then depth
    if (bound && bound != device.W(off::TilingSurfaces + i * 4))
      return false;
  }
  return true;
}

void UpdateTilePredicated(Device device) {
  const u8 flags = device.B(off::Flags);
  const bool predicated = !device.W(off::PredicationSet) && !(flags & 0x08) && !(flags & 0x04) &&
                          !device.B(0x2F9B) && TilingBoundSurfaces(device);
  device.SetB(off::Flags, u8((flags & 0xFE) | (predicated ? 1 : 0)));
}

// void D3DDevice_SetScissorRect(D3DDevice*, const RECT*) [0x82223178]: the
// window scissor is the rect (while ScissorTestEnable) within the viewport.
void SetScissorRect(Device device, i32 left, i32 top, i32 right, i32 bottom) {
  if (!device)
    return;
  i32 x1 = TruncateToI32(device.F(kViewport));
  i32 y1 = TruncateToI32(device.F(kViewport + 4));
  const i32 width = TruncateToI32(device.F(kViewport + 8));
  const i32 height = TruncateToI32(device.F(kViewport + 12));
  device.SetW(off::ScissorRect, u32(left));
  device.SetW(off::ScissorRect + 4, u32(top));
  device.SetW(off::ScissorRect + 8, u32(right));
  device.SetW(off::ScissorRect + 12, u32(bottom));
  i32 x2 = width + x1;
  i32 y2 = height + y1;
  if (device.W(kScissorTestEnable)) {
    if (x1 <= left)
      x1 = left;
    if (y1 <= top)
      y1 = top;
    if (x2 >= right)
      x2 = right;
    if (y2 >= bottom)
      y2 = bottom;
  }
  u32 tl = device.W(off::WindowScissorTL);
  u32 br = device.W(off::WindowScissorBR);
  tl = Insert(tl, u32(y1), 16, 15);
  br = Insert(br, u32(y2), 16, 15);
  tl = Insert(tl, u32(x1), 0, 15);
  br = Insert(br, u32(x2), 0, 15);
  device.SetW(off::WindowScissorTL, tl);
  device.SetW(off::WindowScissorBR, br);
  // SetSurfaceClip (0x82223270) then writes the clip into the ring only.
}

// D3D::SetViewport(D3DDevice*, float X, Y, Width, Height, MinZ, MaxZ)
// [0x82222F20]: clamped to the bound surface (or the tiling extent), with
// the PA_CL_VPORT values; nothing without a target or depth surface.
void SetViewport(Device device, float x, float y, float width, float height, float min_z,
                 float max_z) {
  if (!device)
    return;
  const u32 target = device.W(off::RenderTargets);
  const u32 surface = target ? target : device.W(off::DepthStencil);
  if (!surface)
    return;
  i32 surface_width, surface_height;
  if (TilingBoundSurfaces(device)) {
    surface_width = i32(device.W(off::TilingExtent));
    surface_height = i32(device.W(off::TilingExtent + 4));
  } else {
    const auto *size = GuestPtr<const be_u32>(surface + 0x24);
    const u32 s = size ? u32(*size) : 0;
    surface_width = i32((s >> 18) + 1);
    surface_height = i32(Extract(s, 3, 15) + 1);
  }
  const i32 ix = TruncateToI32(x);
  const i32 iy = TruncateToI32(y);
  i32 right = surface_width;
  if (TruncateToI32(width) + ix < right)
    right = TruncateToI32(width) + ix;
  i32 bottom = TruncateToI32(height) + iy;
  if (bottom >= surface_height)
    bottom = surface_height;
  i32 clamped_width = right - ix;
  i32 clamped_height = bottom - iy;
  if (clamped_width < 0 || clamped_height < 0) {
    clamped_width = 0;
    clamped_height = 0;
  }
  const float fw = float(clamped_width);
  const float fh = float(clamped_height);
  device.SetF(kViewport, x);
  device.SetF(kViewport + 4, y);
  device.SetF(kViewport + 16, min_z);
  device.SetF(kViewport + 20, max_z);
  device.SetF(kViewport + 12, fh);
  device.SetW(kViewport + 24, 0);
  device.SetF(kViewport + 8, fw);
  SetScissorRect(device, i32(device.W(off::ScissorRect)), i32(device.W(off::ScissorRect + 4)),
                 i32(device.W(off::ScissorRect + 8)), i32(device.W(off::ScissorRect + 12)));
  device.SetF(kVportZScale, max_z - min_z);
  device.SetF(kVportZScale + 4, min_z);
  const float half_width = fw * 0.5f;
  const float half_height = fh * 0.5f;
  device.SetF(kVportXScale, half_width);
  device.SetF(kVportXScale + 4, half_width + x);
  device.SetF(kVportXScale + 8, -half_height);
  device.SetF(kVportXScale + 12, half_height + y);
  device.Dirty(kPendingStates, 0x07E00000);
}

// void D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*) [0x82222EA0]
void SetViewport(Device device, u32 viewport_va) {
  const auto *v = GuestPtr<const be_u32>(viewport_va);
  if (!v)
    return;
  SetViewport(device, float(double(u32(v[0]))), float(double(u32(v[1]))),
              float(double(u32(v[2]))), float(double(u32(v[3]))), AsFloat(v[4]), AsFloat(v[5]));
}

// D3D::SetSurfaceInfo(CDevice*, D3DSurface*) [0x8227AEB0]: RB_SURFACE_INFO and
// the MSAA config from the surface; the scissor and viewport reset to it.
void SetSurfaceInfo(Device device, u32 surface) {
  const auto *s = GuestPtr<const be_u32>(surface);
  if (!device || !s)
    return;
  const u32 info = s[6];
  const u32 msaa = Extract(info, 16, 2);
  device.SetW(0x29BC, msaa == 0 ? 0u : msaa == 1 ? 0x8001u : 0xC003u);
  device.SetW(off::SurfaceInfo, info);
  device.Dirty(kPendingStates, Bit(57));
  device.Dirty(kPendingMisc, Bit(36));
  device.SetW(off::ScissorRect, 0);
  device.SetW(off::ScissorRect + 4, 0);
  device.SetW(off::ScissorRect + 8, 0xFFFF);
  device.SetW(off::ScissorRect + 12, 0xFFFF);
  SetViewport(device, 0.0f, 0.0f, 65535.0f, 65535.0f, 0.0f, 1.0f);  // unk_820018DC
}

// void D3DDevice_SetRenderTarget(D3DDevice*, DWORD Index, D3DSurface*) [0x8227AB68]
void SetRenderTarget(Device device, u32 index, u32 surface) {
  if (!device || index >= 4)
    return;
  device.SetW(off::RenderTargets + index * 4, surface);
  if (const auto *s = GuestPtr<const be_u32>(surface)) {
    const u32 slot = index ? index + 1 : 0;
    device.SetW(off::Color0Info + slot * 4, s[7]);
    device.Dirty(kPendingStates, Bit(56 - slot));
    ApplyHighPrecisionBlend(device, slot, device.W(kHighPrecisionBlend + index * 4));
  }
  u32 enable = device.W(kColorWriteEnable + index * 4);
  if (index == 0) {
    u8 flags3 = device.B(off::Flags3) & 0xF7;
    device.SetB(off::Flags3, flags3);
    if (const auto *s = GuestPtr<const be_u32>(surface)) {
      const u32 format = (u32(s[7]) >> 16) & 0xF;
      if (format == 5 || format == 7 || format == 15) {
        device.SetB(off::Flags3, flags3 | 8);
        device.Dirty(kPendingStates, 0x100000);
      }
      SetSurfaceInfo(device, surface);
    } else if (const u32 depth = device.W(off::DepthStencil)) {
      SetSurfaceInfo(device, depth);
    }
    UpdateTilePredicated(device);
  }
  if (!device.W(off::RenderTargets + index * 4))
    enable = 0;
  device.SetW(off::ColorMask, Insert(device.W(off::ColorMask), enable, index * 4, 4));
  device.Dirty(kPendingStates, Bit(37));
}

// void D3DDevice_SetDepthStencilSurface(D3DDevice*, D3DSurface*) [0x8227AF40]
void SetDepthStencilSurface(Device device, u32 surface) {
  if (!device)
    return;
  device.SetW(off::DepthStencil, surface);
  if (const auto *s = GuestPtr<const be_u32>(surface)) {
    if (!device.W(off::RenderTargets))
      SetSurfaceInfo(device, surface);
    device.SetW(off::DepthInfo, s[7]);
    const u32 hi_control = s[8];
    device.SetW(off::HiControl, hi_control);
    device.SetB(off::Flags2, u8(Insert(device.B(off::Flags2), hi_control, 5, 1)));
    if ((device.B(off::Flags3) & 0x20) && TilingBoundSurfaces(device))
      device.SetW(off::SurfaceInfo, Insert(device.W(off::SurfaceInfo), device.W(0x3374), 18, 14));
    device.Dirty(kPendingStates, Bit(55) | 0x100 | 0x20000);
  } else {
    device.SetW(off::HiControl, device.W(off::HiControl) & ~0xFu);
    device.SetB(off::Flags2, device.B(off::Flags2) & 0xDF);
    device.Dirty(kPendingStates, 0x100);
  }
  UpdateTilePredicated(device);
  const bool depth = device.W(off::DepthStencil) != 0;
  const u32 z_enable = depth ? device.W(kZEnable) : 0;
  device.SetW(off::DepthControl, Insert(device.W(off::DepthControl), z_enable, 1, 1));
  device.Dirty(kPendingStates, 0x800 | 0x20000);
  const u32 stencil_enable = depth ? device.W(kStencilEnable) : 0;
  device.SetW(off::DepthControl, (device.W(off::DepthControl) & ~1u) | (stencil_enable & 1));
  device.Dirty(kPendingStates, 0x800 | 0x20000);
}

// void D3DDevice_SetPredication(D3DDevice*, DWORD PredicationMask) [0x8222AA18]
void SetPredication(Device device, u32 mask) {
  if (!device)
    return;
  if (!mask) {
    device.SetW(off::PredicationSet, 0);
    UpdateTilePredicated(device);
    device.SetW(off::Predication, ~0u);
  } else {
    device.SetW(off::Predication, mask);
    device.SetB(off::Flags, device.B(off::Flags) & 0xFE);
    device.SetW(off::PredicationSet, 1);
  }
  device.SetW(off::RunPredicationState, 0);
  device.SetW(off::TilePredication, mask);
  device.SetW(off::RunPredication, 0);
}

// void D3DDevice_SetCommandBufferPredication(D3DDevice*, DWORD TilePredication,
//     DWORD RunPredication) [0x82658678]
void SetCommandBufferPredication(Device device, u32 tile_predication, u32 run_predication) {
  if (!device)
    return;
  if (auto *buffer = GuestPtr<be_u32>(device.W(off::CommandBuffer)))
    buffer[0x6C / 4] = u32(buffer[0x6C / 4]) | 0x40;
  if (!tile_predication && !run_predication) {
    device.SetW(off::PredicationSet, 0);
    UpdateTilePredicated(device);
    device.SetW(off::RunPredicationState, 0);
    device.SetW(off::Predication, ~0u);
  } else {
    device.SetW(off::Predication, tile_predication);
    device.SetB(off::Flags, device.B(off::Flags) & 0xFE);
    device.SetW(off::PredicationSet, 1);
    device.SetW(off::RunPredicationState, run_predication);
  }
  device.SetW(off::TilePredication, tile_predication);
  device.SetW(off::RunPredication, run_predication);
}

// D3D::CleanupGprAllocationSetup (0x8264E2C0): the state a GPR allocation
// change invalidates.
void CleanupGprAllocationSetup(Device device) {
  if (!device)
    return;
  device.Dirty(kPendingStates, Bit(41) | Bit(40) | Bit(39));
  device.Dirty(kPendingFetch, Bit(35));
  device.Dirty(kPendingStates, 0x80 | 0x40 | 0x20);
  device.Dirty(kPendingMisc, 0x80000);
  device.Dirty(kPendingStates, Bit(44) | 0x80000 | 0x100000 | 0x800 | 0x100 | 8 | Bit(37));
  device.Dirty(kPendingFetch, Bit(54));
  device.Dirty(kPendingMisc, Bit(35));
  device.Dirty(kPendingFetch, 0x80000000u);
  // Then SetSurfaceClip, which writes the clip into the ring only.
}

// void D3DDevice_SetShaderGPRAllocation(D3DDevice*, DWORD Flags,
//     DWORD VertexShaderCount, DWORD PixelShaderCount) [0x82220B20]
void SetShaderGPRAllocation(Device device, u32 flags, u32 vertex_count, u32 pixel_count) {
  if (!device)
    return;
  if (!vertex_count && !pixel_count) {
    vertex_count = 64;
    pixel_count = 64;
  }
  u32 allocation = 0;
  if (!(flags & 1)) {
    pixel_count = Insert(pixel_count, vertex_count, 8, 7);
    allocation = device.W(kGprAllocation);
    allocation = Insert(allocation, pixel_count, 4, 7);
    allocation = (allocation & ~0x7F000u) | ((pixel_count << 4) & 0x7F000u);
  }
  device.SetW(kGprAllocation, allocation);
  // The register writes go into the ring.
  CleanupGprAllocationSetup(device);
}

// void D3DDevice_SetScreenExtentQueryMode(D3DDevice*, D3DSCREENEXTENTQUERYMODE)
// [0x82658988]
void SetScreenExtentQueryMode(Device device, u32 mode) {
  if (!device)
    return;
  device.SetW(kScreenExtentQueryMode, Insert(device.W(kScreenExtentQueryMode), mode, 12, 2));
}

// void D3DDevice_UnsetAll(D3DDevice*) [0x8264E1A8]: everything but the
// device's own (InitializePresentationParameters') surfaces.
void UnsetAll(Device device) {
  if (!device)
    return;
  for (u32 i = 0; i < 4; ++i) {
    if (device.W(off::RenderTargets + i * 4) != device.W(off::AutoBackBuffer))
      SetRenderTarget(device, i, 0);
  }
  if (device.W(off::DepthStencil) != device.W(off::AutoDepthStencil))
    SetDepthStencilSurface(device, 0);
  SetVertexShader(device, 0);
  SetPixelShader(device, 0);
  device.SetW(kVertexDeclaration, 0);
  device.Dirty(kPendingStates, 0x80000);
  SetIndices(device, 0);
  for (u32 stream = 0; stream < 16; ++stream)
    SetStreamSource(device, stream, 0, 0, 4);
  for (u32 sampler = 0; sampler < kTextureSamplers; ++sampler)
    SetTexture(device, sampler, 0);
}

// D3D::ResetAllState(CDevice*, BOOL) [0x82287D38]: besides the packets that
// restore the predication and the screen extent mode, every dirty mask set.
void ResetAllState(Device device) {
  if (!device)
    return;
  for (u32 word = 0; word < 5; ++word)
    device.SetPending(word, ~u64(0));
}

// D3D::InvalidateConstants (0x8264E128): an invalidate packet, after which
// the constants are not known to be loaded.
void InvalidateConstants(Device device) {
  if (!device)
    return;
  device.SetB(off::Flags3, device.B(off::Flags3) & 0xFE);
  device.SetB(off::Flags1, device.B(off::Flags1) & 0xFB);
}

// D3D::InvalidateConstants (0x82657488): a cache flush packet, then a wait
// for the current fence.
void InvalidateConstantsAndWait(Device device) {
  if (!device)
    return;
  if (const u32 fence = device.W(off::FenceTag))
    BlockOnFence(device.va, fence);
  // The original then spins while the word at +0x2B00 is non-zero; nothing
  // sets it on a device without GPU hardware.
}

// ---------------------------------------------------------------------------
// Clear and Resolve
// ---------------------------------------------------------------------------

namespace {

// Per D3DFORMAT (low 6 bits) its format description; 0xF00 marks a float
// depth format (word_820052F8).
constexpr u32 kFormatTable = 0x820052F8;
// The register packets between RB_COPY_CONTROL and RB_COPY_DEST_INFO.
constexpr u32 kCopyControl = 0x2A18;
constexpr u32 kCopyDestInfo = 0x2A24;
constexpr u32 kCopyDepthClear = 0x2A28;  // RB_DEPTH_CLEAR's HiZ part
constexpr u32 kDepthClear = 0x2A2C;      // RB_DEPTH_CLEAR

// slw / srw: the shift amount is 6 bits, and 32 or more gives 0.
u32 Slw(u32 value, u32 amount) {
  amount &= 63;
  return amount >= 32 ? 0 : value << amount;
}
u32 Srw(u32 value, u32 amount) {
  amount &= 63;
  return amount >= 32 ? 0 : value >> amount;
}

// The state flush both begin with: the dirty register packets go into the
// ring and their bits are cleared. (Its SetPending_Predicated, 0x8226AAB8,
// clears only bits these masks clear too.)
void FlushStatePackets(Device d) {
  if (const u64 states = d.Pending(kPendingStates))
    d.SetPending(kPendingStates, states & ~(u64(0x1FFFF) | (u64(0x1FFFFFFFFF) << 21)));
  d.SetPending(kPendingFetch, d.Pending(kPendingFetch) & ~(u64(0x1FFFFF) << 34));
  d.SetPending(kPendingMisc, d.Pending(kPendingMisc) & ~u64(0x3FFFFFFFFF));
}

// What both leave dirty for the draws after them: they load their own
// shaders and state.
void DirtyAfterClear(Device d) {
  d.Dirty(kPendingStates, Bit(41) | Bit(40) | Bit(39));
  d.Dirty(kPendingFetch, Bit(35));
  d.Dirty(kPendingStates, 0x80 | 0x40 | 0x20);
  d.Dirty(kPendingMisc, 0x80000);
  d.Dirty(kPendingStates, Bit(44));
}

// D3D::ClearRects (0x822043D8): the dirty bits of one rect clear.
void DirtyClearRects(Device d) {
  d.Dirty(kPendingFetch, 0x80000000u);
  d.Dirty(kPendingStates, Bit(57) | Bit(56));
  d.Dirty(kPendingMisc, Bit(36));
  d.Dirty(kPendingStates, 0x800 | 8);
}

// D3D::ClearSurface (0x8220CB78): one color target's clear (or, with target
// -1, the depth/stencil one), with the depth/stencil clear while flags have
// it; tiled, in each tile it covers.
void ClearSurface(Device d, u32 flags, i32 target, i32 x1, i32 y1, i32 x2, i32 y2) {
  u32 color_info = 0;
  if (target >= 0) {
    const u32 surface = d.W(off::RenderTargets + u32(target) * 4);
    if (const auto *s = surface ? GuestPtr<const be_u32>(surface) : nullptr)
      color_info = s[7];
  }
  if (flags & 0x20)
    d.Dirty(kPendingStates, 0x10000000);
  // A fast clear: only without a depth/stencil part, and over at least a
  // tile.
  if (!(flags & 0x30)) {
    const u32 format = Extract(color_info, 16, 4);
    if (format != 5 && format != 7 && format != 15) {
      const u32 msaa = Extract(d.W(off::SurfaceInfo), 16, 2);
      const i32 min_width = msaa >= 2 ? 40 : 80;
      const i32 min_height = msaa == 0 ? 16 : 8;
      if (x2 - x1 >= min_width && y2 - y1 >= min_height)
        d.Dirty(kPendingStates, 0x100 | Bit(55) | 0x10000000);
    }
  }
  if (!TilingBoundSurfaces(d)) {
    DirtyClearRects(d);
    return;
  }
  u32 predication = d.W(off::Predication);
  if ((d.B(off::Flags3) & 0x20) && (d.B(off::Flags) & 0x40) && (predication & 0x15555555u)) {
    if (flags & 0x30)
      DirtyClearRects(d);
    predication &= 0xEAAAAAAAu;
  }
  const u32 tiles = d.W(off::TileCount);
  for (u32 i = 0; i < tiles; ++i) {
    const u32 tile = off::TileRects + i * 16;
    const i32 left = std::max(x1, i32(d.W(tile)));
    const i32 top = std::max(y1, i32(d.W(tile + 4)));
    const i32 right = std::min(x2, i32(d.W(tile + 8)));
    const i32 bottom = std::min(y2, i32(d.W(tile + 12)));
    if (left < right && top < bottom && (Slw(3, i * 2) & predication))
      DirtyClearRects(d);
  }
}

// RB_DEPTH_CLEAR for a resolve's depth/stencil clear (sub_821FA520): 24-bit
// depth, or float depth in the depth surface's (or the parameters') format.
void SetDepthClear(Device d, u32 parameters, float z, u32 stencil) {
  bool float_depth;
  if (parameters) {
    const auto *table = GuestPtr<const be_u16>(kFormatTable);
    const auto *p = GuestPtr<const be_u32>(parameters);
    float_depth = table && p && (u32(table[p[2] & 0x3F]) & 0xF00) != 0;
  } else {
    float_depth = (d.H(off::DepthInfo) & 1) != 0;
  }
  u32 value, high;
  if (!float_depth) {
    value = (u32(TruncateToI64(z * 16777215.0f)) << 8) | stencil;  // flt_8219F884
    high = u32(TruncateToI64(z * 2047.0f));                         // flt_8219F880
  } else {
    const u32 bits = std::bit_cast<u32>(z);
    u32 depth;
    if (bits & 0x80000000u) {
      depth = 0;
    } else if ((bits >> 23) < 0x71) {
      const u32 shift = 0x71 - ((bits >> 23) & 0xFF);
      depth = Srw((Slw(1, shift - 1) + (bits & 0x7FFFFF)) | 0x800000, shift);
    } else {
      depth = ((bits & 0x07800000) | (bits + 4)) >> 3;
    }
    value = Insert(stencil, depth, 8, 24);
    high = value >> 21;
  }
  d.SetW(kDepthClear, value);
  const u32 stencil_differs = stencil != d.B(off::HiControl + 2) ? 0x800 : 0;
  const u32 hi_bit = (~d.W(off::HiControl) & 0x20) << 6;
  d.SetW(kCopyDepthClear, (stencil_differs ^ hi_bit) | high);
  d.Dirty(kPendingMisc, 0x300);
}

}  // namespace

void Clear(Device device, u32 flags, const Rect *rect) {
  if (!device)
    return;
  Rect r;
  if (rect) {
    r = *rect;
  } else {
    // D3DDevice_ClearF: the bound target's size, or the tiling extent.
    const u32 target = device.W(off::RenderTargets);
    const u32 surface = target ? target : device.W(off::DepthStencil);
    const auto *s = surface ? GuestPtr<const be_u32>(surface) : nullptr;
    if (!s)
      return;
    const u32 size = s[9];
    r = {0, 0, i32((size >> 18) + 1), i32(Extract(size, 3, 15) + 1)};
    if (TilingBoundSurfaces(device)) {
      r.x2 = i32(device.W(off::TilingExtent));
      r.y2 = i32(device.W(off::TilingExtent + 4));
    }
  }
  if (!device.W(off::DepthStencil)) {
    flags &= ~0xF0u;
    if (!flags)
      return;
  }
  // Within the viewport, and the scissor while it is enabled.
  const i32 vx = TruncateToI32(device.F(kViewport));
  const i32 vy = TruncateToI32(device.F(kViewport + 4));
  const i32 vw = TruncateToI32(device.F(kViewport + 8));
  const i32 vh = TruncateToI32(device.F(kViewport + 12));
  i32 x1 = std::max(r.x1, vx);
  i32 y1 = std::max(r.y1, vy);
  i32 x2 = std::min(r.x2, vw + vx);
  i32 y2 = std::min(r.y2, vh + vy);
  if (device.W(kScissorTestEnable)) {
    x1 = std::max(x1, i32(device.W(off::ScissorRect)));
    y1 = std::max(y1, i32(device.W(off::ScissorRect + 4)));
    x2 = std::min(x2, i32(device.W(off::ScissorRect + 8)));
    y2 = std::min(y2, i32(device.W(off::ScissorRect + 12)));
  }
  if (x2 <= x1 || y2 <= y1)
    return;
  FlushStatePackets(device);
  device.SetB(off::Flags3, device.B(off::Flags3) & 0xFE);
  DirtyAfterClear(device);
  device.Dirty(kPendingStates, 0x80000 | 0x100000);
  if (!(flags & 0xF)) {
    ClearSurface(device, flags, -1, x1, y1, x2, y2);
  } else {
    device.Dirty(kPendingStates, Bit(37) | 0x200 | 0x400);
    for (u32 i = 0; i < 4; ++i) {
      if ((flags & (1u << i)) && device.W(off::RenderTargets + i * 4)) {
        ClearSurface(device, flags, i32(i), x1, y1, x2, y2);
        flags &= ~0xF0u;  // depth and stencil with the first target
      }
    }
  }
  // SetSurfaceClip writes the window scissor into the ring.
  device.SetB(off::Flags1, device.B(off::Flags1) | 4);
  device.Dirty(kPendingStates, 0x20000);
}

// The copy's register images (RB_COPY_*, the destination's address, pitch and
// format, and the packed clear color) are only read by the GPU's copy; their
// dirty bits are kept.
void Resolve(Device device, u32 flags, u32 dest_texture, float clear_z, u32 clear_stencil,
             u32 parameters) {
  if (!device)
    return;
  auto *texture = dest_texture ? GuestPtr<be_u32>(dest_texture) : nullptr;
  if (texture && Extract(texture[0x30 / 4], 9, 2) == 2)  // a volume: RB_COPY_DEST's slice
    device.Dirty(kPendingMisc, 4);
  device.Dirty(kPendingMisc, 0x3C00);
  if (flags & 0x200)
    SetDepthClear(device, parameters, clear_z, clear_stencil);
  if (flags & 0x100)
    device.Dirty(kPendingMisc, 0xC0);  // SetColorClear (0x82290DE0)
  FlushStatePackets(device);
  device.SetB(off::Flags2, device.B(off::Flags2) & 0xF7);
  if (flags & 0x200) {
    device.SetB(off::Flags1, device.B(off::Flags1) | 4);
    device.Dirty(kPendingStates, 0x20000);
  }
  device.SetB(off::Flags3, device.B(off::Flags3) & 0xFE);
  DirtyAfterClear(device);
  // The source color target's info, for a destination with field 2-3 of
  // dword 7 at 3 and a target in EDRAM format 1 (8_8_8_8_GAMMA).
  const u32 source = flags & 7;
  if (texture && Extract(texture[0x1C / 4], 2, 2) == 3 && source != 4) {
    const u32 slot = source ? source + 1 : 0;
    if ((device.W(off::Color0Info + slot * 4) & 0xF0000) == 0x10000)
      device.Dirty(kPendingStates, Bit(56 - slot));
  }
  const u32 fence = device.W(off::FenceTag);
  if (texture)
    texture[2] = fence;  // Fence
  device.Dirty(kPendingStates, 0x80000 | 8);
  device.Dirty(kPendingFetch, 0x80000000u);
  device.Dirty(kPendingStates, 0x18000 | 0x800 | 0x100);
  device.SetW(kCopyControl, 0);
  device.SetW(kCopyDestInfo, 0);
  if (texture && fence)
    texture[3] = fence;  // ReadFence
  // Without a fence (a device recording a command buffer) the destination
  // goes into the buffer's resource list, the GPU's.
}

// ---------------------------------------------------------------------------
// Memexport
// ---------------------------------------------------------------------------

namespace {

constexpr u32 kExportAddress = 0x2898;       // [Index]: the stream's GPU address
constexpr u32 kExportCount = 0x34C0;         // BeginExports not yet ended
constexpr u32 kExportNanVertexDone = 0x34C4;
constexpr u32 kExportFlags = 0x34C8;
// N&B's switches for the two GPU workarounds below (dword_82FAD204/208).
constexpr u32 kSkipExportNanVertex = 0x82FAD204;
constexpr u32 kSkipExport512Pixels = 0x82FAD208;

u32 GuestWord(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  return p ? u32(*p) : 0;
}

// DoBeginExportNanVertex (0x82654FD8): a draw of one NaN vertex, from its own
// shaders and state.
void ExportNanVertex(Device d) {
  if (GuestWord(kSkipExportNanVertex))
    return;
  d.Dirty(kPendingStates, 0x80000 | Bit(41) | Bit(40) | Bit(39) | 8);
}

// DoEndExport512Pixels (0x82655130): a 512-pixel draw, from its own shaders
// and state.
void Export512Pixels(Device d) {
  if (GuestWord(kSkipExport512Pixels))
    return;
  d.Dirty(kPendingStates, Bit(41) | Bit(40) | Bit(39));
  d.Dirty(kPendingFetch, Bit(35));
  d.Dirty(kPendingStates, 0x80 | 0x40 | 0x20);
  d.Dirty(kPendingMisc, 0x80000);
  d.Dirty(kPendingStates, Bit(44) | 0x80000 | 0x100000 | 0x800 | 0x100 | 8 | Bit(37));
  d.Dirty(kPendingFetch, Bit(54));
  d.Dirty(kPendingMisc, Bit(35));
  // Then SetSurfaceClip, which writes the clip into the ring only.
}

}  // namespace

void BeginExport(Device device, u32 index, u32 resource, u32 flags) {
  const auto *r = resource ? GuestPtr<const be_u32>(resource) : nullptr;
  if (!device || !r || index > 51)
    return;
  u32 address;
  switch (r[0] & 0xF) {  // Common: the resource type
    case 1:  // vertex buffer
      address = r[6] & ~3u;
      break;
    case 3:  // texture: its base, or its mips without one
      address = r[8] & ~0xFFFu;
      if (!address)
        address = r[12] & ~0xFFFu;
      break;
    default:
      address = r[6];
      break;
  }
  device.SetW(kExportAddress + index * 4, GpuAddress(address));
  device.Dirty(kPendingStates, Bit(51 - index));
  if (!device.W(kExportCount) && (flags == 0xA || !device.W(kExportNanVertexDone)))
    ExportNanVertex(device);
  device.SetW(kExportFlags, flags & 3);
  device.SetW(kExportNanVertexDone, 0);
  device.SetW(kExportCount, device.W(kExportCount) + 1);
}

// The written range's cache invalidation (InvalidateCaches 0x8226BA20) goes
// into the ring.
void EndExport(Device device, u32 index, u32 resource, u32 flags) {
  if (!device || index > 51)
    return;
  device.SetW(kExportAddress + index * 4, 0);
  if (auto *r = resource ? GuestPtr<be_u32>(resource) : nullptr) {
    if (const u32 fence = device.W(off::FenceTag)) {
      r[2] = fence;  // Fence
      r[3] = fence;  // ReadFence
    }
    // Otherwise the command buffer's resource list, the GPU's.
  }
  const u32 count = device.W(kExportCount) - 1;
  device.SetW(kExportCount, count);
  if (count || flags == 1)
    ExportNanVertex(device);
  else if (flags != 2)
    Export512Pixels(device);
  device.SetW(kExportNanVertexDone, (!count && flags == 1) ? 1 : 0);
}

}  // namespace renut::gpu::native
