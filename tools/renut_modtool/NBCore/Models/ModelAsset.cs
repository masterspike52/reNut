using System.Numerics;
using NB.Core.Formats;
using NB.Core.IO;

namespace NB.Core.Models;

public enum VtxFormat
{
    k_8_8_8_8 = 6, k_2_10_10_10 = 7, k_10_11_11 = 16, k_11_11_10 = 17, k_16_16 = 25, k_16_16_16_16 = 26,
    k_16_16_FLOAT = 31, k_16_16_16_16_FLOAT = 32, k_32 = 33, k_32_32 = 34, k_32_32_32_32 = 35,
    k_32_FLOAT = 36, k_32_32_FLOAT = 37, k_32_32_32_32_FLOAT = 38, k_32_32_32_FLOAT = 57,
}

/// <summary>One attribute fetched by a Xenos vertex shader (from its vfetch instruction).</summary>
public sealed record VertexElement(int Offset, VtxFormat Format, bool Signed, bool Normalized, int DestReg)
{
    public override string ToString() => $"+{Offset} {Format}{(Signed ? " s" : "")}{(Normalized ? " n" : " i")} r{DestReg}";
}

/// <summary>A node (chunk 2): 3x4 row-major transform with translation in the last column.</summary>
public sealed class ModelNode
{
    public ushort Flags, Parent;
    public Matrix4x4 Local;
    public Vector4 Extra;
}

/// <summary>A scenery instance from a background model (chunk 12).</summary>
public sealed class SceneInstance
{
    public int Index, RefModel;
    public string Name = "";
    public Matrix4x4 World;          // row-vector convention, translation in M41..M43
    public int RecordOffset, MatrixOffset, PositionOffset; // offsets in .data (for editing)
    public int PlacementNode = -1;   // matching chunk-2 node, if any
}

public sealed class MeshDraw
{
    public int VbRecord, IbObject, Stride, IndexCount, Primitive;
    public List<VertexElement> Layout = new();
    public List<(int Slot, string Texture)> Textures = new();
    public Vector3[] Positions = Array.Empty<Vector3>();
    public Vector3[]? Normals;
    public Vector2[]? UVs;
    public uint[]? Colors;
    /// <summary>Skinned meshes: 4 global joint indices per vertex (skeleton order) and weights 0..1 (sum 1).</summary>
    public int[]? BlendIndices;
    public float[]? BlendWeights;
    public int[] Indices = Array.Empty<int>();
    public int VertexShaderPoolOffset = -1;
    /// <summary>Pool part holding the vertex shader microcode (with <see cref="VertexShaderPoolOffset"/>), or -1.</summary>
    public int VertexShaderPoolPart = -1;
    /// <summary>Pixel-shader constants in effect at this draw (stream op 0x06, kind 0): register → value.
    /// Layered materials use c5 as the overlay (paint) colour — Mumbo's Motors' yellow is (1, 1, 0).</summary>
    public Dictionary<int, Vector4> PixelConstants = new();
    /// <summary>Index values are vertex * <see cref="InstanceStride"/> + instance (see ParseDraws); <see cref="Indices"/>
    /// hold the vertex numbers. Importing into such draws is not supported.</summary>
    public bool Instanced;
    /// <summary>Instanced draws: the number of instance slots packed into each index (4 for Showdown Town's windows and
    /// doors, 32 for vehicle body blocks such as body_heavy_cube: 24 vertices, raw indices up to 767). 1 otherwise.</summary>
    public int InstanceStride = 1;
    /// <summary>Instanced triangle lists: the instance slot (raw index mod <see cref="InstanceStride"/>) of each index,
    /// parallel to <see cref="Indices"/>. Grass tiles use it: each slot is one tile placed by the game (see GrassLayer).</summary>
    public int[]? Slots;
    /// <summary>Rendergraph node of the draw (draw op 0x30 +16; -1 for op 0x01). LOD levels select nodes.</summary>
    public int Node = -1;
    /// <summary>Draw block (stream op 0x17 = {u32 block, u32 end offset}) the draw belongs to; -1 outside any block.
    /// Background models cull whole blocks: culling cell (chunk 0) group g = block g (verified on all 112 Showdown Town cells).</summary>
    public int Block = -1;
    /// <summary>Second texture coordinate set (contact-occlusion / blend maps use it), when the layout has one.</summary>
    public Vector2[]? UVs2;
    /// <summary>Third texture coordinate set (Nutty Acres' fabric materials: the ripped-edge alpha of fabric grass).</summary>
    public Vector2[]? UVs3;
    /// <summary>Tangents (the second 2:10:10:10 element of normal-mapped layouts).</summary>
    public Vector3[]? Tangents;
    /// <summary>Material section flags (stream op 0x15 +8) in effect at this draw: low nibble 1 = opaque, 2 = alpha
    /// blended (render states SRC_ALPHA / ONE_MINUS_SRC_ALPHA set in the section), 0 = alpha-tested cut-outs (grilles, foliage).</summary>
    public uint SectionFlags;
    /// <summary>Per pass of the 3-way pass switch (op 0x16): the state each pass binds before this draw. Pass 1 is the
    /// depth-only pass (no pixel shader, op 0x2A); passes 0 and 2 are colour passes. Empty when the stream has no switch.</summary>
    public DrawPass[] Passes = Array.Empty<DrawPass>();
    /// <summary>Pixel shader in effect for draws outside a pass switch (models without op 0x16).</summary>
    public (int Part, int Offset)? PixelShader;
    public int PixelShaderObject = -1;
    /// <summary>The model this draw belongs to (microcode and shader headers are read through its asset view).</summary>
    public ModelAsset? Owner;
    /// <summary>The draw's colour-pass pixel shader decoded (cached; null when there is none or it could not be read).</summary>
    public XenosShader? ColourShader
    {
        get
        {
            if (_psDone) return _ps;
            _psDone = true;
            try
            {
                var pass = Passes.Length > 0 ? Passes[0] : null;
                var loc = pass != null ? pass.PixelShader : PixelShader;
                int obj = pass != null ? pass.PixelShaderObject : PixelShaderObject;
                if (loc is { } l && Owner != null)
                    _ps = XenosShader.Load(Owner.View.Part(l.Part).Data, l.Offset, obj >= 0 ? Owner.View.Data(".data") : null, obj);
            }
            catch { _ps = null; }
            return _ps;
        }
    }
    XenosShader? _ps; bool _psDone;

