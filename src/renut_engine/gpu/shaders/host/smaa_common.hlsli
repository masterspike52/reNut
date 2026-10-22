// SMAA 1x (thirdparty/smaa, Jimenez et al., MIT) for renut_anti_aliasing,
// run over the finished frame before it is scanned out (backend/smaa.cpp):
// edge detection, blending weights, neighborhood blending, each a fullscreen
// pass drawn by present_vs into a target of the frame's size. SMAA's own
// HLSL ports declare their samplers in effect syntax, which DXC does not take:
// this maps its porting macros onto the bindless tables instead.
#include "utility.hlsli"

// The pass's target size: 1 / width, 1 / height, width, height (SmaaBegin).
static float4 g_SmaaMetrics;
#define SMAA_RT_METRICS g_SmaaMetrics
#define SMAA_PRESET_HIGH 1
#define SMAA_CUSTOM_SL 1

// kLinearClampSamplerSlot and kPointClampSamplerSlot (video.h).
#define SMAA_LINEAR g_Samplers[0]
#define SMAA_POINT g_Samplers[1]

#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(SMAA_LINEAR, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(SMAA_POINT, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(SMAA_LINEAR, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(SMAA_LINEAR, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(SMAA_POINT, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(SMAA_LINEAR, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAAGather(tex, coord) tex.Gather(SMAA_LINEAR, coord, 0)

#include "../../../../../thirdparty/smaa/SMAA.hlsl"

#define SMAA_TEX(index) g_Textures[NonUniformResourceIndex(index)]

struct SmaaInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// The pass draws a target the size of texture 'index' (the frame, or a target
// of its size).
void SmaaBegin(uint index)
{
    uint width, height;
    SMAA_TEX(index).GetDimensions(width, height);
    g_SmaaMetrics = float4(1.0 / float(width), 1.0 / float(height), float(width), float(height));
}
