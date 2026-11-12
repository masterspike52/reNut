// The pixel shader of NutTool's own models (renut_part_* in shader_library.cpp): one flat paint
// colour lit by N&B's two lights, tinted by the draw's highlight as the game's materials are.
//
// N&B's pixel constants: c28 the part's paint (put there by custom_parts.cpp), c32 / c51 the key
// light's colour and direction, c81 / c82 the second light's, c45 the highlight tint.

#ifdef __spirv__
struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

float4 PsConstant(uint r) { return vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + r * 16, 16); }
#else
cbuffer PixelShaderConstants : register(b1, space4)
{
    float4 g_PsConstants[256];
};

float4 PsConstant(uint r) { return g_PsConstants[r]; }
#endif

static const uint kPaint = 28;
static const uint kKeyColour = 32;
static const uint kKeyDirection = 51;
static const uint kFillColour = 81;
static const uint kFillDirection = 82;
static const uint kHighlight = 45;
static const uint kAlphaMode = 50;

// A colour scaled down so its brightest channel is at most 1 (black stays black).
float3 Hue(float3 c)
{
    return c / max(max(max(c.r, c.g), c.b), 1.0);
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
    float3 normal = iTexcoord[0].xyz;
    const float length2 = dot(normal, normal);
    normal = length2 > 1e-8 ? normal * rsqrt(length2) : float3(0.0, 1.0, 0.0);

    // The lights' colours can be brighter than 1 (HDR): only their hue is used, so a lit face is
    // never brighter than its paint and doesn't bloom.
    const float3 key = Hue(PsConstant(kKeyColour).rgb) * saturate(dot(normal, PsConstant(kKeyDirection).xyz));
    const float3 fill = Hue(PsConstant(kFillColour).rgb) * saturate(dot(normal, PsConstant(kFillDirection).xyz));
    // Some light from every side, more from above, so faces away from both lights aren't black. N&B's
    // own lit surfaces come out around 0.1 to 0.5, so a fully lit face is about 0.7 of its paint.
    const float ambient = lerp(0.15, 0.25, normal.y * 0.5 + 0.5);
    const float3 light = saturate(ambient + 0.35 * key + 0.1 * fill);

    const float3 paint = saturate(PsConstant(kPaint).rgb);
    // Alpha as the game sets it per draw (sub_82375928): 1 for solid, non-glowing draws such as the
    // shop preview, which needs it to show, and lower for the see-through placement ghost.
    oC0 = float4(saturate(paint * light * Hue(PsConstant(kHighlight).rgb)), saturate(PsConstant(kAlphaMode).x));
}