    /// <summary>The colour pass's vertex shader decoded (cached; null when unknown).</summary>
    public XenosShader? ColourVertexShader
    {
        get
        {
            if (_vsDone) return _vs;
            _vsDone = true;
            try
            {
                if (Passes.Length > 0 && Passes[0].VertexShader is { } l && Owner != null)
                    _vs = XenosShader.Load(Owner.View.Part(l.Part).Data, l.Offset, null, -1);
            }
            catch { _vs = null; }
            return _vs;
        }
    }
    XenosShader? _vs; bool _vsDone;
}

/// <summary>Render state of one pass at a draw (see <see cref="MeshDraw.Passes"/>).</summary>
public sealed class DrawPass
{
    /// <summary>Pixel shader microcode (pool part, offset), when its patch location is known.</summary>
    public (int Part, int Offset)? PixelShader;
    /// <summary>.data offset of the pass's D3D pixel shader object (stream op 0x02 +4); its 0x102A1100 header lists the
    /// shader's literal constant ranges (see <see cref="XenosShader"/>). -1 when unknown.</summary>
    public int PixelShaderObject = -1;
    /// <summary>Vertex shader microcode bound by this pass (op 0x2E +0x14, patched from the pool), and the vertex shader
    /// constants the stream set (op 0x06 with +4 = 1).</summary>
    public (int Part, int Offset)? VertexShader;
    public Dictionary<int, Vector4> VsConstants = new();
    public bool NullPixelShader;
    public List<(int Slot, string Texture)> Textures = new();
    public Dictionary<int, Vector4> Constants = new();
    /// <summary>Render-state op 0x0C (register, value): 0x48/0x4C/0x50 = src blend / dest blend / blend op (Xenos
    /// enums: 6 = SRC_ALPHA, 7 = ONE_MINUS_SRC_ALPHA), 0x44 = blend factor (alpha in the top byte).</summary>
    public Dictionary<int, uint> States = new();
    public DrawPass Clone() => new() { PixelShader = PixelShader, PixelShaderObject = PixelShaderObject, VertexShader = VertexShader, VsConstants = new(VsConstants), NullPixelShader = NullPixelShader, Textures = Textures, Constants = new(Constants), States = new(States) };
}

/// <summary>
/// Parser for aid_model assets. .data starts with (ptr chunk table, count); chunks are (id, ptr).
/// Rendering is a recorded command stream in .stream (see docs/FORMATS.md §6); vertex layouts come from the
/// vfetch instructions of the vertex shader microcode, which lives in the bundle's shared "pool" asset.
/// </summary>
public sealed class ModelAsset
{
    public AssetView View = null!;
    public Dictionary<int, int> Chunks = new();
    public List<ModelNode> Nodes = new();
    public List<SceneInstance> Instances = new();
    public List<int> ReferenceIds = new();
    public List<string> TextureTable = new();
    /// <summary>.data offsets of the named texture-table entries (parallel to <see cref="TextureTable"/>).</summary>
    public List<int> TextureTableOffsets = new();
    /// <summary>Texture-table entries by the index the command stream uses (op 0x43), unnamed entries left out.</summary>
    public Dictionary<int, string> TextureByIndex = new();
    public List<MeshDraw> Draws = new();
    /// <summary>A copy sharing everything (nodes, draws …) whose lists can then be replaced, e.g. a model drawn without
    /// some of its draws (WorldScene: a world door without Grunty's sign).</summary>
    public ModelAsset ShallowCopy() => (ModelAsset)MemberwiseClone();
    public List<string> Warnings = new();
    public int ResourceHeader = -1;

    public static ModelAsset Parse(CaffFile caff, int symbol, Dictionary<int, List<(int, int)>>? relocIndex = null, bool geometry = true)
    {
        var m = new ModelAsset { View = new AssetView(caff, symbol, relocIndex) };
        m.ParseChunks();
        m.ParseNodes();
        m.ParseInstances();
        m.ParseTextureTable();
        m.ParseLodNodes();
        if (geometry && m.View.Has(".stream") && m.View.Has(".gpu")) m.ParseDraws();
        return m;
    }

    byte[] D => View.Data(".data");

    void ParseChunks()
    {
        var d = D;
        int t = BE.S32(d, 0), n = BE.S32(d, 4);
        for (int i = 0; i < n; i++) Chunks[BE.S32(d, t + 8 * i)] = BE.S32(d, t + 8 * i + 4);
    }

    void ParseNodes()
    {
        if (!Chunks.TryGetValue(2, out int o)) return;
        var d = D;
        int n = BE.S32(d, o);
        for (int i = 0; i < n; i++)
        {
            int b = o + 4 + 68 * i;
            var f = new float[16];
            for (int k = 0; k < 16; k++) f[k] = BE.F32(d, b + 4 + 4 * k);
            // stored as 3 rows of (r0 r1 r2 t); convert to row-vector Matrix4x4 (transpose of rotation)
            var mat = new Matrix4x4(f[0], f[4], f[8], 0, f[1], f[5], f[9], 0, f[2], f[6], f[10], 0, f[3], f[7], f[11], 1);
            Nodes.Add(new ModelNode { Flags = BE.U16(d, b), Parent = BE.U16(d, b + 2), Local = mat, Extra = new Vector4(f[12], f[13], f[14], f[15]) });
        }
    }

