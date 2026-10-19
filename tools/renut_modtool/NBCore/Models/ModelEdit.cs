using System.Numerics;
using System.Text;
using NB.Core.Formats;
using NB.Core.IO;

namespace NB.Core.Models;

/// <summary>Data edits on a model asset that keep its structure: texture retargeting and LOD / cull distances.</summary>
public static class ModelEdit
{
    /// <summary>
    /// Makes every texture-table entry of the model whose texture stem equals <paramref name="oldStem"/> use
    /// <paramref name="newStem"/> instead ("…mip" / "…top" suffixes are kept per entry). Entries are
    /// (u32 a, u32 a, u32 index, u32 b, u32 b, → name string); only the name pointer changes — new strings are appended
    /// to .data and the (relocated) pointer is repointed. The new texture must exist in the bundle (resident mip and
    /// streamed top), e.g. another texture this bundle already uses. Returns the number of entries changed.
    /// </summary>
    public static int RetargetTexture(CaffFile caff, int symbol, string oldStem, string newStem, bool? exact = null, bool requireInBundle = true)
    {
        // exact (one complete texture for mip and top entries) when the bundle has the name itself but no "…mip" asset;
        // a reference to a name that is not in the bundle makes the level load wait forever. Streamed part models
        // (a model + shader pool CAFF in Bundle/50) reference resident textures by name: requireInBundle = false.
        bool Has(string n) => caff.Symbols.Any(s => AssetIds.DisplayName(s).Equals(n, StringComparison.OrdinalIgnoreCase));
        bool ex = exact ?? (Has(newStem) && !Has(newStem + "mip"));
        if (requireInBundle && ex && !Has(newStem)) throw new InvalidDataException($"texture {newStem} is not in this bundle");
        if (requireInBundle && !ex && !Has(newStem + "mip") && !Has(newStem + "top") && !Has(newStem)) throw new InvalidDataException($"texture {newStem}(mip/top) is not in this bundle");
        var v = new AssetView(caff, symbol);
        int pid = v.PartId(".data");
        var part = v.Part(pid);
        var d = part.Data;
        var extra = new MemoryStream();
        var repoint = new List<(int PtrLoc, int ExtraOff)>();
        var strOff = new Dictionary<string, int>();
        foreach (var ((p, off), to) in v.Pointers)
        {
            if (p != pid || to != pid) continue;
            int target = BE.S32(d, off);
            if (target < 0 || target + 12 >= d.Length || !d.AsSpan(target, 12).SequenceEqual("aid_texture_"u8)) continue;
            string name = BE.CStr(d, target, 256);
            string suffix = name.EndsWith("mip") ? "mip" : name.EndsWith("top") ? "top" : "";
            string stem = suffix.Length > 0 ? name[..^3] : name;
            if (!stem.Equals(oldStem, StringComparison.OrdinalIgnoreCase)) continue;
            string nn = ex ? newStem : newStem + suffix;   // exact: one resident texture serves mip and top entries
            if (!strOff.TryGetValue(nn, out int so))
            {
                so = (int)extra.Length; strOff[nn] = so;
                extra.Write(Encoding.Latin1.GetBytes(nn + "\0"));
            }
            repoint.Add((off, so));
        }
        if (repoint.Count == 0) return 0;
        int at = (d.Length + 15) & ~15;
        var nd = new byte[at + (int)extra.Length];
        Buffer.BlockCopy(d, 0, nd, 0, d.Length);
        extra.ToArray().CopyTo(nd, at);
        foreach (var (loc, so) in repoint) BE.W32(nd, loc, at + so);
        part.Data = nd; part.Size = nd.Length;
        return repoint.Count;
    }

