// The pixel shader of NutTool's sky models (renut_sky_* in shader_library.cpp): each vertex's colour
// times the draw's texture (fetch constant 0), unlit and without glow, reading no game constants.

#ifdef __spirv__
struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

uint TextureIndex() { return vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 0); }
uint SamplerIndex() { return vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 384); }
#else
cbuffer SharedConstants : register(b2, space4)
{
    uint g_TextureIndex : packoffset(c0.x);
    uint g_SamplerIndex : packoffset(c24.x);
};

uint TextureIndex() { return g_TextureIndex; }
uint SamplerIndex() { return g_SamplerIndex; }
#endif

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

static const uint kDescriptorIndexMask = 0x03FFFFFFu;

void main(
    in float4 iPos : SV_Position,
    in float4 iTexcoord[16] : TEXCOORD0,
    in float4 iColor[2] : COLOR0,
    in float4 iNormal[3] : NORMAL0,
    in float4 iTangent : TANGENT0,
    in float4 iBinormal : BINORMAL0,
    in float2 iPointCoord : POINTCOORD,
#ifdef __spirv__
    in bool iFace : SV_IsFrontFace,
#else
    in uint iFace : SV_IsFrontFace,
#endif
    out float4 oC0 : SV_Target0)
{
    const float4 texel = g_Texture2DDescriptorHeap[TextureIndex() & kDescriptorIndexMask].Sample(
        g_SamplerDescriptorHeap[SamplerIndex()], iTexcoord[2].xy);
    // N&B's post-processing reads the colour target's alpha as glow: the sky has none.
    oC0 = float4(saturate(iTexcoord[1].rgb * texel.rgb), 0.0);
}