    void ParseInstances()
    {
        if (!Chunks.TryGetValue(12, out int h)) return;
        var d = D;
        // header: +0 reference count, +4 record count, +8 matrix count, +0xC position count, … +0x18 reference ids,
        // +0x1C records (0x144 bytes), +0x20 matrices (64 bytes), +0x24 positions (12 bytes).
        // Record +0x10 = matrix index, +0x14 = position index. In world background models both equal the record index;
        // in reference models (composite buildings) they do not (building5: 106 records, 99 matrices).
        int nref = BE.S32(d, h), n = BE.S32(d, h + 4), nMats = BE.S32(d, h + 8), nPos = BE.S32(d, h + 0xC);
        int pIds = BE.S32(d, h + 0x18), pNames = BE.S32(d, h + 0x1C), pMats = BE.S32(d, h + 0x20), pPos = BE.S32(d, h + 0x24);
        for (int i = 0; i < nref; i++) ReferenceIds.Add(BE.S32(d, pIds + 4 * i));
        for (int i = 0; i < n; i++)
        {
            int ro = pNames + 0x144 * i;
            int mi = BE.S32(d, ro + 0x10), pi = BE.S32(d, ro + 0x14);
            if (mi < 0 || mi >= nMats) { Warnings.Add($"instance {i}: matrix index {mi} out of range"); continue; }
            if (pi < 0 || pi >= nPos) pi = mi;
            int mo = pMats + 64 * mi;
            var f = new float[16];
            for (int k = 0; k < 16; k++) f[k] = BE.F32(d, mo + 4 * k);
            var inst = new SceneInstance
            {
                Index = i, RefModel = BE.S32(d, ro),
                Name = BE.CStr(d, ro + 0x3C, 0x100).Trim('|'),
                World = new Matrix4x4(f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]),
                RecordOffset = ro, MatrixOffset = mo, PositionOffset = pPos + 12 * pi,
            };
            // match the chunk-2 node carrying the same translation
            for (int k = 0; k < Nodes.Count; k++)
            {
                var t = Nodes[k].Local.Translation;
                if (Vector3.Distance(t, inst.World.Translation) < 1e-3f) { inst.PlacementNode = k; break; }
            }
            Instances.Add(inst);
        }
    }

    public static string CleanInstanceName(string raw) => raw.Replace("REFERENCE_", "");

    /// <summary>Rendergraph nodes shown only by LOD levels above 0 (hidden when LOD 0 is kept, e.g. by
    /// ModelEdit.SetLodDistances keepLod0). Node index = (node pointer - first node) / node size (0x50).</summary>
    public HashSet<int> LodOnlyNodes = new();
    /// <summary>LOD groups (chunk 30): per group, its levels (switch distance, rendergraph node indices; empty = cull level).
    /// The engine picks the highest level whose distance &lt;= view depth / instance scale (docs/research/151b).</summary>
    public List<List<(float Distance, HashSet<int> Nodes)>> LodLevels = new();

    /// <summary>View depth (divided by the instance scale) beyond which the game draws nothing of this model: every LOD
    /// group ends with an empty "cull" level (Showdown Town props cull at 83–800 units, docs/research/151b). Infinity when
    /// some group never culls or the model has no LOD table (terrain, hinterlands, skydomes).</summary>
    public float CullDistance
    {
        get
        {
            if (LodLevels.Count == 0) return float.PositiveInfinity;
            float d = 0;
            foreach (var g in LodLevels)
            {
                if (g.Count == 0 || g[^1].Nodes.Count > 0) return float.PositiveInfinity;
                d = MathF.Max(d, g[^1].Distance);
            }
            return d > 0 ? d : float.PositiveInfinity;
        }
    }

    void ParseLodNodes()
    {
        if (!Chunks.TryGetValue(30, out int c30)) return;
        var d = D;
        try
        {
            int rg = BE.S32(d, c30);
            if (rg < 0 || rg + 0x148 > d.Length || !d.AsSpan(rg, 11).SequenceEqual("rendergraph"u8)) return;
            int ng = BE.S32(d, rg + 0x140), gp = BE.S32(d, rg + 0x144);
            var levelNodes = new List<(int Group, int Level, int Ptr)>();
            for (int g = 0; g < ng; g++)
            {
                int e = gp + 0x14 * g, nl = BE.S32(d, e), lp = BE.S32(d, e + 4);
                for (int k = 0; k < nl; k++)
                {
                    int cnt = BE.S32(d, lp + 16 * k + 8), np = BE.S32(d, lp + 16 * k + 12);
                    for (int i = 0; i < cnt; i++) levelNodes.Add((g, k, BE.S32(d, np + 4 * i)));
                }
            }
            if (levelNodes.Count == 0) return;
            int baseNode = levelNodes.Min(x => x.Ptr);
            for (int g = 0; g < ng; g++)
            {
                int e = gp + 0x14 * g, nl = BE.S32(d, e), lp = BE.S32(d, e + 4);
                var levels = new List<(float, HashSet<int>)>();
                for (int k = 0; k < nl; k++)
                {
                    int cnt = BE.S32(d, lp + 16 * k + 8), np = BE.S32(d, lp + 16 * k + 12);
                    var set = new HashSet<int>();
                    for (int i = 0; i < cnt; i++) set.Add((BE.S32(d, np + 4 * i) - baseNode) / 0x50);
                    levels.Add((BE.F32(d, lp + 16 * k), set));
                }
                LodLevels.Add(levels);
            }
            foreach (var grp in levelNodes.GroupBy(x => x.Group))
            {
                var lod0 = grp.Where(x => x.Level == 0).Select(x => x.Ptr).ToHashSet();
                foreach (var x in grp.Where(x => x.Level > 0 && !lod0.Contains(x.Ptr))) LodOnlyNodes.Add((x.Ptr - baseNode) / 0x50);
            }
        }
        catch (Exception e) { Warnings.Add("LOD nodes: " + e.Message); }
    }

    /// <summary>Start of the model's constant block in .data: the ".data" descriptor (name, size, alignment 0x20, pointer)
    /// that every model carries; stream op 0x05 offsets index into it. -1 when not found.</summary>
    int FindConstantBlock()
    {
        var d = D;
        for (int o = 0; o + 24 <= d.Length; o++)
        {
            if (d[o] != 0x2E || d[o + 1] != (byte)'d' || d[o + 2] != (byte)'a' || d[o + 3] != (byte)'t' || d[o + 4] != (byte)'a' || d[o + 5] != 0) continue;
            int q = (o + 6 + 3) & ~3;
            for (int k = q; k < q + 12 && k + 12 <= d.Length; k += 4)
                if (BE.S32(d, k + 4) == 0x20 && View.PtrAt(".data", k + 8) is { } p && View.SectionOfPart(p.Part) == ".data") return p.Offset;
        }
        return -1;
    }

    void ParseTextureTable()
    {
        // 24-byte entries whose last field points at an "aid_texture_…" string in .data.
        var d = D;
        int pid = View.PartId(".data");
        var entries = new SortedDictionary<int, string>();
        foreach (var ((part, off), to) in View.Pointers)
        {
            if (part != pid || to != pid) continue;
            int target = BE.S32(d, off);
            if (target < 0 || target + 12 >= d.Length) continue;
            if (!d.AsSpan(target, 12).SequenceEqual("aid_texture_"u8)) continue;
            entries[off - 20] = BE.CStr(d, target, 256);
        }
        // The command stream indexes all entries in .data order. Many models store the table in two runs separated by a
        // 12- or 60-byte gap (reference models in Showdown Town / Banjoland: 328 + 139 models); stopping at the first
        // gap left the second half unnamed ("#20/21"), so those draws rendered untextured.
        foreach (var name in entries.Values) TextureTable.Add(name);
        TextureTableOffsets.AddRange(entries.Keys);
        // Indices count every 24-byte entry, including entries without a texture name (engine-supplied textures such as
        // the environment cube map of Banjoland's floor: two unnamed entries shift every later index by 2). A gap that is
        // not a whole number of entries (12 / 36 / 60 bytes) starts a new run whose indices continue after the named and
        // unnamed whole entries of the gap.
        string rule = Environment.GetEnvironmentVariable("NB_TEXIDX_RULE") ?? "field";
        if (rule == "field")
        {
            // each 24-byte entry carries its own index at +8 (unnamed entries = engine textures keep their index; models
            // hold up to three tables, one per LOD, with the same numbering: the first entry for an index wins)
            foreach (var (off, name) in entries)
            {
                int ix = BE.S32(d, off + 8);
                if (ix >= 0 && ix < 4096) TextureByIndex.TryAdd(ix, name);
            }
        }
        else if (rule != "concat" && entries.Count > 0)
        {
            int real = 0, prev = int.MinValue;
            foreach (var (off, name) in entries)
            {
                if (prev != int.MinValue) real += rule == "slots2" ? Math.Max(1, (off - prev + 12) / 24) : Math.Max(1, (off - prev) / 24);
                TextureByIndex[real] = name;
                prev = off;
            }
        }
        if (TextureTable.Count > 0) return;
        // Models converted from other sources (Banjoland's N64 exhibits) have no named entries: the table is an array of
        // pointers straight to texture assets stored beside the model (symbols like "_0x09DDDCA5.rgb.bin" and "…(1)",
        // mip/top pairs). Use the longest run of consecutive pointers into other assets' texture headers.
        var texPtrs = new SortedDictionary<int, string>();
        foreach (var ((part, off), to) in View.Pointers)
        {
            if (part != pid || View.IsOwnPart(to)) continue;
            var tp = View.Part(to);
            if (View.SectionOfPart(to) != ".data" || !NB.Core.Textures.TextureHeader.IsTexture(tp.Data)) continue;
            texPtrs[off] = AssetIds.DisplayName(View.Caff.Symbols[tp.Symbol - 1]);
        }
        List<string> best = new(), cur = new(); int last = int.MinValue;
        foreach (var (off, name) in texPtrs)
        {
            if (off != last + 4) cur = new();
            cur.Add(name); last = off;
            if (cur.Count > best.Count) best = cur;
        }
        TextureTable.AddRange(best);
    }

    void ParseDraws()
    {
        var d = D; var s = View.Data(".stream"); var g = View.Data(".gpu");
        int dataPid = View.PartId(".data"), gpuPid = View.PartId(".gpu");

        // Resource header R: R+0x48 points at the GPU buffer table (entries: ptr object, ptr .gpu, size).
        var gpuPtrLocs = new HashSet<int>(View.Pointers.Where(kv => kv.Key.Part == dataPid && kv.Value == gpuPid).Select(kv => kv.Key.Offset));
        var vsPatch = new Dictionary<int, (int Part, int Offset)>();
        var psPatch = new Dictionary<int, (int Part, int Offset)>();
        var ibTable = new Dictionary<int, (int Gpu, int Size, int Fmt)>();
        var dataToData = View.Pointers.Where(kv => kv.Key.Part == dataPid && kv.Value == dataPid).Select(kv => kv.Key.Offset).OrderBy(o => o).ToList();
        {
            var refs = dataToData.Where(r => gpuPtrLocs.Contains(BE.S32(d, r) + 4) && r >= 0x48).ToList();
            foreach (var r in refs)
            {
                int R = r - 0x48;
                if (R < 0) continue;
                if (R + 0x5C > d.Length || View.PtrAt(".data", R + 0x54) is not { } ibp) continue;
                int nIb = BE.S32(d, R + 0x58);
                if (nIb < 0 || ibp.Offset + 16L * nIb > d.Length) continue;
                ResourceHeader = R;
                for (int i = 0; i < nIb; i++)
                {
                    int e = ibp.Offset + 16 * i;
                    ibTable[BE.S32(d, e)] = (BE.S32(d, e + 4), BE.S32(d, e + 8), BE.S32(d, e + 12));
                }
                if (View.PtrAt(".data", R + 0x30) is { } locs && View.PtrAt(".data", R + 0x34) is { } addrs)
                {
                    int n = BE.S32(d, R + 0x38);
                    for (int i = 0; i < n; i++)
                    {
                        int streamLoc = BE.S32(d, locs.Offset + 4 * i);
                        if (View.PtrAt(".data", addrs.Offset + 4 * i) is { } a) vsPatch[streamLoc] = (a.Part, a.Offset);
                    }
                }
                if (View.PtrAt(".data", R + 0x24) is { } plocs && View.PtrAt(".data", R + 0x28) is { } paddrs)
                {
                    int n = BE.S32(d, R + 0x2C);
                    for (int i = 0; i < n && plocs.Offset + 4 * i + 4 <= d.Length; i++)
                    {
                        int streamLoc = BE.S32(d, plocs.Offset + 4 * i);
                        if (View.PtrAt(".data", paddrs.Offset + 4 * i) is { } a) psPatch[streamLoc] = (a.Part, a.Offset);
                    }
                }
                break;
            }
        }
        if (ResourceHeader < 0) Warnings.Add("resource header not found; index buffers unresolved");

        // Walk the command stream linearly; every variant path is visited, the last VB bind before a draw wins.
        int pos = View.PtrAt(".stream", 0)?.Offset ?? 0x24;
        int curVb = -1, curVsLoc = -1, curBlock = -1; List<(int, string)>? curTex = null;
        var psConst = new Dictionary<int, Vector4>();
        var vbShaders = new Dictionary<int, HashSet<int>>();
        var seen = new HashSet<(int, int)>();
        // The 3-way pass switch (op 0x16: three stream targets) runs one variant per render pass; every variant but the last
        // jumps (op 0x19) to the join point. State is simulated per pass: common commands apply to all three.
        var passes = new[] { new DrawPass(), new DrawPass(), new DrawPass() };
        int[]? swStart = null; int swJoin = -1; bool anySwitch = false;
        uint curFlags = 0; bool sawSection = false;
        int constBlock = FindConstantBlock();
        (int, int)? curPs = null; int curPsObj = -1;
        while (pos + 4 <= s.Length)
        {
            uint w = BE.U32(s, pos);
            int size = (int)(w >> 16), op = (int)((w >> 8) & 0xFF);
            if ((w & 0xFF) != 0 || size < 4 || pos + size > s.Length) break;
            int variant = -1;
            if (swStart != null)
            {
                if (swJoin >= 0 && pos >= swJoin) swStart = null;
                else for (int k = 0, best = -1; k < 3; k++) if (swStart[k] <= pos && swStart[k] > best && (swJoin < 0 || swStart[k] < swJoin)) { best = swStart[k]; variant = k; }
            }
            IEnumerable<DrawPass> Targets() => variant < 0 ? passes : new[] { passes[variant] };
            switch (op)
            {
                case 0x16 when size >= 20:
                {
                    var t = new int[3];
                    for (int k = 0; k < 3; k++) t[k] = View.PtrAt(".stream", pos + 8 + 4 * k)?.Offset ?? int.MaxValue;
                    if (t.All(x => x != int.MaxValue)) { swStart = t; swJoin = -1; anySwitch = true; }
                    break;
                }
                case 0x19 when variant >= 0 && swJoin < 0:
                    swJoin = View.PtrAt(".stream", pos + 4)?.Offset ?? -1;
                    break;
                case 0x15: curFlags = size >= 12 ? BE.U32(s, pos + 8) : 0; sawSection = true; break;
                // model-level material flags: they apply to draws of models without 0x15 sections (Jiggoseum's light-shaft
                // dome: 0x104500 = glow, additive)
                case 0x1A when size >= 8 && !sawSection: curFlags = BE.U32(s, pos + 4); break;
                case 0x02:
                {
                    (int, int)? ps = psPatch.TryGetValue(pos + 4, out var pp) ? pp : null;
                    int psObj = View.PtrAt(".stream", pos + 4) is { } po && View.SectionOfPart(po.Part) == ".data" ? po.Offset : -1;
                    foreach (var p in Targets()) { p.PixelShader = ps; p.PixelShaderObject = psObj; p.NullPixelShader = false; }
                    if (variant <= 0) { curPs = ps; curPsObj = psObj; }
                    break;
                }
                case 0x2A: foreach (var p in Targets()) { p.PixelShader = null; p.NullPixelShader = true; } break;
                case 0x0C when size >= 12:
                {
                    int reg = BE.S32(s, pos + 4); uint val = BE.U32(s, pos + 8);
                    foreach (var p in Targets()) p.States[reg] = val;
                    break;
                }
                case 0x17: // draw block: +4 block index, +8 stream offset of the block's end (skipped when culled)
                    curBlock = BE.S32(s, pos + 4);
                    break;
                case 0x2E:   // bind vertex buffer: +8 VB record, +0x14 vertex shader (patched from the pool)
                case 0x2F when size >= 0x28:   // skinned bind: +4 / +0x10 joint data, +8 VB record, +0x24 vertex shader
                    curVb = View.PtrAt(".stream", pos + 8)?.Offset ?? -1;
                    curVsLoc = pos + (op == 0x2E ? 0x14 : 0x24);
                    if (vsPatch.TryGetValue(curVsLoc, out var vsl)) foreach (var p in Targets()) p.VertexShader = vsl;
                    if (curVb >= 0 && vsPatch.ContainsKey(curVsLoc))
                    {
                        if (!vbShaders.TryGetValue(curVb, out var set)) vbShaders[curVb] = set = new();
                        set.Add(curVsLoc);
                    }
                    break;
                case 0x43:
                {
                    int count = (int)(BE.U32(s, pos + 8) >> 16);
                    var list = new List<(int, string)>();
                    for (int k = 0; k < count && pos + 12 + 12 * k + 12 <= pos + size; k++)
                    {
                        int mip = BE.S32(s, pos + 12 + 12 * k), top = BE.S32(s, pos + 16 + 12 * k);
                        int slot = (int)(BE.U32(s, pos + 20 + 12 * k) >> 16);
                        string name = TextureByIndex.Count > 0
                            ? (TextureByIndex.TryGetValue(top, out var tn) ? tn : TextureByIndex.TryGetValue(mip, out var mn) ? mn : $"#{mip}/{top}")
                            : top >= 0 && top < TextureTable.Count ? TextureTable[top] : mip >= 0 && mip < TextureTable.Count ? TextureTable[mip] : $"#{mip}/{top}";
                        list.Add((slot, name));
                    }
                    curTex = list;
                    foreach (var p in Targets()) p.Textures = list;
                    break;
                }
                case 0x05 when size >= 12 && constBlock >= 0:
                {
                    // vertex shader constants from the model's constant block (+4 byte offset, +8 first register << 16 | count):
                    // texture-coordinate transforms (Spiral Mountain's rock detail: (4,0,0,0), (0,4,0,-3))
                    int off = BE.S32(s, pos + 4); uint rc = BE.U32(s, pos + 8); int reg = (int)(rc >> 16), cnt = (int)(rc & 0xFFFF);
                    for (int k = 0; k < cnt && constBlock + off + 16 * k + 16 <= d.Length; k++)
                    {
                        int q = constBlock + off + 16 * k;
                        var cv = new Vector4(BE.F32(d, q), BE.F32(d, q + 4), BE.F32(d, q + 8), BE.F32(d, q + 12));
                        foreach (var p in Targets()) p.VsConstants[reg + k] = cv;
                    }
                    break;
                }
                case 0x06: // set shader constants: +4 kind (0 = pixel), +8 (first register << 16 | count), then float4s
                {
                    if (BE.U32(s, pos + 4) == 1)
                    {
                        uint vrc = BE.U32(s, pos + 8); int vreg = (int)(vrc >> 16), vcnt = (int)(vrc & 0xFFFF);
                        for (int k = 0; k < vcnt && pos + 12 + 16 * k + 16 <= pos + size; k++)
                        {
                            int q = pos + 12 + 16 * k;
                            var cv = new Vector4(BE.F32(s, q), BE.F32(s, q + 4), BE.F32(s, q + 8), BE.F32(s, q + 12));
                            foreach (var p in Targets()) p.VsConstants[vreg + k] = cv;
                        }
                        break;
                    }
                    if (BE.U32(s, pos + 4) != 0) break;
                    uint rc = BE.U32(s, pos + 8); int reg = (int)(rc >> 16), cnt = (int)(rc & 0xFFFF);
                    for (int k = 0; k < cnt && pos + 12 + 16 * k + 16 <= pos + size; k++)
                    {
                        int q = pos + 12 + 16 * k;
                        var cv = new Vector4(BE.F32(s, q), BE.F32(s, q + 4), BE.F32(s, q + 8), BE.F32(s, q + 12));
                        psConst[reg + k] = cv;
                        foreach (var p in Targets()) p.Constants[reg + k] = cv;
                    }
                    break;
                }
                case 0x01: // draw indexed (prim, count, ib)
                case 0x30: // draw indexed (prim, count, ib, index)
                {
                    int prim = BE.S32(s, pos + 4), count = BE.S32(s, pos + 8);
                    int ib = View.PtrAt(".stream", pos + 12)?.Offset ?? -1;
                    int node = op == 0x30 ? BE.S32(s, pos + 16) : -1;   // rendergraph node (LOD levels list nodes)
                    if (curVb >= 0 && ib >= 0 && seen.Add((curVb, ib)))
                        Draws.Add(new MeshDraw { VbRecord = curVb, IbObject = ib, IndexCount = count, Primitive = prim, Textures = curTex ?? new(), PixelConstants = new(psConst), Node = node, Block = curBlock,
                                                 SectionFlags = curFlags, Passes = anySwitch ? passes.Select(p => p.Clone()).ToArray() : Array.Empty<DrawPass>(),
                                                 PixelShader = curPs, PixelShaderObject = curPsObj, Owner = this });
                    break;
                }
            }
            if (op == 0x1D) break;
            pos += size;
        }

        // decode geometry
        var poolCache = new Dictionary<int, List<VertexElement>>();
        // draws of one vertex buffer share its decoded vertices (a model's pieces often draw ranges of one big buffer:
        // the docks' walkway decoded its 330k-vertex buffer nine times, 3M vertices in all)
        var decodedVb = new Dictionary<int, MeshDraw>();
        foreach (var dr in Draws)
        {
            try
            {
                int stride = BE.S32(d, dr.VbRecord);
                var vbGpu = View.PtrAt(".data", dr.VbRecord + 8);
                int vbSize = BE.S32(d, dr.VbRecord + 12);
                dr.Stride = stride;
                if (vbGpu == null || stride <= 0) { Warnings.Add($"draw vb 0x{dr.VbRecord:X}: no gpu pointer"); continue; }
                // choose the richest vertex shader bound with this buffer
                List<VertexElement> best = new();
                if (vbShaders.TryGetValue(dr.VbRecord, out var locs))
                    foreach (var loc in locs)
                    {
                        var (part, off) = vsPatch[loc];
                        var layout = VFetch.Scan(View.Part(part).Data, off, 0x1000, stride / 4);
                        if (layout.Count > best.Count) { best = layout; dr.VertexShaderPoolOffset = off; dr.VertexShaderPoolPart = part; }
                    }
                if (best.Count == 0) best = VFetch.Guess(stride);
                dr.Layout = best;
                int nv = vbSize / stride;
                if (decodedVb.TryGetValue(dr.VbRecord, out var same) && same.Stride == stride && same.Layout.SequenceEqual(best))
                {
                    dr.Positions = same.Positions; dr.Normals = same.Normals; dr.UVs = same.UVs; dr.UVs2 = same.UVs2; dr.UVs3 = same.UVs3;
                    dr.Tangents = same.Tangents; dr.Colors = same.Colors; dr.BlendIndices = same.BlendIndices; dr.BlendWeights = same.BlendWeights;
                }
                else { DecodeVertices(dr, g, vbGpu.Value.Offset, nv); decodedVb[dr.VbRecord] = dr; }
                if (!ibTable.TryGetValue(dr.IbObject, out var ibi)) { Warnings.Add($"draw ib 0x{dr.IbObject:X}: not in table"); continue; }
                int nIdx = Math.Min(dr.IndexCount, ibi.Size / 2);
                var idx = new int[nIdx];
                for (int i = 0; i < nIdx; i++) idx[i] = BE.U16(g, ibi.Gpu + 2 * i);
                // Instanced draws: index = vertex * K + instance (0..K-1); the vertex shader picks one of K transforms held in
                // shader constants. K = 4 in the worlds (Showdown Town: 1,624 draws, e.g. dockwalkway beams), 32 for vehicle
                // body blocks. Decode the vertex; the instance transforms come from the placements (the editor draws the
                // base mesh once per placed instance).
                int k = InstanceFactor(idx, dr.Primitive, nv);
                if (k > 1)
                {
                    dr.Instanced = true; dr.InstanceStride = k;
                    if (dr.Primitive == 4) dr.Slots = idx.Select(i => i % k).ToArray();
                    for (int i = 0; i < idx.Length; i++) if (idx[i] != 0xFFFF || dr.Primitive != 5) idx[i] /= k;
                }
                dr.Indices = dr.Primitive switch { 4 => idx, 5 => StripToList(idx), _ => idx };
                if (dr.Primitive != 4 && dr.Primitive != 5) Warnings.Add($"draw prim {dr.Primitive} treated as triangle list");
            }
            catch (Exception e) { Warnings.Add($"draw vb 0x{dr.VbRecord:X}: {e.Message}"); }
        }
        // Instancing is a property of the vertex buffer's shader: when one draw of a buffer is instanced, all are. A draw whose
        // raw indices stay below the vertex count was not detected above (Banjo's House walls: raw 0..2463 = vertices 0..615,
        // right before the next draw's 616..) and must be decoded the same way.
        foreach (var grp in Draws.Where(x => x.Indices != null).GroupBy(x => x.VbRecord))
        {
            if (!grp.Any(x => x.Instanced)) continue;
            int k = grp.Where(x => x.Instanced).GroupBy(x => x.InstanceStride).OrderByDescending(g => g.Count()).First().Key;
            foreach (var dr in grp.Where(x => !x.Instanced))
            {
                dr.Instanced = true; dr.InstanceStride = k;
                if (dr.Primitive == 4) dr.Slots = dr.Indices.Select(i => i % k).ToArray();
                for (int i = 0; i < dr.Indices.Length; i++) dr.Indices[i] /= k;
            }
        }
    }

    /// <summary>
    /// Instance slots packed into a draw's raw indices (vertex * K + instance), or 1. Only draws whose indices run past the
    /// vertex buffer are instanced here (others inherit it from their buffer). K is the largest power of two (2..64) for
    /// which the three corners of every triangle (consecutive indices of a strip) fall in the same instance slot, provided
    /// the vertex numbers then fit the buffer; else the smallest K that makes them fit (the old 4-slot rule).
    /// </summary>
    static int InstanceFactor(int[] idx, int prim, int nv)
    {
        if (idx.Length == 0 || nv <= 0) return 1;
        int max = 0;
        foreach (int i in idx) if ((prim != 5 || i != 0xFFFF) && i > max) max = i;
        if (max < nv) return 1;
        bool Consistent(int k)
        {
            if (prim == 5)
            {
                for (int i = 1; i < idx.Length; i++)
                    if (idx[i] != 0xFFFF && idx[i - 1] != 0xFFFF && idx[i] % k != idx[i - 1] % k) return false;
                return true;
            }
            for (int i = 0; i + 2 < idx.Length; i += 3)
                if (idx[i] % k != idx[i + 1] % k || idx[i] % k != idx[i + 2] % k) return false;
            return true;
        }
        for (int k = 64; k >= 2; k /= 2)
            if (max < k * nv && Consistent(k)) return k;
        for (int k = 2; k <= 64; k *= 2)
            if (max < k * nv) return k;
        return 1;
    }

    static int[] StripToList(int[] s)
    {
        var l = new List<int>();
        for (int i = 2; i < s.Length; i++)
        {
            if (s[i] == 0xFFFF || s[i - 1] == 0xFFFF || s[i - 2] == 0xFFFF) continue;
            if ((i & 1) == 0) { l.Add(s[i - 2]); l.Add(s[i - 1]); l.Add(s[i]); } else { l.Add(s[i - 1]); l.Add(s[i - 2]); l.Add(s[i]); }
        }
        return l.ToArray();
    }

    static void DecodeVertices(MeshDraw dr, byte[] g, int start, int n)
    {
        var pos = dr.Layout.FirstOrDefault(e => e.Offset == 0) ?? dr.Layout.First();
        var uv = dr.Layout.FirstOrDefault(e => e.Format is VtxFormat.k_16_16_FLOAT or VtxFormat.k_32_32_FLOAT);
        var uv2 = uv == null ? null : dr.Layout.FirstOrDefault(e => e != uv && e.Offset > uv.Offset && e.Format is VtxFormat.k_16_16_FLOAT or VtxFormat.k_32_32_FLOAT);
        var uv3 = uv2 == null ? null : dr.Layout.Where(e => e.Offset > uv2.Offset && e.Format is VtxFormat.k_16_16_FLOAT or VtxFormat.k_32_32_FLOAT).OrderBy(e => e.Offset).FirstOrDefault();
        var nrm = dr.Layout.FirstOrDefault(e => e != pos && e.Format is VtxFormat.k_2_10_10_10 or VtxFormat.k_10_11_11 or VtxFormat.k_11_11_10);
        var tan = nrm == null ? null : dr.Layout.FirstOrDefault(e => e != pos && e != nrm && e.Offset > nrm.Offset && e.Format is VtxFormat.k_2_10_10_10 or VtxFormat.k_10_11_11 or VtxFormat.k_11_11_10);
        // vertex colour: a normalized 8:8:8:8 element, unless the layout is skinned (then it holds the blend weights)
        bool skinned = dr.Layout.Any(e => e != pos && e.Format == VtxFormat.k_16_16_16_16 && !e.Normalized);
        var col = skinned ? null : dr.Layout.FirstOrDefault(e => e.Format == VtxFormat.k_8_8_8_8 && e.Normalized && e != pos);
        dr.Positions = new Vector3[n];
        if (uv != null) dr.UVs = new Vector2[n];
        if (uv2 != null) dr.UVs2 = new Vector2[n];
        if (uv3 != null) dr.UVs3 = new Vector2[n];
        if (nrm != null) dr.Normals = new Vector3[n];
        if (tan != null) dr.Tangents = new Vector3[n];
        if (col != null) dr.Colors = new uint[n];
        for (int i = 0; i < n; i++)
        {
            int b = start + i * dr.Stride;
            if (b + dr.Stride > g.Length) break;
            var p = Fetch(g, b + pos.Offset, pos);
            dr.Positions[i] = new Vector3(p.X, p.Y, p.Z);
            if (uv != null) { var t = Fetch(g, b + uv.Offset, uv); dr.UVs![i] = new Vector2(t.X, t.Y); }
            if (nrm != null) { var t = Fetch(g, b + nrm.Offset, nrm); dr.Normals![i] = Vector3.Normalize(new Vector3(t.X, t.Y, t.Z) + new Vector3(1e-9f)); }
            if (uv2 != null) { var t = Fetch(g, b + uv2.Offset, uv2); dr.UVs2![i] = new Vector2(t.X, t.Y); }
            if (uv3 != null) { var t = Fetch(g, b + uv3.Offset, uv3); dr.UVs3![i] = new Vector2(t.X, t.Y); }
            if (tan != null) { var t = Fetch(g, b + tan.Offset, tan); dr.Tangents![i] = Vector3.Normalize(new Vector3(t.X, t.Y, t.Z) + new Vector3(1e-9f)); }
            if (col != null) dr.Colors![i] = BE.U32(g, b + col.Offset);   // raw big-endian word: memory bytes 0..3 = bits 31..0
        }
        DecodeSkin(dr, g, start, n, pos);
    }

    /// <summary>
    /// Skinned character vertices: an integer k_16_16_16_16 element holds 4 joint indices into the model's skeleton
    /// (global, no per-draw palette) and a normalized k_8_8_8_8 element holds the weights in reverse byte order
    /// (weight of index 0 in the last byte; the four bytes sum to 255). Verified on Banjo/Kazooie: feather vertices
    /// reference HAIRFETH_*/LF_FETH_* joints, weights of shared vertices add up to 255.
    /// </summary>
    static void DecodeSkin(MeshDraw dr, byte[] g, int start, int n, VertexElement pos)
    {
        var idx = dr.Layout.FirstOrDefault(e => e != pos && e.Format == VtxFormat.k_16_16_16_16 && !e.Normalized);
        var wgt = dr.Layout.FirstOrDefault(e => e.Format == VtxFormat.k_8_8_8_8 && e.Normalized);
        if (idx == null || wgt == null) return;
        var bi = new int[n * 4]; var bw = new float[n * 4];
        for (int i = 0; i < n; i++)
        {
            int b = start + i * dr.Stride;
            if (b + dr.Stride > g.Length) return;
            float sum = 0;
            for (int k = 0; k < 4; k++)
            {
                bi[4 * i + k] = (short)((g[b + idx.Offset + 2 * k] << 8) | g[b + idx.Offset + 2 * k + 1]);
                sum += bw[4 * i + k] = g[b + wgt.Offset + 3 - k];
            }
            if (sum <= 0) { bw[4 * i] = 1; continue; }
            for (int k = 0; k < 4; k++) bw[4 * i + k] /= sum;
        }
        dr.BlendIndices = bi; dr.BlendWeights = bw;
    }

    /// <summary>Reads one attribute as floats (components in memory order; big-endian).</summary>
    public static Vector4 Fetch(byte[] g, int o, VertexElement e)
    {
        switch (e.Format)
        {
            case VtxFormat.k_32_32_32_FLOAT: return new(BE.F32(g, o), BE.F32(g, o + 4), BE.F32(g, o + 8), 1);
            case VtxFormat.k_32_32_32_32_FLOAT: return new(BE.F32(g, o), BE.F32(g, o + 4), BE.F32(g, o + 8), BE.F32(g, o + 12));
            case VtxFormat.k_32_32_FLOAT: return new(BE.F32(g, o), BE.F32(g, o + 4), 0, 1);
            case VtxFormat.k_32_FLOAT: return new(BE.F32(g, o), 0, 0, 1);
            case VtxFormat.k_16_16_FLOAT: return new(Half(g, o), Half(g, o + 2), 0, 1);
            case VtxFormat.k_16_16_16_16_FLOAT: return new(Half(g, o), Half(g, o + 2), Half(g, o + 4), Half(g, o + 6));
            case VtxFormat.k_16_16:
            case VtxFormat.k_16_16_16_16:
            {
                int nc = e.Format == VtxFormat.k_16_16 ? 2 : 4;
                Span<float> c = stackalloc float[4] { 0, 0, 0, 1 };
                for (int k = 0; k < nc; k++)
                {
                    float v = e.Signed ? BE.S16(g, o + 2 * k) : BE.U16(g, o + 2 * k);
                    if (e.Normalized) v /= e.Signed ? 32767f : 65535f;
                    c[k] = v;
                }
                return new(c[0], c[1], c[2], c[3]);
            }
            case VtxFormat.k_2_10_10_10:
            {
                uint v = BE.U32(g, o);
                float X(int bits, int shift) { int x = (int)((v >> shift) & ((1u << bits) - 1)); if (e.Signed && x >= 1 << (bits - 1)) x -= 1 << bits; return e.Normalized ? x / (float)((1 << (bits - (e.Signed ? 1 : 0))) - 1) : x; }
                return new(X(10, 0), X(10, 10), X(10, 20), X(2, 30));
            }
            case VtxFormat.k_10_11_11:
            case VtxFormat.k_11_11_10:
            {
                uint v = BE.U32(g, o);
                int[] bits = e.Format == VtxFormat.k_10_11_11 ? new[] { 11, 11, 10 } : new[] { 10, 11, 11 };
                // Xenos: 10_11_11 = X:11 Y:11 Z:10 from the LSB; 11_11_10 = X:10 Y:11 Z:11
                var r = new float[3]; int sh = 0;
                for (int k = 0; k < 3; k++)
                {
                    int b = bits[k]; int x = (int)((v >> sh) & ((1u << b) - 1)); sh += b;
                    if (e.Signed && x >= 1 << (b - 1)) x -= 1 << b;
                    r[k] = e.Normalized ? x / (float)((1 << (b - (e.Signed ? 1 : 0))) - 1) : x;
                }
                return new(r[0], r[1], r[2], 1);
            }
            case VtxFormat.k_8_8_8_8:
            {
                uint v = BE.U32(g, o);
                float C(int sh) { int x = (int)((v >> sh) & 0xFF); if (e.Signed && x >= 128) x -= 256; return e.Normalized ? x / (e.Signed ? 127f : 255f) : x; }
                return new(C(0), C(8), C(16), C(24));
            }
            default: return new(0, 0, 0, 1);
        }
    }

    static float Half(byte[] g, int o) => (float)BitConverter.UInt16BitsToHalf(BE.U16(g, o));
}

