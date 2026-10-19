using NB.Core.IO;

namespace NB.Core.Formats;

/// <summary>
/// Streaming archive (magic 0x438CB47C), used by the Bundle/50 files and nested inside them.
///   0x00 u32 magic        0x04 u32 entry size (12)   0x08 u32 entry count
///   0x0C u32 build timestamp (unix)                 0x10 u32 dependency count
///   u32 dependencies[]   (bundle ids: type byte 0x50 + 24-bit file name)
///   entries[]: u32 asset id, u32 offset, u32 size (offset/size are 0 for "not present")
///   entry payloads (CAFF files, XACT wave banks "DNBW", nested archives), back to back.
/// </summary>
public sealed class BundleArchive
{
    public const uint Magic = 0x438CB47C;
    public uint EntrySize = 12;
    public uint Timestamp;
    public List<uint> Dependencies = new();
    public List<BundleEntry> Entries = new();

    public static bool IsArchive(ReadOnlySpan<byte> d) => d.Length >= 20 && BE.U32(d, 0) == Magic && BE.U32(d, 4) == 12;

    public static BundleArchive Read(byte[] d)
    {
        if (!IsArchive(d)) throw new InvalidDataException("Not a bundle archive");
        var a = new BundleArchive { EntrySize = BE.U32(d, 4), Timestamp = BE.U32(d, 12) };
        int count = BE.S32(d, 8), ndep = BE.S32(d, 16);
        int o = 20;
        for (int i = 0; i < ndep; i++, o += 4) a.Dependencies.Add(BE.U32(d, o));
        for (int i = 0; i < count; i++, o += 12)
        {
            uint id = BE.U32(d, o); int off = BE.S32(d, o + 4), size = BE.S32(d, o + 8);
            a.Entries.Add(new BundleEntry
            {
                Id = id,
                OriginalOffset = off,
                Data = size > 0 || off > 0 ? d.AsSpan(off, size).ToArray() : null,
            });
        }
        return a;
    }

    public byte[] Write()
    {
        int hdr = 20 + 4 * Dependencies.Count + 12 * Entries.Count;
        long total = hdr + Entries.Sum(e => (long)(e.Data?.Length ?? 0));
        var outp = new byte[total];
        BE.W32(outp, 0, Magic); BE.W32(outp, 4, EntrySize); BE.W32(outp, 8, Entries.Count);
        BE.W32(outp, 12, Timestamp); BE.W32(outp, 16, Dependencies.Count);
        int o = 20;
        foreach (var dep in Dependencies) { BE.W32(outp, o, dep); o += 4; }
        int pos = hdr;
        foreach (var e in Entries)
        {
            BE.W32(outp, o, e.Id);
            if (e.Data == null) { BE.W32(outp, o + 4, 0); BE.W32(outp, o + 8, 0); }
            else
            {
                BE.W32(outp, o + 4, pos); BE.W32(outp, o + 8, e.Data.Length);
                Buffer.BlockCopy(e.Data, 0, outp, pos, e.Data.Length);
                e.OriginalOffset = pos;
                pos += e.Data.Length;
            }
            o += 12;
        }
        return outp;
    }
}

public sealed class BundleEntry
{
    public uint Id;
    public int OriginalOffset;
    public byte[]? Data;

    public string Kind => Data == null ? "missing"
        : CaffFile.IsCaff(Data) ? "caff"
        : Data.Length >= 4 && Data[0] == 'D' && Data[1] == 'N' && Data[2] == 'B' && Data[3] == 'W' ? "xwb"
        : BundleArchive.IsArchive(Data) ? "archive"
        : "unknown";
}
