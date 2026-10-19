using System.IO.Compression;
using System.Numerics;
using System.Text;

namespace NB.Core.Models;

/// <summary>One animation curve: key times (FBX KTime) and values, evaluated linearly (constant outside the range).</summary>
public sealed class FbxCurve
{
    public readonly long[] Times; public readonly double[] Values;
    public FbxCurve(long[] times, double[] values) { Times = times; Values = values; }
    public double Eval(long t)
    {
        if (t <= Times[0]) return Values[0];
        if (t >= Times[^1]) return Values[^1];
        int i = Array.BinarySearch(Times, t);
        if (i >= 0) return Values[i];
        i = ~i;   // Times[i-1] < t < Times[i]
        long t0 = Times[i - 1], t1 = Times[i];
        // snap to a key within 1/1000 frame (exporters round frame times differently)
        long tol = FbxReader.KTimePerSecond / 30000;
        if (t - t0 <= tol) return Values[i - 1];
        if (t1 - t <= tol) return Values[i];
        double u = (double)(t - t0) / (t1 - t0);
        return Values[i - 1] + (Values[i] - Values[i - 1]) * u;
    }
}

/// <summary>A Model of an FBX file with its static local transform and the animation curves of one take.</summary>
public sealed class FbxModelAnim
{
    public string Name = "", Type = "";
    public string? Parent;
    public Vector3 T, R, S = Vector3.One, PreRotation, PostRotation;
    public int RotationOrder;
    public bool Animated;
    public readonly FbxCurve?[] TCurves = new FbxCurve?[3], RCurves = new FbxCurve?[3], SCurves = new FbxCurve?[3];

    static Vector3 Eval(FbxCurve?[] c, Vector3 still, long t) =>
        new((float)(c[0]?.Eval(t) ?? still.X), (float)(c[1]?.Eval(t) ?? still.Y), (float)(c[2]?.Eval(t) ?? still.Z));
    public Vector3 Translation(long t) => Eval(TCurves, T, t);
    public Vector3 EulerDegrees(long t) => Eval(RCurves, R, t);
    public Vector3 Scaling(long t) => Eval(SCurves, S, t);
}

/// <summary>An animation take read by <see cref="FbxReader.ReadAnimation"/>; models by name.</summary>
public sealed class FbxAnimation
{
    public string Name = "";
    public List<string> Stacks = new();
    public long LocalStart, LocalStop;
    public double UnitScale = 1;
    public readonly Dictionary<string, FbxModelAnim> Models = new();
}

/// <summary>
/// Binary FBX (7.x) mesh reader for model import: every Geometry becomes one <see cref="ImportMesh"/> per material
/// (named after the material, with its diffuse texture path / embedded image and colour; polygons fan-triangulated;
/// normals/UVs by polygon vertex, control point or polygon, direct or indexed; zlib-compressed arrays supported).
/// Vertices get the model's global transform (parents, Lcl/Pre/Post rotation, geometric offset; pivots ignored) and the
/// file's UnitScaleFactor to metres; axes are kept (FBX Y-up and game space are both right-handed), V flipped.
/// </summary>
public static class FbxReader
{
    sealed class Node { public string Name = ""; public List<object> Props = new(); public List<Node> Children = new(); public Node? Find(string n) => Children.FirstOrDefault(c => c.Name == n); }

    static List<Node> LoadRoot(string path)
    {
        var d = File.ReadAllBytes(path);
        if (d.Length < 27 || Encoding.ASCII.GetString(d, 0, 18) != "Kaydara FBX Binary") throw new InvalidDataException("only binary FBX is supported (Blender and Autodesk tools write binary by default)");
        uint version = BitConverter.ToUInt32(d, 23);
        bool wide = version >= 7500;
        int pos = 27;
        var root = new List<Node>();
        while (true) { var n = ReadNode(d, ref pos, wide); if (n == null) break; root.Add(n); }
        return root;
    }

    /// <summary>GlobalSettings UnitScaleFactor / 100 (file units → metres; our exporter writes 100 = 1 unit per metre).</summary>
    static double UnitScale(List<Node> root)
    {
        double unit = 1;
        var gs = root.FirstOrDefault(n => n.Name == "GlobalSettings")?.Find("Properties70");
        foreach (var p in gs?.Children ?? new())
            if (p.Props.Count > 4 && p.Props[0] is string s && s == "UnitScaleFactor") unit = Convert.ToDouble(p.Props[4]) / 100.0;
        return unit;
    }

