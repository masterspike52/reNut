using System.Text;
using NB.Core.IO;

namespace NB.Core.Formats;

/// <summary>
/// CAFF container ("CAFF07.08.06.0036"). Holds a set of named assets (symbols); each asset is split
/// into parts, one per memory section (.data CPU, .texturegpu, .gpu, .gpucached, .stream).
/// All pointers inside parts are expressed through the relocation table, so a part can be
/// resized freely as long as its own relocation list is rebuilt.
///
/// File layout (big-endian):
///   0x00 "CAFF" + 16-byte version string
///   0x14 u32 header size (0x78)          0x18 u32 header checksum (ELF-style hash over the 0x78 bytes, field zeroed)
///   0x1C u32 symbol count                0x20 u32 part count
///   0x2C u32 relocation group count      0x30 u32 relocation offset count
///   0x3C u32 second-table group count     0x40 u32 second-table offset count (streamed CAFFs only)
///   0x44 u32 pool record count           0x48 u8 type(1), u8 section count, u8 ?, u8 pool part count
///   0x4C u32 section-name bytes          0x50/0x60 u32 info block size   0x64/0x74 u32 relocation block size
///   0x78 section records (0x21 bytes: u32 name offset, u8 align log2, u32 flags, u32 size, 16 bytes, u32 size)
///        section names, u32 symbol-buffer size, u32 offsets[symbols], symbol strings,
///        u32 n + n bytes (extra name, usually empty), parts (14 bytes: u32 symbol, u32 offset, u32 size,
///        u8 section, u8 align log2), pad to 4
///   relocation block: groups (u32 fromPart, u32 toPart, u32 count), u32 offsets (part-relative), then the same for
///        the second table,
///        pool table: poolPartCount * (u32 part, u32 count), pool records (16 bytes each)
///   section data, back to back.
/// </summary>
public sealed class CaffFile
{
    public const int HeaderSize = 0x78;
    const int SectionRecordSize = 0x21;

    public byte[] Header = new byte[HeaderSize]; // original header, unknown fields preserved
    public string Version = "07.08.06.0036";
    public List<CaffSection> Sections = new();
    public List<string> Symbols = new();          // symbol id = index + 1
    public byte[] ExtraName = Array.Empty<byte>();
    public List<CaffPart> Parts = new();          // part id = index + 1
    public List<CaffReloc> Relocs = new();
    public List<(int Part, int Count)> PoolParts = new();
    public byte[] PoolRecords = Array.Empty<byte>();

    public static bool IsCaff(ReadOnlySpan<byte> d) => d.Length >= 4 && d[0] == 'C' && d[1] == 'A' && d[2] == 'F' && d[3] == 'F';

    public static CaffFile Read(byte[] d)
    {
        if (!IsCaff(d)) throw new InvalidDataException("Not a CAFF file");
        var c = new CaffFile();
        Buffer.BlockCopy(d, 0, c.Header, 0, HeaderSize);
        c.Version = BE.CStr(d, 4, 16);
        int hdrSize = BE.S32(d, 0x14);
        if (hdrSize != HeaderSize) throw new InvalidDataException($"CAFF: unexpected header size 0x{hdrSize:X}");
        int nSym = BE.S32(d, 0x1C), nParts = BE.S32(d, 0x20);
        int nGroups = BE.S32(d, 0x2C), nOffsets = BE.S32(d, 0x30), nPool = BE.S32(d, 0x44);
        int nGroups2 = BE.S32(d, 0x3C), nOffsets2 = BE.S32(d, 0x40);
        int nSec = d[0x49], nPoolParts = d[0x4B];
        int infoSize = BE.S32(d, 0x50), relocSize = BE.S32(d, 0x64);

        var r = new BEReader(d, hdrSize);
        var nameOffs = new int[nSec];
        for (int i = 0; i < nSec; i++)
        {
            int o = r.Pos;
            nameOffs[i] = BE.S32(d, o);
            c.Sections.Add(new CaffSection
            {
                AlignLog2 = d[o + 4],
                Flags = BE.U32(d, o + 5),
                Mid = d.AsSpan(o + 13, 16).ToArray(),
                OriginalSize = BE.S32(d, o + 9),
                OriginalSize2 = BE.S32(d, o + 29),
            });
            r.Pos += SectionRecordSize;
        }
        int namesStart = r.Pos;
        for (int i = 0; i < nSec; i++) c.Sections[i].Name = BE.CStr(d, namesStart + nameOffs[i]);
        r.Pos = namesStart + BE.S32(d, 0x4C);

        int symBufSize = r.S32();
        var symOffs = new int[nSym];
        for (int i = 0; i < nSym; i++) symOffs[i] = r.S32();
        int symStart = r.Pos;
        for (int i = 0; i < nSym; i++) c.Symbols.Add(BE.CStr(d, symStart + symOffs[i]));
        r.Pos = symStart + symBufSize;
        int extraLen = r.S32();
        c.ExtraName = r.Bytes(extraLen);
        for (int i = 0; i < nParts; i++)
        {
            var p = new CaffPart
            {
                Symbol = r.S32(),
                OriginalOffset = r.S32(),
                Size = r.S32(),
                Section = r.U8(),
                AlignLog2 = r.U8(),
            };
            c.Parts.Add(p);
        }
        int infoEnd = hdrSize + infoSize;
        int relocStart = infoEnd;
        int dataStart = relocStart + relocSize;

        // relocations
        r.Pos = relocStart;
        void Table(int n, int table)
        {
            var groups = new (int From, int To, int Count)[n];
            for (int i = 0; i < n; i++) groups[i] = (r.S32(), r.S32(), r.S32());
            foreach (var g in groups)
            {
                var offs = new int[g.Count];
                for (int k = 0; k < g.Count; k++) offs[k] = r.S32();
                c.Relocs.Add(new CaffReloc(g.From, g.To, offs) { Table = table });
            }
        }
        Table(nGroups, 0);
        Table(nGroups2, 1);
        if (r.Pos - relocStart != (nGroups + nGroups2) * 12 + (nOffsets + nOffsets2) * 4) throw new InvalidDataException("CAFF: relocation count mismatch");
        for (int i = 0; i < nPoolParts; i++) c.PoolParts.Add((r.S32(), r.S32()));
        c.PoolRecords = r.Bytes(nPool * 16);
        if (r.Pos != dataStart) throw new InvalidDataException($"CAFF: relocation block size mismatch (0x{r.Pos:X} vs 0x{dataStart:X})");

        // section data
        var secStart = new int[nSec];
        int pos = dataStart;
        for (int i = 0; i < nSec; i++) { secStart[i] = pos; pos += c.Sections[i].OriginalSize; }
        if (pos != d.Length) throw new InvalidDataException($"CAFF: section sizes (0x{pos:X}) do not match file length (0x{d.Length:X})");
        foreach (var p in c.Parts)
        {
            if (p.Section < 1 || p.Section > nSec) throw new InvalidDataException("CAFF: part has bad section index");
            p.Data = d.AsSpan(secStart[p.Section - 1] + p.OriginalOffset, p.Size).ToArray();
        }
        return c;
    }

