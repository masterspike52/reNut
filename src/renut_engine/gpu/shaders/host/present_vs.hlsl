// Fullscreen triangle for scanning the guest front buffer out to the swapchain.
#include "utility.hlsli"

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    VSOutput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    // scale/translate select the sub-rectangle of the source to scan out.
    output.uv = uv * g_Push.scale + g_Push.translate;
    return output;
}