    /// <summary>
    /// Recolours the pixel-shader constants of the draws whose first texture (s0) has stem <paramref name="stem"/>:
    /// every constant command (.stream op 0x06, kind 0) between the previous draw and such a draw has registers
    /// <paramref name="firstReg"/>.. rewritten as luminance × <paramref name="tint"/> (alpha kept). Materials such as the
    /// fuel-tank liquid take their colour from these constants and use the texture only as a ramp. Returns the number of
    /// registers changed.
    /// </summary>
    public static int TintConstants(CaffFile caff, int symbol, string stem, Vector3 tint, int firstReg = 5, int lastReg = 14)
    {
        var ma = ModelAsset.Parse(caff, symbol, geometry: false);
        var v = new AssetView(caff, symbol);
        var part = v.Part(v.PartId(".stream"));
        var s = part.Data;
        int pos = v.PtrAt(".stream", 0)?.Offset ?? 0x24;
        var pending = new List<int>(); var done = new HashSet<int>(); bool texHit = false; int n = 0;
        while (pos + 4 <= s.Length)
        {
            uint w = BE.U32(s, pos);
            int size = (int)(w >> 16), op = (int)((w >> 8) & 0xFF);
            if ((w & 0xFF) != 0 || size < 4 || pos + size > s.Length) break;
            if (op == 0x43)
            {
                int count = (int)(BE.U32(s, pos + 8) >> 16); texHit = false;
                for (int k = 0; k < count && pos + 24 + 12 * k <= pos + size; k++)
                {
                    int mip = BE.S32(s, pos + 12 + 12 * k), top = BE.S32(s, pos + 16 + 12 * k), slot = (int)(BE.U32(s, pos + 20 + 12 * k) >> 16);
                    int ti = top >= 0 && top < ma.TextureTable.Count ? top : mip;
                    if (slot != 0 || ti < 0 || ti >= ma.TextureTable.Count) continue;
                    string nm = ma.TextureTable[ti]; string st = nm.EndsWith("mip") || nm.EndsWith("top") ? nm[..^3] : nm;
                    if (st.Equals(stem, StringComparison.OrdinalIgnoreCase) || st.EndsWith(stem, StringComparison.OrdinalIgnoreCase)) texHit = true;
                }
            }
            else if (op == 0x06 && BE.U32(s, pos + 4) == 0) pending.Add(pos);
            else if (op == 0x01 || op == 0x30)
            {
                if (texHit)
                    foreach (int cp in pending.Where(done.Add))
                    {
                        uint rc = BE.U32(s, cp + 8); int reg = (int)(rc >> 16), cnt = (int)(rc & 0xFFFF);
                        int csize = (int)(BE.U32(s, cp) >> 16);
                        for (int k = 0; k < cnt && cp + 12 + 16 * k + 16 <= cp + csize; k++)
                        {
                            if (reg + k < firstReg || reg + k > lastReg) continue;
                            int q = cp + 12 + 16 * k;
                            float lum = 0.299f * BE.F32(s, q) + 0.587f * BE.F32(s, q + 4) + 0.114f * BE.F32(s, q + 8);
                            BE.WF32(s, q, lum * tint.X); BE.WF32(s, q + 4, lum * tint.Y); BE.WF32(s, q + 8, lum * tint.Z); n++;
                        }
                    }
                pending.Clear();
            }
            if (op == 0x1D) break;
            pos += size;
        }
        return n;
    }

    /// <summary>Texture stems used by the model's texture table (without mip/top).</summary>
    public static List<string> TextureStems(CaffFile caff, int symbol) =>
        ModelAsset.Parse(caff, symbol, geometry: false).TextureTable.Select(n => n.EndsWith("mip") || n.EndsWith("top") ? n[..^3] : n).Distinct().ToList();

