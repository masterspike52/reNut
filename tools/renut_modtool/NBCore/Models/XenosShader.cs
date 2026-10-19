using System.Globalization;
using System.Numerics;
using System.Text;
using NB.Core.IO;

namespace NB.Core.Models;

/// <summary>
/// A Xenos (Xbox 360 GPU) pixel shader as the game's models bind it: microcode in the bundle pool (stream op 0x02 is
/// patched with its address) preceded by its literal float table; the D3D shader object in the model's .data
/// (op 0x02 +4) carries a 0x102A1100 header whose literal list maps the table onto constant registers
/// (+0x34 list size, +0x38 entries {u16 0x100 + register, u16 float count, u32 byte offset}).
///
/// Microcode: control flow first (two 48-bit instructions per 3 dwords; exec = address, count, 2-bit-per-slot
/// fetch/ALU sequence), then 96-bit ALU / fetch instructions addressed from the start of the control-flow block.
/// Layout follows Xenia's ucode.h. <see cref="Code"/> is the straight-line path the game takes for scenery: the
/// point-light branches are skipped (cjmp taken), bool-conditional blocks with condition 1 run (the HDR tone-map path),
/// predicated blocks are kept.
/// </summary>
public sealed class XenosShader
{
    public sealed class Instr
    {
        public bool Fetch;
        // fetch
        public string FetchOp = ""; public int Src, Dst, Sampler, Dim; public string SrcSwz = "xyz", DstSwz = "xyzw";
        // ALU
        public int VOp, SOp, VDst, SDst, VMask, SMask; public bool Export, VClamp, SClamp, AbsConst, Predicated, PredCond;
        public (int Reg, bool Temp, int Swz, bool Neg)[] Srcs = Array.Empty<(int, bool, int, bool)>();
        public int Pc;
        public uint W0, W1, W2;
        public override string ToString() => Fetch ? $"{FetchOp} r{Dst}.{DstSwz}, r{Src}.{SrcSwz}, tf{Sampler}" : $"alu v{VOp} s{SOp} r{VDst}/{VMask:X} r{SDst}/{SMask:X}";
    }

    public readonly List<Instr> Code = new();
    public readonly Dictionary<int, Vector4> Literals = new();
    public bool HasLiteralTable;

    public static readonly string[] VectorOps = { "add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor", "mad", "cndeq", "cndge", "cndgt", "dp4", "dp3", "dp2add", "cube", "max4", "setp_eq_push", "setp_ne_push", "setp_gt_push", "setp_ge_push", "kill_eq", "kill_gt", "kill_ge", "kill_ne", "dst", "mova" };
    public static readonly string[] ScalarOps = { "adds", "adds_prev", "muls", "muls_prev", "muls_prev2", "maxs", "mins", "seqs", "sgts", "sges", "snes", "frcs", "truncs", "floors", "exp", "logc", "log", "rcpc", "rcpf", "rcp", "rsqc", "rsqf", "rsq", "movas", "movas_floor", "subs", "subs_prev", "setp_eq", "setp_ne", "setp_gt", "setp_ge", "setp_inv", "setp_pop", "setp_clr", "setp_rstr", "kills_eq", "kills_gt", "kills_ge", "kills_ne", "kills_one", "sqrt", "opc41", "mulsc0", "mulsc1", "addsc0", "addsc1", "subsc0", "subsc1", "sin", "cos", "retain_prev" };
    public static int VectorSources(int op) => op switch { 8 or 9 or 10 or 19 or 29 => 1, 11 or 12 or 13 or 14 or 17 => 3, _ => 2 };
    public string VName(Instr i) => i.VOp < VectorOps.Length ? VectorOps[i.VOp] : "v" + i.VOp;
    public static string SName(int op) => op < ScalarOps.Length ? ScalarOps[op] : "s" + op;

    static uint U(byte[] d, int o) => BE.U32(d, o);

    /// <summary>Reads the shader at <paramref name="offset"/> of a pool part; <paramref name="data"/>/<paramref name="obj"/>
    /// locate its D3D object (for the literal constants). Null when no consistent control flow is found.</summary>
    public static XenosShader? Load(byte[] pool, int offset, byte[]? data, int obj)
    {
        int code = FindCode(pool, offset);
        if (code < 0) return null;
        var sh = new XenosShader();
        if (data != null && obj >= 0)
        {
            int m = -1;
            for (int k = 0; k < 0x60 && obj + k + 4 <= data.Length; k += 4) if (U(data, obj + k) == 0x102A1100) { m = obj + k; break; }
            if (m >= 0 && m + 0x40 <= data.Length)
            {
                int size = BE.S32(data, m + 0x34);
                int n = Math.Clamp((size - 0xC) / 8, 0, 8);
                for (int e = 0; e < n; e++)
                {
                    uint w = U(data, m + 0x38 + 8 * e); int byteOff = BE.S32(data, m + 0x3C + 8 * e);
                    int reg = (int)(w >> 16) - 0x100, floats = (int)(w & 0xFFFF);
                    if (reg < 0 || reg > 255 || floats <= 0 || floats > 1024) continue;
                    for (int k = 0; k < floats / 4; k++)
                    {
                        int q = offset + byteOff + 16 * k;
                        if (q + 16 > pool.Length) break;
                        sh.Literals[reg + k] = new Vector4(BE.F32(pool, q), BE.F32(pool, q + 4), BE.F32(pool, q + 8), BE.F32(pool, q + 12));
                    }
                    sh.HasLiteralTable = true;
                }
            }
        }
        sh.Decode(pool, code);
        return sh.Code.Count > 0 ? sh : null;
    }

    readonly record struct Cf(int Op, int Addr, int Count, int Seq, int Cond);

    static (Cf A, Cf B) CfPair(byte[] d, int o)
    {
        uint w0 = U(d, o), w1 = U(d, o + 4), w2 = U(d, o + 8);
        static Cf One(uint lo, uint hi) => new((int)((hi >> 12) & 0xF), (int)(lo & 0xFFF), (int)((lo >> 12) & 7), (int)((lo >> 16) & 0xFFF), (int)((hi >> 10) & 1));
        return (One(w0, w1 & 0xFFFF), One((w1 >> 16) | (w2 << 16), w2 >> 16));
    }

    static bool IsExec(int op) => op is 1 or 2 or 3 or 4 or 5 or 6 or 13 or 14;
    static bool IsEnd(int op) => op is 2 or 4 or 6 or 14;

    static int FindCode(byte[] d, int start)
    {
        for (int o = start; o < Math.Min(d.Length - 24, start + 0x800); o += 4)
        {
            if (U(d, o) == 0 && U(d, o + 4) == 0 && U(d, o + 8) == 0) continue;
            var (a, _) = CfPair(d, o);
            if (!(a.Op is 1 or 2 or 3 or 5) || a.Addr < 1 || a.Addr > 300 || a.Count < 1) continue;
            bool end = false, ok = true;
            for (int k = 0; k < a.Addr; k++)
            {
                if (o + 12 * k + 12 > d.Length) { ok = false; break; }
                var (x, y) = CfPair(d, o + 12 * k);
                if (IsEnd(x.Op) || IsEnd(y.Op)) end = true;
            }
            if (ok && end) return o;
        }
        return -1;
    }

    void Decode(byte[] d, int b)
    {
        var cfs = new List<Cf>();
        var (first, _) = CfPair(d, b);
        for (int k = 0; k < first.Addr; k++) { var (x, y) = CfPair(d, b + 12 * k); cfs.Add(x); cfs.Add(y); }
        int i = 0, guard = 0;
        while (i < cfs.Count && guard++ < 600)
        {
            var c = cfs[i];
            if (c.Op == 11) { i = c.Addr; continue; }   // conditional jump (point lights off): taken
            bool run = c.Op is 1 or 2 or 5 or 6 || (c.Op is 3 or 4 or 13 or 14 && c.Cond == 1);
            if (IsExec(c.Op) && run)
                for (int k = 0; k < c.Count; k++)
                {
                    int o = b + 12 * (c.Addr + k);
                    if (o + 12 > d.Length) break;
                    var ins = ((c.Seq >> (2 * k)) & 1) != 0 ? DecodeFetch(d, o) : DecodeAlu(d, o);
                    ins.Pc = c.Addr + k;
                    if (c.Op is 5 or 6) { ins.Predicated = true; ins.PredCond = c.Cond == 1; }
                    Code.Add(ins);
                }
            if (IsEnd(c.Op)) break;
            i++;
        }
    }

