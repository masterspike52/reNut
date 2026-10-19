using System.Globalization;
using System.Numerics;
using System.Text;

namespace NB.Core.Models;

/// <summary>
/// Writes a model's draws as Wavefront OBJ + MTL. Game space is right-handed with Y up and counter-clockwise front
/// faces, the same as OBJ, so positions, normals and winding are written unchanged (verified against the game camera,
/// docs/FORMATS.md §17). V is flipped.
/// </summary>
public static class ObjExporter
{
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    /// <summary>Picks the texture used as diffuse colour for a draw. Without a colour texture: another plain texture
    /// (a decal), else the ambient-occlusion map, else the specular map, else a mask, else none (neutral grey). A normal /
    /// bump map is never a colour (it showed purple-blue: Banjoland's gear fence, sign posts, toilet roll …).</summary>
    public static string? DiffuseTexture(MeshDraw d)
    {
        var colour = d.Textures.FirstOrDefault(t => t.Texture.Contains("colour") || t.Texture.Contains("color") || t.Texture.Contains("diffuse"));
        if (colour.Texture != null) return colour.Texture;
        var bySlot = d.Textures.OrderBy(t => t.Slot).Select(t => t.Texture).ToList();
        string Kind(string t) => MaterialImport.Classify(TextureFileStem(t)) is var k && k == "other" && (t.Contains("reflectionmaps") || t.Contains("velvet") || t.Contains("sparkle")) ? "skip" : k;
        return bySlot.FirstOrDefault(t => Kind(t) == "other")
            ?? bySlot.FirstOrDefault(t => Kind(t) == "ao")
            ?? bySlot.FirstOrDefault(t => Kind(t) == "spec")
            ?? bySlot.FirstOrDefault(t => Kind(t) == "mask");
    }

    /// <summary>
    /// Layered materials: a base colour, and optionally a second colour texture blended over it by a "_trans" mask
    /// (Showdown Town's painted wood: slots 0 plainwood colour, 1 painted colour, 2 painted trans, 3 spec, 4 normal).
    /// </summary>
    public static (string? Base, string? Overlay, string? Mask) MaterialLayers(MeshDraw d)
    {
        static bool IsColour(string t) => (t.Contains("colour") || t.Contains("color") || t.Contains("diffuse")) && !t.Contains("normal") && !t.Contains("specular");
        var ordered = d.Textures.OrderBy(t => t.Slot).Select(t => t.Texture).ToList();
        var colours = ordered.Where(IsColour).Select(TextureFileStem).Distinct().ToList();
        string? baseTex = DiffuseTexture(d);
        if (colours.Count < 2) return (baseTex, null, null);
        string overlayStem = colours[1];
        string? overlay = ordered.First(t => TextureFileStem(t) == overlayStem);
        string? mask = ordered.FirstOrDefault(t => t.Contains("_trans") || t.Contains("_mask") || t.Contains("_alpha"));
        return mask == null ? (baseTex, null, null) : (baseTex, overlay, mask);
    }

    public static string TextureFileStem(string texName) =>
        (texName.EndsWith("mip") || texName.EndsWith("top")) ? texName[..^3] : texName;

    public static void Write(string objPath, string name, IEnumerable<(MeshDraw Draw, Matrix4x4 Transform)> draws, string textureExt = ".png")
    {
        var sb = new StringBuilder();
        var mtl = new StringBuilder();
        string mtlName = Path.GetFileNameWithoutExtension(objPath) + ".mtl";
        sb.AppendLine($"# {name} — exported by NB Mod Tool");
        sb.AppendLine($"mtllib {mtlName}");
        int vBase = 1, i = 0;
        var materials = new HashSet<string>();
        foreach (var (d, xf) in draws)
        {
            if (d.Positions.Length == 0 || d.Indices.Length < 3) continue;
            string? tex = DiffuseTexture(d);
            string mat = tex != null ? TextureFileStem(tex) : "untextured";
            if (materials.Add(mat))
            {
                mtl.AppendLine($"newmtl {mat}\nKd 1 1 1");
                if (tex != null) mtl.AppendLine($"map_Kd textures/{TextureFileStem(tex)}{textureExt}");
                foreach (var t in d.Textures) mtl.AppendLine($"# slot {t.Slot}: {t.Texture}");
                mtl.AppendLine();
            }
            sb.AppendLine($"o draw{i++}_vb{d.VbRecord:X}");
            sb.AppendLine($"usemtl {mat}");
            var nm = Matrix4x4.Transpose(Matrix4x4.Invert(xf, out var inv) ? inv : Matrix4x4.Identity);
            foreach (var p0 in d.Positions)
            {
                var p = Vector3.Transform(p0, xf);
                sb.Append("v ").Append(F(p.X)).Append(' ').Append(F(p.Y)).Append(' ').Append(F(p.Z)).Append('\n');
            }
            if (d.UVs != null) foreach (var t in d.UVs) sb.Append("vt ").Append(F(t.X)).Append(' ').Append(F(1 - t.Y)).Append('\n');
            if (d.Normals != null) foreach (var n0 in d.Normals) { var n = Vector3.Normalize(Vector3.TransformNormal(n0, xf)); sb.Append("vn ").Append(F(n.X)).Append(' ').Append(F(n.Y)).Append(' ').Append(F(n.Z)).Append('\n'); }
            for (int k = 0; k + 2 < d.Indices.Length; k += 3)
            {
                int a = d.Indices[k], b = d.Indices[k + 1], c = d.Indices[k + 2];
                if (a >= d.Positions.Length || b >= d.Positions.Length || c >= d.Positions.Length) continue;
                sb.Append("f ").Append(V(a, d)).Append(' ').Append(V(b, d)).Append(' ').Append(V(c, d)).Append('\n');
            }
            string V(int x, MeshDraw dd)
            {
                int v = x + vBase;
                if (dd.UVs != null && dd.Normals != null) return $"{v}/{v}/{v}";
                if (dd.UVs != null) return $"{v}/{v}";
                if (dd.Normals != null) return $"{v}//{v}";
                return v.ToString(Inv);
            }
            vBase += d.Positions.Length;
        }
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(objPath))!);
        File.WriteAllText(objPath, sb.ToString());
        File.WriteAllText(Path.Combine(Path.GetDirectoryName(Path.GetFullPath(objPath))!, mtlName), mtl.ToString());
    }

    static string F(float v) => v.ToString("0.######", Inv);
}
