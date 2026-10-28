// renut_ssao (render/ambient_occlusion.h): screen-space ambient occlusion from
// the scene's resolved depth, drawn with present_vs. Bindings as utility.hlsli's
// (bindless textures t0, space0; samplers s0, space1), with a push block of its
// own (b0, space2) whose first fields are UtilityPush's scale and translate,
// which present_vs reads. Must match AmbientOcclusionPush in
// ambient_occlusion.cpp.

struct AmbientOcclusionPush
{
    float2 scale;
    float2 translate;
    uint depthIndex;    // the resolved scene depth (R32_FLOAT, 0 near .. 1 far)
    uint inputIndex;    // the blur's and the composite's AO texture (R16G16_FLOAT: AO, view depth)
    uint samplerIndex;  // point clamp
    uint flags;         // bits 0-7 the AO pass's sample count; bit 8 the blur runs vertically
    float2 texel;       // 1 / the size of the texture the pass reads texel by texel
    float2 viewRay;     // tan of half the horizontal and vertical field of view
    float nearZ;
    float farZ;
    float radius;       // world units
    float intensity;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<AmbientOcclusionPush> g_Ao;
#else
ConstantBuffer<AmbientOcclusionPush> g_Ao : register(b0, space2);
#endif

Texture2D<float4> g_Textures[] : register(t0, space0);
SamplerState g_Samplers[] : register(s0, space1);

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float SceneDepth(float2 uv)
{
    return g_Textures[g_Ao.depthIndex].SampleLevel(g_Samplers[g_Ao.samplerIndex], uv, 0.0).x;
}

float4 AoInput(float2 uv)
{
    return g_Textures[g_Ao.inputIndex].SampleLevel(g_Samplers[g_Ao.samplerIndex], uv, 0.0);
}

// The sky and whatever else lies at the far plane.
bool IsFar(float depth)
{
    return depth >= 0.99999;
}

// N&B's projection (meCalculateProjection, 0x822BC0E0) maps view distance z to
// depth far / (far - near) - far * near / ((far - near) z).
float ViewDepth(float depth)
{
    return g_Ao.nearZ * g_Ao.farZ / (g_Ao.farZ - depth * (g_Ao.farZ - g_Ao.nearZ));
}

// View space: x right, y up, z into the screen.
float3 ViewPosition(float2 uv, float z)
{
    return float3((uv.x * 2.0 - 1.0) * g_Ao.viewRay.x * z, (1.0 - uv.y * 2.0) * g_Ao.viewRay.y * z, z);
}