    static Instr DecodeFetch(byte[] d, int o)
    {
        uint w0 = U(d, o), w1 = U(d, o + 4), w2 = U(d, o + 8);
        int op = (int)(w0 & 0x1F);
        int sswz = (int)((w0 >> 26) & 0x3F), dswz = (int)(w1 & 0xFFF);
        var ss = new string(Enumerable.Range(0, 3).Select(k => "xyzw"[(sswz >> (2 * k)) & 3]).ToArray());
        var ds = new string(Enumerable.Range(0, 4).Select(k => "xyzw01?_"[(dswz >> (3 * k)) & 7]).ToArray());
        return new Instr
        {
            W0 = w0, W1 = w1, W2 = w2,
            Fetch = true, FetchOp = op switch { 1 => "tfetch", 0 => "vfetch", _ => "fetch" + op },
            Src = (int)((w0 >> 5) & 0x3F), Dst = (int)((w0 >> 12) & 0x3F), Sampler = (int)((w0 >> 20) & 0x1F),
            SrcSwz = ss, DstSwz = ds, Dim = (int)((w2 >> 14) & 3),
        };
    }

    static Instr DecodeAlu(byte[] d, int o)
    {
        uint w0 = U(d, o), w1 = U(d, o + 4), w2 = U(d, o + 8);
        var ins = new Instr
        {
            VDst = (int)(w0 & 0x3F), AbsConst = ((w0 >> 7) & 1) != 0, SDst = (int)((w0 >> 8) & 0x3F), Export = ((w0 >> 15) & 1) != 0,
            VMask = (int)((w0 >> 16) & 0xF), SMask = (int)((w0 >> 20) & 0xF), VClamp = ((w0 >> 24) & 1) != 0, SClamp = ((w0 >> 25) & 1) != 0,
            SOp = (int)((w0 >> 26) & 0x3F), VOp = (int)((w2 >> 24) & 0x1F),
            Predicated = ((w1 >> 28) & 1) != 0, PredCond = ((w1 >> 27) & 1) != 0,
        };
        ins.Srcs = new[]
        {
            ((int)((w2 >> 16) & 0xFF), ((w2 >> 31) & 1) != 0, (int)((w1 >> 16) & 0xFF), ((w1 >> 26) & 1) != 0),
            ((int)((w2 >> 8) & 0xFF), ((w2 >> 30) & 1) != 0, (int)((w1 >> 8) & 0xFF), ((w1 >> 25) & 1) != 0),
            ((int)(w2 & 0xFF), ((w2 >> 29) & 1) != 0, (int)(w1 & 0xFF), ((w1 >> 24) & 1) != 0),
        };
        return ins;
    }

    /// <summary>A texture-coordinate interpolator written by the vertex shader: UV set (0 first float2 vertex element, 1 the
    /// next) and, when transformed, the rows (cu, cv, 0, c) with out = cu*u + cv*v + c.</summary>
    public sealed record UvSource(int Set, Vector4? RowX, Vector4? RowY, bool Swap = false);

    /// <summary>Key of an interpolator's second half (.zw) in <see cref="InterpolatorUvs"/>: the vertex shaders pack two
    /// coordinate sets into one interpolator (fabric grass: o1.xy = third set, o1.zw = fourth set swapped).</summary>
    public const int ZwKey = 0x100;

    /// <summary>
    /// Interpolators the vertex shader fills straight from a UV vertex element, copied (max r,r) or transformed by two dp4
    /// rows of vertex constants (Spiral Mountain's detail textures tile 4x). A vertex fetch of 16_16 data returns the second
    /// half in x (the compiler's "yx01" swizzle restores memory order, the order the viewer decodes).
    /// </summary>
    public static Dictionary<int, UvSource> InterpolatorUvs(XenosShader vs, IReadOnlyDictionary<int, Vector4> vsConsts, IReadOnlyList<VertexElement> layout)
    {
        var uvOffsets = layout.Where(e => e.Format is VtxFormat.k_16_16_FLOAT or VtxFormat.k_32_32_FLOAT).Select(e => e.Offset).OrderBy(x => x).ToList();
        // register -> (element offset, per component: 0 = u, 1 = v, 2 = constant 0, 3 = constant 1, -1 other)
        var src = new Dictionary<int, (int Off, int[] Map)>();
        var res = new Dictionary<int, UvSource>();
        var partial = new Dictionary<int, (int Set, Vector4?[] Rows)>();
        foreach (var i in vs.Code)
        {
            if (i.Fetch)
            {
                if (i.FetchOp != "vfetch") { src.Remove(i.Dst); continue; }
                int off = (int)((i.W2 >> 8) & 0x7FFFFF) * 4;
                int fmt = (int)((i.W1 >> 16) & 0x3F);
                int dsw = (int)(i.W1 & 0xFFF);
                var map = new int[4];
                for (int c = 0; c < 4; c++)
                {
                    int code = (dsw >> (3 * c)) & 7;
                    map[c] = code switch { 0 => fmt == 31 ? 1 : 0, 1 => fmt == 31 ? 0 : 1, 4 => 2, 5 => 3, _ => -1 };
                }
                if (uvOffsets.Contains(off)) src[i.Dst] = (off, map); else src.Remove(i.Dst);
                continue;
            }
            string vn = VectorOps[Math.Min(i.VOp, VectorOps.Length - 1)];
            if (!i.Export) { if (i.VMask != 0) src.Remove(i.VDst); if (i.SMask != 0) src.Remove(i.SDst); continue; }
            if (i.VDst >= 16 || i.VMask == 0) continue;
            int k = i.VDst;
            int Comp((int Reg, bool Temp, int Swz, bool Neg) s, int c) => src.TryGetValue(s.Reg & 0x3F, out var e) ? e.Map[Swz(s.Swz, c)] : -1;
            if (vn == "max" && i.Srcs[0].Temp && i.Srcs[0] == i.Srcs[1] && src.TryGetValue(i.Srcs[0].Reg & 0x3F, out var e0) && !i.Srcs[0].Neg)
            {
                bool ok = ((i.VMask & 1) == 0 || Comp(i.Srcs[0], 0) == 0) && ((i.VMask & 2) == 0 || Comp(i.Srcs[0], 1) == 1);
                if (ok && (i.VMask & 3) == 3) res[k] = new UvSource(uvOffsets.IndexOf(e0.Off), null, null);
                // the second half: a coordinate set copied to .zw, in order (u, v) or swapped (v, u)
                if ((i.VMask & 12) == 12)
                {
                    int cz = Comp(i.Srcs[0], 2), cw = Comp(i.Srcs[0], 3);
                    if (cz == 0 && cw == 1) res[k | ZwKey] = new UvSource(uvOffsets.IndexOf(e0.Off), null, null);
                    else if (cz == 1 && cw == 0) res[k | ZwKey] = new UvSource(uvOffsets.IndexOf(e0.Off), null, null, Swap: true);
                }
                continue;
            }
            if (vn == "dp4")
            {
                int t = i.Srcs[0].Temp ? 0 : i.Srcs[1].Temp ? 1 : -1;
                if (t < 0 || i.Srcs[1 - t].Temp || !src.TryGetValue(i.Srcs[t].Reg & 0x3F, out var e1) || !vsConsts.TryGetValue(i.Srcs[1 - t].Reg, out var row)) continue;
                var r = new Vector4(0);
                bool bad = false;
                for (int c = 0; c < 4; c++)
                {
                    float rc = row[Swz(i.Srcs[1 - t].Swz, c)] * (i.Srcs[1 - t].Neg ? -1 : 1) * (i.Srcs[t].Neg ? -1 : 1);
                    switch (Comp(i.Srcs[t], c)) { case 0: r.X += rc; break; case 1: r.Y += rc; break; case 3: r.W += rc; break; case 2: break; default: bad |= rc != 0; break; }
                }
                if (bad) continue;
                if ((i.VMask & 3) != 0)
                {
                    if (!partial.TryGetValue(k, out var pr)) partial[k] = pr = (uvOffsets.IndexOf(e1.Off), new Vector4?[2]);
                    if ((i.VMask & 1) != 0) pr.Rows[0] = r;
                    if ((i.VMask & 2) != 0) pr.Rows[1] = r;
                    if (pr.Rows[0] != null && pr.Rows[1] != null) res[k] = new UvSource(pr.Set, pr.Rows[0], pr.Rows[1]);
                }
                if ((i.VMask & 12) != 0)
                {
                    if (!partial.TryGetValue(k | ZwKey, out var pz)) partial[k | ZwKey] = pz = (uvOffsets.IndexOf(e1.Off), new Vector4?[2]);
                    if ((i.VMask & 4) != 0) pz.Rows[0] = r;
                    if ((i.VMask & 8) != 0) pz.Rows[1] = r;
                    if (pz.Rows[0] != null && pz.Rows[1] != null) res[k | ZwKey] = new UvSource(pz.Set, pz.Rows[0], pz.Rows[1]);
                }
            }
        }
        return res;
    }

