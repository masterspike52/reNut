using System.Numerics;
using System.Text;
using NB.Core.Formats;
using NB.Core.Textures;

namespace NB.Core.Models;

/// <summary>
/// Replaces a model's geometry with imported meshes AND their materials: every material of the file gets its own game
/// texture (created in the bundle from the file's diffuse image, or from its colour) and a material slot of the model.
///
/// Material slots are the model's LOD-0 diffuse textures (draws with UVs, not instanced). A slot is retargeted — in this
/// model only — to the new texture, so the model's other models keep the original. When the file has more materials than
/// the model has slots, the smaller materials share the last slot through a texture atlas: each gets a cell holding its
/// texture repeated as often as its UVs tile (up to <see cref="Options.MaxRepeat"/>), with a wrapped gutter against mip
/// bleeding, and its UVs are remapped into that cell. Materials whose name equals a texture of the model (a model exported
/// by this tool and re-imported) keep that game texture.
///
/// The other textures of a slot's draws are neutralised (they were painted for the old shape): normal maps → flat,
/// specular → dark grey, ambient occlusion / masks / anything else → white, a second colour layer → the new texture.
/// Vertex colours are set to the original buffer's average, and LOD 0 is drawn at every distance up to the model's cull
/// distance (the lower LODs still hold the old shape).
/// </summary>
public static class MaterialImport
{
    public sealed class Options
    {
        /// <summary>Largest side of a texture made for one material (power of two).</summary>
        public int MaxTextureSize = 1024;
        /// <summary>Largest side of a shared atlas texture.</summary>
        public int AtlasSize = 2048;
        /// <summary>Most repeats of a tiling texture baked into an atlas cell (per axis).</summary>
        public int MaxRepeat = 4;
        /// <summary>Retarget normal/specular/AO/mask textures of the used slots to neutral ones.</summary>
        public bool NeutraliseDetail = true;
    }

    public enum Mode { Unassigned, Keep, Own, Atlas }

    public sealed class MaterialPlan
    {
        public string Name = "";
        public ImportMaterial? Material;
        public List<ImportMesh> Meshes = new();
        public int Triangles, Vertices;
        public Mode Mode;
        /// <summary>The model's texture stem (slot) this material is drawn with.</summary>
        public string Slot = "";
        /// <summary>Most texture repeats of its UVs (max of U and V span).</summary>
        public float Repeat;
        public string Source => Mode == Mode.Keep ? "the game texture of the same name (file not needed)" : Material?.Describe() ?? "no material (untextured grey)";
        public string? Problem;
    }

    public sealed class Plan
    {
        public List<MaterialPlan> Materials = new();
        public List<string> Slots = new();
        public List<string> Notes = new();
        public override string ToString()
        {
            var sb = new StringBuilder();
            foreach (var m in Materials)
                sb.AppendLine($"{m.Name}: {m.Triangles} triangles, {(m.Mode == Mode.Keep ? "keeps game texture" : m.Mode == Mode.Own ? "own texture" : "atlas")} → slot {m.Slot}; {m.Source}{(m.Problem != null ? "  [" + m.Problem + "]" : "")}");
            foreach (var n in Notes) sb.AppendLine(n);
            return sb.ToString();
        }
    }

    public sealed record Result(Plan Plan, ModelImporter.Result Geometry, List<string> Textures, List<string> Notes);

    static string? MatOf(MeshDraw x) => ObjExporter.DiffuseTexture(x) is string t ? ObjExporter.TextureFileStem(t) : null;
    static bool HasUv(MeshDraw x) => x.Layout.Any(e => e.Format is VtxFormat.k_16_16_FLOAT or VtxFormat.k_32_32_FLOAT);
    static string KeyOf(ImportMesh m) => m.Material?.Name ?? m.Name;

