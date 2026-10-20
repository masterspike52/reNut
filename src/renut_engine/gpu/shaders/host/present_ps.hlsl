#include "utility.hlsli"

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 SampleAt(float2 uv)
{
    return g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].SampleLevel(
        g_Samplers[NonUniformResourceIndex(g_Push.samplerIndex)], uv, 0.0);
}

float Luma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

float LumaAt(float2 uv)
{
    return Luma(SampleAt(uv).rgb);
}

// renut_anti_aliasing FXAA: fast approximate anti-aliasing of the frame, in the
// frame's own texels. Where the local contrast is high enough to be an edge,
// the edge's direction (horizontal or vertical) is found from the 3x3
// neighbourhood, the edge is followed both ways to its ends, and the pixel is
// resampled half a texel across the edge by how far it is from the nearer end
// (plus a sub-pixel term for single-texel detail).
float3 Fxaa(float2 uv, float2 texel)
{
    const float kEdgeThreshold = 0.125;     // contrast that counts as an edge, relative to the brightest
    const float kEdgeThresholdMin = 0.0312; // ... and at least this (dark areas)
    const float kSubpixel = 0.75;           // how much single-texel detail is smoothed
    const int kSteps = 10;
    const float kStepSizes[10] = { 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0 };

    const float3 rgbM = SampleAt(uv).rgb;
    const float lumaM = Luma(rgbM);
    const float lumaN = LumaAt(uv + float2(0.0, -texel.y));
    const float lumaS = LumaAt(uv + float2(0.0, texel.y));
    const float lumaE = LumaAt(uv + float2(texel.x, 0.0));
    const float lumaW = LumaAt(uv + float2(-texel.x, 0.0));
    const float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    const float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    const float range = lumaMax - lumaMin;
    if (range < max(kEdgeThresholdMin, lumaMax * kEdgeThreshold))
        return rgbM;

    const float lumaNW = LumaAt(uv + float2(-texel.x, -texel.y));
    const float lumaNE = LumaAt(uv + float2(texel.x, -texel.y));
    const float lumaSW = LumaAt(uv + float2(-texel.x, texel.y));
    const float lumaSE = LumaAt(uv + float2(texel.x, texel.y));

    // Sub-pixel blend: how far the centre is from its neighbours' average.
    const float lumaAverage = (2.0 * (lumaN + lumaS + lumaE + lumaW) + lumaNW + lumaNE + lumaSW + lumaSE) / 12.0;
    float subpixel = saturate(abs(lumaAverage - lumaM) / range);
    subpixel = smoothstep(0.0, 1.0, subpixel);
    subpixel = subpixel * subpixel * kSubpixel;

    // Edge direction: the larger of the horizontal and vertical second derivatives.
    const float edgeH = abs(lumaNW - 2.0 * lumaN + lumaNE) + 2.0 * abs(lumaW - 2.0 * lumaM + lumaE) +
                        abs(lumaSW - 2.0 * lumaS + lumaSE);
    const float edgeV = abs(lumaNW - 2.0 * lumaW + lumaSW) + 2.0 * abs(lumaN - 2.0 * lumaM + lumaS) +
                        abs(lumaNE - 2.0 * lumaE + lumaSE);
    const bool horizontal = edgeH >= edgeV;

    // Which side of the pixel the edge is on (the steeper gradient).
    const float luma1 = horizontal ? lumaN : lumaW;
    const float luma2 = horizontal ? lumaS : lumaE;
    const float gradient1 = abs(luma1 - lumaM);
    const float gradient2 = abs(luma2 - lumaM);
    const bool side1 = gradient1 >= gradient2;
    const float gradient = max(gradient1, gradient2) * 0.25;
    float stepLength = horizontal ? texel.y : texel.x;
    float lumaLocal;
    if (side1)
    {
        stepLength = -stepLength;
        lumaLocal = 0.5 * (luma1 + lumaM);
    }
    else
    {
        lumaLocal = 0.5 * (luma2 + lumaM);
    }

    // Start half a texel across, on the edge, and walk along it both ways.
    float2 edgeUv = uv;
    if (horizontal)
        edgeUv.y += stepLength * 0.5;
    else
        edgeUv.x += stepLength * 0.5;
    const float2 along = horizontal ? float2(texel.x, 0.0) : float2(0.0, texel.y);
    float2 uv1 = edgeUv - along;
    float2 uv2 = edgeUv + along;
    float end1 = LumaAt(uv1) - lumaLocal;
    float end2 = LumaAt(uv2) - lumaLocal;
    bool done1 = abs(end1) >= gradient;
    bool done2 = abs(end2) >= gradient;
    [loop]
    for (int i = 1; i < kSteps && !(done1 && done2); ++i)
    {
        if (!done1)
        {
            uv1 -= along * kStepSizes[i];
            end1 = LumaAt(uv1) - lumaLocal;
            done1 = abs(end1) >= gradient;
        }
        if (!done2)
        {
            uv2 += along * kStepSizes[i];
            end2 = LumaAt(uv2) - lumaLocal;
            done2 = abs(end2) >= gradient;
        }
    }

    const float distance1 = horizontal ? uv.x - uv1.x : uv.y - uv1.y;
    const float distance2 = horizontal ? uv2.x - uv.x : uv2.y - uv.y;
    const bool nearer1 = distance1 < distance2;
    const float distanceNearest = min(distance1, distance2);
    const float edgeLength = distance1 + distance2;
    // Only blend when the centre's luma is on the other side of the local
    // average than the nearer end (the pixel is on the edge's step).
    const bool centreSmaller = lumaM < lumaLocal;
    const bool correct = ((nearer1 ? end1 : end2) < 0.0) != centreSmaller;
    const float edgeBlend = correct ? 0.5 - distanceNearest / edgeLength : 0.0;
    const float blend = max(edgeBlend, subpixel);

    float2 finalUv = uv;
    if (horizontal)
        finalUv.y += blend * stepLength;
    else
        finalUv.x += blend * stepLength;
    return SampleAt(finalUv).rgb;
}

float4 main(PSInput input) : SV_Target0
{
    float3 color;
    // param1 = 1: FXAA (renut_anti_aliasing).
    if (g_Push.param1 > 0.5)
    {
        uint width, height;
        g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].GetDimensions(width, height);
        color = Fxaa(input.uv, 1.0 / float2(width, height));
    }
    else
    {
        color = SampleAt(input.uv).rgb;
    }
    // param0 = output gamma exponent (1 = passthrough).
    color = pow(saturate(color), g_Push.param0);
    return float4(color, 1.0);
}