    /// <summary>Component of a relative ALU swizzle (component i of the operand reads component <c>((swz &gt;&gt; 2i) + i) &amp; 3</c>).</summary>
    public static int Swz(int swz, int i) => ((swz >> (2 * i)) + i) & 3;

    public string Disassemble()
    {
        var sb = new StringBuilder();
        foreach (var i in Code)
        {
            if (i.Fetch) { sb.AppendLine($"{i.Pc,4} {i}"); continue; }
            string S(int k)
            {
                var (r, t, s, n) = i.Srcs[k];
                var sw = new string(Enumerable.Range(0, 4).Select(c => "xyzw"[Swz(s, c)]).ToArray());
                return (n ? "-" : "") + (t ? ((r & 0x80) != 0 ? $"|r{r & 0x3F}|" : $"r{r & 0x3F}") : $"c{r}") + (sw == "xyzw" ? "" : "." + sw);
            }
            var parts = new List<string>();
            string mk(int m) => new(Enumerable.Range(0, 4).Select(c => ((m >> c) & 1) != 0 ? "xyzw"[c] : '_').ToArray());
            if (i.VMask != 0 || VName(i).StartsWith("kill") || VName(i).StartsWith("setp"))
                parts.Add($"{VName(i)}{(i.VClamp ? "_sat" : "")} {(i.Export ? "o" : "r")}{i.VDst}.{mk(i.VMask)}, " + string.Join(", ", Enumerable.Range(0, VectorSources(i.VOp)).Select(S)));
            if (i.SMask != 0) parts.Add($"{SName(i.SOp)}{(i.SClamp ? "_sat" : "")} {(i.Export ? "o" : "r")}{i.SDst}.{mk(i.SMask)}, {S(2)}");
            sb.AppendLine($"{i.Pc,4} {(i.Predicated ? (i.PredCond ? "(p) " : "(!p) ") : "")}{string.Join(" ; ", parts)}");
        }
        return sb.ToString();
    }
}

/// <summary>
/// The colour composition of a model pixel shader translated to GLSL: the "tail" after the lighting code (the part
/// after the last instruction that reads the sun direction / colour, the point lights or the shadow maps) plus every
/// earlier instruction that only depends on material data (textures, material and literal constants, texture
/// coordinates, vertex colours). Values the lighting code left in registers are supplied by the viewer by kind:
/// N·L, sun colour × shadow, the Blinn specular term (pow(N·H, c0.x)), their products, the perturbed normal and the view
/// vector. Engine constants used by the tail: c35/c37/c39 ambient (dot(vec4(1, N.yzx), c)), c45 exposure, c52 eye,
/// c53/c54 camera right/up (sphere-mapped reflections), c81/c82 fill light colour/direction.
/// </summary>
public sealed class ShaderTranslation
{
    /// <summary>GLSL statements (registers r0..r31, predicate p, previous scalar ps; inputs gUV, gUV2, gCol, gN, gV, gNdotL,
    /// gSun, gSpec, gPos; uniforms uC[n] for <see cref="ConstRegs"/>, samplers tS&lt;k&gt;); writes oc (the exported colour) and
    /// pre (the colour before the tone map, when found).</summary>
    public string Body = "";
    public string? Fail;
    public readonly List<int> ConstRegs = new();
    /// <summary>Values of <see cref="ConstRegs"/> for this draw (literals of the shader, else the stream's constants).</summary>
    public readonly List<Vector4> ConstValues = new();
    public readonly SortedSet<int> Samplers = new();
    /// <summary>Samplers fetched with the second texture coordinate set.</summary>
    public readonly HashSet<int> SamplerUv2 = new();
    /// <summary>Normal map sampler used by the lighting code (-1 none).</summary>
    public int NormalSampler = -1;
    /// <summary>The normal map's texture coordinates: UV set and, when the vertex shader transforms them, the two rows.</summary>
    public int NormalSet;
    public (Vector4 X, Vector4 Y)? NormalRows;
    public bool NormalUv2;
    public bool UsesVertexColour, HasPreTone, Kills;
    public float SpecPower;
    public readonly HashSet<string> Engine = new();
    public string Key => Body.GetHashCode().ToString("X8") + "|" + Body.Length;
    public override string ToString() => Fail ?? $"{Body.Split('\n').Length} lines, samplers {string.Join(",", Samplers)}, consts {string.Join(",", ConstRegs)}";
}

public static class XenosTranslator
{
    // per-component taint bits
    const long TTex = 1, TMat = 2, TUv = 4, TCol = 8, TC51 = 16, TC32 = 32, TC52 = 64, TShadow = 128, TExp = 256, TC0 = 512,
        TRsq = 1024, TFrame = 2048, TEngine = 4096, TPos = 8192, TNormTex = 16384;
    const long MaterialMask = TTex | TMat | TUv | TCol;

    static readonly HashSet<int> LightRegs = new(new[] { 32, 51 }.Concat(Enumerable.Range(83, 18)));
    static readonly HashSet<int> ShadowSamplers = new() { 12, 13, 14, 15 };

    /// <summary>Translates the shader of a draw. <paramref name="material"/> = constant registers the stream set (their
    /// values come from the draw); <paramref name="hasColours"/> / <paramref name="hasUv2"/> describe the vertex data.</summary>
    public static ShaderTranslation Translate(XenosShader sh, IReadOnlyDictionary<int, Vector4> material, bool hasColours, bool hasUv2,
        IReadOnlyDictionary<int, XenosShader.UvSource>? uvs = null, bool hasUv3 = false)
    {
        var t = new ShaderTranslation();
        try { Run(sh, material, hasColours, hasUv2, t, uvs, hasUv3); }
        catch (Exception e) { t.Fail = "exception: " + e.Message; }
        return t;
    }