    public CaffSection SectionOf(CaffPart p) => Sections[p.Section - 1];
    public IEnumerable<CaffPart> PartsOf(int symbol) => Parts.Where(p => p.Symbol == symbol);
    public int PartId(CaffPart p) => Parts.IndexOf(p) + 1;

    /// <summary>Serializes the container. Parts are laid out in table order, each at its alignment.</summary>
    public byte[] Write()
    {
        int nSec = Sections.Count;
        // lay out parts inside sections
        var secSize = new int[nSec];
        var offsets = new int[Parts.Count];
        for (int i = 0; i < Parts.Count; i++)
        {
            var p = Parts[i];
            int s = p.Section - 1;
            int off = BE.Align(secSize[s], 1 << p.AlignLog2);
            offsets[i] = off;
            secSize[s] = off + p.Data.Length;
        }
        for (int i = 0; i < nSec; i++) secSize[i] = BE.Align(secSize[i], 4);

        var ms = new MemoryStream();
        var w = new BEWriter(ms);
        w.Bytes(new byte[HeaderSize]);
        // section records
        var names = new MemoryStream();
        var nameOffs = new int[nSec];
        for (int i = 0; i < nSec; i++) { nameOffs[i] = (int)names.Length; var b = Encoding.Latin1.GetBytes(Sections[i].Name + "\0"); names.Write(b); }
        for (int i = 0; i < nSec; i++)
        {
            var s = Sections[i];
            w.S32(nameOffs[i]); w.U8(s.AlignLog2); w.U32(s.Flags); w.S32(secSize[i]); w.Bytes(s.Mid); w.S32(secSize[i]);
        }
        w.Bytes(names.ToArray());
        // symbols
        var symBuf = new MemoryStream();
        var symOffs = new int[Symbols.Count];
        for (int i = 0; i < Symbols.Count; i++) { symOffs[i] = (int)symBuf.Length; symBuf.Write(Encoding.Latin1.GetBytes(Symbols[i] + "\0")); }
        w.S32((int)symBuf.Length);
        foreach (var o in symOffs) w.S32(o);
        w.Bytes(symBuf.ToArray());
        w.S32(ExtraName.Length); w.Bytes(ExtraName);
        for (int i = 0; i < Parts.Count; i++)
        {
            var p = Parts[i];
            w.S32(p.Symbol); w.S32(offsets[i]); w.S32(p.Data.Length); w.U8((byte)p.Section); w.U8(p.AlignLog2);
        }
        while (ms.Length % 4 != 0) w.U8(0);
        int infoSize = (int)ms.Length - HeaderSize;
        // relocations
        long relocStart = ms.Length;
        for (int t = 0; t < 2; t++)
        {
            foreach (var g in Relocs.Where(x => x.Table == t)) { w.S32(g.FromPart); w.S32(g.ToPart); w.S32(g.Offsets.Length); }
            foreach (var g in Relocs.Where(x => x.Table == t)) foreach (var o in g.Offsets) w.S32(o);
        }
        foreach (var (part, count) in PoolParts) { w.S32(part); w.S32(count); }
        w.Bytes(PoolRecords);
        int relocSize = (int)(ms.Length - relocStart);
        // section data
        long dataStart = ms.Length;
        var secStart = new long[nSec];
        long pos = dataStart;
        for (int i = 0; i < nSec; i++) { secStart[i] = pos; pos += secSize[i]; }
        ms.SetLength(pos);
        var buf = ms.GetBuffer();
        for (int i = 0; i < Parts.Count; i++)
        {
            var p = Parts[i];
            Buffer.BlockCopy(p.Data, 0, buf, (int)(secStart[p.Section - 1] + offsets[i]), p.Data.Length);
        }
        var result = ms.ToArray();

        // header
        var h = result.AsSpan(0, HeaderSize);
        Header.CopyTo(h);
        h[..4].Clear(); Encoding.ASCII.GetBytes("CAFF").CopyTo(h);
        h.Slice(4, 16).Clear(); Encoding.ASCII.GetBytes(Version).CopyTo(h[4..]);
        BE.W32(h, 0x14, HeaderSize);
        BE.W32(h, 0x1C, Symbols.Count);
        BE.W32(h, 0x20, Parts.Count);
        BE.W32(h, 0x2C, Relocs.Count(r => r.Table == 0));
        BE.W32(h, 0x30, Relocs.Where(r => r.Table == 0).Sum(r => r.Offsets.Length));
        BE.W32(h, 0x3C, Relocs.Count(r => r.Table == 1));
        BE.W32(h, 0x40, Relocs.Where(r => r.Table == 1).Sum(r => r.Offsets.Length));
        BE.W32(h, 0x44, PoolRecords.Length / 16);
        h[0x49] = (byte)nSec;
        h[0x4B] = (byte)PoolParts.Count;
        BE.W32(h, 0x4C, (int)names.Length);
        BE.W32(h, 0x50, infoSize); BE.W32(h, 0x60, infoSize);
        BE.W32(h, 0x64, relocSize); BE.W32(h, 0x74, relocSize);
        BE.W32(h, 0x18, 0u);
        BE.W32(h, 0x18, HeaderChecksum(h));
        for (int i = 0; i < Parts.Count; i++) Parts[i].OriginalOffset = offsets[i];
        for (int i = 0; i < nSec; i++) { Sections[i].OriginalSize = secSize[i]; Sections[i].OriginalSize2 = secSize[i]; }
        return result;
    }

