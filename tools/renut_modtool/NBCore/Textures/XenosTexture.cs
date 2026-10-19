using System.Numerics;
using NB.Core.IO;

namespace NB.Core.Textures;

/// <summary>Xenos (Xbox 360 GPU) texture formats used by the game (low 6 bits of the XDK D3DFORMAT).</summary>
public enum XenosFormat
{
    k8 = 0x02, k1_5_5_5 = 0x03, k5_6_5 = 0x04, k8_8_8_8 = 0x06, k8_8 = 0x0A, k4_4_4_4 = 0x0F,
    DXT1 = 0x12, DXT2_3 = 0x13, DXT4_5 = 0x14, DXN = 0x31, DXT3A = 0x3A, DXT5A = 0x3B, CTX1 = 0x3C,
}

public readonly record struct FormatInfo(int BlockW, int BlockH, int BytesPerBlock)
{
    public static FormatInfo Of(XenosFormat f) => f switch
    {
        XenosFormat.DXT1 or XenosFormat.DXT5A or XenosFormat.DXT3A or XenosFormat.CTX1 => new(4, 4, 8),
        XenosFormat.DXT2_3 or XenosFormat.DXT4_5 or XenosFormat.DXN => new(4, 4, 16),
        XenosFormat.k8 => new(1, 1, 1),
        XenosFormat.k8_8 or XenosFormat.k5_6_5 or XenosFormat.k1_5_5_5 or XenosFormat.k4_4_4_4 => new(1, 1, 2),
        XenosFormat.k8_8_8_8 => new(1, 1, 4),
        _ => throw new NotSupportedException($"Xenos texture format 0x{(int)f:X2} not supported"),
    };
}

/// <summary>One stored mip level inside a texture's GPU data.</summary>
public sealed record LevelLayout(int Level, int Width, int Height, int OffsetBytes, int PitchBlocks, int HeightBlocks, int PackedX, int PackedY);

/// <summary>
/// The CPU-side "texture" header (0x70+ bytes) written by the game's asset pipeline:
///   0x00 "texture\0"   0x08 version "04.05.05.0032"
///   0x18 u32 D3DFORMAT (bits 0-5 format, 6-7 endian, 8 tiled)
///   0x24 u16 width, u16 height   0x28 ptr -> GPU part   0x2C 0xFFFFFFFF if the GPU data starts with the base level, 0 if it holds mips only
///   0x30 u8 level count          0x34 ptr -> 0x3C (runtime D3D texture object, zero on disk)
/// </summary>
public sealed class TextureHeader
{
    public uint D3DFormat;
    public int Width, Height, Levels;
    public bool HasBase;
    public int HeaderSize;
    /// <summary>0x1C: 0 = 2D, 2 = cube map (6 faces), 4 = multi-frame (count at 0x38), 5 = volume (depth at 0x38).</summary>
    public int Kind;
    /// <summary>Number of faces / frames / slices stored back to back (1 for plain 2D).</summary>
    public int Count = 1;

    public XenosFormat Format => (XenosFormat)(D3DFormat & 0x3F);
    public int Endian => (int)((D3DFormat >> 6) & 3);
    public bool Tiled => ((D3DFormat >> 8) & 1) != 0;

    public static bool IsTexture(ReadOnlySpan<byte> cpu) => cpu.Length >= 0x3C && cpu[..8].SequenceEqual("texture\0"u8);

    public static TextureHeader Parse(ReadOnlySpan<byte> cpu)
    {
        if (!IsTexture(cpu)) throw new InvalidDataException("Not a texture header");
        return new TextureHeader
        {
            D3DFormat = BE.U32(cpu, 0x18),
            Width = BE.U16(cpu, 0x24),
            Height = BE.U16(cpu, 0x26),
            HasBase = BE.U32(cpu, 0x2C) == 0xFFFFFFFF,
            Levels = cpu[0x30],
            HeaderSize = cpu.Length,
            Kind = BE.S32(cpu, 0x1C),
            Count = BE.S32(cpu, 0x1C) switch { 2 => 6, 4 or 5 => Math.Max(1, BE.S32(cpu, 0x38)), _ => 1 },
        };
    }

    public string KindName => Kind switch { 0 => "2D", 2 => "cube", 4 => $"{Count} frames", 5 => $"volume x{Count}", _ => $"kind {Kind}" };
    public override string ToString() => $"{Format} {Width}x{Height} {KindName} levels={Levels} {(HasBase ? "base+" : "mips")} {(Tiled ? "tiled" : "linear")}";
}

/// <summary>Xenos tiled texture layout and conversion, following Xenia's texture_util / texture_address.</summary>
public static class XenosTexture
{
    static int Log2Ceil(int v) => v <= 1 ? 0 : 32 - BitOperations.LeadingZeroCount((uint)(v - 1));
    static int NextPow2(int v) => 1 << Log2Ceil(v);
    static int Align(int v, int a) => (v + a - 1) / a * a;