    static void Run(XenosShader sh, IReadOnlyDictionary<int, Vector4> material, bool hasColours, bool hasUv2, ShaderTranslation t,
        IReadOnlyDictionary<int, XenosShader.UvSource>? uvs, bool hasUv3 = false)
    {
        var code = sh.Code;
        bool IsLiteral(int r) => sh.Literals.ContainsKey(r);
        bool IsMat(int r) => IsLiteral(r) || material.ContainsKey(r);
        var c0 = material.TryGetValue(0, out var c0v) ? c0v : new Vector4(0, 0, 0, 1);

        // ---- 1. input classification: inputs used (through constants only) as texture coordinates are UVs
        var taintIn = new Dictionary<int, long[]>();   // register -> per component: bit (1<<inputIndex) of raw inputs
        long[] InTaint(int r) { if (!taintIn.TryGetValue(r, out var a)) taintIn[r] = a = new[] { 1L << Math.Min(r, 40), 1L << Math.Min(r, 40), 1L << Math.Min(r, 40), 1L << Math.Min(r, 40) }; return a; }
        var inputsWritten = new HashSet<int>();
        var uvInputs = new HashSet<int>(); var otherUse = new HashSet<int>();
        // which registers are still raw inputs (never written) at each read
        var written = new HashSet<int>();
        foreach (var i in code)
        {
            if (i.Fetch)
            {
                if (i.FetchOp == "tfetch" && !ShadowSamplers.Contains(i.Sampler) && !written.Contains(i.Src)) uvInputs.Add(i.Src);
                written.Add(i.Dst);
                continue;
            }
            int nv = XenosShader.VectorSources(i.VOp);
            for (int k = 0; k < 3; k++)
            {
                bool used = (k < nv && (i.VMask != 0 || i.VOp >= 20)) || (k == 2 && i.SMask != 0);
                var (r, temp, _, _) = i.Srcs[k];
                if (used && temp && !written.Contains(r & 0x3F)) otherUse.Add(r & 0x3F);
            }
            if (!i.Export) { if (i.VMask != 0) written.Add(i.VDst); if (i.SMask != 0) written.Add(i.SDst); }
        }
        // raw inputs read together with the eye position or the shadow matrices: the world position
        var posInputs = new HashSet<int>();
        {
            var w3 = new HashSet<int>();
            foreach (var i in code)
            {
                if (i.Fetch) { w3.Add(i.Dst); continue; }
                int nv3 = XenosShader.VectorSources(i.VOp);
                var usedSrcs = Enumerable.Range(0, 3).Where(k => (k < nv3 && (i.VMask != 0 || i.VOp >= 20)) || (k == 2 && i.SMask != 0)).Select(k => i.Srcs[k]).ToList();
                bool engineConst = usedSrcs.Any(x => !x.Temp && (x.Reg == 52 || x.Reg is >= 55 and <= 67 || x.Reg is 46 or 47));
                if (engineConst) foreach (var x in usedSrcs) if (x.Temp && !w3.Contains(x.Reg & 0x3F)) posInputs.Add(x.Reg & 0x3F);
                if (!i.Export) { if (i.VMask != 0) w3.Add(i.VDst); if (i.SMask != 0) w3.Add(i.SDst); }
            }
        }
        // ---- 2. taint analysis per register component
        var taint = new Dictionary<int, long[]>();
        long InputTaint(int r)
        {
            if (uvInputs.Contains(r)) return TUv;
            return hasColours ? TCol | TFrame : TFrame;   // decided later by use (vertex colour vs. normal / position)
        }
        long[] Get(int r)
        {
            if (!taint.TryGetValue(r, out var a)) { long v = InputTaint(r); taint[r] = a = new[] { v, v, v, v }; }
            return a;
        }
        long ConstTaint(int r) => IsMat(r) || r == 69 || r == 42 ? TMat :   // c69 overlay switch, c42 Jinjo colour: constants in the viewer (see Const)
             LightRegs.Contains(r) ? (r == 51 ? TC51 : r == 32 ? TC32 : TC51 | TC32) : r == 52 ? TC52 : r == 0 ? TMat : TEngine;
        long SrcTaint(XenosShader.Instr i, int k, int comps)
        {
            var (r, temp, swz, _) = i.Srcs[k];
            if (!temp) return ConstTaint(r) | (r == 0 ? TC0 : 0);
            var a = Get(r & 0x3F); long v = 0;
            for (int c = 0; c < comps; c++) v |= a[XenosShader.Swz(swz, c)];
            return v;
        }
        // tail start: after the last read of the sun / point lights or a shadow fetch
        int tail = 0;
        for (int n = 0; n < code.Count; n++)
        {
            var i = code[n];
            if (i.Fetch) { if (i.FetchOp == "tfetch" && ShadowSamplers.Contains(i.Sampler)) tail = n + 1; continue; }
            int nv = XenosShader.VectorSources(i.VOp);
            for (int k = 0; k < 3; k++)
            {
                bool used = (k < nv && (i.VMask != 0 || i.VOp >= 20)) || (k == 2 && i.SMask != 0);
                if (used && !i.Srcs[k].Temp && LightRegs.Contains(i.Srcs[k].Reg)) tail = n + 1;
            }
        }
        // the per-instruction taint (before) to decide what is translated before the tail
        var translate = new bool[code.Count];
        var lastWriter = new Dictionary<(int, int), int>();
        var normalSamplers = new HashSet<int>();
        for (int n = 0; n < code.Count; n++)
        {
            var i = code[n];
            if (i.Fetch)
            {
                if (i.FetchOp != "tfetch") { translate[n] = false; continue; }
                long ct = 0; var a = Get(i.Src);
                foreach (char ch in i.SrcSwz[..2]) ct |= a["xyzw".IndexOf(ch)];
                long v = ShadowSamplers.Contains(i.Sampler) ? TShadow : TTex | (ct & ~TUv & ~TMat);
                translate[n] = n >= tail || (v & ~MaterialMask) == 0 && (ct & ~(TUv | TMat)) == 0;
                var d = Get(i.Dst);
                for (int c = 0; c < 4; c++)
                {
                    char sc = i.DstSwz[c];
                    if (sc == '_') continue;
                    d[c] = sc is '0' or '1' ? 0 : v;
                    lastWriter[(i.Dst, c)] = n;
                }
                continue;
            }
            int nv = XenosShader.VectorSources(i.VOp);
            string vn = XenosShader.VectorOps[Math.Min(i.VOp, XenosShader.VectorOps.Length - 1)];
            long vt = 0;
            for (int k = 0; k < nv; k++) vt |= SrcTaint(i, k, vn is "dp3" ? 3 : vn is "dp2add" && k < 2 ? 2 : 4);
            string sn = XenosShader.SName(i.SOp);
            long st = SrcTaint(i, 2, 4);
            if (sn is "exp" or "log" or "logc") st |= TExp;
            if (sn is "rsq" or "rsqc" or "rsqf") st |= TRsq;
            // frame inputs feeding a dot product with a texture: that texture is the normal map
            if (vn is "dp3" or "dp4" && n < tail)
                for (int k = 0; k < 2; k++)
                {
                    var (r, temp, _, _) = i.Srcs[k];
                    if (temp && (Get(r & 0x3F)[0] & TTex) != 0 && (SrcTaint(i, 1 - k, 3) & TFrame) != 0)
                        foreach (var m in code.Take(n).Where(x => x.Fetch && x.Dst == (r & 0x3F) && x.FetchOp == "tfetch")) normalSamplers.Add(m.Sampler);
                }
            bool vecUsed = i.VMask != 0 && !i.Export;
            bool sclUsed = i.SMask != 0 && !i.Export;
            bool vecMat = (vt & ~MaterialMask) == 0, sclMat = (st & ~MaterialMask) == 0;
            translate[n] = n >= tail || ((!vecUsed || vecMat) && (!sclUsed || sclMat) && (i.VMask != 0 || i.SMask != 0) && !(vn.StartsWith("kill") || vn.StartsWith("setp")));
            if (vecUsed) { var d = Get(i.VDst); for (int c = 0; c < 4; c++) if (((i.VMask >> c) & 1) != 0) { d[c] = vn.StartsWith("dp") || vn == "max4" ? vt : vt; lastWriter[(i.VDst, c)] = n; } }
            if (sclUsed) { var d = Get(i.SDst); for (int c = 0; c < 4; c++) if (((i.SMask >> c) & 1) != 0) { d[c] = st; lastWriter[(i.SDst, c)] = n; } }
            if (n == tail - 1) { }
        }
        // the normal map: a texture whose value reaches the lighting through frame dot products
        t.NormalSampler = normalSamplers.Count > 0 ? normalSamplers.Min() : -1;
        if (t.NormalSampler >= 0)
        {
            var nf = code.FirstOrDefault(x => x.Fetch && x.FetchOp == "tfetch" && x.Sampler == t.NormalSampler);
            if (nf != null && uvs != null && uvs.TryGetValue(nf.Src, out var nus) && nus.Set is 0 or 1 && (nus.Set == 0 || hasUv2))
            {
                t.NormalSet = nus.Set;
                if (nus.RowX != null) t.NormalRows = (nus.RowX.Value, nus.RowY!.Value);
            }
        }

        float? specPower = null;
        for (int n = 0; n + 1 < tail && specPower == null; n++)
        {
            var i = code[n];
            if (i.Fetch || i.SMask == 0 || XenosShader.SName(i.SOp) is not ("log" or "logc")) continue;
            int lr = i.SDst;
            for (int m2 = n + 1; m2 < Math.Min(tail, n + 4); m2++)
            {
                var j = code[m2];
                if (j.Fetch || XenosShader.VectorOps[Math.Min(j.VOp, XenosShader.VectorOps.Length - 1)] != "mul" || j.VMask == 0) continue;
                for (int k = 0; k < 2; k++)
                    if (j.Srcs[k].Temp && (j.Srcs[k].Reg & 0x3F) == lr && !j.Srcs[1 - k].Temp)
                    {
                        int cr = j.Srcs[1 - k].Reg; int comp = XenosShader.Swz(j.Srcs[1 - k].Swz, 0);
                        if (sh.Literals.TryGetValue(cr, out var lv)) specPower = lv[comp];
                        else if (material.TryGetValue(cr, out var mv)) specPower = mv[comp];
                    }
                if (specPower != null) break;
            }
        }
        // ---- 3. the incoming (lighting) register values at the tail start, re-run the taint up to the tail
        taint.Clear();
        var lwTail = new Dictionary<(int, int), int>();
        for (int n = 0; n < tail; n++)
        {
            var i = code[n];
            if (i.Fetch)
            {
                if (i.FetchOp != "tfetch") continue;
                for (int c = 0; c < 4; c++) if (i.DstSwz[c] != '_') lwTail[(i.Dst, c)] = n;
                long ct = 0; var a = Get(i.Src);
                foreach (char ch in i.SrcSwz[..2]) ct |= a["xyzw".IndexOf(ch)];
                long v = ShadowSamplers.Contains(i.Sampler) ? TShadow : TTex | (normalSamplers.Contains(i.Sampler) ? TNormTex : 0) | (ct & ~TUv & ~TMat);
                var d = Get(i.Dst);
                for (int c = 0; c < 4; c++) { char sc = i.DstSwz[c]; if (sc == '_') continue; d[c] = sc is '0' or '1' ? 0 : v; }
                continue;
            }
            int nv = XenosShader.VectorSources(i.VOp);
            string vn = XenosShader.VectorOps[Math.Min(i.VOp, XenosShader.VectorOps.Length - 1)];
            long vt = 0;
            for (int k = 0; k < nv; k++) vt |= SrcTaint(i, k, vn is "dp3" ? 3 : 4);
            string sn = XenosShader.SName(i.SOp);
            long st = SrcTaint(i, 2, 4);
            if (sn is "exp" or "log" or "logc") st |= TExp;
            if (sn is "rsq" or "rsqc" or "rsqf") st |= TRsq;
            if (i.VMask != 0 && !i.Export) { var d = Get(i.VDst); for (int c = 0; c < 4; c++) if (((i.VMask >> c) & 1) != 0) { d[c] = vt; lwTail[(i.VDst, c)] = n; } }
            if (i.SMask != 0 && !i.Export) { var d = Get(i.SDst); for (int c = 0; c < 4; c++) if (((i.SMask >> c) & 1) != 0) { d[c] = st; lwTail[(i.SDst, c)] = n; } }
        }
        // numeric values known at the tail start (material constants, literals; shadow maps read as lit; 0 * x = 0)
        var known = new Dictionary<(int, int), float>();
        {
            float? K(int r, bool temp, int comp)
            {
                if (!temp) return sh.Literals.TryGetValue(r, out var lv) ? lv[comp] : material.TryGetValue(r, out var mv) ? mv[comp] : r == 0 ? (comp == 3 ? 1 : 0) : null;
                return known.TryGetValue((r & 0x3F, comp), out var v) ? v : null;
            }
            for (int n = 0; n < tail; n++)
            {
                var i = code[n];
                if (i.Fetch)
                {
                    for (int c = 0; c < 4; c++)
                    {
                        char sc = i.DstSwz[c]; if (sc == '_') continue;
                        if (sc == '0') known[(i.Dst, c)] = 0; else if (sc == '1') known[(i.Dst, c)] = 1;
                        else if (ShadowSamplers.Contains(i.Sampler)) known[(i.Dst, c)] = 1; else known.Remove((i.Dst, c));
                    }
                    continue;
                }
                string vn = XenosShader.VectorOps[Math.Min(i.VOp, XenosShader.VectorOps.Length - 1)];
                float? S(int k, int c)
                {
                    var (r, temp, swz, neg) = i.Srcs[k];
                    var v = K(r, temp, XenosShader.Swz(swz, c));
                    if (v == null) return null;
                    float f = v.Value; if (temp && (r & 0x80) != 0 || !temp && i.AbsConst) f = MathF.Abs(f);
                    return neg ? -f : f;
                }
                static float? Mul(float? a, float? b) => a == 0 || b == 0 ? 0 : a * b;
                var res = new float?[4];
                for (int c = 0; c < 4; c++)
                {
                    res[c] = vn switch
                    {
                        "add" => S(0, c) + S(1, c),
                        "mul" => Mul(S(0, c), S(1, c)),
                        "mad" => Mul(S(0, c), S(1, c)) + S(2, c),
                        "max" => S(0, c) is { } a && S(1, c) is { } b ? MathF.Max(a, b) : null,
                        "min" => S(0, c) is { } a2 && S(1, c) is { } b2 ? MathF.Min(a2, b2) : null,
                        _ => null,
                    };
                    if (res[c] is { } rv && i.VClamp) res[c] = Math.Clamp(rv, 0, 1);
                }
                if (i.VMask != 0 && !i.Export) for (int c = 0; c < 4; c++) if (((i.VMask >> c) & 1) != 0) { if (res[c] is { } v) known[(i.VDst, c)] = v; else known.Remove((i.VDst, c)); }
                if (i.SMask != 0 && !i.Export)
                {
                    string sn = XenosShader.SName(i.SOp);
                    float? a = S(2, 0), b = S(2, 3);
                    float? sv = sn switch { "maxs" => a is { } x && b is { } y ? MathF.Max(x, y) : null, "mins" => a is { } x2 && b is { } y2 ? MathF.Min(x2, y2) : null, "muls" => Mul(a, b), "adds" => a + b, _ => null };
                    for (int c = 0; c < 4; c++) if (((i.SMask >> c) & 1) != 0) { if (sv is { } v) known[(i.SDst, c)] = v; else known.Remove((i.SDst, c)); }
                }
            }
        }
        string? Tag(long v, int reg, bool raw)
        {
            if ((v & ~MaterialMask) == 0) return null;   // material: translated
            if ((v & TExp) != 0 && (v & TC0) != 0) return (v & (TC32 | TShadow)) != 0 ? "gSpec" : "gSpecPow";
            if ((v & TC51) != 0) return (v & (TC32 | TShadow)) != 0 ? "gDiff" : "gNdotL";
            if ((v & (TC32 | TShadow)) != 0) return "gSun";
            if ((v & TC52) != 0) return "gV";
            if ((v & TUv) != 0 && (v & TFrame) == 0) return "gUVp";   // parallax-offset coordinates (a normal also carries the frame)   // parallax-offset texture coordinates: the plain UV (no offset)
            if ((v & TFrame) != 0 && (v & TEngine) == 0) return raw ? "gIn" : "gN";
            return "?";
        }
        // registers still holding raw inputs at the tail start
        var rawAtTail = new HashSet<int>();
        {
            var w2 = new HashSet<int>();
            for (int n = 0; n < tail; n++) { var i = code[n]; if (i.Fetch) w2.Add(i.Dst); else if (!i.Export) { if (i.VMask != 0) w2.Add(i.VDst); if (i.SMask != 0) w2.Add(i.SDst); } }
            foreach (var r in taint.Keys) if (!w2.Contains(r)) rawAtTail.Add(r);
        }

        // ---- 4. emit GLSL
        var sb = new StringBuilder();
        var constIdx = new Dictionary<int, int>();
        var extraConst = new Dictionary<int, Vector4>();
        int AddConst(int key, Vector4 v)
        {
            if (!constIdx.TryGetValue(key, out int k)) { k = constIdx[key] = t.ConstRegs.Count; t.ConstRegs.Add(key); extraConst[key] = v; }
            return k;
        }
        string Const(int r)
        {
            if (sh.Literals.TryGetValue(r, out var lv) || material.ContainsKey(r) || r == 0)
            {
                if (!constIdx.TryGetValue(r, out int k)) { k = constIdx[r] = t.ConstRegs.Count; t.ConstRegs.Add(r); }
                return $"uC[{k}]";
            }
            switch (r)
            {
                case 35: t.Engine.Add("sh"); return "uSH0";
                case 37: t.Engine.Add("sh"); return "uSH1";
                case 39: t.Engine.Add("sh"); return "uSH2";
                case 36: case 38: case 40: t.Engine.Add("sh"); return "vec4(0.0)";
                case 45: t.Engine.Add("expo"); return "uExpo";
                case 52: return "vec4(uEye, 1.0)";
                case 53: t.Engine.Add("cam"); return "uCamR";
                case 54: t.Engine.Add("cam"); return "uCamU";
                case 81: t.Engine.Add("fill"); return "uFillCol";
                case 82: t.Engine.Add("fill"); return "uFillDir";
                case 28: t.Engine.Add("vcolscale"); return "uVcolScale";
                // c69: the engine's surface overlay of characters (x/y blend an engine colour texture tf11 over the
                // colour, z/w an engine normal texture tf10 over the normal; every use is predicated on c69.x > 0). Off
                // in normal play: 0 keeps those branches out (Boggy, Bottles, Mumbo … failed to translate on it).
                case 69: return "vec4(0.0)";
                // c42: the object's colour for "colour change" materials (Jinjos, King Jingaling, component crates): hue
                // offset, saturation, lightness of an HSL colour applied to the texture's hue mask; set per object by the
                // game. The viewer uses no hue offset at medium saturation / lightness (the texture's own hue).
                case 42: return "vec4(0.0, 0.75, 0.5, 1.0)";
            }
            throw new NotSupportedException($"engine constant c{r}");
        }
        string Input(int r)
        {
            // raw interpolators read by translated code
            string? Half(int key, int slot)
            {
                // one coordinate pair of the interpolator as the vertex shader writes it (copy, swapped copy or two rows)
                if (uvs == null || !uvs.TryGetValue(key, out var us)) return null;
                if (!(us.Set == 0 || us.Set == 1 && hasUv2 || us.Set == 2 && hasUv3)) return null;
                string g = us.Set switch { 1 => "gUV2", 2 => "gUV3", _ => "gUV" };
                if (us.RowX == null) return us.Swap ? $"{g}.yx" : g;
                int kx = AddConst(1000 + 2 * r + slot * 100, us.RowX.Value), ky = AddConst(1001 + 2 * r + slot * 100, us.RowY!.Value);
                return $"vec2(dot(vec4({g}, 0.0, 1.0), uC[{kx}]), dot(vec4({g}, 0.0, 1.0), uC[{ky}]))";
            }
            var hx = Half(r, 0); var hz = Half(r | XenosShader.ZwKey, 1);
            if (hx != null || hz != null)
            {
                string xy = hx ?? (uvInputs.Contains(r) ? ((hasUv2 && UvSet(r) == 1) ? "gUV2" : "gUV") : "vec2(0.0)");
                return $"vec4({xy}, {hz ?? "0.0, 1.0"})";
            }
            if (uvInputs.Contains(r)) return (hasUv2 && UvSet(r) == 1) ? "vec4(gUV2, 0.0, 1.0)" : "vec4(gUV, 0.0, 1.0)";
            if (posInputs.Contains(r)) return "vec4(gPos, 1.0)";
            if (hasColours) { t.UsesVertexColour = true; return "gCol"; }
            return "vec4(1.0)";
        }
        // the second texture coordinate set: the third and later coordinate inputs, or the second one when the shader
        // has three (the blend-mask of a two-layer material, which the previous viewer verified on UV2)
        int UvSet(int r)
        {
            var ordered = uvInputs.OrderBy(x => x).ToList();
            int idx = ordered.IndexOf(r);
            return idx <= 0 ? 0 : idx >= 2 ? 1 : (ordered.Count >= 3 ? 0 : 1);
        }
        var declared = new HashSet<int>();
        string Reg(int r)
        {
            r &= 0x3F;
            if (!declared.Contains(r))
            {
                declared.Add(r);
                return $"r{r}";
            }
            return $"r{r}";
        }
        var initialised = new HashSet<int>();
        void EnsureInit(int r, int n)
        {
            // a register read by translated code before any translated write: raw input or engine value
            if (initialised.Contains(r)) return;
            initialised.Add(r);
            if (rawAtTail.Contains(r) || n < tail && !code.Take(n).Any(x => x.Fetch ? x.Dst == r : !x.Export && (x.VMask != 0 && x.VDst == r || x.SMask != 0 && x.SDst == r)))
            {
                sb.Append($"  r{r} = {Input(r)};\n");
                return;
            }
        }
        string Src(XenosShader.Instr i, int k, int n)
        {
            var (r, temp, swz, neg) = i.Srcs[k];
            string b;
            if (temp)
            {
                int rr = r & 0x3F;
                EnsureInit(rr, n);
                b = $"r{rr}";
                if ((r & 0x80) != 0) b = $"abs({b})";
            }
            else
            {
                b = Const(r);
                if (i.AbsConst) b = $"abs({b})";
            }
            var sw = new string(Enumerable.Range(0, 4).Select(c => "xyzw"[XenosShader.Swz(swz, c)]).ToArray());
            if (sw != "xyzw") b = $"{b}.{sw}";
            return neg ? $"(-{b})" : b;
        }
        string Comp(XenosShader.Instr i, int k, int which, int n)
        {
            var (r, temp, swz, neg) = i.Srcs[k];
            string b;
            if (temp) { int rr = r & 0x3F; EnsureInit(rr, n); b = $"r{rr}"; if ((r & 0x80) != 0) b = $"abs({b})"; }
            else { b = Const(r); if (i.AbsConst) b = $"abs({b})"; }
            b = $"{b}.{"xyzw"[XenosShader.Swz(swz, which)]}";
            return neg ? $"(-{b})" : b;
        }
        static string Mask(int m) => new(Enumerable.Range(0, 4).Where(c => ((m >> c) & 1) != 0).Select(c => "xyzw"[c]).ToArray());
        static string Narrow(string vec4expr, string mask) => mask.Length == 4 ? vec4expr : $"({vec4expr}).{mask}";

        // engine values the tail reads, initialised at the tail start (per component)
        // register components the tail reads before writing them
        var tailReads = new HashSet<(int, int)>();
        var firstRead = new Dictionary<(int, int), int>(); int curPc = 0;
        {
            var tw = new HashSet<(int, int)>();
            void Read(int r, int c) { if (!tw.Contains((r, c)) && tailReads.Add((r, c))) firstRead[(r, c)] = curPc; }
            for (int n = tail; n < code.Count; n++)
            {
                var i = code[n]; curPc = i.Pc;
                if (i.Fetch)
                {
                    if (i.FetchOp == "tfetch") foreach (char ch in i.SrcSwz[..(i.Dim == 3 ? 3 : 2)]) Read(i.Src, "xyzw".IndexOf(ch));
                    for (int c = 0; c < 4; c++) if (i.DstSwz[c] != '_' && !i.Predicated) tw.Add((i.Dst, c));
                    continue;
                }
                int nv = XenosShader.VectorSources(i.VOp);
                string vname = XenosShader.VectorOps[Math.Min(i.VOp, XenosShader.VectorOps.Length - 1)];
                for (int k = 0; k < 3; k++)
                {
                    var (r, temp, swz, _) = i.Srcs[k];
                    if (!temp) continue;
                    if (k < nv && (i.VMask != 0 || i.VOp >= 20))
                        for (int c = 0; c < 4; c++)
                        {
                            bool need = vname switch
                            {
                                "dp4" or "max4" or "cube" => true,
                                "dp3" => c < 3,
                                "dp2add" => k < 2 ? c < 2 : c == 0,
                                _ when vname.StartsWith("kill") || vname.StartsWith("setp") => true,
                                _ => ((i.VMask >> c) & 1) != 0,
                            };
                            if (need) Read(r & 0x3F, XenosShader.Swz(swz, c));
                        }
                    if (k == 2 && i.SMask != 0)
                    {
                        Read(r & 0x3F, XenosShader.Swz(swz, 0));
                        string sname = XenosShader.SName(i.SOp);
                        if (sname is "adds" or "muls" or "maxs" or "mins" or "subs") Read(r & 0x3F, XenosShader.Swz(swz, 3));
                    }
                }
                if (!i.Export && !i.Predicated)
                {
                    for (int c = 0; c < 4; c++) if (((i.VMask >> c) & 1) != 0) tw.Add((i.VDst, c));
                    for (int c = 0; c < 4; c++) if (((i.SMask >> c) & 1) != 0) tw.Add((i.SDst, c));
                }
            }
        }

        string? pendingPre = null;
        for (int n = 0; n < code.Count; n++)
        {
            if (n == tail)
            {
                // supply the lighting values the tail reads
                foreach (var g in tailReads.GroupBy(x => x.Item1))
                {
                    int r = g.Key;
                    if (!taint.TryGetValue(r, out var tv)) tv = new long[4];   // never touched before the tail: raw interpolator
                    var byTag = new Dictionary<string, List<int>>();
                    foreach (var (_, c) in g)
                    {
                        if (lwTail.TryGetValue((r, c), out int lw) && translate[lw]) continue;   // translated material value
                        string? tag;
                        if (lwTail.ContainsKey((r, c)) && known.TryGetValue((r, c), out var kv))
                        {
                            sb.Append($"  r{r}.{"xyzw"[c]} = {F(kv)};\n");
                            initialised.Add(r);
                            continue;
                        }
                        if (!lwTail.ContainsKey((r, c))) tag = posInputs.Contains(r) ? "gIn" : "IN";   // still the raw interpolator
                        else tag = Tag(tv[c], r, false);
                        if (tag == null) continue;
                        if (tag == "?") throw new NotSupportedException($"unknown lighting value in r{r}.{"xyzw"[c]} (taint {tv[c]:X}) read at pc {firstRead.GetValueOrDefault((r, c))}");
                        if (!byTag.TryGetValue(tag, out var l)) byTag[tag] = l = new();
                        l.Add(c);
                    }
                    foreach (var (tag, comps) in byTag)
                    {
                        string m = new(comps.OrderBy(x => x).Select(c => "xyzw"[c]).ToArray());
                        string val = tag switch
                        {
                            "gSpec" => "vec4(gSpec, 1.0)", "gSpecPow" => "vec4(gSpecPow)", "gDiff" => "vec4(gDiff, 1.0)", "gNdotL" => "vec4(gNdotL)",
                            "gSun" => "vec4(gSun, 1.0)", "gV" => "vec4(gV, 1.0)", "gN" => "vec4(gN, 1.0)", "gIn" => "vec4(gPos, 1.0)", "gUVp" => "vec4(gUV, 0.0, 1.0)",
                            _ => Input(r),
                        };
                        if (tag is "gSpec" or "gSpecPow") t.SpecPower = specPower ?? c0.X;
                        sb.Append($"  r{r}.{m} = {Narrow(val, m)};\n");
                        initialised.Add(r);
                    }
                }
            }
            var i = code[n];
            if (!translate[n]) continue;
            string pred = i.Predicated ? (i.PredCond ? "if (p) " : "if (!p) ") : "";
            if (i.Fetch)
            {
                if (i.FetchOp != "tfetch") throw new NotSupportedException(i.FetchOp);
                if (i.Sampler is 10 or 11)
                {
                    // engine overlay textures (see c69): only read under c69.x > 0, which the viewer keeps off
                    EnsureInit(i.Src, n);
                    var z = new StringBuilder();
                    for (int c = 0; c < 4; c++) if (i.DstSwz[c] != '_') z.Append($" r{i.Dst}.{"xyzw"[c]} = 0.0;");
                    initialised.Add(i.Dst);
                    sb.Append($"  {pred}{{{z} }}\n");
                    continue;
                }
                if (i.Sampler >= 8) throw new NotSupportedException($"sampler tf{i.Sampler}");
                bool cubeFetch = i.Dim == 3;
                t.Samplers.Add(i.Sampler);
                EnsureInit(i.Src, n);
                string coord = $"r{i.Src}.{i.SrcSwz[..2]}";
                if (uvInputs.Contains(i.Src) && !code.Take(n).Any(x => x.Fetch ? x.Dst == i.Src : !x.Export && (x.VMask != 0 && x.VDst == i.Src || x.SMask != 0 && x.SDst == i.Src)) && hasUv2 && UvSet(i.Src) == 1)
                    t.SamplerUv2.Add(i.Sampler);
                // cube maps: the decoded face is a picture of the surroundings, not a cube; a heavily blurred level gives their
                // average colour, which is what the game's glossy floors and metals mostly show
                string tex = cubeFetch ? $"(textureLod(tS{i.Sampler}, CubeUV(cubeDir), 6.0) * 0.5)" : $"texture(tS{i.Sampler}, {coord})";
                var dst = new StringBuilder();
                for (int c = 0; c < 4; c++)
                {
                    char sc = i.DstSwz[c];
                    if (sc == '_') continue;
                    string v = sc == '0' ? "0.0" : sc == '1' ? "1.0" : sc == '?' ? "0.0" : $"tv.{sc}";
                    dst.Append($" r{i.Dst}.{"xyzw"[c]} = {v};");
                }
                initialised.Add(i.Dst);
                sb.Append($"  {pred}{{ vec4 tv = {tex};{dst} }}\n");
                continue;
            }
            string vn = XenosShader.VectorOps[Math.Min(i.VOp, XenosShader.VectorOps.Length - 1)];
            string sn = XenosShader.SName(i.SOp);
            var stmt = new StringBuilder();
            string? vexpr = null, sexpr = null;
            bool vecActive = i.VMask != 0 || vn.StartsWith("kill") || vn.StartsWith("setp");
            if (vecActive)
            {
                string A() => Src(i, 0, n);
                string B() => Src(i, 1, n);
                string C() => Src(i, 2, n);
                vexpr = vn switch
                {
                    "add" => $"{A()} + {B()}",
                    "mul" => $"{A()} * {B()}",
                    "max" => $"max({A()}, {B()})",
                    "min" => $"min({A()}, {B()})",
                    "seq" => $"vec4(equal({A()}, {B()}))",
                    "sgt" => $"vec4(greaterThan({A()}, {B()}))",
                    "sge" => $"vec4(greaterThanEqual({A()}, {B()}))",
                    "sne" => $"vec4(notEqual({A()}, {B()}))",
                    "frc" => $"fract({A()})",
                    "trunc" => $"trunc({A()})",
                    "floor" => $"floor({A()})",
                    "mad" => $"{A()} * {B()} + {C()}",
                    "cndeq" => $"mix({C()}, {B()}, vec4(equal({A()}, vec4(0.0))))",
                    "cndge" => $"mix({C()}, {B()}, vec4(greaterThanEqual({A()}, vec4(0.0))))",
                    "cndgt" => $"mix({C()}, {B()}, vec4(greaterThan({A()}, vec4(0.0))))",
                    "dp4" => $"vec4(dot({A()}, {B()}))",
                    "dp3" => $"vec4(dot(({A()}).xyz, ({B()}).xyz))",
                    "dp2add" => $"vec4(dot(({A()}).xy, ({B()}).xy) + ({C()}).x)",
                    "max4" => $"vec4(max(max(({A()}).x, ({A()}).y), max(({A()}).z, ({A()}).w)))",
                    // cube-map face selection: keep the direction for the following cube fetch (the game's cube maps
                    // decode as one face here, sampled like a sphere map around the view)
                    "cube" => $"vec4(cubeDir = ({A()}).zwx, 0.0)",
                    "kill_eq" => $"KILL(any(equal({A()}, {B()})))",
                    "kill_gt" => $"KILL(any(greaterThan({A()}, {B()})))",
                    "kill_ge" => $"KILL(any(greaterThanEqual({A()}, {B()})))",
                    "kill_ne" => $"KILL(any(notEqual({A()}, {B()})))",
                    _ => throw new NotSupportedException("vector op " + vn),
                };
                if (vn.StartsWith("kill")) { t.Kills = true; stmt.Append($"  {pred}if ({vexpr[5..^1]}) discard;\n"); vexpr = null; }
                else if (i.VClamp) vexpr = $"clamp({vexpr}, 0.0, 1.0)";
            }
            if (i.SMask != 0 || sn.StartsWith("kills") || sn.StartsWith("setp"))
            {
                string a = Comp(i, 2, 0, n), b = Comp(i, 2, 3, n);
                switch (sn)
                {
                    case "adds": sexpr = $"{a} + {b}"; break;
                    case "adds_prev": sexpr = $"{a} + ps"; break;
                    case "muls": sexpr = $"{a} * {b}"; break;
                    case "muls_prev": sexpr = $"{a} * ps"; break;
                    case "maxs": sexpr = $"max({a}, {b})"; break;
                    case "mins": sexpr = $"min({a}, {b})"; break;
                    case "seqs": sexpr = $"({a} == 0.0 ? 1.0 : 0.0)"; break;
                    case "sgts": sexpr = $"({a} > 0.0 ? 1.0 : 0.0)"; break;
                    case "sges": sexpr = $"({a} >= 0.0 ? 1.0 : 0.0)"; break;
                    case "snes": sexpr = $"({a} != 0.0 ? 1.0 : 0.0)"; break;
                    case "frcs": sexpr = $"fract({a})"; break;
                    case "truncs": sexpr = $"trunc({a})"; break;
                    case "floors": sexpr = $"floor({a})"; break;
                    case "exp": sexpr = $"exp2({a})"; break;
                    case "logc": case "log": sexpr = $"log2(max({a}, 1e-30))"; break;
                    case "rcpc": case "rcpf": case "rcp": sexpr = $"(1.0 / ({a} == 0.0 ? 1e-30 : {a}))"; break;
                    case "rsqc": case "rsqf": case "rsq": sexpr = $"inversesqrt(max({a}, 1e-30))"; break;
                    case "subs": sexpr = $"{a} - {b}"; break;
                    case "subs_prev": sexpr = $"{a} - ps"; break;
                    case "sqrt": sexpr = $"sqrt(max({a}, 0.0))"; break;
                    case "sin": sexpr = $"sin({a})"; break;
                    case "cos": sexpr = $"cos({a})"; break;
                    case "retain_prev": sexpr = "ps"; break;
                    case "setp_eq": stmt.Append($"  {pred}{{ p = {a} == 0.0; }}\n"); sexpr = $"(p ? 0.0 : 1.0)"; break;
                    case "setp_ne": stmt.Append($"  {pred}{{ p = {a} != 0.0; }}\n"); sexpr = $"(p ? 0.0 : 1.0)"; break;
                    case "setp_gt": stmt.Append($"  {pred}{{ p = {a} > 0.0; }}\n"); sexpr = $"(p ? 0.0 : 1.0)"; break;
                    case "setp_ge": stmt.Append($"  {pred}{{ p = {a} >= 0.0; }}\n"); sexpr = $"(p ? 0.0 : 1.0)"; break;
                    case "setp_inv": stmt.Append($"  {pred}{{ p = {a} == 1.0; }}\n"); sexpr = $"(p ? 0.0 : ({a} == 0.0 ? 1.0 : {a}))"; break;
                    case "kills_eq": t.Kills = true; stmt.Append($"  {pred}if ({a} == 0.0) discard;\n"); sexpr = "0.0"; break;
                    case "kills_gt": t.Kills = true; stmt.Append($"  {pred}if ({a} > 0.0) discard;\n"); sexpr = "0.0"; break;
                    case "kills_ge": t.Kills = true; stmt.Append($"  {pred}if ({a} >= 0.0) discard;\n"); sexpr = "0.0"; break;
                    case "kills_ne": t.Kills = true; stmt.Append($"  {pred}if ({a} != 0.0) discard;\n"); sexpr = "0.0"; break;
                    case "kills_one": t.Kills = true; stmt.Append($"  {pred}if ({a} == 1.0) discard;\n"); sexpr = "0.0"; break;
                    default: throw new NotSupportedException("scalar op " + sn);
                }
                if (sexpr != null && i.SClamp) sexpr = $"clamp({sexpr}, 0.0, 1.0)";
            }
            // the colour before the tone map: the register the exposure-scaled "x * c45 + 1" denominator is built from
            if (pendingPre == null && vn == "mad" && i.VMask != 0 && !i.Export && i.Srcs[0].Temp && !i.Srcs[1].Temp && i.Srcs[1].Reg == 45 && !i.Srcs[2].Temp)
            {
                pendingPre = $"r{i.Srcs[0].Reg & 0x3F}";
                stmt.Append($"  pre = {pendingPre};\n");
                t.HasPreTone = true;
            }
            // evaluate both, then write (sources are read before the destinations change)
            if (vexpr != null) stmt.Append($"  vec4 v_{n} = {vexpr};\n");
            if (sexpr != null) stmt.Append($"  float s_{n} = {sexpr}; ps = s_{n};\n");
            if (vexpr != null && i.VMask != 0)
            {
                string m = Mask(i.VMask);
                string dst = i.Export ? (i.VDst == 0 ? "oc" : null!) : $"r{i.VDst}";
                if (dst != null) { stmt.Append($"  {pred}{dst}.{m} = v_{n}.{m};\n"); if (!i.Export) initialised.Add(i.VDst); }
            }
            if (sexpr != null && i.SMask != 0)
            {
                string m = Mask(i.SMask);
                string dst = i.Export ? (i.SDst == 0 ? "oc" : null!) : $"r{i.SDst}";
                if (dst != null) { stmt.Append($"  {pred}{dst}.{m} = vec4(s_{n}).{m};\n"); if (!i.Export) initialised.Add(i.SDst); }
            }
            sb.Append(stmt);
        }
        if (code.All(x => x.Fetch || !x.Export || x.VDst != 0)) throw new NotSupportedException("no colour export");
        t.Body = sb.ToString();
        foreach (var r in t.ConstRegs)
            t.ConstValues.Add(extraConst.TryGetValue(r, out var xv) ? xv : sh.Literals.TryGetValue(r, out var lv) ? lv : material.TryGetValue(r, out var mv) ? mv : new Vector4(0, 0, 0, 1));
    }

    static string F(float v) => v.ToString("0.0######", CultureInfo.InvariantCulture);
}
