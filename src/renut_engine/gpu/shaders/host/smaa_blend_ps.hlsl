// SMAA pass 3 (smaa_common.hlsli): the frame (textureIndex) blended with its
// neighbours by the weights (their slot in param0, as uint bits).
#include "smaa_common.hlsli"

float4 main(SmaaInput input) : SV_Target0
{
    SmaaBegin(g_Push.textureIndex);
    float4 offset;
    SMAANeighborhoodBlendingVS(input.uv, offset);
    return SMAANeighborhoodBlendingPS(input.uv, offset, SMAA_TEX(g_Push.textureIndex),
                                      SMAA_TEX(asuint(g_Push.param0)));
}
