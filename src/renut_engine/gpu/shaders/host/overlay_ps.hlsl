#include "utility.hlsli"

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

float4 main(PSInput input) : SV_Target0
{
    float4 texel = SampleBindless(g_Push.textureIndex, g_Push.samplerIndex, input.uv);
    // param0 = 1: the game's frame (GameFrameTexture), opaque whatever its alpha.
    if (g_Push.param0 > 0.5)
        texel.a = 1.0;
    return input.color * texel;
}