    /// <summary>The model's material slots: diffuse stems of LOD-0, non-instanced draws with UVs; simplest first
    /// (no vertex colour, fewest texture layers), then by the number of vertex buffers (capacity).</summary>
    public static List<string> Slots(ModelAsset m)
    {
        // (a draw whose only texture is a normal/specular/mask map is not a colour slot)
        var usable = m.Draws.Where(x => !m.LodOnlyNodes.Contains(x.Node) && !x.Instanced && HasUv(x) && MatOf(x) is string st && Classify(st) is "colour" or "other").ToList();
        return usable.GroupBy(x => MatOf(x)!, StringComparer.OrdinalIgnoreCase)
            .OrderBy(g => g.Any(x => x.Layout.Any(e => e.Format == VtxFormat.k_8_8_8_8)) ? 1 : 0)
            .ThenBy(g => g.Max(x => x.Textures.Count))
            .ThenByDescending(g => g.Select(x => x.VbRecord).Distinct().Count())
            .Select(g => g.Key).ToList();
    }

    static float RepeatOf(IEnumerable<ImportMesh> meshes)
    {
        float r = 0;
        foreach (var me in meshes.Where(x => x.UVs != null && x.UVs.Count > 0))
        {
            var mn = me.UVs!.Aggregate(Vector2.Min); var mx = me.UVs!.Aggregate(Vector2.Max);
            r = MathF.Max(r, MathF.Max(mx.X - MathF.Floor(mn.X), mx.Y - MathF.Floor(mn.Y)));
        }
        return r;
    }

