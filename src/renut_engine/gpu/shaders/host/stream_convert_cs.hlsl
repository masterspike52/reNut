// Builds a host vertex stream on the GPU from a memexport window, so a draw
// can read what an earlier draw exported without the CPU waiting for it (see
// ConvertExportedStreamsLocked in draw.cpp). Writes what ConvertVertices
// (vertex_format.cpp) writes: each element either its words endian-swapped
// (Copy32) or decoded and swizzled to four floats (ExpandFloat). One thread
// per element of a vertex.
//   set 0 : RWByteAddressBuffer[3] (u0, space0): the slots' export buffers,
//           then the current slot's stream buffer
//   push  : StreamConvertPush       (b0, space1)
// Must match BuildStreamConvertPipeline in video.cpp and StreamConvertPush in
// video.h.

struct StreamConvertPush
{
    uint source;           // g_Buffers index of the export buffer
    uint sourceOffset;     // bytes, of vertex 0
    uint destOffset;       // bytes, in the stream buffer
    uint vertices;
    uint guestStrideWords;
    uint hostStrideWords;
    uint elementCount;     // up to 8
    uint pad;
    // Two words per element (StreamConvertElement in draw.cpp):
    //   x: guest offset (words) 0-7, host offset (words) 8-15, endian 16-17,
    //      expand 18, words copied 19-21
    //   y: its type as bound: format 0-5, signed 8, integer 9, swizzles 10-21
    uint4 elements[4];
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<StreamConvertPush> g_Push;
#else
ConstantBuffer<StreamConvertPush> g_Push : register(b0, space1);
#endif

RWByteAddressBuffer g_Buffers[3] : register(u0, space0);

static const uint kStreamBuffer = 2;

uint LoadSource(uint address)
{
    return g_Buffers[g_Push.source].Load(address);
}

// SwapDword in vertex_format.cpp.
uint SwapWord(uint value, uint endian)
{
    const uint swap8 = (endian == 1u || endian == 2u) ? 0xFFFFFFFFu : 0u;
    const uint swap16 = (endian == 2u || endian == 3u) ? 0xFFFFFFFFu : 0u;
    const uint swapped8 = ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
    value = (swapped8 & swap8) | (value & ~swap8);
    const uint swapped16 = (value << 16) | (value >> 16);
    return (swapped16 & swap16) | (value & ~swap16);
}

// DecodeComponent in vertex_format.cpp.
float DecodeComponent(uint word, uint shift, uint bits, bool isSigned, bool isInteger)
{
    const uint raw = bits == 32 ? word : (word >> shift) & ((1u << bits) - 1u);
    if (isSigned)
    {
        const int value = bits == 32 ? int(raw) : (int(raw << (32 - bits)) >> (32 - bits));
        if (isInteger)
            return float(value);
        const float maxValue = bits == 32 ? 2147483647.0f : float((1u << (bits - 1)) - 1u);
        return max(float(value) / maxValue, -1.0f);
    }
    if (isInteger)
        return float(raw);
    return float(raw) / (bits == 32 ? 4294967295.0f : float((1u << bits) - 1u));
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint count = g_Push.elementCount;
    if (count == 0 || id.x >= g_Push.vertices * count)
        return;
    const uint vertex = id.x / count;
    const uint element = id.x % count;
    const uint pairIndex = element >> 1;
    const uint4 pair = pairIndex == 0 ? g_Push.elements[0]
                     : pairIndex == 1 ? g_Push.elements[1]
                     : pairIndex == 2 ? g_Push.elements[2]
                                      : g_Push.elements[3];
    const uint layout = (element & 1) ? pair.z : pair.x;
    const uint type = (element & 1) ? pair.w : pair.y;

    const uint guestOffset = layout & 0xFF;
    const uint hostOffset = (layout >> 8) & 0xFF;
    const uint endian = (layout >> 16) & 3;
    const uint src = g_Push.sourceOffset + (vertex * g_Push.guestStrideWords + guestOffset) * 4;
    const uint dst = g_Push.destOffset + (vertex * g_Push.hostStrideWords + hostOffset) * 4;

    if (!((layout >> 18) & 1))
    {
        // Copy32: the words endian-swapped (the halves too when the endian
        // mode says so).
        const uint words = (layout >> 19) & 7;
        for (uint i = 0; i < words; ++i)
            g_Buffers[kStreamBuffer].Store(dst + i * 4, SwapWord(LoadSource(src + i * 4), endian));
        return;
    }

    // ExpandFloat (ExpandElement): components before the swizzle, then the
    // swizzle constants 0 and 1.
    const uint format = type & 0x3F;
    const bool isSigned = (type >> 8) & 1;
    const bool isInteger = (type >> 9) & 1;
    float c[6] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f };
    const uint w0 = SwapWord(LoadSource(src), endian);
    switch (format)
    {
    case 6: // 8_8_8_8
        for (uint i = 0; i < 4; ++i)
            c[i] = DecodeComponent(w0, i * 8, 8, isSigned, isInteger);
        break;
    case 7: // 2_10_10_10
        c[0] = DecodeComponent(w0, 0, 10, isSigned, isInteger);
        c[1] = DecodeComponent(w0, 10, 10, isSigned, isInteger);
        c[2] = DecodeComponent(w0, 20, 10, isSigned, isInteger);
        c[3] = DecodeComponent(w0, 30, 2, isSigned, isInteger);
        break;
    case 16: // 10_11_11
        c[0] = DecodeComponent(w0, 0, 11, isSigned, isInteger);
        c[1] = DecodeComponent(w0, 11, 11, isSigned, isInteger);
        c[2] = DecodeComponent(w0, 22, 10, isSigned, isInteger);
        break;
    case 17: // 11_11_10
        c[0] = DecodeComponent(w0, 0, 10, isSigned, isInteger);
        c[1] = DecodeComponent(w0, 10, 11, isSigned, isInteger);
        c[2] = DecodeComponent(w0, 21, 11, isSigned, isInteger);
        break;
    case 25: // 16_16
        c[0] = DecodeComponent(w0, 0, 16, isSigned, isInteger);
        c[1] = DecodeComponent(w0, 16, 16, isSigned, isInteger);
        break;
    case 26: // 16_16_16_16
    {
        const uint w1 = SwapWord(LoadSource(src + 4), endian);
        c[0] = DecodeComponent(w0, 0, 16, isSigned, isInteger);
        c[1] = DecodeComponent(w0, 16, 16, isSigned, isInteger);
        c[2] = DecodeComponent(w1, 0, 16, isSigned, isInteger);
        c[3] = DecodeComponent(w1, 16, 16, isSigned, isInteger);
        break;
    }
    case 31: // 16_16_FLOAT
        c[0] = f16tof32(w0);
        c[1] = f16tof32(w0 >> 16);
        break;
    case 32: // 16_16_16_16_FLOAT
    {
        const uint w1 = SwapWord(LoadSource(src + 4), endian);
        c[0] = f16tof32(w0);
        c[1] = f16tof32(w0 >> 16);
        c[2] = f16tof32(w1);
        c[3] = f16tof32(w1 >> 16);
        break;
    }
    case 33: // 32
    case 34: // 32_32
    case 35: // 32_32_32_32
    {
        const uint components = format == 33 ? 1 : (format == 34 ? 2 : 4);
        for (uint i = 0; i < components; ++i)
            c[i] = DecodeComponent(i == 0 ? w0 : SwapWord(LoadSource(src + i * 4), endian), 0, 32,
                                   isSigned, isInteger);
        break;
    }
    case 36: // 32_FLOAT
    case 37: // 32_32_FLOAT
    case 38: // 32_32_32_32_FLOAT
    case 57: // 32_32_32_FLOAT
    {
        const uint components = format == 36 ? 1 : (format == 37 ? 2 : (format == 57 ? 3 : 4));
        for (uint i = 0; i < components; ++i)
            c[i] = asfloat(i == 0 ? w0 : SwapWord(LoadSource(src + i * 4), endian));
        break;
    }
    default:
        break;
    }
    uint4 result;
    for (uint i = 0; i < 4; ++i)
    {
        uint pick = (type >> (10 + 3 * i)) & 7;
        pick = pick < 6 ? pick : 4;  // 4: 0, 5: 1; the rest read 0
        result[i] = asuint(c[pick]);
    }
    g_Buffers[kStreamBuffer].Store4(dst, result);
}
