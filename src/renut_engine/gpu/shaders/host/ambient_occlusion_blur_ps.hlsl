// renut_ssao: one direction of a depth-aware blur of the AO (see
// ambient_occlusion.hlsli): nine texels, gaussian, each weighed down the more
// its view depth differs from the centre's, so the AO does not bleed over edges.
#include "ambient_occlusion.hlsli"

float4 main(PSInput input) : SV_Target0
{
    const float4 center = AoInput(input.uv);
    if (center.y >= g_Ao.farZ * 0.999)
        return center;
    const float2 step = (g_Ao.flags & 0x100u) != 0u ? float2(0.0, g_Ao.texel.y) : float2(g_Ao.texel.x, 0.0);
    const float tolerance = 0.05 * center.y + 0.05;
    float sum = center.x;
    float weight = 1.0;
    [unroll] for (int i = -4; i <= 4; i++)
    {
        if (i == 0)
            continue;
        const float4 s = AoInput(input.uv + step * float(i));
        const float w = exp(-float(i * i) / 8.0) * saturate(1.0 - abs(s.y - center.y) / tolerance);
        sum += s.x * w;
        weight += w;
    }
    return float4(sum / weight, center.y, 0.0, 0.0);
}