    /// <summary>
    /// LOD table in the rendergraph (chunk 30 → header; +0x140 group count, +0x144 → groups of 0x14 bytes:
    /// nLevels, → levels, centre[3]; level = 16 bytes: f32 switch distance, 0, u32 node count, → nodes). A level with 0
    /// nodes is the cull level: the model is not drawn past its distance (docs/research/151b_drawdistance.md).
    /// Multiplies every switch distance by <paramref name="lodScale"/> and sets cull levels to <paramref name="cullDistance"/>.
    /// Returns (groups, levels changed).
    /// </summary>
    /// <summary>
    /// Makes every reference model used ONLY by hidden instances never draw (B22). Hidden instances are scaled to 0.001,
    /// and the engine's LOD metric is view depth / instance scale, so they always select their LAST LOD level; models whose
    /// last level still has nodes (no cull level) kept drawing everywhere. Here that last level's node count is set to 0,
    /// which makes it a cull level. Models also used by a visible instance (directly or nested) are left alone.
    /// Returns (models culled, models without a LOD table that could not be culled).
    /// </summary>
    public static (int Culled, int NoTable) CullHiddenOnlyModels(CaffFile caff, int backgroundSymbol, ISet<int> hiddenInstances)
    {
        int culled = 0, noTable = 0;
        foreach (int sym in HiddenOnlyModels(caff, backgroundSymbol, hiddenInstances))
        {
            var v = new AssetView(caff, sym);
            var d = v.Data(".data");
            int t = BE.S32(d, 0), cn = BE.S32(d, 4), c30 = -1;
            for (int i = 0; i < cn; i++) if (BE.S32(d, t + 8 * i) == 30) c30 = BE.S32(d, t + 8 * i + 4);
            if (c30 < 0) { noTable++; continue; }
            int hdr = BE.S32(d, c30), ng = BE.S32(d, hdr + 0x140), groups = BE.S32(d, hdr + 0x144);
            if (ng == 0) { noTable++; continue; }
            bool changed = false;
            for (int g = 0; g < ng; g++)
            {
                int gb = groups + 0x14 * g, nl = BE.S32(d, gb), levels = BE.S32(d, gb + 4);
                if (nl == 0) continue;
                int last = levels + 16 * (nl - 1);
                if (BE.S32(d, last + 8) != 0) { BE.W32(d, last + 8, 0); changed = true; }
            }
            if (changed) culled++;
        }
        return (culled, noTable);
    }

    /// <summary>Model symbols reached (directly or as nested reference models) only from the given hidden instances of
    /// the background model, never from a visible one.</summary>
    public static List<int> HiddenOnlyModels(CaffFile caff, int backgroundSymbol, ISet<int> hiddenInstances)
    {
        var modelSym = new Dictionary<uint, int>();
        for (int s = 1; s <= caff.Symbols.Count; s++)
        {
            var nm = AssetIds.DisplayName(caff.Symbols[s - 1]);
            if (nm.StartsWith("aid_model_") && AssetIds.IdOf(caff.Symbols[s - 1]) is uint id) modelSym.TryAdd(id, s);
        }
        var parsed = new Dictionary<int, ModelAsset>();
        ModelAsset M(int sym) => parsed.TryGetValue(sym, out var m) ? m : parsed[sym] = ModelAsset.Parse(caff, sym, geometry: false);
        void Reach(int sym, HashSet<int> set, int depth)
        {
            if (!set.Add(sym) || depth > 6) return;
            var m = M(sym);
            foreach (var inst in m.Instances)
                if (inst.RefModel >= 0 && inst.RefModel < m.ReferenceIds.Count && modelSym.TryGetValue((uint)m.ReferenceIds[inst.RefModel], out int cs) && cs != sym)
                    Reach(cs, set, depth + 1);
        }
        var bg = M(backgroundSymbol);
        var hidden = new HashSet<int>(); var visible = new HashSet<int>();
        foreach (var inst in bg.Instances)
        {
            if (inst.RefModel < 0 || inst.RefModel >= bg.ReferenceIds.Count || !modelSym.TryGetValue((uint)bg.ReferenceIds[inst.RefModel], out int sym)) continue;
            Reach(sym, hiddenInstances.Contains(inst.Index) ? hidden : visible, 0);
        }
        return hidden.Where(s => !visible.Contains(s)).ToList();
    }

    /// <summary>The LOD table (chunk 30): per group, its levels as (switch distance, node count). Empty when absent.</summary>
    public static List<List<(float Distance, int Nodes)>> GetLodTable(CaffFile caff, int symbol)
    {
        var res = new List<List<(float, int)>>();
        var v = new AssetView(caff, symbol);
        var d = v.Data(".data");
        int t = BE.S32(d, 0), cn = BE.S32(d, 4), c30 = -1;
        for (int i = 0; i < cn; i++) if (BE.S32(d, t + 8 * i) == 30) c30 = BE.S32(d, t + 8 * i + 4);
        if (c30 < 0) return res;
        int hdr = BE.S32(d, c30);
        int ng = BE.S32(d, hdr + 0x140), groups = BE.S32(d, hdr + 0x144);
        for (int g = 0; g < ng; g++)
        {
            int gb = groups + 0x14 * g, nl = BE.S32(d, gb), levels = BE.S32(d, gb + 4);
            var lv = new List<(float, int)>();
            for (int l = 0; l < nl; l++) lv.Add((BE.F32(d, levels + 16 * l), BE.S32(d, levels + 16 * l + 8)));
            res.Add(lv);
        }
        return res;
    }