    static string ObjectName(Node n) => n.Props.Count > 1 && n.Props[1] is string s ? s.Split('\0')[0] : "";

    public static List<ImportMesh> Read(string path)
    {
        var root = LoadRoot(path);
        double unit = UnitScale(root);
        string baseDir = Path.GetDirectoryName(Path.GetFullPath(path)) ?? "";
        var meshes = new List<ImportMesh>();
        var objects = root.FirstOrDefault(n => n.Name == "Objects")?.Children ?? new();
        var byId = objects.Where(o => o.Props.Count > 0 && o.Props[0] is long).GroupBy(o => (long)o.Props[0]).ToDictionary(g => g.Key, g => g.First());
        var conns = (root.FirstOrDefault(n => n.Name == "Connections")?.Children ?? new())
            .Where(c => c.Props.Count >= 3 && c.Props[0] is string && c.Props[1] is long && c.Props[2] is long)
            .Select(c => (Type: (string)c.Props[0], Child: (long)c.Props[1], Parent: (long)c.Props[2], Prop: c.Props.Count > 3 ? c.Props[3] as string : null))
            .ToList();
        string KindOf(long id) => byId.TryGetValue(id, out var n) ? n.Name : "";
        // geometry → model, model → parent model
        var geomToModel = new Dictionary<long, long>();
        var parentOf = new Dictionary<long, long>();
        foreach (var c in conns.Where(c => c.Type == "OO"))
        {
            if (KindOf(c.Child) == "Geometry" && KindOf(c.Parent) == "Model") geomToModel.TryAdd(c.Child, c.Parent);
            else if (KindOf(c.Child) == "Model" && KindOf(c.Parent) == "Model") parentOf.TryAdd(c.Child, c.Parent);
        }
        // local transform of a model: S · Rpost⁻¹ · R · Rpre · T (row vectors; FBX rotation order XYZ, pivots ignored)
        static Vector3 P3(Node m, string name, Vector3 def)
        {
            foreach (var p in m.Find("Properties70")?.Children ?? new())
                if (p.Props.Count >= 7 && p.Props[0] is string pn && pn == name)
                    return new Vector3((float)Convert.ToDouble(p.Props[4]), (float)Convert.ToDouble(p.Props[5]), (float)Convert.ToDouble(p.Props[6]));
            return def;
        }
        static Matrix4x4 Rot(Vector3 deg) =>
            Matrix4x4.CreateRotationX(deg.X * MathF.PI / 180) * Matrix4x4.CreateRotationY(deg.Y * MathF.PI / 180) * Matrix4x4.CreateRotationZ(deg.Z * MathF.PI / 180);
        Matrix4x4 Local(Node m)
        {
            Matrix4x4.Invert(Rot(P3(m, "PostRotation", Vector3.Zero)), out var postInv);
            return Matrix4x4.CreateScale(P3(m, "Lcl Scaling", Vector3.One)) * postInv * Rot(P3(m, "Lcl Rotation", Vector3.Zero)) *
                   Rot(P3(m, "PreRotation", Vector3.Zero)) * Matrix4x4.CreateTranslation(P3(m, "Lcl Translation", Vector3.Zero));
        }
        Matrix4x4 Global(long model)
        {
            var xf = Matrix4x4.Identity;
            long id = model;
            for (int depth = 0; depth < 64 && byId.TryGetValue(id, out var m); depth++)
            {
                xf *= Local(m);
                if (!parentOf.TryGetValue(id, out id)) break;
            }
            return xf;
        }
        // materials: per model in connection order (LayerElementMaterial indexes this list)
        var materials = new Dictionary<long, ImportMaterial>();
        static bool NotColour(string p)
        {
            p = p.ToLowerInvariant();
            return p.Contains("normal") || p.Contains("bump") || p.Contains("spec") || p.Contains("emiss") || p.Contains("rough") || p.Contains("metal") ||
                   p.Contains("gloss") || p.Contains("reflect") || p.Contains("transparen") || p.Contains("opacity") || p.Contains("shin") || p.Contains("displace");
        }
        static string? Str(Node n, string child) => n.Find(child)?.Props.FirstOrDefault() is string v && v.Length > 0 ? v : null;
        ImportMaterial MaterialOf(long id)
        {
            if (materials.TryGetValue(id, out var im)) return im;
            var node = byId[id];
            im = new ImportMaterial { Name = ObjectName(node), BaseDir = baseDir, Color = P3(node, "DiffuseColor", P3(node, "Diffuse", Vector3.One)) };
            // the texture on the base-colour channel (Blender/Maya/Max property names differ); any colour-like link as a fallback
            var texLinks = conns.Where(c => c.Parent == id && KindOf(c.Child) == "Texture").ToList();
            var pick = texLinks.FirstOrDefault(c => c.Prop != null && c.Prop.Contains("Diffuse", StringComparison.OrdinalIgnoreCase) && !NotColour(c.Prop));
            if (pick.Type == null) pick = texLinks.FirstOrDefault(c => c.Prop != null && (c.Prop.Contains("base", StringComparison.OrdinalIgnoreCase) || c.Prop.Contains("color", StringComparison.OrdinalIgnoreCase) || c.Prop.Contains("albedo", StringComparison.OrdinalIgnoreCase)) && !NotColour(c.Prop));
            if (pick.Type == null) pick = texLinks.FirstOrDefault(c => c.Prop == null || !NotColour(c.Prop));
            if (pick.Type != null && byId.TryGetValue(pick.Child, out var tex))
            {
                im.TexturePath = Str(tex, "RelativeFilename") is string rel && File.Exists(Path.Combine(baseDir, rel)) ? rel : Str(tex, "FileName") ?? Str(tex, "RelativeFilename");
                // embedded image: a Video linked to the texture with Content bytes
                foreach (var vc in conns.Where(c => c.Parent == pick.Child && KindOf(c.Child) == "Video"))
                {
                    var video = byId[vc.Child];
                    if (video.Find("Content")?.Props.FirstOrDefault() is byte[] content && content.Length > 16)
                    {
                        im.EmbeddedImage = content;
                        im.TexturePath ??= Str(video, "RelativeFilename") ?? Str(video, "Filename") ?? "embedded.png";
                    }
                    else im.TexturePath ??= Str(video, "RelativeFilename") ?? Str(video, "Filename");
                }
            }
            return materials[id] = im;
        }
        foreach (var g in objects.Where(c => c.Name == "Geometry"))
        {
            var vtx = g.Find("Vertices")?.Props[0] as double[]; var pvi = g.Find("PolygonVertexIndex")?.Props[0] as int[];
            if (vtx == null || pvi == null) continue;
            long gid = g.Props.Count > 0 && g.Props[0] is long gl ? gl : -1;
            long modelId = geomToModel.TryGetValue(gid, out var mid) ? mid : -1;
            var xf = Matrix4x4.Identity;
            if (modelId >= 0 && byId.TryGetValue(modelId, out var modelNode))
                xf = Matrix4x4.CreateScale(P3(modelNode, "GeometricScaling", Vector3.One)) * Rot(P3(modelNode, "GeometricRotation", Vector3.Zero)) *
                     Matrix4x4.CreateTranslation(P3(modelNode, "GeometricTranslation", Vector3.Zero)) * Global(modelId);
            bool mirror = xf.GetDeterminant() < 0;
            Matrix4x4.Invert(xf, out var inv); var nxf = Matrix4x4.Transpose(inv);
            var modelMats = modelId < 0 ? new List<ImportMaterial>() :
                conns.Where(c => c.Type == "OO" && c.Parent == modelId && KindOf(c.Child) == "Material").Select(c => MaterialOf(c.Child)).ToList();
            string geomName = g.Props.Count > 1 ? ((string)g.Props[1]).Split('\0')[0] : "mesh";
            var nrmEl = g.Find("LayerElementNormal"); var uvEl = g.Find("LayerElementUV"); var matEl = g.Find("LayerElementMaterial");
            double[]? nrm = nrmEl?.Find("Normals")?.Props[0] as double[];
            int[]? nrmIdx = nrmEl?.Find("NormalsIndex")?.Props[0] as int[];
            string nrmMap = nrmEl?.Find("MappingInformationType")?.Props[0] as string ?? "";
            double[]? uv = uvEl?.Find("UV")?.Props[0] as double[];
            int[]? uvIdx = uvEl?.Find("UVIndex")?.Props[0] as int[];
            string uvMap = uvEl?.Find("MappingInformationType")?.Props[0] as string ?? "";
            int[]? polyMat = matEl?.Find("Materials")?.Props[0] as int[];
            string matMap = matEl?.Find("MappingInformationType")?.Props[0] as string ?? "";
            // one mesh per material of this geometry
            var perMat = new SortedDictionary<int, (ImportMesh Mesh, Dictionary<(int, int, int), int> Map)>();
            (ImportMesh, Dictionary<(int, int, int), int>) MeshFor(int mi)
            {
                if (perMat.TryGetValue(mi, out var e)) return e;
                var mat = mi >= 0 && mi < modelMats.Count ? modelMats[mi] : null;
                var mesh = new ImportMesh { Name = mat?.Name ?? geomName, Material = mat, Normals = nrm != null ? new() : null, UVs = uv != null ? new() : null };
                return perMat[mi] = (mesh, new());
            }
            static int Pick(string mapping, int[]? index, int vi, int c, int poly)
            {
                int k = mapping switch { "ByVertice" or "ByVertex" or "ByControlPoint" => vi, "ByPolygon" => poly, "AllSame" => 0, _ => c };
                return index != null ? index[k] : k;
            }
            var corners = new List<(int Vi, int Ni, int Ti)>();
            int polyIndex = 0;
            for (int c = 0; c < pvi.Length; c++)
            {
                int vi = pvi[c] < 0 ? ~pvi[c] : pvi[c];
                corners.Add((vi, nrm != null ? Pick(nrmMap, nrmIdx, vi, c, polyIndex) : -1, uv != null ? Pick(uvMap, uvIdx, vi, c, polyIndex) : -1));
                if (pvi[c] >= 0) continue;
                int mi = polyMat == null || polyMat.Length == 0 ? -1 : matMap == "AllSame" ? polyMat[0] : polyMat[Math.Min(polyIndex, polyMat.Length - 1)];
                var (mesh, map) = MeshFor(mi);
                var poly = new List<int>();
                foreach (var (pv, ni, ti) in corners)
                {
                    if (!map.TryGetValue((pv, ni, ti), out int outIndex))
                    {
                        outIndex = mesh.Positions.Count; map[(pv, ni, ti)] = outIndex;
                        var p = Vector3.Transform(new Vector3((float)vtx[3 * pv], (float)vtx[3 * pv + 1], (float)vtx[3 * pv + 2]), xf);
                        mesh.Positions.Add(p * (float)unit);
                        if (mesh.Normals != null) mesh.Normals.Add(Vector3.Normalize(Vector3.TransformNormal(new Vector3((float)nrm![3 * ni], (float)nrm[3 * ni + 1], (float)nrm[3 * ni + 2]), nxf) + new Vector3(1e-9f)));
                        if (mesh.UVs != null) mesh.UVs.Add(new Vector2((float)uv![2 * ti], 1 - (float)uv[2 * ti + 1]));
                    }
                    poly.Add(outIndex);
                }
                // fan triangulation; a mirroring transform (negative scale) reverses the winding
                for (int i = 1; i + 1 < poly.Count; i++)
                {
                    mesh.Triangles.Add(poly[0]);
                    if (mirror) { mesh.Triangles.Add(poly[i + 1]); mesh.Triangles.Add(poly[i]); }
                    else { mesh.Triangles.Add(poly[i]); mesh.Triangles.Add(poly[i + 1]); }
                }
                corners.Clear(); polyIndex++;
            }
            foreach (var (mesh, _) in perMat.Values)
            {
                mesh.Normals ??= ObjReader.ComputeNormals(mesh);
                if (mesh.Triangles.Count > 0) meshes.Add(mesh);
            }
        }
        return meshes;
    }

