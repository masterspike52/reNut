// One direction of the frame blur (backend/frame_blur.cpp): 15 taps of a gaussian whose sigma is 3 taps.
// param0 = the taps' spacing in texels; param1 = 1 for vertical, 0 for horizontal.
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
    const float2 step = (g_Push.param1 > 0.5 ? float2(0.0, texel.y) : float2(texel.x, 0.0)) * g_Push.param0;
    float3 sum = 0.0;
    float total = 0.0;
    [unroll] for (int i = -7; i <= 7; i++)
    {
        const float weight = exp(-float(i * i) / 18.0);
        sum += SampleBindless(g_Push.textureIndex, g_Push.samplerIndex, input.uv + step * float(i)).rgb * weight;
        total += weight;
    }
    return float4(sum / total, 1.0);
}
