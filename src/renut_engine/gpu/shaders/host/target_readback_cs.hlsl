// Reads a render target back for the CPU (ReadRenderTargetLocked in draw.cpp):
// what a color resolve into an X8R8G8B8 texture leaves in guest memory, one
// thread per guest pixel. A guest pixel is the average of the host texels its
// edges cover at the target's render scale (RenderScale::Coord rounding), and
// is stored as the guest's big-endian word 0xFFRRGGBB, row after row.
//   set 0 : bindless Texture2D table (t0, space0), a single-sampled copy of
//           the target
//   set 1 : RWByteAddressBuffer      (u0, space1), the words
//   push  : TargetReadbackPush       (b0, space2)
// Must match BuildTargetReadbackPipeline in video.cpp and TargetReadbackPush
// in video.h.

struct TargetReadbackPush
{
    uint textureIndex;
    uint width;        // guest pixels
    uint height;
    uint scaleNumerator;
    uint scaleDenominator;
    uint pad0;
    uint pad1;
    uint pad2;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<TargetReadbackPush> g_Push;
#else
ConstantBuffer<TargetReadbackPush> g_Push : register(b0, space2);
#endif

Texture2D<float4> g_Textures[] : register(t0, space0);
RWByteAddressBuffer g_Words : register(u0, space1);

// RenderScale::Coord: a guest edge in host texels, to the nearest.
uint HostCoord(uint guest)
{
    return (guest * g_Push.scaleNumerator * 2u + g_Push.scaleDenominator) /
           (g_Push.scaleDenominator * 2u);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Push.width || id.y >= g_Push.height)
        return;
    Texture2D<float4> source = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)];
    uint textureWidth, textureHeight;
    source.GetDimensions(textureWidth, textureHeight);
    const uint2 last = uint2(textureWidth, textureHeight) - 1u;
    const uint2 first = min(uint2(HostCoord(id.x), HostCoord(id.y)), last);
    const uint2 end = clamp(uint2(HostCoord(id.x + 1u), HostCoord(id.y + 1u)), first + 1u, last + 1u);
    float4 sum = 0.0;
    for (uint y = first.y; y < end.y; ++y)
    {
        for (uint x = first.x; x < end.x; ++x)
            sum += source.Load(int3(x, y, 0));
    }
    const float3 color = saturate(sum.rgb / float((end.x - first.x) * (end.y - first.y)));
    const uint3 bytes = uint3(color * 255.0 + 0.5);
    // Bytes FF RR GG BB in memory: the big-endian word 0xFFRRGGBB.
    g_Words.Store((id.y * g_Push.width + id.x) * 4u,
                  0xFFu | (bytes.r << 8) | (bytes.g << 16) | (bytes.b << 24));
}
