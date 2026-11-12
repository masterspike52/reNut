// The frame blur's first pass (backend/frame_blur.cpp): the shown frame at a quarter of its size, four bilinear
// taps a texel apart averaging 4x4 texels. param0 = the output gamma, as present_ps applies it.
#include "utility.hlsli"

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    uint width, height;
    g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].GetDimensions(width, height);
    const float2 texel = 1.0 / float2(max(width, 1u), max(height, 1u));
    float3 sum = 0.0;
    sum += SampleBindless(g_Push.textureIndex, g_Push.samplerIndex, input.uv + texel * float2(-1.0, -1.0)).rgb;
    sum += SampleBindless(g_Push.textureIndex, g_Push.samplerIndex, input.uv + texel * float2(1.0, -1.0)).rgb;
    sum += SampleBindless(g_Push.textureIndex, g_Push.samplerIndex, input.uv + texel * float2(-1.0, 1.0)).rgb;
    sum += SampleBindless(g_Push.textureIndex, g_Push.samplerIndex, input.uv + texel * float2(1.0, 1.0)).rgb;
    return float4(pow(saturate(sum * 0.25), g_Push.param0), 1.0);
}
