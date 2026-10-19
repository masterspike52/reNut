using System.Globalization;
using System.Numerics;

namespace NB.Core.Models;

/// <summary>A triangle mesh in game space (the inverse of <see cref="ObjExporter"/>'s conventions).</summary>
public sealed class ImportMesh
{
    public string Name = "";
    public List<Vector3> Positions = new();
    public List<Vector3>? Normals;
    public List<Vector2>? UVs;
    /// <summary>Second texture coordinate set (lightmaps / AO), per vertex; OBJ "vx u v" lines, one per "v".</summary>
    public List<Vector2>? UVs2;
    public List<int> Triangles = new();   // indices into the lists above, game winding
    /// <summary>The source file's material of this mesh (OBJ usemtl + MTL, FBX Material), when it has one.</summary>
    public ImportMaterial? Material;

    public (Vector3 Min, Vector3 Max) Bounds()
    {
        var mn = new Vector3(float.MaxValue); var mx = new Vector3(float.MinValue);
        foreach (var p in Positions) { mn = Vector3.Min(mn, p); mx = Vector3.Max(mx, p); }
        return (mn, mx);
    }
}

/// <summary>
/// A material of an imported file: its name, the diffuse (base colour) texture — a file path as written in the file,
/// resolved against the model's folder, or image bytes embedded in the file — and the diffuse colour.
/// </summary>
public sealed class ImportMaterial
{
    public string Name = "";
    /// <summary>Texture path as stored in the file (absolute or relative), or null.</summary>
    public string? TexturePath;
    /// <summary>Image bytes embedded in the file (FBX Video Content), with the file name they were stored under.</summary>
    public byte[]? EmbeddedImage;
    /// <summary>Folder of the model file (for relative texture paths).</summary>
    public string BaseDir = "";
    public Vector3 Color = Vector3.One;

    /// <summary>The texture file on disk: the stored path, the path relative to the model's folder, then the file name
    /// in the model's folder and in common texture sub-folders (textures, tex, images, &lt;model&gt;.fbm).</summary>
    public string? ResolveTextureFile()
    {
        if (string.IsNullOrWhiteSpace(TexturePath)) return null;
        var raw = TexturePath.Replace("\\\\", "\\").Trim().Trim('"');
        var name = Path.GetFileName(raw.Replace('/', '\\'));
        var cands = new List<string>();
        try { if (Path.IsPathRooted(raw)) cands.Add(raw); } catch (ArgumentException) { }
        try { cands.Add(Path.GetFullPath(Path.Combine(BaseDir, raw))); } catch (Exception) { }
        foreach (var sub in new[] { "", "textures", "Textures", "tex", "images", "maps" }) cands.Add(Path.Combine(BaseDir, sub, name));
        if (Directory.Exists(BaseDir))
            foreach (var fbm in Directory.GetDirectories(BaseDir, "*.fbm")) cands.Add(Path.Combine(fbm, name));
        return cands.FirstOrDefault(File.Exists);
    }

    /// <summary>A short description of where the texture comes from (for dialogs and logs).</summary>
    public string Describe() =>
        EmbeddedImage != null ? $"embedded {Path.GetFileName(TexturePath ?? "image")}" :
        TexturePath == null ? $"colour ({Color.X:0.##}, {Color.Y:0.##}, {Color.Z:0.##})" :
        ResolveTextureFile() is string f ? f : $"MISSING {TexturePath}";

    /// <summary>Loads the diffuse image (embedded, then file), or null when there is none or it cannot be read.</summary>
    public (byte[] Rgba, int W, int H)? LoadImage(out string? error)
    {
        error = null;
        try
        {
            if (EmbeddedImage != null) return NB.Core.Textures.ImageIO.Load(EmbeddedImage, TexturePath ?? "embedded.png");
            if (TexturePath == null) return null;
            var f = ResolveTextureFile();
            if (f == null) { error = $"texture file not found: {TexturePath}"; return null; }
            if (Path.GetExtension(f).Equals(".dds", StringComparison.OrdinalIgnoreCase)) { error = $"{Path.GetFileName(f)}: DDS is not supported, save the texture as PNG/TGA/JPG"; return null; }
            return NB.Core.Textures.ImageIO.Load(f);
        }
        catch (Exception e) { error = $"{TexturePath}: {e.Message}"; return null; }
    }
}

