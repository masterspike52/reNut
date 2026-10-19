using System.Numerics;
using NB.Core.IO;

namespace NB.Core.Parts;

/// <summary>
/// Footprint and attachment points of a vehicle part: aid_avatarhavokdata_banjox_vehicleblock_* (objparams +0x12C;
/// resident next to the part models and streamed from Bundle/50/685374).
/// <para>Layout (verified on 63 shipped parts): +0 u32 version (14), +4 u32 cell count, +8..+0x1C s32 cell bounds
/// xmin, ymin, zmin, xmax, ymax, zmax (cells, inclusive; one cell = one garage grid unit). Then per-cell data and one
/// 0x28-byte record per outer face of the footprint: s32 cell x, y, z, u32 direction (0 +Y, 1 +Z, 2 -Y, 3 -Z, 4 -X,
/// 5 +X), f32 x, y, z (face centre, cell units, so one coordinate is ±0.5 off the grid), u32 attachable (1 = another part
/// can attach there), s32 neighbour record (-1 none), f32 2.0. After
/// the records: mass/inertia data.</para>
/// Examples: a 1×1×1 cube has 6 records, all attachable; Spirit of Pants only its bottom face; the Energy Shield
/// footprint is 3×1×3 (9 cells).
/// </summary>
public sealed class AttachData
{
    public sealed class Point
    {
        public int Offset;
        public Vector3 Position;
        public bool Attachable;
        /// <summary>Outward face direction (the coordinate that is off the cell grid).</summary>
        public Vector3 Normal;
        public override string ToString() => $"({Position.X:0.#}, {Position.Y:0.#}, {Position.Z:0.#}) {(Attachable ? "attach" : "-")}";
    }

    public int Version, Cells;
    public int XMin, YMin, ZMin, XMax, YMax, ZMax;
    public readonly List<Point> Points = new();
    public (int X, int Y, int Z) Size => (XMax - XMin + 1, YMax - YMin + 1, ZMax - ZMin + 1);

    /// <summary>Face direction codes of the face records (verified on the L-shaped large engine: the record's cell and code
    /// give the outward normal; the old guess from the footprint centre gave (0, 0, 0) for its inner corner faces).</summary>
    static readonly Vector3[] Dirs = { Vector3.UnitY, Vector3.UnitZ, -Vector3.UnitY, -Vector3.UnitZ, -Vector3.UnitX, Vector3.UnitX };

    static bool Half(float v) => MathF.Abs(v * 2 - MathF.Round(v * 2)) < 1e-4f && MathF.Abs(v) < 64;
    static bool OffGrid(float v) => MathF.Abs(MathF.Abs(v - MathF.Floor(v)) - 0.5f) < 1e-4f;

    public static AttachData Parse(byte[] d)
    {
        if (d.Length < 0x40) throw new InvalidDataException("attach data too short");
        var a = new AttachData
        {
            Version = BE.S32(d, 0), Cells = BE.S32(d, 4),
            XMin = BE.S32(d, 8), YMin = BE.S32(d, 12), ZMin = BE.S32(d, 16), XMax = BE.S32(d, 20), YMax = BE.S32(d, 24), ZMax = BE.S32(d, 28),
        };
        for (int o = 0x40; o + 0x28 <= d.Length; o += 4)
        {
            if (BE.U32(d, o + 0x14) != 0x40000000) continue;                       // f32 2.0
            uint flag = BE.U32(d, o + 0xC); if (flag > 1) continue;
            float x = BE.F32(d, o), y = BE.F32(d, o + 4), z = BE.F32(d, o + 8);
            if (!Half(x) || !Half(y) || !Half(z)) continue;
            int off = (OffGrid(x) ? 1 : 0) + (OffGrid(y) ? 1 : 0) + (OffGrid(z) ? 1 : 0);
            if (off != 1) continue;                                                 // a face centre: exactly one half coordinate
            var p = new Vector3(x, y, z);
            // the record starts 0x10 earlier: s32 cell x, y, z and u32 direction (0 +Y, 1 +Z, 2 -Y, 3 -Z, 4 -X, 5 +X)
            uint dir = o >= 0x50 ? BE.U32(d, o - 4) : 99;
            var n = dir < 6 ? Dirs[dir]
                  : OffGrid(x) ? new Vector3(MathF.Sign(x - (a.XMin + a.XMax) / 2f), 0, 0)
                  : OffGrid(y) ? new Vector3(0, MathF.Sign(y - (a.YMin + a.YMax) / 2f), 0)
                  : new Vector3(0, 0, MathF.Sign(z - (a.ZMin + a.ZMax) / 2f));
            a.Points.Add(new Point { Offset = o, Position = p, Attachable = flag == 1, Normal = n });
            o += 0x24;                                                              // next record
        }
        return a;
    }

    /// <summary>Writes the attachable flags back into <paramref name="d"/> (same layout it was parsed from).</summary>
    public void WriteFlags(byte[] d)
    {
        foreach (var p in Points) BE.W32(d, p.Offset + 0xC, p.Attachable ? 1u : 0u);
    }

    /// <summary>Sets flags from a rule: "all", "none", "bottom", "top", "sides", or a list of face centres "x,y,z;x,y,z".</summary>
    public void SetAttachable(string rule)
    {
        rule = rule.Trim().ToLowerInvariant();
        if (rule is "all" or "none" or "bottom" or "top" or "sides" or "bottom+sides")
        {
            foreach (var p in Points)
                p.Attachable = rule switch
                {
                    "all" => true, "none" => false,
                    "bottom" => p.Normal.Y < 0, "top" => p.Normal.Y > 0,
                    "sides" => p.Normal.Y == 0, _ => p.Normal.Y <= 0,
                };
            return;
        }
        var want = rule.Split(';', StringSplitOptions.RemoveEmptyEntries).Select(s =>
        {
            var c = s.Split(',').Select(v => float.Parse(v, System.Globalization.CultureInfo.InvariantCulture)).ToArray();
            return new Vector3(c[0], c[1], c[2]);
        }).ToList();
        foreach (var p in Points) p.Attachable = want.Any(w => Vector3.Distance(w, p.Position) < 1e-3f);
    }

    public override string ToString() =>
        $"{Size.X}x{Size.Y}x{Size.Z} ({Cells} cells, bounds {XMin},{YMin},{ZMin}..{XMax},{YMax},{ZMax}), {Points.Count} faces, {Points.Count(p => p.Attachable)} attachable";
}
