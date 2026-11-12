// Xenos rectangle lists: emits each three-corner rectangle as a two-triangle strip,
// adding the corner across the diagonal from the right angle.

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

// a + b - c for every output: the corner across the diagonal a-b from c.
Vertex Opposite(Vertex a, Vertex b, Vertex c)
{
    Vertex d;
    d.position = a.position + b.position - c.position;
    [unroll] for (uint i = 0; i < 16; ++i)
        d.texcoord[i] = a.texcoord[i] + b.texcoord[i] - c.texcoord[i];
    [unroll] for (uint j = 0; j < 2; ++j)
        d.color[j] = a.color[j] + b.color[j] - c.color[j];
    [unroll] for (uint k = 0; k < 3; ++k)
        d.normal[k] = a.normal[k] + b.normal[k] - c.normal[k];
    d.tangent = a.tangent + b.tangent - c.tangent;
    d.binormal = a.binormal + b.binormal - c.binormal;
    d.pointCoord = a.pointCoord + b.pointCoord - c.pointCoord;
    d.pointSize = a.pointSize + b.pointSize - c.pointSize;
    return d;
}

float2 ScreenPosition(Vertex v)
{
    return v.position.xy / v.position.w;
}

// How far the corner at p is from a right angle: 0 when its edges to a and b
// are perpendicular; the other two corners of a rectangle are acute.
float Skew(float2 p, float2 a, float2 b)
{
    return abs(dot(a - p, b - p));
}

[maxvertexcount(4)]
void main(triangle Vertex input[3], inout TriangleStream<Vertex> output)
{
    const float2 p0 = ScreenPosition(input[0]);
    const float2 p1 = ScreenPosition(input[1]);
    const float2 p2 = ScreenPosition(input[2]);
    const float s0 = Skew(p0, p1, p2);
    const float s1 = Skew(p1, p2, p0);
    const float s2 = Skew(p2, p0, p1);
    uint corner = 0;
    if (s1 < s0 && s1 <= s2)
        corner = 1;
    else if (s2 < s0 && s2 < s1)
        corner = 2;

    // Starting the strip from the right angle, in the given cyclic order,
    // keeps the winding of the three given vertices for both triangles.
    const Vertex c = input[corner];
    const Vertex a = input[(corner + 1) % 3];
    const Vertex b = input[(corner + 2) % 3];
    output.Append(c);
    output.Append(a);
    output.Append(b);
    output.Append(Opposite(a, b, c));
}
