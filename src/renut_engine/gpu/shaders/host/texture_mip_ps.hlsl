// One host mip level of a scaled resolve destination: each texel is the average
// of the param0 x param0 texels it covers in the level above.
#include "utility.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0
{
    Texture2D<float4> source = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)];
    uint width, height;
    source.GetDimensions(width, height);
    const int box = int(g_Push.param0);
    const int2 origin = int2(position.xy) * box;
    const int2 last = int2(width, height) - 1;
    float4 sum = 0.0;
    for (int y = 0; y < box; ++y)
    {
        for (int x = 0; x < box; ++x)
            sum += source.Load(int3(min(origin + int2(x, y), last), 0));
    }
    return sum / float(box * box);
}