    public static (int Groups, int Levels) SetLodDistances(CaffFile caff, int symbol, float cullDistance, float lodScale = 1f, bool keepLod0 = false)
    {
        var v = new AssetView(caff, symbol);
        var d = v.Data(".data");
        int t = BE.S32(d, 0), cn = BE.S32(d, 4), c30 = -1;
        for (int i = 0; i < cn; i++) if (BE.S32(d, t + 8 * i) == 30) c30 = BE.S32(d, t + 8 * i + 4);
        if (c30 < 0) return (0, 0);
        int hdr = BE.S32(d, c30);
        int ng = BE.S32(d, hdr + 0x140), groups = BE.S32(d, hdr + 0x144), changed = 0;
        for (int g = 0; g < ng; g++)
        {
            int gb = groups + 0x14 * g;
            int nl = BE.S32(d, gb), levels = BE.S32(d, gb + 4);
            for (int l = 0; l < nl; l++)
            {
                int lb = levels + 16 * l;
                float dist = BE.F32(d, lb);
                int nodes = BE.S32(d, lb + 8);
                // keepLod0: every level after the first switches at the cull distance, so level 0 is always drawn
                // (lower LODs of a template may use a different, untextured shader or lack UVs)
                float nd = nodes == 0 || (keepLod0 && l > 0) ? cullDistance : dist * lodScale;
                if (nd != dist) { BE.WF32(d, lb, nd); changed++; }
            }
        }
        return (ng, changed);
    }

    public sealed record CullCell(int Index, int Offset, int Group, Vector3 Min, Vector3 Max);

    /// <summary>Culling cells of a background model (chunk 0): root box, then a grid header {nx, ny, nz, 0, 0, 0, → cells}
    /// of 32-byte cells {u32 triangle count, u32 group (= draw block, 0xFFFFFFFF = empty), AABB min, AABB max}.</summary>
    public static (int RootOffset, List<CullCell> Cells) GetCullCells(CaffFile caff, int symbol)
    {
        var d = new AssetView(caff, symbol).Data(".data");
        int t = BE.S32(d, 0), cn = BE.S32(d, 4), c0 = -1;
        for (int i = 0; i < cn; i++) if (BE.S32(d, t + 8 * i) == 0) c0 = BE.S32(d, t + 8 * i + 4);
        var res = new List<CullCell>();
        if (c0 < 0 || c0 + 0x20 > d.Length || BE.S32(d, c0 + 0x18) != 1) return (-1, res);
        int hdr = BE.S32(d, c0 + 0x1C);
        int n = BE.S32(d, hdr) * BE.S32(d, hdr + 4) * BE.S32(d, hdr + 8), cells = BE.S32(d, hdr + 0x18);
        for (int i = 0; i < n; i++)
        {
            int o = cells + 32 * i;
            res.Add(new CullCell(i, o, (int)BE.U32(d, o + 4), new(BE.F32(d, o + 8), BE.F32(d, o + 12), BE.F32(d, o + 16)), new(BE.F32(d, o + 20), BE.F32(d, o + 24), BE.F32(d, o + 28))));
        }
        return (c0, res);
    }

