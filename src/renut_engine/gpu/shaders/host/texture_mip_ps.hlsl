// One host level of a scaled resolve destination (textures.cpp,
// UpdateResolveMipsLocked): each texel is the average of the param0 x param0
// texels of the level above it covers (level 0, or the previous level's
// scratch target). The source is read with Load through a view without the
// fetch swizzle, so any color format works. present_vs draws the fullscreen
// triangle; the scissor limits it to the resolved area.
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
