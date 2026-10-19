using System.Numerics;
using NB.Core.IO;

namespace NB.Core.Havok;

/// <summary>
/// In-place edits of convex collision shapes (vehicle part collision assets aid_havok_banjox_vehicleparts_* are one
/// hkpConvexVerticesShape): the vertices (rotatedVertices, FourVectors blocks of x[4] y[4] z[4]), the plane equations and
/// the AABB are transformed by a per-axis scale and an offset. The packfile layout does not change, so the asset needs
/// no relocation changes.
/// </summary>
public static class HkConvexEdit
{
    /// <summary>Bounds of all convex vertices in the asset (null if it has no convex vertices shape).</summary>
    public static (Vector3 Min, Vector3 Max)? Bounds(byte[] asset)
    {
        if (!HkPackfile.IsPackfileAsset(asset)) return null;
        var pf = HkPackfile.FromAsset(asset);
        var mn = new Vector3(float.MaxValue); var mx = new Vector3(float.MinValue); bool any = false;
        foreach (var s in pf.ObjectsOf("hkpConvexVerticesShape"))
            foreach (var v in Vertices(pf, s)) { mn = Vector3.Min(mn, v); mx = Vector3.Max(mx, v); any = true; }
        return any ? (mn, mx) : null;
    }

    static IEnumerable<Vector3> Vertices(HkPackfile pf, HkObject s)
    {
        int n = s.I("numVertices"); var (vp, _) = s.Array("rotatedVertices");
        if (vp == null) yield break;
        var b = pf.SectionBytes(vp.Value.Section);
        for (int i = 0; i < n; i++)
        {
            int blk = vp.Value.Offset + 0x30 * (i / 4), k = i % 4;
            yield return new Vector3(BE.F32(b, blk + 4 * k), BE.F32(b, blk + 16 + 4 * k), BE.F32(b, blk + 32 + 4 * k));
        }
    }

    /// <summary>Applies x' = scale * x + offset to every convex vertices shape of the asset (written into <paramref name="asset"/>).
    /// Returns the number of shapes changed.</summary>
    public static int Transform(byte[] asset, Vector3 scale, Vector3 offset)
    {
        if (!HkPackfile.IsPackfileAsset(asset)) return 0;
        int pack = BE.S32(asset, 0x20);
        var pf = HkPackfile.FromAsset(asset);
        int Abs(int section, int off) => pack + pf.Sections[section].Start + off;
        int count = 0;
        foreach (var s in pf.ObjectsOf("hkpConvexVerticesShape"))
        {
            int n = s.I("numVertices");
            var (vp, _) = s.Array("rotatedVertices");
            if (vp != null)
                for (int blk = 0; blk < (n + 3) / 4; blk++)
                    for (int k = 0; k < 4; k++)
                    {
                        int o = Abs(vp.Value.Section, vp.Value.Offset + 0x30 * blk);
                        BE.WF32(asset, o + 4 * k, BE.F32(asset, o + 4 * k) * scale.X + offset.X);
                        BE.WF32(asset, o + 16 + 4 * k, BE.F32(asset, o + 16 + 4 * k) * scale.Y + offset.Y);
                        BE.WF32(asset, o + 32 + 4 * k, BE.F32(asset, o + 32 + 4 * k) * scale.Z + offset.Z);
                    }
            var (pp, pn) = s.Array("planeEquations");
            if (pp != null)
                for (int f = 0; f < pn; f++)
                {
                    int o = Abs(pp.Value.Section, pp.Value.Offset + 16 * f);
                    var nrm = new Vector3(BE.F32(asset, o), BE.F32(asset, o + 4), BE.F32(asset, o + 8)); float d = BE.F32(asset, o + 12);
                    // n·v + d = 0 with v = (x - t) / s  →  (n / s)·x + d - (n / s)·t = 0, then renormalise
                    var n2 = nrm / scale; float d2 = d - Vector3.Dot(n2, offset); float len = n2.Length();
                    if (len < 1e-9f) continue;
                    n2 /= len; d2 /= len;
                    BE.WF32(asset, o, n2.X); BE.WF32(asset, o + 4, n2.Y); BE.WF32(asset, o + 8, n2.Z); BE.WF32(asset, o + 12, d2);
                }
            foreach (var (field, isCentre) in new[] { ("aabbHalfExtents", false), ("aabbCenter", true) })
            {
                if (!s.Has(field)) continue;
                var v = s.V4(field);
                var w = isCentre ? new Vector3(v.X, v.Y, v.Z) * scale + offset : new Vector3(v.X, v.Y, v.Z) * Vector3.Abs(scale);
                int o = Abs(s.Section, s.Offset + s.Class!.Find(field)!.Offset);
                BE.WF32(asset, o, w.X); BE.WF32(asset, o + 4, w.Y); BE.WF32(asset, o + 8, w.Z);
            }
            count++;
        }
        return count;
    }

    /// <summary>Scales/moves the asset's convex shapes so their bounds become (<paramref name="min"/>, <paramref name="max"/>).</summary>
    public static int FitTo(byte[] asset, Vector3 min, Vector3 max)
    {
        var b = Bounds(asset); if (b == null) return 0;
        var (a0, a1) = b.Value; var size = Vector3.Max(a1 - a0, new Vector3(1e-4f));
        var scale = (max - min) / size; var offset = min - a0 * scale;
        return Transform(asset, scale, offset);
    }
}