    /// <summary>
    /// Refits every culling cell to the geometry its draw block now holds (after an import): box = bounds of the block's
    /// drawn vertices, triangle count updated; a block that draws nothing gets a tiny box far below the level so it is
    /// never visible. The root box becomes the union. Keeps the engine's per-block frustum culling working (B22).
    /// Returns (cells with geometry, emptied cells).
    /// </summary>
    public static (int Filled, int Emptied) FitCullCells(CaffFile caff, int symbol)
    {
        var (root, cells) = GetCullCells(caff, symbol);
        if (root < 0) return (0, 0);
        var m = ModelAsset.Parse(caff, symbol);
        var byBlock = m.Draws.Where(x => x.Block >= 0).GroupBy(x => x.Block).ToDictionary(g => g.Key, g => g.ToList());
        var d = new AssetView(caff, symbol).Data(".data");
        void Box(int o, Vector3 mn, Vector3 mx) { BE.WF32(d, o, mn.X); BE.WF32(d, o + 4, mn.Y); BE.WF32(d, o + 8, mn.Z); BE.WF32(d, o + 12, mx.X); BE.WF32(d, o + 16, mx.Y); BE.WF32(d, o + 20, mx.Z); }
        var all = (new Vector3(float.MaxValue), new Vector3(float.MinValue));
        int filled = 0, emptied = 0;
        foreach (var c in cells)
        {
            if (c.Group < 0) continue;
            var mn = new Vector3(float.MaxValue); var mx = new Vector3(float.MinValue); long tris = 0;
            if (byBlock.TryGetValue(c.Group, out var draws))
                foreach (var dr in draws)
                {
                    var idx = dr.Indices;
                    if (idx.Length < 3 || idx.All(k => k == idx[0])) continue;   // the importer's hidden draw (0,0,0)
                    foreach (var k in idx) if (k < dr.Positions.Length) { mn = Vector3.Min(mn, dr.Positions[k]); mx = Vector3.Max(mx, dr.Positions[k]); }
                    tris += idx.Length / 3;
                }
            if (tris == 0)
            {
                Box(c.Offset + 8, new Vector3(0, -100000, 0), new Vector3(0.01f, -99999.99f, 0.01f));
                BE.W32(d, c.Offset, 0); emptied++;
            }
            else
            {
                Box(c.Offset + 8, mn - new Vector3(1), mx + new Vector3(1));
                BE.W32(d, c.Offset, (uint)Math.Min(tris, uint.MaxValue)); filled++;
                all = (Vector3.Min(all.Item1, mn), Vector3.Max(all.Item2, mx));
            }
        }
        if (filled > 0) Box(root, all.Item1 - new Vector3(1), all.Item2 + new Vector3(1));
        return (filled, emptied);
    }

    /// <summary>
    /// Culling tree of background models (chunk 0): root AABB (6 floats), u32 1, → header {u32 node count, 1, 1, 0, 0, 0,
    /// → nodes}; node = 32 bytes {u32, u32 mesh group (0xFFFFFFFF = empty), AABB min[3], max[3]}. The engine skips the
    /// draws of a node whose box is outside the view, so geometry imported far from where the original draw was is
    /// invisible unless the camera happens to see the original box (verified 2026-09-28: Seattle pier decks rendered
    /// only when looking at the original plank location). Sets the root and every non-empty node to the given box.
    /// Returns the number of nodes changed (0 when the model has no culling tree).
    /// </summary>
    public static int SetCullBounds(CaffFile caff, int symbol, System.Numerics.Vector3 min, System.Numerics.Vector3 max)
    {
        var v = new AssetView(caff, symbol);
        var d = v.Data(".data");
        int t = BE.S32(d, 0), cn = BE.S32(d, 4), c0 = -1;
        for (int i = 0; i < cn; i++) if (BE.S32(d, t + 8 * i) == 0) c0 = BE.S32(d, t + 8 * i + 4);
        if (c0 < 0 || c0 + 0x20 > d.Length || BE.S32(d, c0 + 0x18) != 1) return 0;
        int hdr = BE.S32(d, c0 + 0x1C);
        if (hdr <= 0 || hdr + 0x1C > d.Length) return 0;
        int n = BE.S32(d, hdr), nodes = BE.S32(d, hdr + 0x18);
        if (n <= 0 || nodes <= 0 || nodes + 32L * n > d.Length) return 0;
        void Box(int o) { BE.WF32(d, o, min.X); BE.WF32(d, o + 4, min.Y); BE.WF32(d, o + 8, min.Z); BE.WF32(d, o + 12, max.X); BE.WF32(d, o + 16, max.Y); BE.WF32(d, o + 20, max.Z); }
        Box(c0);
        int changed = 0;
        for (int i = 0; i < n; i++)
        {
            int o = nodes + 32 * i;
            if (BE.U32(d, o + 4) == 0xFFFFFFFF) continue;
            Box(o + 8); changed++;
        }
        return changed;
    }
}
