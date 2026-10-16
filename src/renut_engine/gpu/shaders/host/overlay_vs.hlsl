// ImGui / immediate-drawer vertices: pixel-space position, UV, RGBA8 color.
#include "utility.hlsli"

struct VSInput
{
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position * g_Push.scale + g_Push.translate, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
