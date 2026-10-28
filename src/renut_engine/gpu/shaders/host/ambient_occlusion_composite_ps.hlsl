// renut_ssao: the AO over the scene (see ambient_occlusion.hlsli), drawn into
// the scene's color target with a multiplying blend (dest * source). The AO
// texture may be smaller than the scene: its four texels around the pixel are
// weighted as bilinear filtering weighs them and by how near their view depth
// is to the pixel's, so the AO keeps to the surfaces it belongs to.
#include "ambient_occlusion.hlsli"

float4 main(PSInput input) : SV_Target0
{
    const float depth = SceneDepth(input.uv);
    if (IsFar(depth))
        return 1.0;
    const float z = ViewDepth(depth);
    const float2 size = 1.0 / g_Ao.texel;
    const float2 coord = input.uv * size - 0.5;
    const float2 f = frac(coord);
    const float2 base = (floor(coord) + 0.5) * g_Ao.texel;
    const float tolerance = 0.05 * z + 0.05;
    float sum = 0.0;
    float weight = 0.0;
    [unroll] for (int j = 0; j < 2; j++)
    {
        [unroll] for (int i = 0; i < 2; i++)
        {
            const float4 s = AoInput(base + float2(i, j) * g_Ao.texel);
            const float bilinear = (i != 0 ? f.x : 1.0 - f.x) * (j != 0 ? f.y : 1.0 - f.y);
            const float w = (bilinear + 1e-3) * saturate(1.0 - abs(s.y - z) / tolerance);
            sum += s.x * w;
            weight += w;
        }
    }
    const float ao = weight > 1e-4 ? sum / weight : AoInput(input.uv).x;
    return float4(ao.xxx, 1.0);
}