    public static Plan MakePlan(CaffFile caff, int symbol, IReadOnlyList<ImportMesh> meshes, Options? o = null)
    {
        o ??= new Options();
        var m = ModelAsset.Parse(caff, symbol);
        var plan = new Plan { Slots = Slots(m) };
        var allStems = m.Draws.Select(MatOf).Where(x => x != null).Select(x => x!).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var g in meshes.GroupBy(KeyOf, StringComparer.OrdinalIgnoreCase))
        {
            var mp = new MaterialPlan { Name = g.Key, Material = g.First().Material, Meshes = g.ToList(), Triangles = g.Sum(x => x.Triangles.Count / 3), Vertices = g.Sum(x => x.Positions.Count), Repeat = RepeatOf(g) };
            if (allStems.Contains(g.Key)) { mp.Mode = Mode.Keep; mp.Slot = allStems.First(s => s.Equals(g.Key, StringComparison.OrdinalIgnoreCase)); }
            if (mp.Material != null && mp.Mode != Mode.Keep)
            {
                if (mp.Material.TexturePath != null && mp.Material.EmbeddedImage == null && mp.Material.ResolveTextureFile() == null) mp.Problem = "texture file not found; its colour is used";
                else if (mp.Material.TexturePath != null && Path.GetExtension(mp.Material.TexturePath).Equals(".dds", StringComparison.OrdinalIgnoreCase)) mp.Problem = "DDS is not supported (save as PNG/TGA/JPG); its colour is used";
            }
            if (g.All(x => x.UVs == null) && mp.Mode != Mode.Keep && mp.Material?.TexturePath != null) mp.Problem = (mp.Problem != null ? mp.Problem + "; " : "") + "the mesh has no UVs (the texture's centre colour is used)";
            plan.Materials.Add(mp);
        }
        var free = plan.Slots.Where(s => !plan.Materials.Any(x => x.Mode == Mode.Keep && x.Slot.Equals(s, StringComparison.OrdinalIgnoreCase))).ToList();
        // materials needing their own slot most: textures tiling more than an atlas cell can hold, then the biggest
        var todo = plan.Materials.Where(x => x.Mode != Mode.Keep)
            .OrderByDescending(x => x.Repeat > o.MaxRepeat ? 1 : 0).ThenByDescending(x => x.Triangles).ToList();
        if (todo.Count > 0 && free.Count == 0)
            throw new InvalidDataException(plan.Slots.Count == 0
                ? "this model has no textured LOD-0 draw with texture coordinates to hold imported materials"
                : "every material slot of this model is taken by a material that keeps its game texture");
        int own = todo.Count <= free.Count ? todo.Count : free.Count - 1;
        for (int i = 0; i < todo.Count; i++)
        {
            if (i < own) { todo[i].Mode = Mode.Own; todo[i].Slot = free[i]; }
            else
            {
                todo[i].Mode = Mode.Atlas; todo[i].Slot = free[own];
                if (todo[i].Repeat > o.MaxRepeat) todo[i].Problem = (todo[i].Problem != null ? todo[i].Problem + "; " : "") + $"tiles {todo[i].Repeat:0.#}x — the atlas holds {o.MaxRepeat}x, triangles are shifted per tile and larger spans are stretched";
            }
        }
        if (todo.Count > free.Count)
            plan.Notes.Add($"{todo.Count} new material(s) but {free.Count} free slot(s) in this model: {todo.Count - own} share an atlas texture on slot {free[own]}");
        foreach (var s in free.Where(s => !plan.Materials.Any(x => x.Slot == s)))
            plan.Notes.Add($"slot {s}: unused (its draws are hidden)");
        return plan;
    }

    // ------------------------------------------------------------------ apply

    public static Result Apply(CaffFile caff, int symbol, IReadOnlyList<ImportMesh> meshes, Options? o = null)
    {
        o ??= new Options();
        var plan = MakePlan(caff, symbol, meshes, o);
        var notes = new List<string>();
        var created = new List<string>();
        string modelName = AssetIds.DisplayName(caff.Symbols[symbol - 1]);
        int Sym() => caff.Symbols.FindIndex(s => AssetIds.DisplayName(s) == modelName) + 1;   // ids shift when assets are added/removed
        string prefix = "aid_texture_banjox_imp_" + Sanitise(modelName.StartsWith("aid_model_banjox_") ? modelName["aid_model_banjox_".Length..] : modelName, 36) + "_";

        // 1. textures and UVs per slot
        var slotTexture = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);   // slot stem → new texture
        var finalMeshes = new List<(ImportMesh Mesh, string Slot)>();
        foreach (var mp in plan.Materials.Where(x => x.Mode == Mode.Keep))
            foreach (var me in mp.Meshes) finalMeshes.Add((me, mp.Slot));
        foreach (var slotGroup in plan.Materials.Where(x => x.Mode != Mode.Keep).GroupBy(x => x.Slot))
        {
            var mats = slotGroup.ToList();
            string texName;
            (byte[] Rgba, int W, int H) img;
            if (mats.Count == 1 && mats[0].Mode == Mode.Own)
            {
                var mp = mats[0];
                var src = LoadMaterialImage(mp, notes);
                int w = Pow2(src.W, o.MaxTextureSize), h = Pow2(src.H, o.MaxTextureSize);
                img = (w == src.W && h == src.H) ? src : ImageIO.Resize(src.Rgba, src.W, src.H, w, h);
                texName = prefix + Sanitise(mp.Name, 24) + "_colour";
                foreach (var me in mp.Meshes) finalMeshes.Add((Centred(me), slotGroup.Key));
                notes.Add($"material {mp.Name}: own texture {w}x{h} on slot {slotGroup.Key}");
            }
            else
            {
                var atlas = BuildAtlas(mats, o, notes);
                img = (atlas.Rgba, atlas.Size, atlas.Size);
                texName = prefix + "atlas_colour";
                finalMeshes.AddRange(atlas.Meshes.Select(x => (x, slotGroup.Key)));
                notes.Add($"atlas {atlas.Size}x{atlas.Size} on slot {slotGroup.Key}: {string.Join(", ", mats.Select(x => x.Name))}");
            }
            if (img.Rgba.Where((b, i) => i % 4 == 3).Any(a => a < 250)) notes.Add($"{texName}: the image has transparency; the game texture is opaque (DXT1)");
            RemoveIfPresent(caff, texName);
            TextureFactory.Create(caff, texName, img.Rgba, img.W, img.H, img.W, img.H, XenosFormat.DXT1);
            created.Add(texName);
            slotTexture[slotGroup.Key] = texName;
        }

        // 2. retarget the used slots (this model only) and neutralise their detail textures
        int sym = Sym();
        var before = ModelAsset.Parse(caff, sym);
        var keptStems = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var d in before.Draws.Where(x => MatOf(x) is string s && plan.Materials.Any(p => p.Mode == Mode.Keep && p.Slot.Equals(s, StringComparison.OrdinalIgnoreCase))))
            foreach (var t in d.Textures) keptStems.Add(ObjExporter.TextureFileStem(t.Texture));
        var retarget = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var (slot, tex) in slotTexture)
        {
            retarget[slot] = tex;
            if (!o.NeutraliseDetail) continue;
            foreach (var d in before.Draws.Where(x => string.Equals(MatOf(x), slot, StringComparison.OrdinalIgnoreCase)))
                foreach (var (_, t) in d.Textures)
                {
                    string st = ObjExporter.TextureFileStem(t);
                    if (st.Equals(slot, StringComparison.OrdinalIgnoreCase) || retarget.ContainsKey(st) || keptStems.Contains(st)) continue;
                    retarget[st] = Classify(st) switch
                    {
                        "normal" => Neutral(caff, "flatnormal", 128, 128, 255, created),
                        "spec" => Neutral(caff, "spec", 40, 40, 40, created),
                        "colour" => tex,
                        _ => Neutral(caff, "white", 255, 255, 255, created),
                    };
                }
        }
        sym = Sym();
        foreach (var (from, to) in retarget)
        {
            int n = ModelEdit.RetargetTexture(caff, sym, from, to, exact: true);
            notes.Add($"texture {from} → {to}: {n} entr{(n == 1 ? "y" : "ies")}");
        }

        // 3. geometry: each mesh named after the (new) diffuse texture of its slot's draws
        var after = ModelAsset.Parse(caff, sym);
        string SlotName(string slot)
        {
            string target = slotTexture.TryGetValue(slot, out var t) ? t : slot;
            var d = after.Draws.FirstOrDefault(x => string.Equals(MatOf(x), target, StringComparison.OrdinalIgnoreCase));
            if (d == null) throw new InvalidDataException($"after retargeting, no draw uses {target} as its diffuse texture (draws: {string.Join(", ", after.Draws.Select(x => MatOf(x)).Distinct())}; notes: {string.Join("; ", notes)})");
            return MatOf(d)!;
        }
        var named = finalMeshes.Select(x => new ImportMesh { Name = SlotName(x.Slot), Positions = x.Mesh.Positions, Normals = x.Mesh.Normals, UVs = x.Mesh.UVs, Triangles = x.Mesh.Triangles, Material = x.Mesh.Material }).ToList();
        var geo = ModelImporter.Replace(caff, sym, named, forceByMaterial: true, uniformVertexColour: true);

        // 4. LOD 0 at every distance up to the model's own cull distance (lower LODs hold the old shape)
        var lods = ModelEdit.GetLodTable(caff, sym);
        var culls = lods.SelectMany(g => g.Where(l => l.Nodes == 0).Select(l => l.Distance)).ToList();
        float cull = culls.Count > 0 ? culls.Min() : 1e6f;
        var lr = ModelEdit.SetLodDistances(caff, sym, cull, 1f, keepLod0: true);
        if (lr.Groups > 0) notes.Add($"LOD: level 0 drawn up to the cull distance {cull:G4} ({lr.Levels} level(s) changed)");

        // 5. textures an earlier import of this model created and nothing uses any more
        foreach (var old in caff.Symbols.Select(AssetIds.DisplayName).Where(n => n.StartsWith(prefix) && !created.Contains(n)).ToList())
            if (!Referenced(caff, old)) { RemoveIfPresent(caff, old); notes.Add($"removed unused {old}"); }
        return new Result(plan, geo, created, notes);
    }

    // ------------------------------------------------------------------ helpers

    /// <summary>"normal", "spec", "mask", "ao", "colour" or "other" from a texture name.</summary>
    public static string Classify(string stem)
    {
        var t = stem.ToLowerInvariant();
        if (t.Contains("normal") || t.Contains("_nm") || t.Contains("bump") || t.Contains("parallax") || t.EndsWith("_norm") || t.Contains("_norm_") || t.Contains("flatnormal")) return "normal";
        if (t.Contains("spec")) return "spec";
        if (t.Contains("_trans") || t.Contains("_mask") || t.Contains("_alpha")) return "mask";
        if (t.Contains("ambientocclusion") || t.EndsWith("_ao") || t.Contains("_ao_")) return "ao";
        if (t.Contains("colour") || t.Contains("color") || t.Contains("diffuse")) return "colour";
        return "other";
    }

    /// <summary>A shared neutral texture (32x32, one colour) of this bundle, created on first use.</summary>
    static string Neutral(CaffFile caff, string kind, byte r, byte g, byte b, List<string> created)
    {
        string name = "aid_texture_banjox_imp_neutral_" + kind;
        if (!caff.Symbols.Any(s => AssetIds.DisplayName(s) == name))
        {
            var px = new byte[32 * 32 * 4];
            for (int i = 0; i < px.Length; i += 4) { px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 255; }
            TextureFactory.Create(caff, name, px, 32, 32, 32, 32, XenosFormat.DXT1);
            created.Add(name);
        }
        return name;
    }

    static void RemoveIfPresent(CaffFile caff, string name)
    {
        int s = caff.Symbols.FindIndex(x => AssetIds.DisplayName(x) == name) + 1;
        if (s > 0) CaffEdit.RemoveAsset(caff, s);
    }

    /// <summary>True when any asset's data other than the texture itself contains its name (a model's texture table).</summary>
    static bool Referenced(CaffFile caff, string name)
    {
        var needle = Encoding.Latin1.GetBytes(name + "\0");
        int own = caff.Symbols.FindIndex(x => AssetIds.DisplayName(x) == name) + 1;
        foreach (var p in caff.Parts)
            if (p.Symbol != own && p.Data.AsSpan().IndexOf(needle) >= 0) return true;
        return false;
    }

    static string Sanitise(string s, int max)
    {
        var sb = new StringBuilder();
        foreach (char c in s.ToLowerInvariant()) sb.Append(c is >= 'a' and <= 'z' or >= '0' and <= '9' ? c : '_');
        var r = sb.ToString().Trim('_');
        if (r.Length > max)
        {
            r = r[^max..];
            int u = r.IndexOf('_');
            if (u >= 0 && u < r.Length - 4) r = r[(u + 1)..];   // start at a word boundary
            r = r.Trim('_');
        }
        return r.Length == 0 ? "mat" : r;
    }

    static int Pow2(int v, int max)
    {
        int p = 8;
        while (p < v && p < max) p *= 2;
        return Math.Min(p, max);
    }

    /// <summary>A mesh without UVs samples the middle of its texture.</summary>
    static ImportMesh Centred(ImportMesh m) => m.UVs != null ? m :
        new ImportMesh { Name = m.Name, Positions = m.Positions, Normals = m.Normals, Triangles = m.Triangles, Material = m.Material, UVs = m.Positions.Select(_ => new Vector2(0.5f)).ToList() };

    /// <summary>The material's image: its texture, or an 8x8 swatch of its colour (mid grey without a material).</summary>
    static (byte[] Rgba, int W, int H) LoadMaterialImage(MaterialPlan mp, List<string> notes)
    {
        if (mp.Material != null)
        {
            var img = mp.Material.LoadImage(out var err);
            if (img != null) return img.Value;
            if (err != null) notes.Add($"material {mp.Name}: {err} — using its colour");
        }
        var c = mp.Material?.Color ?? new Vector3(0.6f);
        var px = new byte[8 * 8 * 4];
        for (int i = 0; i < px.Length; i += 4)
        {
            px[i] = (byte)Math.Clamp(c.X * 255, 0, 255); px[i + 1] = (byte)Math.Clamp(c.Y * 255, 0, 255); px[i + 2] = (byte)Math.Clamp(c.Z * 255, 0, 255); px[i + 3] = 255;
        }
        return (px, 8, 8);
    }

    /// <summary>
    /// Packs the materials into one square texture: a grid of cells, each holding its texture repeated rx × ry times
    /// (the span of its UVs, per triangle when the whole mesh spans more than <see cref="Options.MaxRepeat"/>), inside
    /// a gutter that continues the wrapped pattern. Returns the image and the meshes with UVs remapped into their cells.
    /// </summary>
    static (byte[] Rgba, int Size, List<ImportMesh> Meshes) BuildAtlas(List<MaterialPlan> mats, Options o, List<string> notes)
    {
        int n = mats.Count, cols = (int)Math.Ceiling(Math.Sqrt(n)), rows = (n + cols - 1) / cols;
        var images = mats.Select(x => LoadMaterialImage(x, notes)).ToList();
        // UV layout per material: merged mesh, integer shift, repeats
        var prepared = new List<(ImportMesh Mesh, Vector2 Shift, int Rx, int Ry)>();
        foreach (var mp in mats)
        {
            var me = Merge(mp.Meshes.Select(Centred).ToList(), mp.Name);
            var (mn, mx) = UvBounds(me.UVs!);
            var shift = new Vector2(MathF.Floor(mn.X), MathF.Floor(mn.Y));
            if (mx.X - shift.X > o.MaxRepeat || mx.Y - shift.Y > o.MaxRepeat)
            {
                me = PerTriangleShift(me);
                (mn, mx) = UvBounds(me.UVs!);
                shift = new Vector2(MathF.Floor(mn.X), MathF.Floor(mn.Y));
            }
            int rx = Math.Clamp((int)MathF.Ceiling(mx.X - shift.X - 1e-4f), 1, o.MaxRepeat), ry = Math.Clamp((int)MathF.Ceiling(mx.Y - shift.Y - 1e-4f), 1, o.MaxRepeat);
            float sx = MathF.Max(1, (mx.X - shift.X) / rx), sy = MathF.Max(1, (mx.Y - shift.Y) / ry);   // > 1: squeezed (stretched texture)
            if (sx > 1.01f || sy > 1.01f) notes.Add($"material {mp.Name}: UVs span more than {o.MaxRepeat} repeats inside one triangle; the texture is stretched in the atlas");
            prepared.Add((me, shift, rx, ry));
        }
        // atlas size: enough for every cell at its source resolution, capped
        double texels = mats.Select((_, i) => (double)images[i].W * prepared[i].Rx * images[i].H * prepared[i].Ry).Max() * cols * rows;
        int size = 256;
        while (size < o.AtlasSize && (double)size * size < texels) size *= 2;
        int cw = size / cols, ch = size / rows;
        int gutter = Math.Max(2, Math.Min(cw, ch) / 32);
        var atlas = new byte[size * size * 4];
        var outMeshes = new List<ImportMesh>();
        for (int i = 0; i < n; i++)
        {
            int x0 = (i % cols) * cw, y0 = (i / cols) * ch;
            int iw = cw - 2 * gutter, ih = ch - 2 * gutter;
            var (me, shift, rx, ry) = prepared[i];
            // source prefiltered to one repeat's size in the cell
            int tw = Math.Max(1, iw / rx), th = Math.Max(1, ih / ry);
            var src = images[i];
            var small = src.W == tw && src.H == th ? src.Rgba : ImageIO.Resize(src.Rgba, src.W, src.H, tw, th).Rgba;
            for (int y = 0; y < ch; y++)
                for (int x = 0; x < cw; x++)
                {
                    // cell pixel → texture coordinate (repeats), wrapped (the gutter continues the tiling)
                    double u = (x - gutter + 0.5) / iw * rx, v = (y - gutter + 0.5) / ih * ry;
                    SampleWrap(small, tw, th, u, v, atlas, ((y0 + y) * size + x0 + x) * 4);
                }
            var span = UvBounds(me.UVs!).Max - shift;
            float dx = MathF.Max(rx, span.X), dy = MathF.Max(ry, span.Y);
            var uvs = me.UVs!.Select(t =>
            {
                var f = new Vector2((t.X - shift.X) / dx, (t.Y - shift.Y) / dy);
                return new Vector2((x0 + gutter + f.X * iw) / size, (y0 + gutter + f.Y * ih) / size);
            }).ToList();
            outMeshes.Add(new ImportMesh { Name = me.Name, Positions = me.Positions, Normals = me.Normals, Triangles = me.Triangles, Material = me.Material, UVs = uvs });
        }
        return (atlas, size, outMeshes);
    }

    static (Vector2 Min, Vector2 Max) UvBounds(List<Vector2> uv) => (uv.Aggregate(Vector2.Min), uv.Aggregate(Vector2.Max));

    static ImportMesh Merge(List<ImportMesh> list, string name)
    {
        var r = new ImportMesh { Name = name, Material = list[0].Material, Normals = new(), UVs = new() };
        foreach (var x in list)
        {
            int b = r.Positions.Count;
            r.Positions.AddRange(x.Positions); r.Normals.AddRange(x.Normals ?? ObjReader.ComputeNormals(x)); r.UVs.AddRange(x.UVs!);
            r.Triangles.AddRange(x.Triangles.Select(t => t + b));
        }
        return r;
    }

    /// <summary>Unshares vertices so each triangle's UVs can move by its own whole number of repeats (to its lowest tile).</summary>
    static ImportMesh PerTriangleShift(ImportMesh m)
    {
        var r = new ImportMesh { Name = m.Name, Material = m.Material, Normals = new(), UVs = new() };
        for (int t = 0; t + 2 < m.Triangles.Count; t += 3)
        {
            var uv = new[] { m.UVs![m.Triangles[t]], m.UVs[m.Triangles[t + 1]], m.UVs[m.Triangles[t + 2]] };
            var s = new Vector2(MathF.Floor(MathF.Min(uv[0].X, MathF.Min(uv[1].X, uv[2].X))), MathF.Floor(MathF.Min(uv[0].Y, MathF.Min(uv[1].Y, uv[2].Y))));
            for (int k = 0; k < 3; k++)
            {
                int v = m.Triangles[t + k];
                r.Triangles.Add(r.Positions.Count);
                r.Positions.Add(m.Positions[v]); r.Normals.Add(m.Normals![v]); r.UVs.Add(uv[k] - s);
            }
        }
        return r;
    }

    static void SampleWrap(byte[] src, int w, int h, double u, double v, byte[] dst, int o)
    {
        double fx = (u - Math.Floor(u)) * w - 0.5, fy = (v - Math.Floor(v)) * h - 0.5;
        int x0 = (int)Math.Floor(fx), y0 = (int)Math.Floor(fy);
        double ax = fx - x0, ay = fy - y0;
        int X(int x) => ((x % w) + w) % w; int Y(int y) => ((y % h) + h) % h;
        int p00 = (Y(y0) * w + X(x0)) * 4, p10 = (Y(y0) * w + X(x0 + 1)) * 4, p01 = (Y(y0 + 1) * w + X(x0)) * 4, p11 = (Y(y0 + 1) * w + X(x0 + 1)) * 4;
        for (int c = 0; c < 4; c++)
        {
            double top = src[p00 + c] * (1 - ax) + src[p10 + c] * ax, bot = src[p01 + c] * (1 - ax) + src[p11 + c] * ax;
            dst[o + c] = (byte)Math.Clamp(Math.Round(top * (1 - ay) + bot * ay), 0, 255);
        }
    }
}
