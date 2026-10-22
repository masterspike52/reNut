// SMAA pass 2 (smaa_common.hlsli): blending weights from the edges
// (textureIndex), with SMAA's area and search textures (their slots in param0
// and param1, as uint bits).
#include "smaa_common.hlsli"

float4 main(SmaaInput input) : SV_Target0
{
    SmaaBegin(g_Push.textureIndex);
    float2 pixcoord;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(input.uv, pixcoord, offset);
    return SMAABlendingWeightCalculationPS(input.uv, pixcoord, offset, SMAA_TEX(g_Push.textureIndex),
                                           SMAA_TEX(asuint(g_Push.param0)), SMAA_TEX(asuint(g_Push.param1)),
                                           float4(0.0, 0.0, 0.0, 0.0));
}
