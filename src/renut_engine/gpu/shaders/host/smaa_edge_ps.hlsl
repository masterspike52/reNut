// SMAA pass 1 (smaa_common.hlsli): edges of the frame (textureIndex), by luma.
// Pixels without an edge are discarded, so the target is cleared first.
#include "smaa_common.hlsli"

float4 main(SmaaInput input) : SV_Target0
{
    SmaaBegin(g_Push.textureIndex);
    float4 offset[3];
    SMAAEdgeDetectionVS(input.uv, offset);
    return float4(SMAALumaEdgeDetectionPS(input.uv, offset, SMAA_TEX(g_Push.textureIndex)), 0.0, 0.0);
}
