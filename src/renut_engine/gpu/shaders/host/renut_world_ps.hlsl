// The pixel shader of NutTool's world models (renut_world_* in shader_library.cpp): each vertex's
// colour times the draw's texture (Xenos texture fetch constant 0), lit by N&B's two lights.
//
// N&B's pixel constants: c32 / c51 the key light's colour and direction, c81 / c82 the second
// light's, c45 the highlight tint, c50 x the alpha the game sets for the draw (sub_82375928).

#ifdef __spirv__
struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

float4 PsConstant(uint r) { return vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + r * 16, 16); }
// The shared constants' descriptor indices: fetch constant n's 2D texture (word n) and sampler (word 96 + n).
uint TextureIndex(uint n) { return vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + n * 4); }
uint SamplerIndex(uint n) { return vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 384 + n * 4); }
#else
cbuffer PixelShaderConstants : register(b1, space4)
{
    float4 g_PsConstants[256];
};
cbuffer SharedConstants : register(b2, space4)
{
    uint4 g_TextureIndices : packoffset(c0);
    uint4 g_SamplerIndices : packoffset(c24);
};

float4 PsConstant(uint r) { return g_PsConstants[r]; }
uint TextureIndex(uint n) { return g_TextureIndices[n]; }
uint SamplerIndex(uint n) { return g_SamplerIndices[n]; }
#endif

// The descriptor heaps, as recompiled shaders declare them (XenosRecomp's shader_common.h).
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

// A descriptor index carries flags above its low 26 bits.
static const uint kDescriptorIndexMask = 0x03FFFFFFu;

static const uint kKeyColour = 32;
static const uint kKeyDirection = 51;
static const uint kFillColour = 81;
static const uint kFillDirection = 82;
static const uint kHighlight = 45;
static const uint kAlphaMode = 50;
// reNut's (draw.cpp MaterialConstants): c240 the material's emissive rgb and glow, c241 its opacity,
// cutoff, alpha mode and flags. Its emissive texture is in fetch constant 1.
static const uint kMaterialEmissive = 240;
static const uint kMaterialAlpha = 241;

// c241.z: the alpha mode; c241.w: flags (double-sided, unlit, emissive texture, a material at all).
static const uint kModeOpaque = 0, kModeCutout = 1, kModeBlend = 2, kModeAdd = 3;
static const uint kFlagDoubleSided = 4, kFlagUnlit = 8, kFlagEmissiveTexture = 16, kFlagMaterial = 32;

// A colour scaled down so its brightest channel is at most 1 (black stays black).
float3 Hue(float3 c)
{
    return c / max(max(max(c.r, c.g), c.b), 1.0);
}

float4 Fetch(uint n, float2 uv)
{
    return g_Texture2DDescriptorHeap[TextureIndex(n) & kDescriptorIndexMask].Sample(g_SamplerDescriptorHeap[SamplerIndex(n)], uv);
}

void main(
    in float4 iPos : SV_Position,
    in float4 iTexcoord[16] : TEXCOORD0,
    in float4 iColor[2] : COLOR0,
    in float4 iNormal[3] : NORMAL0,
    in float4 iTangent : TANGENT0,
    in float4 iBinormal : BINORMAL0,
    in float2 iPointCoord : POINTCOORD,
#ifdef __spirv__
    in bool iFace : SV_IsFrontFace,
#else
    in uint iFace : SV_IsFrontFace,
#endif
    out float4 oC0 : SV_Target0)
{
    const float4 emissiveGlow = PsConstant(kMaterialEmissive);
    const float4 alphaInfo = PsConstant(kMaterialAlpha);
    const uint mode = asuint(alphaInfo.z);
    const uint flags = asuint(alphaInfo.w);

    float3 normal = iTexcoord[0].xyz;
    const float length2 = dot(normal, normal);
    normal = length2 > 1e-8 ? normal * rsqrt(length2) : float3(0.0, 1.0, 0.0);
    // A double-sided face seen from behind is lit as its back.
    if ((flags & kFlagDoubleSided) != 0u && !iFace)
        normal = -normal;

    // As the part shader: the lights' hues only, and some light from every side.
    const float3 key = Hue(PsConstant(kKeyColour).rgb) * saturate(dot(normal, PsConstant(kKeyDirection).xyz));
    const float3 fill = Hue(PsConstant(kFillColour).rgb) * saturate(dot(normal, PsConstant(kFillDirection).xyz));
    const float ambient = lerp(0.15, 0.25, normal.y * 0.5 + 0.5);
    const float3 light = (flags & kFlagUnlit) != 0u ? 1.0 : saturate(ambient + 0.35 * key + 0.1 * fill);

    // The same index for every pixel of a draw. Cutting out takes the texture's alpha (times the
    // opacity); see-through modes take the vertex colour's too (a glTF base colour's alpha).
    const bool material = (flags & kFlagMaterial) != 0u;
    const float4 texel = Fetch(0u, iTexcoord[2].xy);
    const float alpha = texel.a * (material ? alphaInfo.x : 1.0) * (mode >= kModeBlend ? iTexcoord[1].a : 1.0);
    // Cut out (leaves, fences): what is under the cutoff isn't drawn.
    if (mode == kModeCutout)
        clip(alpha - alphaInfo.y);
    const float3 colour = saturate(iTexcoord[1].rgb * texel.rgb);
    float3 lit = colour * light * Hue(PsConstant(kHighlight).rgb);

    // What it gives off, unlit, over the light: the emissive colour, times its texture when it has one.
    float3 emissive = emissiveGlow.rgb;
    if ((flags & kFlagEmissiveTexture) != 0u)
        emissive *= Fetch(1u, iTexcoord[2].xy).rgb;
    lit += emissive;

    // N&B's bloom reads the target's alpha as glow (its bright pass takes rgb times 0.05 plus it): the
    // material's own, else full where it gives off light, else the game's alpha as before.
    const float emitting = max(max(emissive.r, emissive.g), emissive.b) > 0.0 ? 1.0 : 0.0;
    const float glow = emissiveGlow.a >= 0.0 ? emissiveGlow.a : (material && emitting > 0.0 ? 1.0 : saturate(PsConstant(kAlphaMode).x));
    if (mode == kModeBlend)
        oC0 = float4(lit, saturate(alpha));  // blended by its alpha; the target's glow is kept
    else if (mode == kModeAdd)
        oC0 = float4(lit * saturate(alpha), saturate(glow));
    else
        oC0 = float4(lit, saturate(glow));
}
