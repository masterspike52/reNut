// Depth resolve: writes a host depth target as a Xenos EDRAM depth resolve would,
// as fetched depth (SV_Target0) and guest words as 8_8_8_8 bytes (SV_Target1).
#define UTILITY_OWN_TEXTURE_TABLE
#include "utility.hlsli"

#if DEPTH_RESOLVE_MS
Texture2DMS<float> g_Depth[] : register(t0, space0);
#else
Texture2D<float> g_Depth[] : register(t0, space0);
#endif

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PSOutput
{
    float depth : SV_Target0;
    float4 words : SV_Target1;
};

uint EncodeUnorm24(float depth)
{
    return uint(saturate(depth) * 16777215.0 + 0.5);
}

// 20e4: a 4-bit exponent (bias 15) above a 20-bit mantissa, covering [0, 2);
// denormals below 2^-14 count steps of 2^-34. Truncated, like the unorm path.
uint EncodeFloat20e4(float depth)
{
    const float d = clamp(depth, 0.0, 1.99999988);
    if (d < 6.103515625e-5) // 2^-14
        return uint(d * 17179869184.0); // 2^34
    const uint bits = asuint(d);
    return (((bits >> 23) - 112) << 20) | ((bits >> 3) & 0xFFFFF);
}

PSOutput main(PSInput input)
{
    const int2 texel = int2(input.position.xy) + int2(g_Push.translate);
    const uint index = NonUniformResourceIndex(g_Push.textureIndex);
#if DEPTH_RESOLVE_MS
    const float depth = g_Depth[index].Load(texel, 0);
#else
    const float depth = g_Depth[index].Load(int3(texel, 0));
#endif
    const uint word = (g_Push.param0 != 0.0 ? EncodeFloat20e4(depth) : EncodeUnorm24(depth)) << 8;

    PSOutput output;
    output.depth = depth;
    output.words = float4(word & 0xFF, (word >> 8) & 0xFF, (word >> 16) & 0xFF, word >> 24) / 255.0;
    return output;
}
