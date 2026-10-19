using System.Text;

namespace NB.Core.Formats;

/// <summary>Structural edits of a CAFF: adding assets.</summary>
public static class CaffEdit
{
    /// <summary>
    /// Adds a copy of asset <paramref name="symbol"/> under <paramref name="newName"/> (its asset id is derived from the
    /// name, like every asset: type &lt;&lt; 24 | crc24(name)). All parts are copied; relocation groups of the asset
    /// are duplicated, with pointers between its own parts redirected to the copies and pointers into shared data (the
    /// shader pool, other assets) kept. A debug name at the end of .stream that equals the old name is replaced.
    /// Returns the new symbol id (1-based).
    /// </summary>
    public static int CloneAsset(CaffFile caff, int symbol, string newName)
    {
        if (symbol < 1 || symbol > caff.Symbols.Count) throw new ArgumentOutOfRangeException(nameof(symbol));
        if (caff.Symbols.Any(s => AssetIds.DisplayName(s).Equals(newName, StringComparison.OrdinalIgnoreCase)))
            throw new InvalidOperationException($"an asset named {newName} already exists");
        string oldName = AssetIds.DisplayName(caff.Symbols[symbol - 1]);
        // keep "manifest" the last symbol (as in every shipped bundle): insert before it and renumber its parts
        int ns;
        int msIdx = caff.Symbols.IndexOf("manifest");
        if (msIdx >= 0 && msIdx == caff.Symbols.Count - 1)
        {
            caff.Symbols.Insert(msIdx, newName);
            ns = msIdx + 1;
            foreach (var p in caff.Parts) if (p.Symbol >= ns) p.Symbol++;
        }
        else { caff.Symbols.Add(newName); ns = caff.Symbols.Count; }
        var map = new Dictionary<int, int>();
        var old = caff.Parts.Select((p, i) => (p, id: i + 1)).Where(x => x.p.Symbol == symbol).ToList();
        foreach (var (p, id) in old)
        {
            var np = new CaffPart { Symbol = ns, Section = p.Section, AlignLog2 = p.AlignLog2, Data = (byte[])p.Data.Clone(), Size = p.Data.Length };
            caff.Parts.Add(np);
            map[id] = caff.Parts.Count;
        }
        foreach (var r in caff.Relocs.Where(r => map.ContainsKey(r.FromPart)).ToList())
            caff.Relocs.Add(new CaffReloc(map[r.FromPart], map.TryGetValue(r.ToPart, out int t) ? t : r.ToPart, (int[])r.Offsets.Clone()) { Table = r.Table });
        // debug name string at the end of the stream part
        foreach (var (p, id) in old)
        {
            var np = caff.Parts[map[id] - 1];
            if (caff.SectionOf(np).Name != ".stream") continue;
            var oldBytes = Encoding.Latin1.GetBytes(oldName + "\0");
            int at = IndexOfLast(np.Data, oldBytes);
            if (at < 0 || at + oldBytes.Length < np.Data.Length - 4) continue;   // only a trailing name
            var nb = Encoding.Latin1.GetBytes(newName + "\0");
            var nd = new byte[at + nb.Length];   // streams end right after the name (stream +0x14 points at it)
            Buffer.BlockCopy(np.Data, 0, nd, 0, at);
            Buffer.BlockCopy(nb, 0, nd, at, nb.Length);
            np.Data = nd; np.Size = nd.Length;
        }
        RegisterInManifest(caff, ns);
        return ns;
    }