/// <summary>
/// Wavefront OBJ reader. OBJ and game space are both right-handed, Y up, counter-clockwise front faces, so positions,
/// normals and winding are taken unchanged; only V is flipped — an exported model re-imports to its original coordinates.
/// Each "g"/"o"/"usemtl" section becomes one mesh; polygons are fan-triangulated; vertices are de-duplicated per
/// (position, uv, normal) triple.
/// </summary>
public static class ObjReader
{
    /// <summary>Reads .obj or .fbx (binary) into import meshes.</summary>
    public static List<ImportMesh> ReadAny(string path) =>
        path.EndsWith(".fbx", StringComparison.OrdinalIgnoreCase) ? FbxReader.Read(path) : Read(path);

    public static List<ImportMesh> Read(string path)
    {
        var P = new List<Vector3>(); var T = new List<Vector2>(); var N = new List<Vector3>(); var X = new List<Vector2>();
        var meshes = new List<ImportMesh>();
        ImportMesh? cur = null; Dictionary<(int, int, int), int>? map = null;
        static float F(string s) => float.Parse(s, NumberStyles.Float, CultureInfo.InvariantCulture);
        string dir = Path.GetDirectoryName(Path.GetFullPath(path)) ?? "";
        var mats = new Dictionary<string, ImportMaterial>();
        ImportMaterial? curMat = null;
        void Start(string name)
        {
            if (cur != null && cur.Triangles.Count == 0) { cur.Name = name; cur.Material = curMat; return; }
            cur = new ImportMesh { Name = name, Material = curMat }; map = new(); meshes.Add(cur);
        }
        foreach (var raw in File.ReadLines(path))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line[0] == '#') continue;
            var t = line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            switch (t[0])
            {
                case "v": P.Add(new Vector3(F(t[1]), F(t[2]), F(t[3]))); break;
                case "vt": T.Add(new Vector2(F(t[1]), 1 - F(t[2]))); break;
                case "vn": N.Add(new Vector3(F(t[1]), F(t[2]), F(t[3]))); break;
                case "vx": X.Add(new Vector2(F(t[1]), F(t[2]))); break;   // NB extension: second UV set per position
                case "mtllib": foreach (var m in ReadMtl(Path.Combine(dir, line[6..].Trim()), dir)) mats[m.Name] = m; break;
                case "usemtl":
                {
                    string mn = t.Length > 1 ? string.Join(' ', t[1..]) : t[0];
                    curMat = mats.TryGetValue(mn, out var mm) ? mm : new ImportMaterial { Name = mn, BaseDir = dir };
                    Start(mn); break;
                }
                case "g" or "o": Start(t.Length > 1 ? string.Join(' ', t[1..]) : t[0]); break;
                case "f":
                {
                    if (cur == null) Start("mesh");
                    var poly = new List<int>();
                    for (int i = 1; i < t.Length; i++)
                    {
                        var p = t[i].Split('/');
                        int Idx(string s, int count) { int v = int.Parse(s, CultureInfo.InvariantCulture); return v < 0 ? count + v : v - 1; }
                        int pi = Idx(p[0], P.Count);
                        int ti = p.Length > 1 && p[1].Length > 0 ? Idx(p[1], T.Count) : -1;
                        int ni = p.Length > 2 && p[2].Length > 0 ? Idx(p[2], N.Count) : -1;
                        if (!map!.TryGetValue((pi, ti, ni), out int vi))
                        {
                            vi = cur!.Positions.Count; map[(pi, ti, ni)] = vi;
                            cur.Positions.Add(P[pi]);
                            if (ti >= 0) (cur.UVs ??= new()).Add(T[ti]);
                            if (ni >= 0) (cur.Normals ??= new()).Add(N[ni]);
                            if (pi < X.Count) (cur.UVs2 ??= new()).Add(X[pi]);
                        }
                        poly.Add(vi);
                    }
                    // fan triangulation, winding kept (OBJ and the game both use counter-clockwise front faces)
                    for (int i = 1; i + 1 < poly.Count; i++) { cur!.Triangles.Add(poly[0]); cur.Triangles.Add(poly[i]); cur.Triangles.Add(poly[i + 1]); }
                    break;
                }
            }
        }
        meshes.RemoveAll(m => m.Triangles.Count == 0);
        foreach (var m in meshes)
        {
            if (m.UVs != null && m.UVs.Count != m.Positions.Count) m.UVs = null;       // mixed faces with/without uv
            if (m.Normals != null && m.Normals.Count != m.Positions.Count) m.Normals = null;
            if (m.UVs2 != null && m.UVs2.Count != m.Positions.Count) m.UVs2 = null;
            m.Normals ??= ComputeNormals(m);
        }
        return meshes;
    }

    /// <summary>Materials of an MTL file: name, map_Kd (options such as "-s 1 1 1" skipped; Blender escapes each
    /// backslash of the path as two) and Kd. A missing MTL file gives no materials.</summary>
    public static List<ImportMaterial> ReadMtl(string mtlPath, string baseDir)
    {
        var res = new List<ImportMaterial>();
        if (!File.Exists(mtlPath)) return res;
        ImportMaterial? m = null;
        foreach (var raw in File.ReadLines(mtlPath))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line[0] == '#') continue;
            int sp = line.IndexOfAny(new[] { ' ', '\t' });
            string key = sp < 0 ? line : line[..sp], rest = sp < 0 ? "" : line[(sp + 1)..].Trim();
            switch (key)
            {
                case "newmtl": m = new ImportMaterial { Name = rest, BaseDir = Path.GetDirectoryName(Path.GetFullPath(mtlPath)) ?? baseDir }; res.Add(m); break;
                case "Kd" when m != null:
                {
                    var v = rest.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                    if (v.Length >= 3) m.Color = new Vector3(F(v[0]), F(v[1]), F(v[2]));
                    break;
                }
                case "map_Kd" when m != null:
                {
                    // skip options: -blendu/-blendv/-clamp/-cc/-imfchan/-texres/-bm take 1 value, -mm 2, -o/-s/-t 3
                    var tok = rest.Split(' ', StringSplitOptions.RemoveEmptyEntries).ToList();
                    int i = 0;
                    while (i < tok.Count && tok[i].StartsWith('-'))
                        i += 1 + (tok[i] switch { "-o" or "-s" or "-t" => 3, "-mm" => 2, _ => 1 });
                    if (i < tok.Count) m.TexturePath = string.Join(' ', tok.Skip(i)).Replace("\\\\", "\\");
                    break;
                }
            }
        }
        return res;
        static float F(string s) => float.Parse(s, NumberStyles.Float, CultureInfo.InvariantCulture);
    }

    public static List<Vector3> ComputeNormals(ImportMesh m)
    {
        var n = new Vector3[m.Positions.Count];
        for (int i = 0; i + 2 < m.Triangles.Count; i += 3)
        {
            int a = m.Triangles[i], b = m.Triangles[i + 1], c = m.Triangles[i + 2];
            var fn = Vector3.Cross(m.Positions[b] - m.Positions[a], m.Positions[c] - m.Positions[a]);
            n[a] += fn; n[b] += fn; n[c] += fn;
        }
        return n.Select(v => v.LengthSquared() > 1e-20f ? Vector3.Normalize(v) : Vector3.UnitY).ToList();
    }
}
