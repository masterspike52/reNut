#include "utility.hlsli"

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    float4 color = SampleBindless(g_Push.textureIndex, g_Push.samplerIndex, input.uv);
    // param0 = output gamma exponent (1 = passthrough).
    color.rgb = pow(saturate(color.rgb), g_Push.param0);
    return float4(color.rgb, 1.0);
}