    /// <summary>
    /// Copies asset <paramref name="symbol"/> of <paramref name="src"/> into another bundle <paramref name="dst"/> under
    /// <paramref name="newName"/>: its parts go into the destination's sections of the same names and pointers between its
    /// own parts are redirected to the copies. Refused when the asset points into data it does not own (shared pools, other
    /// assets): those pointers would be meaningless in another bundle. Returns the new symbol id (1-based).
    /// </summary>
    /// <summary>
    /// Cross-bundle copy of an asset whose only foreign pointers go to the source bundle's shared "pool" .data string table
    /// (animations point at interned joint-name strings there): each such pointer is retargeted to the same string in the
    /// destination pool's .data (appended when missing). Assets without foreign pointers are copied as
    /// <see cref="CopyAsset"/>. Research: coop/research/charsel (character select copies 300 animations this way).
    /// </summary>
    public static int CopyAssetWithPoolStrings(CaffFile src, int symbol, CaffFile dst, string newName)
    {
        static int Sym(CaffFile c, string name) => c.Symbols.FindIndex(s => AssetIds.DisplayName(s) == name) + 1;
        var own = src.Parts.Select((p, i) => (p, id: i + 1)).Where(x => x.p.Symbol == symbol).ToList();
        var ownIds = own.Select(x => x.id).ToHashSet();
        var foreign = src.Relocs.Where(r => ownIds.Contains(r.FromPart) && !ownIds.Contains(r.ToPart)).ToList();
        if (foreign.Count == 0) return CopyAsset(src, symbol, dst, newName);
        int srcPool = Sym(src, "pool"), dstPool = Sym(dst, "pool");
        foreach (var r in foreign)
        {
            var tp = src.Parts[r.ToPart - 1];
            if (tp.Symbol != srcPool || src.SectionOf(tp).Name != ".data")
                throw new InvalidDataException($"{newName}: points into {AssetIds.DisplayName(src.Symbols[tp.Symbol - 1])} {src.SectionOf(tp).Name}; it cannot be copied to another bundle");
        }
        if (dstPool == 0) throw new InvalidDataException("the destination bundle has no pool");
        int dstPoolData = dst.Parts.FindIndex(p => p.Symbol == dstPool && dst.SectionOf(p).Name == ".data") + 1;
        if (dstPoolData == 0) throw new InvalidDataException("the destination pool has no .data part");
        var dpd = dst.Parts[dstPoolData - 1];
        // copy without the foreign pointers (removed from the source for the copy, then restored)
        var saved = src.Relocs.ToList();
        src.Relocs.RemoveAll(r => foreign.Contains(r));
        int ns;
        try { ns = CopyAsset(src, symbol, dst, newName); }
        finally { src.Relocs.Clear(); src.Relocs.AddRange(saved); }
        var newParts = dst.Parts.Select((p, i) => (p, id: i + 1)).Where(x => x.p.Symbol == ns).Select(x => x.id).ToList();
        var map = own.Select((x, k) => (x.id, nid: newParts[k])).ToDictionary(t => t.id, t => t.nid);
        var pool = new List<byte>(dpd.Data);
        var index = new Dictionary<string, int>();
        // existing strings of the destination pool (NUL-separated)
        for (int i = 0, start = 0; i < dpd.Data.Length; i++)
            if (dpd.Data[i] == 0) { if (i > start) index.TryAdd(System.Text.Encoding.Latin1.GetString(dpd.Data, start, i - start), start); start = i + 1; }
        int Find(string s)
        {
            if (index.TryGetValue(s, out var at)) return at;
            at = pool.Count; pool.AddRange(System.Text.Encoding.Latin1.GetBytes(s + "\0"));
            return index[s] = at;
        }
        foreach (var r in foreign)
        {
            var sp = src.Parts[r.FromPart - 1]; var tp = src.Parts[r.ToPart - 1];
            var np = dst.Parts[map[r.FromPart] - 1];
            foreach (int o in r.Offsets)
            {
                int t = (int)NB.Core.IO.BE.U32(sp.Data, o);
                int e = Array.IndexOf(tp.Data, (byte)0, t);
                NB.Core.IO.BE.W32(np.Data, o, (uint)Find(System.Text.Encoding.Latin1.GetString(tp.Data, t, e - t)));
            }
            dst.Relocs.Add(new CaffReloc(map[r.FromPart], dstPoolData, (int[])r.Offsets.Clone()) { Table = r.Table });
        }
        while (pool.Count % 4 != 0) pool.Add(0);
        dpd.Data = pool.ToArray(); dpd.Size = dpd.Data.Length;
        return ns;
    }