    public static int PackedMipLevel(int w, int h) { int l = Log2Ceil(Math.Min(w, h)); return l > 4 ? l - 4 : 0; }

    /// <summary>Offset in blocks of a mip inside the packed mip tail (Xenia GetPackedMipOffset, 2D only).</summary>
    public static bool PackedMipOffset(int width, int height, FormatInfo fi, int mip, out int xBlocks, out int yBlocks)
    {
        int log2W = Log2Ceil(width), log2H = Log2Ceil(height);
        int log2Size = Math.Min(log2W, log2H);
        xBlocks = yBlocks = 0;
        if (log2Size > 4 + mip) return false;
        int packedBase = log2Size > 4 ? log2Size - 4 : 0;
        int packedMip = mip - packedBase;
        if (packedMip < 3)
        {
            if (log2W > log2H) { xBlocks = 0; yBlocks = 16 >> packedMip; }
            else { xBlocks = 16 >> packedMip; yBlocks = 0; }
        }
        else
        {
            if (log2W > log2H) { xBlocks = (1 << (log2W - packedBase)) >> (packedMip - 2); yBlocks = 0; }
            else { xBlocks = 0; yBlocks = (1 << (log2H - packedBase)) >> (packedMip - 2); }
        }
        xBlocks /= fi.BlockW; yBlocks /= fi.BlockH;
        return true;
    }

    /// <summary>
    /// Lays out the levels stored in one GPU blob. <paramref name="firstLevel"/> is 0 for base-level data
    /// (texture "top" assets and single-level textures) and 1 for mip-chain data ("mip" assets).
    /// </summary>
    public static List<LevelLayout> Layout(int width, int height, int levels, XenosFormat fmt, int firstLevel, int lastLevel, bool tiled = true)
    {
        var fi = FormatInfo.Of(fmt);
        var list = new List<LevelLayout>();
        // Mip-tail packing only exists for tiled textures; linear levels are stored plainly.
        int packedLevel = tiled ? PackedMipLevel(width, height) : int.MaxValue;
        int offset = 0;
        int bpb = fi.BytesPerBlock;
        for (int level = firstLevel; level <= lastLevel && level < levels; level++)
        {
            bool isBase = level == 0;
            int lw = Math.Max(width >> level, 1), lh = Math.Max(height >> level, 1);
            int pitchTexels, heightTexels, px = 0, py = 0;
            if (isBase)
            {
                // Base level: pitch from the fetch constant (width rounded to 32 texels); a base that is itself
                // small enough to be packed uses power-of-two rows like the mips.
                pitchTexels = Align(width, 32);
                heightTexels = packedLevel == 0 ? NextPow2(height) : height;
                if (packedLevel == 0) PackedMipOffset(width, height, fi, 0, out px, out py);
            }
            else
            {
                int storageLevel = Math.Min(level, packedLevel);
                pitchTexels = Math.Max(NextPow2(width) >> storageLevel, 1);
                heightTexels = Math.Max(NextPow2(height) >> storageLevel, 1);
                if (level >= packedLevel) PackedMipOffset(width, height, fi, level, out px, out py);
            }
            int pitchBlocks, heightBlocks;
            if (tiled)
            {
                pitchBlocks = Align((pitchTexels + fi.BlockW - 1) / fi.BlockW, 32);
                heightBlocks = Align((heightTexels + fi.BlockH - 1) / fi.BlockH, 32);
            }
            else
            {
                // linear: rows padded to 256 bytes, no height padding (e.g. 447x503 ARGB → 1792-byte rows × 503)
                int wb = isBase ? (width + fi.BlockW - 1) / fi.BlockW : (pitchTexels + fi.BlockW - 1) / fi.BlockW;
                pitchBlocks = Align(wb * bpb, 256) / bpb;
                heightBlocks = isBase ? (height + fi.BlockH - 1) / fi.BlockH : (heightTexels + fi.BlockH - 1) / fi.BlockH;
            }
            list.Add(new LevelLayout(level, lw, lh, offset, pitchBlocks, heightBlocks, px, py));
            // Each unpacked level (and the base) occupies its own 4 KiB-aligned slice; packed mips share the tail.
            if (isBase || level < packedLevel)
                offset += Align(pitchBlocks * heightBlocks * bpb, 4096);
        }
        return list;
    }

    /// <summary>Total bytes a layout occupies (each non-packed level page aligned, tail counted once).</summary>
    public static int LayoutSize(List<LevelLayout> layout, XenosFormat fmt, bool tiled = true)
    {
        var fi = FormatInfo.Of(fmt);
        int end = 0;
        foreach (var l in layout)
        {
            int size = l.PitchBlocks * l.HeightBlocks * fi.BytesPerBlock;
            end = Math.Max(end, l.OffsetBytes + (tiled ? Align(size, 4096) : size));
        }
        return end;
    }

