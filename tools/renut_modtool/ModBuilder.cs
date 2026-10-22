using System.Numerics;
using System.Text.Json;
using NB.Core.Compression;
using NB.Core.Formats;
using NB.Core.IO;
using NB.Core.Models;
using NB.Core.Parts;
using NB.Core.Textures;

namespace Renut.ModTool;

/// <summary>mod.json: the mod's identity, written into the patch's patch.json.</summary>
public sealed class ModInfo
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public string Version { get; set; } = "1";
    public string Author { get; set; } = "";
    public string Description { get; set; } = "";
}

/// <summary>models.json (see docs/modding/custom-parts.md).</summary>
public sealed class ModelsSpec
{
    public string? Bundle { get; set; }
    public List<ModelSpec> Models { get; set; } = new();
    public List<AttachSpec> Attach { get; set; } = new();
}

public sealed class ModelSpec
{
    public string Id { get; set; } = "";
    public string Template { get; set; } = "";
    public string? Obj { get; set; }
    public string? PreRotate { get; set; }
    public string? CollisionFit { get; set; }
    /// <summary>The collision's box in model space, overriding <see cref="CollisionFit"/>.</summary>
    public BoxSpec? CollisionBox { get; set; }
    public Dictionary<string, string> Tint { get; set; } = new();
    public Dictionary<string, string> Materials { get; set; } = new();
    public Dictionary<string, string> Retarget { get; set; } = new();
    public Dictionary<string, string> Textures { get; set; } = new();
    public bool KeepAnimated { get; set; }
    /// <summary>The model goes on the template's moving pieces (a wheel's turning rim and tyre), so it turns with them; the
    /// rest of the template draws nothing.</summary>
    public bool Spins { get; set; }
    /// <summary>Keeps the template model's bounds (chunk 5) instead of the imported geometry's.</summary>
    public bool KeepBounds { get; set; }
}

public sealed class BoxSpec
{
    public float[] Min { get; set; } = Array.Empty<float>();
    public float[] Max { get; set; } = Array.Empty<float>();

    public (Vector3 Min, Vector3 Max) ToBounds(string model)
    {
        if (Min.Length != 3 || Max.Length != 3 || Min[0] >= Max[0] || Min[1] >= Max[1] || Min[2] >= Max[2])
            throw new ArgumentException($"model {model}: collisionBox is {{ \"min\": [x, y, z], \"max\": [x, y, z] }} with min below max");
        return (new Vector3(Min[0], Min[1], Min[2]), new Vector3(Max[0], Max[1], Max[2]));
    }
}

public sealed class AttachSpec
{
    public string Template { get; set; } = "";
    public string Id { get; set; } = "";
    public string? Attachable { get; set; }
}

/// <summary>
/// Builds a mod's models into a bundle pair of its own, the way reNut loads them: a stream archive
/// aid_streambundle_banjox_&lt;bundle&gt; (Bundle/50) with each model (a copy of a shipped part model's streamed entry, model +
/// shader pool, renamed aid_model_banjox_vehicleparts_&lt;id&gt; with the OBJ imported), its collision
/// aid_havok_banjox_vehicleparts_&lt;id&gt; and each footprint aid_avatarhavokdata_banjox_vehicleblock_&lt;id&gt;; and, only with
/// new textures, a resident bundle aid_bundle_banjox_&lt;bundle&gt; (Bundle/4f), since streamed models find their textures by
/// name among the resident bundles. The templates come from the game's common bundles (4f/50 685374).
/// </summary>
public sealed class ModBuilder
{
    public const uint Common = 0x685374;
    public const string ModelPrefix = "aid_model_banjox_vehicleparts_";
    public const string HavokPrefix = "aid_havok_banjox_vehicleparts_";
    public const string AttachPrefix = "aid_avatarhavokdata_banjox_vehicleblock_";
    public const string PartPrefix = "aid_objparams_banjox_vehicleblock_";

    readonly string _game;
    BundleArchive? _stream;
    CaffFile? _resident;
    public readonly List<string> Log = new();

    public ModBuilder(string gameFolder) { _game = gameFolder; }

    public string ResidentPath(uint id) => Path.Combine(_game, "Bundle", "4f", (id & 0xFFFFFF).ToString("x6"));
    public string StreamPath(uint id) => Path.Combine(_game, "Bundle", "50", (id & 0xFFFFFF).ToString("x6"));

