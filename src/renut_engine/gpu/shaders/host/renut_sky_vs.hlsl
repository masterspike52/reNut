// The vertex shader of NutTool's sky models (renut_sky_* in shader_library.cpp), run in place of a
// guest one. The game draws a sky (scene op_2C) around the camera; each vertex here is a direction.

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
#else
cbuffer VertexShaderConstants : register(b0, space4)
{
    float4 g_VsConstants[256];
};
cbuffer SharedConstants : register(b2, space4)
{
    float2 g_ScreenOffset : packoffset(c34.x);
    float2 g_ScreenScale : packoffset(c35.z);
    uint g_ScreenSpace : packoffset(c60.z);
    float2 g_ViewportSize : packoffset(c83.x);
    float4 g_ClipPlanes[6] : packoffset(c85);
    uint g_ClipPlaneMask : packoffset(c91.x);
};

float4 VsConstant(uint r) { return g_VsConstants[r]; }
float2 ScreenOffset() { return g_ScreenOffset; }
float2 ScreenScale() { return g_ScreenScale; }
uint ScreenSpace() { return g_ScreenSpace; }
float2 ViewportSize() { return g_ViewportSize; }
float4 ClipPlane(uint i) { return g_ClipPlanes[i]; }
uint ClipPlaneMask() { return g_ClipPlaneMask; }
#endif

float ClipDistance(float4 position, uint mask, uint index)
{
    return ((mask >> index) & 1u) != 0u ? dot(position, ClipPlane(index)) : 1.0;
}

// Depth over w of every point at infinity (a perspective projection's is the same for all of them):
// near 1 when depth grows away from the camera, near 0 when it is reversed.
float DepthAtInfinity()
{
    const float4 w = VsConstant(3), z = VsConstant(2);
    const uint k = abs(w.x) >= abs(w.y) && abs(w.x) >= abs(w.z) ? 0u : (abs(w.y) >= abs(w.z) ? 1u : 2u);
    return abs(w[k]) > 1e-12 ? z[k] / w[k] : 1.0;
}

void main(
    [[vk::location(0)]] in float4 iPosition : POSITION0,
    [[vk::location(5)]] in float4 iNormal : NORMAL0,
    [[vk::location(12)]] in float4 iColor : COLOR0,
    [[vk::location(8)]] in float4 iTexcoord : TEXCOORD0,
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
    // The vertex's direction from the camera, at infinity, just inside the far plane: the world
    // always draws in front of it, however far it reaches.
    const float4 direction = float4(iPosition.xyz, 0.0);
    oPos = float4(dot(direction, VsConstant(0)), dot(direction, VsConstant(1)), dot(direction, VsConstant(2)),
                  dot(direction, VsConstant(3)));
    oPos.z = oPos.w * (DepthAtInfinity() > 0.5 ? 0.99999 : 0.00001);

    [unroll] for (uint i = 0; i < 16; ++i)
        oTexcoord[i] = 0.0;
    // The half4 colour arrives with each word's halves exchanged, as the world shader's does.
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
    if (ScreenSpace() != 0)
    {
        const float2 pixel = oPos.xy * ScreenScale() + ScreenOffset() * oPos.w;
        oPos.xy = pixel * float2(2.0, -2.0) / ViewportSize() + float2(-1.0, 1.0) * oPos.w;
    }
}