    // --- tiled addressing (Xenia texture_address.h) ---
    public static int Tiled2D(int x, int y, int pitchAligned, int bppLog2)
    {
        int outerBlocks = (((y >> 5) * (pitchAligned >> 5)) + (x >> 5)) << 6;
        int innerBlocks = (((y >> 1) & 7) << 3) | (x & 7);
        int outerInnerBytes = (outerBlocks | innerBlocks) << bppLog2;
        int bank = (y >> 4) & 1;
        int pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
        return ((y & 1) << 4) | (pipe << 6) | (bank << 11)
               | (outerInnerBytes & 0xF)
               | (((outerInnerBytes >> 4) & 1) << 5)
               | (((outerInnerBytes >> 5) & 7) << 8)
               | ((outerInnerBytes >> 8) << 12);
    }

    static void SwapBlock(Span<byte> b, int endian)
    {
        switch (endian)
        {
            case 1: for (int i = 0; i + 1 < b.Length; i += 2) (b[i], b[i + 1]) = (b[i + 1], b[i]); break;
            case 2: for (int i = 0; i + 3 < b.Length; i += 4) { (b[i], b[i + 3]) = (b[i + 3], b[i]); (b[i + 1], b[i + 2]) = (b[i + 2], b[i + 1]); } break;
            case 3: for (int i = 0; i + 3 < b.Length; i += 4) { (b[i], b[i + 2]) = (b[i + 2], b[i]); (b[i + 1], b[i + 3]) = (b[i + 3], b[i + 1]); } break;
        }
    }

    /// <summary>Extracts one level as linear (untiled, little-endian) block data: rows of blocks, tightly packed.</summary>
    public static byte[] ReadLevelBlocks(ReadOnlySpan<byte> gpu, LevelLayout l, XenosFormat fmt, int endian, bool tiled)
    {
        var fi = FormatInfo.Of(fmt);
        int bw = (l.Width + fi.BlockW - 1) / fi.BlockW, bh = (l.Height + fi.BlockH - 1) / fi.BlockH;
        int bpb = fi.BytesPerBlock, log2 = BitOperations.Log2((uint)bpb);
        var outp = new byte[bw * bh * bpb];
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++)
            {
                int sx = x + l.PackedX, sy = y + l.PackedY;
                int addr = l.OffsetBytes + (tiled ? Tiled2D(sx, sy, l.PitchBlocks, log2) : (sy * l.PitchBlocks + sx) * bpb);
                var dst = outp.AsSpan((y * bw + x) * bpb, bpb);
                if (addr + bpb <= gpu.Length) gpu.Slice(addr, bpb).CopyTo(dst);
                SwapBlock(dst, endian);
            }
        return outp;
    }

    /// <summary>Inverse of <see cref="ReadLevelBlocks"/>: writes little-endian linear blocks into tiled GPU memory.</summary>
    public static void WriteLevelBlocks(Span<byte> gpu, LevelLayout l, XenosFormat fmt, int endian, bool tiled, ReadOnlySpan<byte> blocks)
    {
        var fi = FormatInfo.Of(fmt);
        int bw = (l.Width + fi.BlockW - 1) / fi.BlockW, bh = (l.Height + fi.BlockH - 1) / fi.BlockH;
        int bpb = fi.BytesPerBlock, log2 = BitOperations.Log2((uint)bpb);
        Span<byte> tmp = stackalloc byte[16];
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++)
            {
                int sx = x + l.PackedX, sy = y + l.PackedY;
                int addr = l.OffsetBytes + (tiled ? Tiled2D(sx, sy, l.PitchBlocks, log2) : (sy * l.PitchBlocks + sx) * bpb);
                if (addr < 0 || addr + bpb > gpu.Length) throw new InvalidDataException($"texture block ({x},{y}) of level {l.Level} maps outside the GPU blob (0x{addr:X} ≥ 0x{gpu.Length:X})");
                var t = tmp[..bpb];
                blocks.Slice((y * bw + x) * bpb, bpb).CopyTo(t);
                SwapBlock(t, endian);
                t.CopyTo(gpu.Slice(addr, bpb));
            }
    }

    /// <summary>Decodes a level to RGBA8 (width*height*4).</summary>
    public static byte[] DecodeLevel(ReadOnlySpan<byte> gpu, LevelLayout l, XenosFormat fmt, int endian, bool tiled)
    {
        var blocks = ReadLevelBlocks(gpu, l, fmt, endian, tiled);
        return BlockCodec.Decode(blocks, l.Width, l.Height, fmt);
    }
}