    /// <summary>CAFF header checksum (ELF-hash variant with sign-extended bytes), computed with the field at 0x18 zeroed.</summary>
    public static uint HeaderChecksum(ReadOnlySpan<byte> header)
    {
        uint h = 0;
        foreach (byte b in header)
        {
            uint v = (b & 0x80) != 0 ? 0xFFFFFF80u | b : b;
            h = v + (h << 4);
            uint t = h & 0xF0000000;
            if (t != 0) h ^= t | (t >> 24);
        }
        return h;
    }

    public static bool VerifyHeaderChecksum(ReadOnlySpan<byte> file)
    {
        Span<byte> h = stackalloc byte[HeaderSize];
        file[..HeaderSize].CopyTo(h);
        uint stored = BE.U32(h, 0x18);
        BE.W32(h, 0x18, 0u);
        return HeaderChecksum(h) == stored;
    }
}

public sealed class CaffSection
{
    public string Name = "";
    public byte AlignLog2;
    public uint Flags;
    public byte[] Mid = new byte[16];
    public int OriginalSize, OriginalSize2;
}

public sealed class CaffPart
{
    public int Symbol;
    public int Section;
    public byte AlignLog2;
    public int OriginalOffset;
    public int Size;
    public byte[] Data = Array.Empty<byte>();
}

/// <summary>A group of pointer fields living in <see cref="FromPart"/> that point into <see cref="ToPart"/>.
/// Each offset is part-relative; the stored u32 at that offset is an offset into the target part.</summary>
/// <remarks>Streamed CAFFs (Bundle/50 entries) split their pointers over a second relocation table (header 0x3C groups,
/// 0x40 offsets, written right after the first); resident bundles only use the first. <see cref="Table"/> keeps each
/// group in the table it came from.</remarks>
public sealed record CaffReloc(int FromPart, int ToPart, int[] Offsets)
{
    public int[] Offsets { get; set; } = Offsets;
    public int Table { get; init; }
}

public sealed class BEWriter
{
    readonly Stream _s;
    public BEWriter(Stream s) { _s = s; }
    public void U8(byte v) => _s.WriteByte(v);
    public void U16(ushort v) { _s.WriteByte((byte)(v >> 8)); _s.WriteByte((byte)v); }
    public void U32(uint v) { Span<byte> b = stackalloc byte[4]; BE.W32(b, 0, v); _s.Write(b); }
    public void S32(int v) => U32((uint)v);
    public void F32(float v) { Span<byte> b = stackalloc byte[4]; BE.WF32(b, 0, v); _s.Write(b); }
    public void Bytes(ReadOnlySpan<byte> b) => _s.Write(b);
    public long Position => _s.Position;
}
