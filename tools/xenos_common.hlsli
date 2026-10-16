// Helpers for Xenos shaders decompiled to D3D9 / SM3 HLSL by decompile.py.
// Each emulates a Xenos ALU / fetch behaviour that has no direct SM3 equivalent.

#ifndef XENOS_COMMON_HLSLI
#define XENOS_COMMON_HLSLI

#define XE_FLT_MAX 3.402823466e+38

// ---------------------------------------------------------------- scalar ops

// muls_prev2: a * ps, unless ps is -FLT_MAX / non-finite or b is non-finite / <= 0.
float xe_muls_prev2(float a, float b, float ps)
{
    bool bad = ps == -XE_FLT_MAX || isinf(ps) || isnan(ps) || isinf(b) || isnan(b) || b <= 0.0;
    return bad ? -XE_FLT_MAX : a * ps;
}

// These test the input rather than the result: the infinite results only come from a
// zero input, and fxc rejects the infinity literals that folding 1/0 would create.

// logc: log2 with -inf (log2(0)) clamped to -FLT_MAX.
float xe_logc(float a)
{
    return a == 0.0 ? -XE_FLT_MAX : log2(a);
}

// rcpc / rsqc: the infinite result of a zero input clamps to FLT_MAX.
float xe_rcpc(float a)
{
    return a == 0.0 ? XE_FLT_MAX : 1.0 / a;
}

float xe_rsqc(float a)
{
    return a == 0.0 ? XE_FLT_MAX : rsqrt(a);
}

// rcpf / rsqf: the infinite result of a zero input flushes to 0.
float xe_rcpf(float a)
{
    return a == 0.0 ? 0.0 : 1.0 / a;
}

float xe_rsqf(float a)
{
    return a == 0.0 ? 0.0 : rsqrt(a);
}

// ---------------------------------------------------------------- address register

// mova / maxa: round to nearest, clamped to the a0 range [-256, 255].
int xe_mova(float a)
{
    return (int)clamp(floor(a + 0.5), -256.0, 255.0);
}

// mova_floor (maxasf): truncate toward -inf, clamped to the a0 range.
int xe_mova_floor(float a)
{
    return (int)clamp(floor(a), -256.0, 255.0);
}

// ---------------------------------------------------------------- vector ops

float xe_max4(float4 v)
{
    return max(max(v.x, v.y), max(v.z, v.w));
}

// cube: dir is the (x, y, z) direction (the instruction's src0.zzxy picks x, y, z).
// Result = (tc, sc, 2 * major axis, face id), faces +X -X +Y -Y +Z -Z = 0..5,
// using the D3D cube map face conventions.
float4 xe_cube(float3 dir)
{
    float3 a = abs(dir);
    float ma, sc, tc, face;
    if (a.z >= a.x && a.z >= a.y)
    {
        ma = dir.z;
        face = dir.z < 0.0 ? 5.0 : 4.0;
        sc = dir.z < 0.0 ? -dir.x : dir.x;
        tc = -dir.y;
    }
    else if (a.y >= a.x)
    {
        ma = dir.y;
        face = dir.y < 0.0 ? 3.0 : 2.0;
        sc = dir.x;
        tc = dir.y < 0.0 ? -dir.z : dir.z;
    }
    else
    {
        ma = dir.x;
        face = dir.x < 0.0 ? 1.0 : 0.0;
        sc = dir.x < 0.0 ? dir.z : -dir.z;
        tc = -dir.y;
    }
    return float4(tc, sc, 2.0 * ma, face);
}

// Inverse for cube texture fetches: coord = (s, t, face) as the shader passes it to
// tfetchCube, where s / t are sc / tc divided by |2 * ma| and biased by +1.5 (the
// standard compiler sequence after cube). Returns a direction usable with texCUBE.
float3 xe_cube_dir(float3 coord)
{
    float s = coord.x - 1.5;
    float t = coord.y - 1.5;
    float face = floor(coord.z + 0.5);
    float3 dir;
    if (face < 0.5)
        dir = float3(0.5, -t, -s);        // +X
    else if (face < 1.5)
        dir = float3(-0.5, -t, s);        // -X
    else if (face < 2.5)
        dir = float3(s, 0.5, t);          // +Y
    else if (face < 3.5)
        dir = float3(s, -0.5, -t);        // -Y
    else if (face < 4.5)
        dir = float3(s, -t, 0.5);         // +Z
    else
        dir = float3(-s, -t, -0.5);       // -Z
    return dir;
}

// ---------------------------------------------------------------- fetches

// vfetch with a computed index: an indexed read from a vertex buffer (bone / instance
// data), not a per-vertex stream attribute. SM3 has no buffer loads (and only 4 vertex
// samplers), so every such buffer lives in one float4 texture, xe_vfetch_heap: one texel
// per 4 dwords, row-major, size = (w, h); base = the buffer's first texel.
float4 xe_vfetch(sampler2D heap, float2 size, float base, float index, float offsetDwords, float strideDwords)
{
    float texel = base + floor(index + 0.5) * (strideDwords / 4.0) + floor(offsetDwords / 4.0);
    float y = floor(texel / size.x);
    float x = texel - y * size.x;
    return tex2Dlod(heap, float4((x + 0.5) / size.x, (y + 0.5) / size.y, 0.0, 0.0));
}

// getCompTexLOD: the mip LOD the hardware would select for coord; texelSize = 1 / size.
float xe_comp_lod(float coord, float texelSize)
{
    float dx = ddx(coord) / texelSize;
    float dy = ddy(coord) / texelSize;
    return 0.5 * log2(max(dx * dx, dy * dy));
}

float xe_comp_lod(float2 coord, float2 texelSize)
{
    float2 dx = ddx(coord) / texelSize;
    float2 dy = ddy(coord) / texelSize;
    return 0.5 * log2(max(dot(dx, dx), dot(dy, dy)));
}

float xe_comp_lod(float3 coord, float3 texelSize)
{
    float3 dx = ddx(coord) / texelSize;
    float3 dy = ddy(coord) / texelSize;
    return 0.5 * log2(max(dot(dx, dx), dot(dy, dy)));
}

#endif // XENOS_COMMON_HLSLI