    /// <summary>The game's common stream archive (Bundle/50/685374, ~800 MB; read once).</summary>
    public BundleArchive CommonStream => _stream ??= BundleArchive.Read(File.ReadAllBytes(StreamPath(Common)));

    /// <summary>The game's common resident bundle (Bundle/4f/685374).</summary>
    public CaffFile CommonResident
    {
        get
        {
            if (_resident != null) return _resident;
            var raw = File.ReadAllBytes(ResidentPath(Common));
            return _resident = CaffFile.Read(XCompressFile.IsCompressed(raw) ? XCompressFile.Decompress(raw) : raw);
        }
    }

    public bool StreamHas(string asset) => AssetIds.IdOf(asset) is uint id && CommonStream.Entries.Any(e => e.Id == id && e.Kind == "caff");
    public bool ResidentHas(string asset) => CommonResident.Symbols.Any(s => AssetIds.DisplayName(s) == asset);

    /// <summary>A shipped streamed asset (a CAFF holding it) and its symbol.</summary>
    (CaffFile Caff, int Symbol) Streamed(string asset)
    {
        uint id = AssetIds.IdOf(asset) ?? throw new InvalidDataException("no id for " + asset);
        var e = CommonStream.Entries.FirstOrDefault(x => x.Id == id && x.Kind == "caff")
                ?? throw new InvalidDataException($"{asset} is not a part asset of the game (not in Bundle/50/{Common:x6})");
        var c = CaffFile.Read(e.Data!);
        int sym = c.Symbols.FindIndex(s => AssetIds.DisplayName(s) == asset) + 1;
        if (sym == 0) throw new InvalidDataException($"stream entry {id:X8} does not hold {asset}");
        return (c, sym);
    }

    /// <summary>The template model's colour textures (what OBJ materials map to); <paramref name="turning"/>: only those on
    /// its turning part (a wheel's, for "spins").</summary>
    public List<string> TemplateMaterials(string template, bool turning = false)
    {
        var (c, sym) = Streamed(ModelPrefix + template);
        var ma = ModelAsset.Parse(c, sym);
        var lodNodes = ma.LodLevels.SelectMany(g => g.SelectMany(l => l.Nodes)).ToHashSet();
        var turningVbs = ma.Draws.GroupBy(x => x.VbRecord)
            .Where(g => g.Any(x => !lodNodes.Contains(x.Node) && x.Node > 3) && !g.Any(x => lodNodes.Contains(x.Node))).Select(g => g.Key).ToHashSet();
        return ma.Draws.Where(x => !turning || turningVbs.Contains(x.VbRecord)).Select(x => ObjExporter.DiffuseTexture(x)).Where(x => x != null)
            .Select(x => ObjExporter.TextureFileStem(x!)).Distinct().ToList();
    }

