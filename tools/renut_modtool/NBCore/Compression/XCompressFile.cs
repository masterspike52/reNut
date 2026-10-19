using NB.Core.IO;

namespace NB.Core.Compression;

/// <summary>
/// Xbox 360 "xbcompress" file container, identifier 0x0FF512ED (XCOMPRESS_FILE_IDENTIFIER_LZXTDECODE).
/// This is what the Bundle/4f files use (the same thing xbdecompress.exe undoes).
///
/// Layout (big-endian):
///   0x00 u32 Identifier  0x0FF512ED
///   0x04 u16 Version     0x0100
///   0x06 u16 Reserved
///   0x08 u32 Hash        (not validated here)
///   0x0C u32 Flags       bits 0-3: window = 1 &lt;&lt; (n + 15)
///                        bits 4-5: segment (compressed block) size = 0x8000 &lt;&lt; n
///                        bits 6-21: segment count
///                        bits 22-23: 0 = 20-bit size table entries, 1 = 32-bit
///   0x10 bit-packed (MSB first) table of per-segment uncompressed sizes, padded to u32
///   then segments: segment i starts at file offset i * segmentSize (segment 0 right after the table).
///   Each segment is an independent LZX stream (decoder reset per segment) made of XMem frames:
///     u16 compressedSize (BE) -> 32 KiB of output, or
///     0xFF, u16 uncompressedSize, u16 compressedSize.
/// </summary>
public static class XCompressFile
{
    public const uint MagicTDecode = 0x0FF512ED;
    public const uint MagicNative = 0x0FF512EE;

    public sealed record Header(ushort Version, uint Hash, uint Flags, int Segments, int SizeBits, int SegmentSize, int WindowBits, int TableEnd, long[] SegmentSizes)
    {
        public long UncompressedSize => SegmentSizes.Sum();
    }

    public static bool IsCompressed(ReadOnlySpan<byte> d) => d.Length >= 16 && BE.U32(d, 0) == MagicTDecode;

    public static Header ReadHeader(ReadOnlySpan<byte> d)
    {
        if (!IsCompressed(d)) throw new InvalidDataException("Not an 0x0FF512ED xcompress file");
        uint flags = BE.U32(d, 12);
        int segs = (int)((flags >> 6) & 0xFFFF);
        int sizeBits = ((flags >> 22) & 3) switch { 0 => 20, 1 => 32, _ => throw new InvalidDataException("xcompress: unknown size-table width") };
        int segSize = 0x8000 << (int)((flags >> 4) & 3);
        int windowBits = (int)(flags & 0xF) + 15;
        int nbits = 20 + ((flags & 0x00C00000) != 0 ? 12 : 0);
        int words = (nbits * segs + 31) >> 5;
        int tableEnd = 16 + words * 4;
        var sizes = new long[segs];
        long bitPos = 16 * 8;
        for (int i = 0; i < segs; i++)
        {
            ulong v = 0;
            for (int b = 0; b < sizeBits; b++, bitPos++)
                v = (v << 1) | (uint)((d[(int)(bitPos >> 3)] >> (7 - (int)(bitPos & 7))) & 1);
            sizes[i] = (long)v;
        }
        return new Header(BE.U16(d, 4), BE.U32(d, 8), flags, segs, sizeBits, segSize, windowBits, tableEnd, sizes);
    }

    public static byte[] Decompress(byte[] file, IProgress<double>? progress = null)
    {
        var h = ReadHeader(file);
        long total = h.UncompressedSize;
        if (total > int.MaxValue - 64) throw new InvalidDataException("xcompress: output too large");
        var output = new byte[total];
        var lzx = new LzxDecoder(h.WindowBits);
        long outPos = 0;
        for (int s = 0; s < h.Segments; s++)
        {
            int p = s == 0 ? h.TableEnd : s * h.SegmentSize;
            int segEnd = Math.Min(file.Length, (s + 1) * h.SegmentSize);
            lzx.Reset();
            long remaining = h.SegmentSizes[s];
            if (BE.U16(file, p) == 0)
            {
                // Stored segment: a zero frame header followed by the raw bytes (used for incompressible data).
                p += 2;
                if (p + remaining > segEnd) throw new InvalidDataException($"xcompress: stored segment {s} truncated");
                Buffer.BlockCopy(file, p, output, (int)outPos, (int)remaining);
                outPos += remaining;
                progress?.Report((double)(s + 1) / h.Segments);
                continue;
            }
            while (remaining > 0)
            {
                if (p >= segEnd) throw new InvalidDataException($"xcompress: segment {s} truncated");
                int uSize = LzxDecoder.FrameSize, cSize;
                if (file[p] == 0xFF) { uSize = BE.U16(file, p + 1); cSize = BE.U16(file, p + 3); p += 5; }
                else { cSize = BE.U16(file, p); p += 2; }
                if (uSize > remaining) uSize = (int)remaining;
                if (p + cSize > segEnd) throw new InvalidDataException($"xcompress: frame overruns segment {s}");
                try { lzx.DecodeFrame(file, p, cSize, output.AsSpan((int)outPos, uSize), uSize); }
                catch (InvalidDataException e)
                {
                    throw new InvalidDataException($"{e.Message} (segment {s}, frame at 0x{p:X}, csize {cSize}, usize {uSize}, segment offset {outPos - (h.SegmentSizes[s] - remaining)})", e);
                }
                p += cSize; outPos += uSize; remaining -= uSize;
            }
            progress?.Report((double)(s + 1) / h.Segments);
        }
        return output;
    }
}