/// <summary>Scanner for Xenos vfetch instructions inside shader microcode.</summary>
public static class VFetch
{
    static readonly HashSet<int> Formats = new() { 6, 7, 16, 17, 25, 26, 31, 32, 33, 34, 35, 36, 37, 38, 57 };

    public static List<VertexElement> Scan(byte[] code, int start, int maxLen, int strideDwords)
    {
        var byOffset = new SortedDictionary<int, VertexElement>();
        int end = Math.Min(code.Length - 12, start + maxLen);
        for (int o = start; o < end; o += 4)
        {
            uint w0 = BE.U32(code, o), w1 = BE.U32(code, o + 4), w2 = BE.U32(code, o + 8);
            if ((w0 & 0x1F) != 0 || ((w0 >> 19) & 1) == 0) continue;
            if ((w2 & 0xFF) != strideDwords) continue;
            int fmt = (int)((w1 >> 16) & 0x3F);
            if (!Formats.Contains(fmt)) continue;
            int off = (int)((w2 >> 8) & 0x7FFFFF);
            if (off >= strideDwords) continue;
            if (byOffset.ContainsKey(off * 4)) continue;
            byOffset[off * 4] = new VertexElement(off * 4, (VtxFormat)fmt, ((w1 >> 12) & 1) != 0, ((w1 >> 13) & 1) == 0, (int)((w0 >> 12) & 0x3F));
        }
        return byOffset.Values.ToList();
    }

    /// <summary>Fallback when no shader is known: float3 position.</summary>
    public static List<VertexElement> Guess(int stride) => new() { new VertexElement(0, VtxFormat.k_32_32_32_FLOAT, true, false, 0) };
}
