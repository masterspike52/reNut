// The vertex shader of NutTool's world models (renut_world_* in shader_library.cpp), run in place of
// a guest one: N&B sets world x view-projection in c0-c3 and the world matrix in c120-c122.

#ifdef __spirv__
struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

float4 VsConstant(uint r) { return vk::RawBufferLoad<float4>(g_PushConstants.VertexShaderConstants + r * 16, 16); }
float2 ScreenOffset() { return vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 544); }
float2 ScreenScale() { return vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 568); }
uint ScreenSpace() { return vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 968); }
float2 ViewportSize() { return vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 1328); }
float4 ClipPlane(uint i) { return vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 1360 + i * 16, 16); }
uint ClipPlaneMask() { return vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 1456); }
uint VertexFetchSlot(uint i) { return vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 576 + i * 4); }
uint ShapeCount() { return vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 972); }
uint VertexIndexOffset() { return vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 1052); }
uint4 VertexFetchData4(uint offset)
{
    const uint64_t base = vk::RawBufferLoad<uint64_t>(g_PushConstants.SharedConstants + 560, 8);
    return vk::RawBufferLoad<uint4>(base + offset, 4);
}
#else
cbuffer VertexShaderConstants : register(b0, space4)
{
    float4 g_VsConstants[256];
};
cbuffer SharedConstants : register(b2, space4)
{
    float2 g_ScreenOffset : packoffset(c34.x);
    float2 g_ScreenScale : packoffset(c35.z);
    uint4 g_VertexFetchSlots[24] : packoffset(c36);
    uint g_ScreenSpace : packoffset(c60.z);
    uint g_ShapeCount : packoffset(c60.w);
    uint g_VertexIndexOffset : packoffset(c65.w);
    float2 g_ViewportSize : packoffset(c83.x);
    float4 g_ClipPlanes[6] : packoffset(c85);
    uint g_ClipPlaneMask : packoffset(c91.x);
};
// The draw's copies of the buffers its vertex shader reads by address (UploadVertexFetchData).
ByteAddressBuffer g_VertexFetchData : register(t0, space5);

float4 VsConstant(uint r) { return g_VsConstants[r]; }
float2 ScreenOffset() { return g_ScreenOffset; }
float2 ScreenScale() { return g_ScreenScale; }
uint ScreenSpace() { return g_ScreenSpace; }
float2 ViewportSize() { return g_ViewportSize; }
float4 ClipPlane(uint i) { return g_ClipPlanes[i]; }
uint ClipPlaneMask() { return g_ClipPlaneMask; }
uint VertexFetchSlot(uint i) { return g_VertexFetchSlots[i / 4][i % 4]; }
uint ShapeCount() { return g_ShapeCount; }
uint VertexIndexOffset() { return g_VertexIndexOffset; }
uint4 VertexFetchData4(uint offset) { return g_VertexFetchData.Load4(offset); }
#endif

#include "renut_shapes.hlsli"

// A user clip plane's distance, or 1 (kept) when it is off, as recompiled shaders give it.
float ClipDistance(float4 position, uint mask, uint index)
{
    return ((mask >> index) & 1u) != 0u ? dot(position, ClipPlane(index)) : 1.0;
}

void main(
    [[vk::location(0)]] in float4 iPosition : POSITION0,
    [[vk::location(5)]] in float4 iNormal : NORMAL0,
    [[vk::location(12)]] in float4 iColor : COLOR0,
    [[vk::location(8)]] in float4 iTexcoord : TEXCOORD0,
    in uint iVertexId : SV_VertexID,
    out float4 oPos : SV_Position,
    out float4 oTexcoord[16] : TEXCOORD0,
    out float4 oColor[2] : COLOR0,
    out float4 oNormal[3] : NORMAL0,
    out float4 oTangent : TANGENT0,
    out float4 oBinormal : BINORMAL0,
    out float2 oPointCoord : POINTCOORD,
    out float oPointSize : POINTSIZE,
    out float4 oClip0 : SV_ClipDistance0,
    out float2 oClip1 : SV_ClipDistance1)
{
    float3 shaped = iPosition.xyz, shapedNormal = iNormal.xyz;
    ApplyShapes(iVertexId, shaped, shapedNormal);
    shapedNormal *= rsqrt(max(dot(shapedNormal, shapedNormal), 1e-12));
    const float4 position = float4(shaped, 1.0);
    oPos = float4(dot(position, VsConstant(0)), dot(position, VsConstant(1)), dot(position, VsConstant(2)),
                  dot(position, VsConstant(3)));
    const float3 normal = float3(dot(shapedNormal, VsConstant(120).xyz), dot(shapedNormal, VsConstant(121).xyz),
                                 dot(shapedNormal, VsConstant(122).xyz));

    [unroll] for (uint i = 0; i < 16; ++i)
        oTexcoord[i] = 0.0;
    oTexcoord[0] = float4(normal, 0.0);
    // The half4 colour arrives with each word's halves exchanged, as the part shader's position does.
    oTexcoord[1] = float4(iColor.y, iColor.x, iColor.w, iColor.z);
    oTexcoord[2] = float4(iTexcoord.xy, 0.0, 0.0);
    oColor[0] = 0.0;
    oColor[1] = 0.0;
    oNormal[0] = 0.0;
    oNormal[1] = 0.0;
    oNormal[2] = 0.0;
    oTangent = 0.0;
    oBinormal = 0.0;
    oPointCoord = 0.0;
    oPointSize = 0.0;

    const uint mask = ClipPlaneMask();
    oClip0 = float4(ClipDistance(oPos, mask, 0u), ClipDistance(oPos, mask, 1u), ClipDistance(oPos, mask, 2u),
                    ClipDistance(oPos, mask, 3u));
    oClip1 = float2(ClipDistance(oPos, mask, 4u), ClipDistance(oPos, mask, 5u));
    // With the viewport transform off, positions are pixels (as for recompiled shaders).
    if (ScreenSpace() != 0)
    {
        const float2 pixel = oPos.xy * ScreenScale() + ScreenOffset() * oPos.w;
        oPos.xy = pixel * float2(2.0, -2.0) / ViewportSize() + float2(-1.0, 1.0) * oPos.w;
    }
}
