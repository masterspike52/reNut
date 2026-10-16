// Shared bindings for the renderer's own (non-guest) shaders.
//   set 0 : bindless Texture2D table   (t0, space0)
//   set 1 : bindless sampler table     (s0, space1)
//   push  : UtilityPush                (b0, space2), vertex + pixel stages
// Must match BuildUtilityLayout in video.cpp.

struct UtilityPush
{
    float2 scale;
    float2 translate;
    uint textureIndex;
    uint samplerIndex;
    float param0;
    float param1;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<UtilityPush> g_Push;
#else
ConstantBuffer<UtilityPush> g_Push : register(b0, space2);
#endif

// A shader that reads the table as another texture type (depth_resolve.hlsli)
// defines UTILITY_OWN_TEXTURE_TABLE and declares t0, space0 itself.
#ifndef UTILITY_OWN_TEXTURE_TABLE
Texture2D<float4> g_Textures[] : register(t0, space0);
SamplerState g_Samplers[] : register(s0, space1);

float4 SampleBindless(uint textureIndex, uint samplerIndex, float2 uv)
{
    return g_Textures[NonUniformResourceIndex(textureIndex)].Sample(
        g_Samplers[NonUniformResourceIndex(samplerIndex)], uv);
}
#endif