    // ------------------------------------------------------------------ animation

    /// <summary>FBX time units per second (KTime).</summary>
    public const long KTimePerSecond = 46186158000L;

    /// <summary>
    /// Reads one animation take: every Model (LimbNodes and others) with its static Lcl Translation / Rotation / Scaling,
    /// Pre/PostRotation, RotationOrder, parent, and the AnimationCurves of the chosen AnimationStack (first one, or the
    /// one named <paramref name="stackName"/>) connected through AnimationCurveNodes. Translations are scaled to metres
    /// (UnitScaleFactor / 100); axes are left in FBX space (see <see cref="AnimImport"/> for the game conversion).
    /// </summary>
    public static FbxAnimation ReadAnimation(string path, string? stackName = null)
    {
        var root = LoadRoot(path);
        double unit = UnitScale(root);
        var objects = root.FirstOrDefault(n => n.Name == "Objects")?.Children ?? new();
        var conns = (root.FirstOrDefault(n => n.Name == "Connections")?.Children ?? new())
            .Where(c => c.Props.Count >= 3 && c.Props[0] is string && c.Props[1] is long && c.Props[2] is long)
            .Select(c => (Type: (string)c.Props[0], Child: (long)c.Props[1], Parent: (long)c.Props[2], Prop: c.Props.Count > 3 ? c.Props[3] as string : null))
            .ToList();
        var byId = objects.Where(o => o.Props.Count > 0 && o.Props[0] is long).GroupBy(o => (long)o.Props[0]).ToDictionary(g => g.Key, g => g.First());

        var fa = new FbxAnimation { UnitScale = unit };
        var models = new Dictionary<long, FbxModelAnim>();
        foreach (var m in objects.Where(o => o.Name == "Model" && o.Props.Count > 0 && o.Props[0] is long))
        {
            var ma = new FbxModelAnim { Name = ObjectName(m), Type = m.Props.Count > 2 ? m.Props[2] as string ?? "" : "" };
            foreach (var p in m.Find("Properties70")?.Children ?? new())
            {
                if (p.Props.Count < 5 || p.Props[0] is not string pn) continue;
                if (pn == "RotationOrder") { ma.RotationOrder = Convert.ToInt32(p.Props[4]); continue; }
                if (p.Props.Count < 7) continue;
                var v = new Vector3((float)Convert.ToDouble(p.Props[4]), (float)Convert.ToDouble(p.Props[5]), (float)Convert.ToDouble(p.Props[6]));
                switch (pn)
                {
                    case "Lcl Translation": ma.T = v * (float)unit; break;
                    case "Lcl Rotation": ma.R = v; break;
                    case "Lcl Scaling": ma.S = v; break;
                    case "PreRotation": ma.PreRotation = v; break;
                    case "PostRotation": ma.PostRotation = v; break;
                }
            }
            models[(long)m.Props[0]] = ma;
        }
        foreach (var c in conns.Where(c => c.Type == "OO" && models.ContainsKey(c.Child) && models.ContainsKey(c.Parent)))
            models[c.Child].Parent = models[c.Parent].Name;

        // stack → layers → curve nodes
        var stacks = objects.Where(o => o.Name == "AnimationStack" && o.Props.Count > 0 && o.Props[0] is long).ToList();
        var stack = (stackName != null ? stacks.FirstOrDefault(s => ObjectName(s) == stackName) : null) ?? stacks.FirstOrDefault();
        if (stack != null)
        {
            fa.Name = ObjectName(stack);
            foreach (var p in stack.Find("Properties70")?.Children ?? new())
                if (p.Props.Count > 4 && p.Props[0] is string pn && p.Props[4] is long t)
                {
                    if (pn == "LocalStart") fa.LocalStart = t; else if (pn == "LocalStop") fa.LocalStop = t;
                }
            fa.Stacks = stacks.Select(ObjectName).ToList();
            long sid = (long)stack.Props[0];
            var layers = conns.Where(c => c.Parent == sid && byId.TryGetValue(c.Child, out var o) && o.Name == "AnimationLayer").Select(c => c.Child).ToHashSet();
            var curveNodes = conns.Where(c => layers.Contains(c.Parent) && byId.TryGetValue(c.Child, out var o) && o.Name == "AnimationCurveNode").Select(c => c.Child).ToHashSet();
            foreach (var cn in curveNodes)
            {
                var target = conns.FirstOrDefault(c => c.Child == cn && c.Type == "OP" && models.ContainsKey(c.Parent));
                if (target.Prop == null) continue;
                var ma = models[target.Parent];
                FbxCurve?[]? slot = target.Prop switch { "Lcl Translation" => ma.TCurves, "Lcl Rotation" => ma.RCurves, "Lcl Scaling" => ma.SCurves, _ => null };
                if (slot == null) continue;
                // curve-node defaults (d|X/d|Y/d|Z) act as constant curves when an axis has no AnimationCurve
                var defaults = new double?[3];
                foreach (var p in byId[cn].Find("Properties70")?.Children ?? new())
                    if (p.Props.Count > 4 && p.Props[0] is string pn && pn.Length == 3 && pn.StartsWith("d|") && "XYZ".IndexOf(pn[2]) is int ax && ax >= 0)
                        defaults[ax] = Convert.ToDouble(p.Props[4]);
                double scale = target.Prop == "Lcl Translation" ? unit : 1;
                foreach (var c in conns.Where(c => c.Parent == cn && c.Type == "OP" && c.Prop is "d|X" or "d|Y" or "d|Z"))
                {
                    if (!byId.TryGetValue(c.Child, out var cv) || cv.Name != "AnimationCurve") continue;
                    var times = cv.Find("KeyTime")?.Props[0] as long[];
                    var vals = cv.Find("KeyValueFloat")?.Props[0] as double[];
                    if (times == null || vals == null || times.Length == 0 || times.Length != vals.Length) continue;
                    int axis = c.Prop![2] - 'X';
                    slot[axis] = new FbxCurve(times, vals.Select(x => x * scale).ToArray());
                    defaults[axis] = null;
                }
                for (int a = 0; a < 3; a++)
                    if (defaults[a] is double dv && slot[a] == null) slot[a] = new FbxCurve(new[] { 0L }, new[] { dv * scale });
                ma.Animated = true;
            }
        }
        foreach (var m in models.Values) fa.Models.TryAdd(m.Name, m);
        return fa;
    }