    public static int CopyAsset(CaffFile src, int symbol, CaffFile dst, string newName)
    {
        if (symbol < 1 || symbol > src.Symbols.Count) throw new ArgumentOutOfRangeException(nameof(symbol));
        if (dst.Symbols.Any(s => AssetIds.DisplayName(s).Equals(newName, StringComparison.OrdinalIgnoreCase)))
            throw new InvalidOperationException($"an asset named {newName} already exists in the destination");
        var own = src.Parts.Select((p, i) => (p, id: i + 1)).Where(x => x.p.Symbol == symbol).ToList();
        var ownIds = own.Select(x => x.id).ToHashSet();
        var relocs = src.Relocs.Where(r => ownIds.Contains(r.FromPart)).ToList();
        var foreign = relocs.Where(r => !ownIds.Contains(r.ToPart)).ToList();
        if (foreign.Count > 0) throw new InvalidDataException($"{AssetIds.DisplayName(src.Symbols[symbol - 1])} points into {foreign.Count} part(s) it does not own; it cannot be copied to another bundle");
        int ns;
        int msIdx = dst.Symbols.IndexOf("manifest");
        if (msIdx >= 0 && msIdx == dst.Symbols.Count - 1)
        {
            dst.Symbols.Insert(msIdx, newName);
            ns = msIdx + 1;
            foreach (var p in dst.Parts) if (p.Symbol >= ns) p.Symbol++;
        }
        else { dst.Symbols.Add(newName); ns = dst.Symbols.Count; }
        var map = new Dictionary<int, int>();
        foreach (var (p, id) in own)
        {
            string sec = src.SectionOf(p).Name;
            int di = dst.Sections.FindIndex(s => s.Name == sec);
            if (di < 0) throw new InvalidDataException($"the destination bundle has no {sec} section");
            dst.Parts.Add(new CaffPart { Symbol = ns, Section = di + 1, AlignLog2 = p.AlignLog2, Data = (byte[])p.Data.Clone(), Size = p.Data.Length });
            map[id] = dst.Parts.Count;
        }
        foreach (var r in relocs) dst.Relocs.Add(new CaffReloc(map[r.FromPart], map[r.ToPart], (int[])r.Offsets.Clone()) { Table = r.Table });
        string oldName = AssetIds.DisplayName(src.Symbols[symbol - 1]);
        foreach (var (p, id) in own)
        {
            var np = dst.Parts[map[id] - 1];
            if (dst.SectionOf(np).Name != ".stream") continue;
            var oldBytes = Encoding.Latin1.GetBytes(oldName + "\0");
            int at = IndexOfLast(np.Data, oldBytes);
            if (at < 0 || at + oldBytes.Length < np.Data.Length - 4) continue;
            var nb = Encoding.Latin1.GetBytes(newName + "\0");
            var nd = new byte[at + nb.Length];
            Buffer.BlockCopy(np.Data, 0, nd, 0, at); Buffer.BlockCopy(nb, 0, nd, at, nb.Length);
            np.Data = nd; np.Size = nd.Length;
        }
        RegisterInManifest(dst, ns);
        return ns;
    }

    /// <summary>
    /// Renames asset <paramref name="symbol"/> (its id follows the name) and a trailing debug name at the end of its
    /// .stream part. For CAFFs without a manifest (streamed single-asset CAFFs of Bundle/50); a manifest entry is updated
    /// when there is one.
    /// </summary>
    public static void RenameAsset(CaffFile caff, int symbol, string newName)
    {
        string oldName = AssetIds.DisplayName(caff.Symbols[symbol - 1]);
        caff.Symbols[symbol - 1] = newName;
        foreach (var np in caff.PartsOf(symbol).Where(x => caff.SectionOf(x).Name == ".stream").ToList())
        {
            var oldBytes = Encoding.Latin1.GetBytes(oldName + "\0");
            int at = IndexOfLast(np.Data, oldBytes);
            if (at < 0 || at + oldBytes.Length < np.Data.Length - 4) continue;
            var nb = Encoding.Latin1.GetBytes(newName + "\0");
            var nd = new byte[at + nb.Length];
            Buffer.BlockCopy(np.Data, 0, nd, 0, at); Buffer.BlockCopy(nb, 0, nd, at, nb.Length);
            np.Data = nd; np.Size = nd.Length;
        }
        if (caff.Symbols.IndexOf("manifest") >= 0) RegisterInManifest(caff, symbol);
    }

    /// <summary>
    /// Adds (asset id, 0-based symbol index) for symbol <paramref name="symbol"/> to the bundle's "manifest" asset:
    /// .data = magic 0x438CB47C, timestamp, → entries, count, → dependency bundle ids, count, 8 bytes 0; entries are
    /// (u32 id, u32 ordinal) in symbol order. The game finds a bundle's assets through this list — an asset that is only
    /// in the symbol table is never registered (a cloned model missing here left the level loading forever).
    /// </summary>
    public static void RegisterInManifest(CaffFile caff, int symbol)
    {
        int ms = caff.Symbols.IndexOf("manifest") + 1;
        if (ms == 0) throw new InvalidDataException("bundle has no manifest asset");
        uint id = AssetIds.IdOf(caff.Symbols[symbol - 1]) ?? throw new InvalidDataException("cannot derive an asset id for " + caff.Symbols[symbol - 1]);
        var part = caff.Parts.First(p => p.Symbol == ms && caff.SectionOf(p).Name == ".data");
        var d = part.Data;
        if (NB.Core.IO.BE.U32(d, 0) != 0x438CB47C) throw new InvalidDataException("manifest: bad magic");
        int ep = NB.Core.IO.BE.S32(d, 8), cnt = NB.Core.IO.BE.S32(d, 12), dp = NB.Core.IO.BE.S32(d, 16), dc = NB.Core.IO.BE.S32(d, 20);
        for (int i = 0; i < cnt; i++) if (NB.Core.IO.BE.U32(d, ep + 8 * i) == id) return;   // already listed
        // rebuild: header (0x20), entries, deps
        int nep = 0x20, ndp = nep + 8 * (cnt + 1);
        var nd = new byte[ndp + 4 * dc];
        Buffer.BlockCopy(d, 0, nd, 0, 0x20);
        Buffer.BlockCopy(d, ep, nd, nep, 8 * cnt);
        NB.Core.IO.BE.W32(nd, nep + 8 * cnt, id); NB.Core.IO.BE.W32(nd, nep + 8 * cnt + 4, symbol - 1);
        Buffer.BlockCopy(d, dp, nd, ndp, 4 * dc);
        NB.Core.IO.BE.W32(nd, 8, nep); NB.Core.IO.BE.W32(nd, 12, cnt + 1); NB.Core.IO.BE.W32(nd, 16, ndp);
        part.Data = nd; part.Size = nd.Length;
    }

