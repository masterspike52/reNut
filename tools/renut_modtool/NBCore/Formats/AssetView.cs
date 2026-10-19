using NB.Core.IO;

namespace NB.Core.Formats;

/// <summary>
/// One asset (symbol) of a CAFF with its parts and a resolved pointer map. A pointer is identified by
/// (part id, offset) and targets another part (possibly of another asset, e.g. the bundle's shared "pool").
/// </summary>
public sealed class AssetView
{
    public readonly CaffFile Caff;
    public readonly int Symbol;
    public string Name => Caff.Symbols[Symbol - 1];
    public readonly Dictionary<string, int> PartBySection = new();
    /// <summary>(fromPart, offset) → toPart for pointers stored in this asset's parts.</summary>
    public readonly Dictionary<(int Part, int Offset), int> Pointers = new();

    public AssetView(CaffFile caff, int symbol, Dictionary<int, List<(int Offset, int To)>>? relocIndex = null)
    {
        Caff = caff; Symbol = symbol;
        for (int i = 0; i < caff.Parts.Count; i++)
            if (caff.Parts[i].Symbol == symbol) PartBySection[caff.SectionOf(caff.Parts[i]).Name] = i + 1;
        relocIndex ??= BuildRelocIndex(caff);
        foreach (var pid in PartBySection.Values)
            if (relocIndex.TryGetValue(pid, out var list))
                foreach (var (off, to) in list) Pointers[(pid, off)] = to;
    }

    /// <summary>Index of relocations by source part (build once per CAFF when viewing many assets).</summary>
    public static Dictionary<int, List<(int Offset, int To)>> BuildRelocIndex(CaffFile caff)
    {
        var idx = new Dictionary<int, List<(int, int)>>();
        foreach (var r in caff.Relocs)
        {
            if (!idx.TryGetValue(r.FromPart, out var l)) idx[r.FromPart] = l = new();
            foreach (var o in r.Offsets) l.Add((o, r.ToPart));
        }
        return idx;
    }

    public bool Has(string section) => PartBySection.ContainsKey(section);
    public int PartId(string section) => PartBySection[section];
    public CaffPart Part(int partId) => Caff.Parts[partId - 1];
    public byte[] Data(string section) => Part(PartBySection[section]).Data;
    public string SectionOfPart(int partId) => Caff.SectionOf(Part(partId)).Name;
    public bool IsOwnPart(int partId) => Part(partId).Symbol == Symbol;

    /// <summary>Resolves the pointer stored at <paramref name="offset"/> of the given section's part.</summary>
    public Ptr? PtrAt(string section, int offset)
    {
        int pid = PartBySection[section];
        if (!Pointers.TryGetValue((pid, offset), out int to)) return null;
        return new Ptr(to, BE.S32(Part(pid).Data, offset));
    }

    public readonly record struct Ptr(int Part, int Offset);
}
