// Xenos point sprites: expands each point into a screen-aligned square strip of
// the exported or render-state size, with pointCoord 0,0 top left to 1,1 bottom right.

struct Vertex
{
    float4 position : SV_Position;
    float4 texcoord[16] : TEXCOORD0;
    float4 color[2] : COLOR0;
    float4 normal[3] : NORMAL0;
    float4 tangent : TANGENT0;
    float4 binormal : BINORMAL0;
    float2 pointCoord : POINTCOORD;
    float pointSize : POINTSIZE;
};

// SharedConstants (draw.cpp): at 1312 the point width, height, min and max size,
// at 1328 the viewport size clip space spans, all in guest pixels.
#ifdef __spirv__
struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

float4 PointState() { return vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 1312, 16); }
float4 ViewportState() { return vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 1328, 16); }

// The vertex shaders' clip space has y flipped (-fvk-invert-y).
static const float kUp = -1.0;
#else
cbuffer SharedConstants : register(b2, space4)
{
    float4 g_PointState : packoffset(c82);
    float4 g_ViewportState : packoffset(c83);
};

float4 PointState() { return g_PointState; }
float4 ViewportState() { return g_ViewportState; }

static const float kUp = 1.0;
#endif

[maxvertexcount(4)]
void main(point Vertex input[1], inout TriangleStream<Vertex> output)
{
    const Vertex v = input[0];
    if (v.pointSize < 0.0)
        return;

    const float4 state = PointState();
    float2 size = v.pointSize > 0.0 ? v.pointSize.xx : state.xy;
    if (state.w > 0.0)
        size = min(size, state.w);
    size = max(size, state.z);

    // Half the size in clip space: a pixel is 2 / viewport of the NDC range.
    const float2 half_size = size / ViewportState().xy * v.position.w;
    const float2 corners[4] = { float2(-1.0, 1.0), float2(1.0, 1.0), float2(-1.0, -1.0), float2(1.0, -1.0) };
    const float2 coords[4] = { float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0), float2(1.0, 1.0) };
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        Vertex corner = v;
        corner.position.xy += corners[i] * float2(1.0, kUp) * half_size;
        corner.pointCoord = coords[i];
        output.Append(corner);
    }
}