    /// <summary>
    /// Removes asset <paramref name="symbol"/>: its parts and relocation groups, its manifest entry (later ordinals move
    /// down) and the symbol. Refused when another asset points into it or when shader-pool parts would be renumbered
    /// (meant for assets this tool added, whose parts sit at the end of the part table).
    /// </summary>
    public static void RemoveAsset(CaffFile caff, int symbol)
    {
        if (symbol < 1 || symbol > caff.Symbols.Count) throw new ArgumentOutOfRangeException(nameof(symbol));
        uint? id = AssetIds.IdOf(caff.Symbols[symbol - 1]);
        var drop = caff.Parts.Select((p, i) => (p, id: i + 1)).Where(x => x.p.Symbol == symbol).Select(x => x.id).ToHashSet();
        if (caff.Relocs.Any(r => drop.Contains(r.ToPart) && !drop.Contains(r.FromPart)))
            throw new InvalidOperationException($"{caff.Symbols[symbol - 1]}: other assets point into it");
        if (drop.Count > 0 && caff.PoolParts.Any(pp => pp.Part > drop.Min()))
            throw new InvalidOperationException($"{caff.Symbols[symbol - 1]}: removing it would renumber shader-pool parts");
        var map = new Dictionary<int, int>(); int k = 0;
        for (int i = 1; i <= caff.Parts.Count; i++) if (!drop.Contains(i)) map[i] = ++k;
        var relocs = caff.Relocs.Where(r => !drop.Contains(r.FromPart))
            .Select(r => new CaffReloc(map[r.FromPart], map[r.ToPart], r.Offsets) { Table = r.Table }).ToList();
        caff.Relocs.Clear(); caff.Relocs.AddRange(relocs);
        caff.Parts.RemoveAll(p => p.Symbol == symbol);
        foreach (var p in caff.Parts) if (p.Symbol > symbol) p.Symbol--;
        caff.Symbols.RemoveAt(symbol - 1);
        int ms = caff.Symbols.IndexOf("manifest") + 1;
        if (ms == 0 || id == null) return;
        var part = caff.Parts.First(p => p.Symbol == ms && caff.SectionOf(p).Name == ".data");
        var d = part.Data;
        int ep = NB.Core.IO.BE.S32(d, 8), cnt = NB.Core.IO.BE.S32(d, 12), dp = NB.Core.IO.BE.S32(d, 16), dc = NB.Core.IO.BE.S32(d, 20);
        var entries = new List<(uint Id, int Ord)>();
        for (int i = 0; i < cnt; i++)
        {
            uint eid = NB.Core.IO.BE.U32(d, ep + 8 * i); int ord = NB.Core.IO.BE.S32(d, ep + 8 * i + 4);
            if (eid == id) continue;
            entries.Add((eid, ord > symbol - 1 ? ord - 1 : ord));
        }
        int nep = 0x20, ndp = nep + 8 * entries.Count;
        var nd = new byte[ndp + 4 * dc];
        Buffer.BlockCopy(d, 0, nd, 0, 0x20);
        for (int i = 0; i < entries.Count; i++) { NB.Core.IO.BE.W32(nd, nep + 8 * i, entries[i].Id); NB.Core.IO.BE.W32(nd, nep + 8 * i + 4, entries[i].Ord); }
        Buffer.BlockCopy(d, dp, nd, ndp, 4 * dc);
        NB.Core.IO.BE.W32(nd, 8, nep); NB.Core.IO.BE.W32(nd, 12, entries.Count); NB.Core.IO.BE.W32(nd, 16, ndp);
        part.Data = nd; part.Size = nd.Length;
    }

    static int IndexOfLast(byte[] hay, byte[] needle)
    {
        for (int i = hay.Length - needle.Length; i >= 0; i--)
        {
            int k = 0;
            while (k < needle.Length && hay[i + k] == needle[k]) k++;
            if (k == needle.Length) return i;
        }
        return -1;
    }
}
