#include "utility.hlsli"

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

float4 main(PSInput input) : SV_Target0
{
    return input.color * SampleBindless(g_Push.textureIndex, g_Push.samplerIndex, input.uv);
}