    static Node? ReadNode(byte[] d, ref int pos, bool wide)
    {
        long end = wide ? (long)BitConverter.ToUInt64(d, pos) : BitConverter.ToUInt32(d, pos);
        long nprops = wide ? (long)BitConverter.ToUInt64(d, pos + 8) : BitConverter.ToUInt32(d, pos + 4);
        int hdr = wide ? 24 : 12;
        int nameLen = d[pos + hdr];
        if (end == 0) { pos += hdr + 1; return null; }
        var n = new Node { Name = Encoding.ASCII.GetString(d, pos + hdr + 1, nameLen) };
        int p = pos + hdr + 1 + nameLen;
        for (long i = 0; i < nprops; i++) n.Props.Add(ReadProp(d, ref p));
        while (p < end)
        {
            var c = ReadNode(d, ref p, wide);
            if (c == null) break;
            n.Children.Add(c);
        }
        pos = (int)end;
        return n;
    }

    static object ReadProp(byte[] d, ref int p)
    {
        char t = (char)d[p++];
        switch (t)
        {
            case 'C': return d[p++] != 0;
            case 'Y': { var v = BitConverter.ToInt16(d, p); p += 2; return (int)v; }
            case 'I': { var v = BitConverter.ToInt32(d, p); p += 4; return v; }
            case 'F': { var v = BitConverter.ToSingle(d, p); p += 4; return (double)v; }
            case 'D': { var v = BitConverter.ToDouble(d, p); p += 8; return v; }
            case 'L': { var v = BitConverter.ToInt64(d, p); p += 8; return v; }
            case 'S': case 'R': { int len = BitConverter.ToInt32(d, p); p += 4; var s = t == 'S' ? Encoding.UTF8.GetString(d, p, len) : (object)d[p..(p + len)]; p += len; return s; }
            case 'f': case 'd': case 'l': case 'i': case 'b':
            {
                int count = BitConverter.ToInt32(d, p), enc = BitConverter.ToInt32(d, p + 4), clen = BitConverter.ToInt32(d, p + 8); p += 12;
                byte[] raw = d[p..(p + clen)]; p += clen;
                if (enc == 1) { using var z = new ZLibStream(new MemoryStream(raw), CompressionMode.Decompress); using var ms = new MemoryStream(); z.CopyTo(ms); raw = ms.ToArray(); }
                return t switch
                {
                    'f' => Enumerable.Range(0, count).Select(i => (double)BitConverter.ToSingle(raw, 4 * i)).ToArray(),
                    'd' => Enumerable.Range(0, count).Select(i => BitConverter.ToDouble(raw, 8 * i)).ToArray(),
                    'i' => Enumerable.Range(0, count).Select(i => BitConverter.ToInt32(raw, 4 * i)).ToArray(),
                    'l' => Enumerable.Range(0, count).Select(i => BitConverter.ToInt64(raw, 8 * i)).ToArray(),
                    _ => raw,
                };
            }
            default: throw new InvalidDataException($"unknown FBX property type '{t}'");
        }
    }
}