    /// <summary>Builds the bundles: (game-relative path, bytes) of each new file.</summary>
    public List<(string Path, byte[] Data)> Build(ModelsSpec spec, string bundle, string baseDir)
    {
        var files = new List<(string, byte[])>();
        uint resId = AssetIds.IdOf("aid_bundle_banjox_" + bundle)!.Value, strId = AssetIds.IdOf("aid_streambundle_banjox_" + bundle)!.Value;
        if (File.Exists(ResidentPath(resId)) || File.Exists(StreamPath(strId)))
            throw new ArgumentException($"bundle name '{bundle}' gives the id of a bundle of the game ({resId & 0xFFFFFF:x6}); choose another \"bundle\"");
        var arch = new BundleArchive { Timestamp = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() };
        string Add(string tpl, string name, Func<CaffFile, int, string>? edit)
        {
            var (sc, sym) = Streamed(tpl);
            CaffEdit.RenameAsset(sc, sym, name);
            string note = edit?.Invoke(sc, sym) ?? "copied";
            uint id = AssetIds.IdOf(name)!.Value;
            if (arch.Entries.Any(e => e.Id == id)) throw new ArgumentException($"{name} is listed twice");
            arch.Entries.Add(new BundleEntry { Id = id, Data = sc.Write() });
            return $"{name}: {note}";
        }

        var footprints = new Dictionary<string, AttachData>();
        foreach (var a in spec.Attach)
        {
            CheckId(a.Id, "attach id");
            Log.Add(Add(AttachPrefix + a.Template, AttachPrefix + a.Id, (sc, sym) =>
            {
                var d = sc.PartsOf(sym).First(x => sc.SectionOf(x).Name == ".data").Data;
                var ad = AttachData.Parse(d);
                if (a.Attachable != null) { ad.SetAttachable(a.Attachable); ad.WriteFlags(d); }
                footprints[a.Id] = ad;
                return ad.ToString();
            }));
        }
        foreach (var m in spec.Models)
        {
            CheckId(m.Id, "model id");
            if (string.IsNullOrEmpty(m.Template)) throw new ArgumentException($"model {m.Id}: \"template\" (a shipped part model, e.g. gadgets_horn) is needed");
            var meshes = ReadMeshes(m, baseDir);
            (Vector3 Min, Vector3 Max)? fit = null;
            if (m.CollisionFit == "model" && meshes != null) fit = Bounds(meshes);
            else if (m.CollisionFit == "footprint")
                fit = footprints.TryGetValue(m.Id, out var fp) ? FootprintBounds(fp)
                    : throw new ArgumentException($"model {m.Id}: collisionFit \"footprint\" needs an attach entry with id {m.Id}");
            else if (m.CollisionFit != null && m.CollisionFit != "model") throw new ArgumentException($"model {m.Id}: collisionFit is \"model\" or \"footprint\"");
            if (m.CollisionBox != null) fit = m.CollisionBox.ToBounds(m.Id);
            Log.Add(Add(ModelPrefix + m.Template, ModelPrefix + m.Id, (sc, sym) => ImportModel(sc, sym, m, meshes)));
            Log.Add(Add(HavokPrefix + m.Template, HavokPrefix + m.Id, (sc, sym) => FitCollision(sc, sym, fit)));
        }
        files.Add(($"Bundle/50/{strId & 0xFFFFFF:x6}", arch.Write()));
        Log.Add($"Bundle/50/{strId & 0xFFFFFF:x6} (aid_streambundle_banjox_{bundle}): {arch.Entries.Count} asset(s)");

        var textures = spec.Models.SelectMany(m => m.Textures).GroupBy(kv => kv.Key).Select(g => g.First()).ToList();
        if (textures.Count > 0)
        {
            var cc = CommonResident;
            var caff = NewResident(cc);
            int tsym = TextureFactory.FindTemplate(cc, XenosFormat.DXT1);
            if (tsym == 0) throw new InvalidDataException("the common bundle has no DXT1 texture to start new textures from");
            int copy = CaffEdit.CopyAsset(cc, tsym, caff, "aid_texture_banjox_renut_template");
            foreach (var (tn, png) in textures)
            {
                if (!tn.StartsWith("aid_texture_")) throw new ArgumentException($"texture '{tn}': names start with aid_texture_ (e.g. aid_texture_banjox_mymod_paint)");
                var (rgba, w, h) = ImageIO.Load(Path.IsPathRooted(png) ? png : Path.Combine(baseDir, png));
                TextureFactory.Create(caff, tn, rgba, w, h, w, h);
                Log.Add($"texture {tn}: {w}x{h}");
            }
            CaffEdit.RemoveAsset(caff, copy);
            var bytes = caff.Write();
            CaffFile.Read(bytes);
            files.Add(($"Bundle/4f/{resId & 0xFFFFFF:x6}", bytes));
            Log.Add($"Bundle/4f/{resId & 0xFFFFFF:x6} (aid_bundle_banjox_{bundle}): {textures.Count} texture(s)");
        }
        return files;
    }

