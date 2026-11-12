// Shape keys of NutTool's models (NutTool's docs/renut-shapes.md), for the part, world and skinned vertex
// shaders: they define VsConstant, VertexFetchSlot, VertexFetchData4, ShapeCount and VertexIndexOffset.

// A delta's big-endian words in host order. The stream's own endian mode is for its vertices (a
// part's half floats swap 8in16), so the deltas' 32-bit floats always take a full swap.
uint4 ShapeSwap(uint4 value)
{
    const uint4 swapped8 = ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
    return (swapped8 << 16) | (swapped8 >> 16);
}

// Adds each shape's delta times its weight to the vertex: c200 = (deltaOffset, vertexCount, count),
// c201-c208 the weights; the deltas are in stream 0's copy (fetch constant 95), 32 bytes a vertex.
void ApplyShapes(uint vertexId, inout float3 position, inout float3 normal)
{
    const uint count = min(ShapeCount(), 32u);
    if (count == 0u)
        return;
    const uint4 info = asuint(VsConstant(200u));
    const uint vertex = vertexId + VertexIndexOffset();
    if (vertex >= info.y)
        return;
    const uint slot = VertexFetchSlot(95u);
    for (uint s = 0u; s < count; ++s)
    {
        const float weight = VsConstant(201u + s / 4u)[s % 4u];
        if (weight == 0.0)
            continue;
        const uint at = (slot & ~3u) + info.x + (s * info.y + vertex) * 32u;
        position += asfloat(ShapeSwap(VertexFetchData4(at))).xyz * weight;
        normal += asfloat(ShapeSwap(VertexFetchData4(at + 16u))).xyz * weight;
    }
}
