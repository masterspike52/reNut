// renut_ssao: the occlusion of each pixel (see ambient_occlusion.hlsli). Samples
// a spiral disk of the occlusion radius around the pixel, turned by the
// pixel's noise, and counts what rises over the surface's plane within it,
// weighted by how far it rises and fading with distance (Alchemy / SAO).
// Out: x the AO (1 unoccluded), y the view depth (for the blur and composite).
#include "ambient_occlusion.hlsli"

float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float3 PositionAt(float2 uv)
{
    return ViewPosition(uv, ViewDepth(SceneDepth(uv)));
}

float4 main(PSInput input) : SV_Target0
{
    const float depth = SceneDepth(input.uv);
    if (IsFar(depth))
        return float4(1.0, g_Ao.farZ, 0.0, 0.0);
    const float z = ViewDepth(depth);
    const float3 p = ViewPosition(input.uv, z);

    // The normal from the depth's neighbours, each axis from the side nearer in
    // depth, so it does not bend over an edge.
    const float2 t = g_Ao.texel;
    const float3 right = PositionAt(input.uv + float2(t.x, 0.0));
    const float3 left = PositionAt(input.uv - float2(t.x, 0.0));
    const float3 down = PositionAt(input.uv + float2(0.0, t.y));
    const float3 up = PositionAt(input.uv - float2(0.0, t.y));
    const float3 dx = abs(right.z - z) < abs(z - left.z) ? right - p : p - left;
    const float3 dy = abs(down.z - z) < abs(z - up.z) ? down - p : p - up;
    float3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0.0)
        n = -n;  // towards the camera, at the origin

    // The radius on screen: the view spans 2 * viewRay * z, in uv 1.
    const float2 radiusUV = min(g_Ao.radius / (2.0 * z * g_Ao.viewRay), 0.2);
    const float turn = InterleavedGradientNoise(input.position.xy) * 6.28318531;
    const uint count = max(g_Ao.flags & 0xFFu, 1u);
    const float radius2 = g_Ao.radius * g_Ao.radius;
    float occlusion = 0.0;
    [loop] for (uint i = 0; i < count; i++)
    {
        const float along = (float(i) + 0.5) / float(count);
        const float angle = along * 43.9822972 + turn;  // seven turns out
        const float2 uv = input.uv + float2(cos(angle), sin(angle)) * radiusUV * along;
        const float3 v = PositionAt(uv) - p;
        const float vv = dot(v, v);
        const float falloff = saturate(1.0 - vv / radius2);
        occlusion += max(dot(v, n) * rsqrt(vv + 1e-4) - 0.1, 0.0) * falloff;
    }
    float ao = saturate(1.0 - g_Ao.intensity * occlusion / float(count));
    // Far off the radius covers a pixel or two: let it go.
    ao = lerp(ao, 1.0, saturate((z - 150.0) / 100.0));
    return float4(ao, z, 0.0, 0.0);
}