    static void CheckId(string id, string what)
    {
        if (id.Length == 0 || id.Length > 40 || !id.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '_'))
            throw new ArgumentException($"{what} '{id}' must be 1-40 characters of a-z, 0-9 and _");
    }

    /// <summary>An empty resident bundle: the template's header and sections, an empty manifest (its self pointers kept).</summary>
    static CaffFile NewResident(CaffFile template)
    {
        var c = new CaffFile { Version = template.Version, ExtraName = (byte[])template.ExtraName.Clone() };
        template.Header.CopyTo(c.Header, 0);
        foreach (var s in template.Sections)
            c.Sections.Add(new CaffSection { Name = s.Name, AlignLog2 = s.AlignLog2, Flags = s.Flags, Mid = (byte[])s.Mid.Clone() });
        int ms = template.Symbols.IndexOf("manifest") + 1;
        if (ms == 0) throw new InvalidDataException("the template bundle has no manifest");
        var mp = template.Parts.Select((p, i) => (p, id: i + 1)).Single(x => x.p.Symbol == ms && template.SectionOf(x.p).Name == ".data");
        var d = new byte[0x20];
        Buffer.BlockCopy(mp.p.Data, 0, d, 0, 0x20);
        BE.W32(d, 8, 0x20); BE.W32(d, 12, 0); BE.W32(d, 16, 0x20); BE.W32(d, 20, 0);
        c.Symbols.Add("manifest");
        c.Parts.Add(new CaffPart { Symbol = 1, Section = mp.p.Section, AlignLog2 = mp.p.AlignLog2, Data = d, Size = d.Length });
        foreach (var r in template.Relocs.Where(r => r.FromPart == mp.id && r.ToPart == mp.id))
            c.Relocs.Add(new CaffReloc(1, 1, (int[])r.Offsets.Clone()) { Table = r.Table });
        return c;
    }

    // ------------------------------------------------------------------ model import (from NB Studio's PartFactory)

    static List<ImportMesh>? ReadMeshes(ModelSpec m, string baseDir)
    {
        if (string.IsNullOrEmpty(m.Obj)) return null;
        var meshes = ObjReader.ReadAny(Path.IsPathRooted(m.Obj) ? m.Obj : Path.Combine(baseDir, m.Obj));
        if (string.IsNullOrEmpty(m.PreRotate)) return meshes;
        var rot = Matrix4x4.Identity;
        foreach (var r0 in m.PreRotate.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var kv = r0.Split(':'); float a = float.Parse(kv[1], System.Globalization.CultureInfo.InvariantCulture) * MathF.PI / 180;
            rot *= kv[0].Trim().ToLowerInvariant() switch { "x" => Matrix4x4.CreateRotationX(a), "y" => Matrix4x4.CreateRotationY(a), _ => Matrix4x4.CreateRotationZ(a) };
        }
        foreach (var mesh in meshes)
        {
            for (int i = 0; i < mesh.Positions.Count; i++) mesh.Positions[i] = Vector3.Transform(mesh.Positions[i], rot);
            if (mesh.Normals != null) for (int i = 0; i < mesh.Normals.Count; i++) mesh.Normals[i] = Vector3.TransformNormal(mesh.Normals[i], rot);
        }
        return meshes;
    }

    static (Vector3 Min, Vector3 Max) Bounds(List<ImportMesh> meshes) =>
        (new Vector3(meshes.Min(m => m.Bounds().Min.X), meshes.Min(m => m.Bounds().Min.Y), meshes.Min(m => m.Bounds().Min.Z)),
         new Vector3(meshes.Max(m => m.Bounds().Max.X), meshes.Max(m => m.Bounds().Max.Y), meshes.Max(m => m.Bounds().Max.Z)));

    static (Vector3 Min, Vector3 Max) FootprintBounds(AttachData fp) =>
        (new Vector3(fp.XMin - 0.5f, fp.YMin - 0.5f, fp.ZMin - 0.5f) * 0.98f, new Vector3(fp.XMax + 0.5f, fp.YMax + 0.5f, fp.ZMax + 0.5f) * 0.98f);

    static string FitCollision(CaffFile caff, int hs, (Vector3 Min, Vector3 Max)? fit)
    {
        if (fit == null) return "template collision";
        var d = caff.PartsOf(hs).First(x => caff.SectionOf(x).Name == ".data").Data;
        int n = NB.Core.Havok.HkConvexEdit.FitTo(d, fit.Value.Min, fit.Value.Max);
        return n > 0 ? $"collision fitted to {fit.Value.Min:0.00}..{fit.Value.Max:0.00}" : "collision: no convex shape to fit";
    }

    static string ImportModel(CaffFile caff, int nm, ModelSpec spec, List<ImportMesh>? meshes)
    {
        // streamed models find their textures by name among the resident bundles: none need be in this CAFF
        foreach (var (from, to) in spec.Retarget)
            ModelEdit.RetargetTexture(caff, nm, from, to, spec.Textures.ContainsKey(to) ? true : null, requireInBundle: false);
        if (meshes == null) { Tint(caff, nm, spec); return $"template geometry, {spec.Retarget.Count} texture(s) retargeted"; }
        // OBJ materials -> the (retargeted) colour textures of the draws that held the template texture
        var ma = ModelAsset.Parse(caff, nm);
        // moving pieces: vertex buffers drawn on rendergraph nodes outside the LOD table (a wheel's turning part)
        var lodNodes = ma.LodLevels.SelectMany(g => g.SelectMany(l => l.Nodes)).ToHashSet();
        var animated = ma.Draws.GroupBy(x => x.VbRecord).Where(g => g.Any(x => !lodNodes.Contains(x.Node) && x.Node > 3)).Select(g => g.Key).ToHashSet();
        // the turning part itself: buffers none of whose draws hang on a LOD table node. A buffer drawn on LOD table nodes
        // (a wheel's distant-LOD rim) shows with one wheel but not once the game pairs two wheels into an axle.
        var turning = ma.Draws.GroupBy(x => x.VbRecord).Where(g => animated.Contains(g.Key) && !g.Any(x => lodNodes.Contains(x.Node))).Select(g => g.Key).ToHashSet();
        string Final(string tplStem) => spec.Retarget.TryGetValue(tplStem, out var t) ? ObjExporter.TextureFileStem(t.Replace("aid_texture_banjox_", "")) : tplStem;
        string? Stem(MeshDraw x) => ObjExporter.DiffuseTexture(x) is string t ? ObjExporter.TextureFileStem(t) : null;
        var stems = ma.Draws.Where(x => !spec.Spins || turning.Contains(x.VbRecord)).Select(Stem).OfType<string>().Distinct().ToList();
        string Which() => spec.Spins ? "the template's colour textures on its turning part" : "the template's colour textures";
        var named = new List<ImportMesh>();
        foreach (var m in meshes)
        {
            if (!spec.Materials.TryGetValue(m.Name, out var tstem))
                throw new InvalidDataException($"model {spec.Id}: OBJ material '{m.Name}' isn't in \"materials\" ({Which()}: {string.Join(", ", stems)})");
            string want = Final(tstem);
            string? hit = stems.FirstOrDefault(s => s.Equals(want, StringComparison.OrdinalIgnoreCase) || s.EndsWith(want, StringComparison.OrdinalIgnoreCase) || want.EndsWith(s, StringComparison.OrdinalIgnoreCase));
            if (hit == null) throw new InvalidDataException($"model {spec.Id}: material '{m.Name}' -> '{want}' is not one of {Which()} ({string.Join(", ", stems)})");
            named.Add(new ImportMesh { Name = hit, Positions = m.Positions, Normals = m.Normals, UVs = m.UVs, Triangles = m.Triangles });
        }
        HashSet<int>? keep = null, exclude;
        if (spec.Spins)
        {
            if (turning.Count == 0) throw new InvalidDataException($"model {spec.Id}: \"spins\": the template {spec.Template} has no turning part (use a wheel)");
            exclude = ma.Draws.Select(x => x.VbRecord).Where(vb => !turning.Contains(vb)).ToHashSet();
        }
        else
        {
            keep = spec.KeepAnimated ? animated : null;
            exclude = spec.KeepAnimated ? null : animated;
        }
        const int BoundsSize = 88;   // chunk 5: 22 floats
        byte[]? bounds = null;
        if (spec.KeepBounds && ma.Chunks.TryGetValue(5, out int b5))
            bounds = new AssetView(caff, nm).Data(".data").AsSpan(b5, BoundsSize).ToArray();
        var r = ModelImporter.Replace(caff, nm, named, false, keep, exclude);
        if (bounds != null && ModelAsset.Parse(caff, nm, geometry: false).Chunks.TryGetValue(5, out int nb5))
            bounds.CopyTo(new AssetView(caff, nm).Data(".data"), nb5);
        ModelEdit.SetLodDistances(caff, nm, 1e6f, 1f, keepLod0: true);
        Tint(caff, nm, spec);
        return $"{r.Vertices} vertices, {r.Triangles} triangles{(spec.Spins ? ", on the moving pieces" : "")}";
    }

    static void Tint(CaffFile caff, int nm, ModelSpec spec)
    {
        foreach (var (stem, rgb) in spec.Tint)
        {
            var c = rgb.Split(',').Select(x => float.Parse(x, System.Globalization.CultureInfo.InvariantCulture)).ToArray();
            if (c.Length != 3) throw new ArgumentException($"model {spec.Id}: tint '{rgb}' is \"r,g,b\"");
            ModelEdit.TintConstants(caff, nm, stem.Replace("aid_texture_banjox_", ""), new Vector3(c[0], c[1], c[2]));
        }
    }

    public static readonly JsonSerializerOptions Json = new()
    {
        PropertyNameCaseInsensitive = true, ReadCommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true,
    };
}
