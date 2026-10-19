using System.Buffers.Binary;
using System.Text;

namespace NB.Core.IO;

/// <summary>Big-endian helpers over byte spans. The game data is Xbox 360 (PowerPC, big-endian).</summary>
public static class BE
{
    public static ushort U16(ReadOnlySpan<byte> d, int o) => BinaryPrimitives.ReadUInt16BigEndian(d[o..]);
    public static short S16(ReadOnlySpan<byte> d, int o) => BinaryPrimitives.ReadInt16BigEndian(d[o..]);
    public static uint U32(ReadOnlySpan<byte> d, int o) => BinaryPrimitives.ReadUInt32BigEndian(d[o..]);
    public static int S32(ReadOnlySpan<byte> d, int o) => BinaryPrimitives.ReadInt32BigEndian(d[o..]);
    public static ulong U64(ReadOnlySpan<byte> d, int o) => BinaryPrimitives.ReadUInt64BigEndian(d[o..]);
    public static float F32(ReadOnlySpan<byte> d, int o) => BinaryPrimitives.ReadSingleBigEndian(d[o..]);

    public static void W16(Span<byte> d, int o, ushort v) => BinaryPrimitives.WriteUInt16BigEndian(d[o..], v);
    public static void W64(Span<byte> d, int o, ulong v) => BinaryPrimitives.WriteUInt64BigEndian(d[o..], v);
    public static void W32(Span<byte> d, int o, uint v) => BinaryPrimitives.WriteUInt32BigEndian(d[o..], v);
    public static void W32(Span<byte> d, int o, int v) => BinaryPrimitives.WriteInt32BigEndian(d[o..], v);
    public static void WF32(Span<byte> d, int o, float v) => BinaryPrimitives.WriteSingleBigEndian(d[o..], v);

    /// <summary>Reads a NUL-terminated ASCII/Latin-1 string.</summary>
    public static string CStr(ReadOnlySpan<byte> d, int o, int max = int.MaxValue)
    {
        int end = o;
        int lim = (int)Math.Min((long)d.Length, (long)o + max);
        while (end < lim && d[end] != 0) end++;
        return Encoding.Latin1.GetString(d[o..end]);
    }

    public static int Align(int v, int a) => (v + a - 1) & ~(a - 1);
}

/// <summary>Sequential big-endian reader over a byte array.</summary>
public sealed class BEReader
{
    public readonly byte[] Data;
    public int Pos;
    public BEReader(byte[] data, int pos = 0) { Data = data; Pos = pos; }
    public int Length => Data.Length;
    public byte U8() => Data[Pos++];
    public ushort U16() { var v = BE.U16(Data, Pos); Pos += 2; return v; }
    public uint U32() { var v = BE.U32(Data, Pos); Pos += 4; return v; }
    public int S32() { var v = BE.S32(Data, Pos); Pos += 4; return v; }
    public float F32() { var v = BE.F32(Data, Pos); Pos += 4; return v; }
    public byte[] Bytes(int n) { var b = Data.AsSpan(Pos, n).ToArray(); Pos += n; return b; }
    public string CStr() { var s = BE.CStr(Data, Pos); Pos += s.Length + 1; return s; }
    public void Skip(int n) => Pos += n;
    public void AlignTo(int a) => Pos = BE.Align(Pos, a);
}
