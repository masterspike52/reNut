using NB.Core.Formats;
using NB.Core.IO;

namespace NB.Core.Textures;

/// <summary>
/// Creates new resident textures inside a bundle: a clone of a small full-size 2D texture (header + .texturegpu)
/// whose width, height and level count are rewritten, then encoded from an image (full mip chain, base level included,
/// so no streamed "top" asset is needed). Models reference it by exact name for both their mip and top entries
/// (<see cref="NB.Core.Models.ModelEdit.RetargetTexture"/> with exact = true).
/// Header fields: 0x24 u16 width, 0x26 u16 height, 0x2C 0xFFFFFFFF = GPU data starts with the base level, 0x30 u8 levels.
/// </summary>
public static class TextureFactory
{
    /// <summary>Finds a resident 2D texture carrying its base level plus a mip chain in the given format (a template).</summary>
    public static int FindTemplate(CaffFile caff, XenosFormat format, bool allowMipOnly = false)
    {
        int best = 0, bestSize = 0;
        for (int s = 1; s <= caff.Symbols.Count; s++)
        {
            var nm = AssetIds.DisplayName(caff.Symbols[s - 1]);
            if (!nm.StartsWith("aid_texture_")) continue;
            var cpu = caff.PartsOf(s).FirstOrDefault(p => caff.SectionOf(p).Name == ".data");
            var gpu = caff.PartsOf(s).FirstOrDefault(p => caff.SectionOf(p).Name == ".texturegpu");
            if (cpu == null || gpu == null || !TextureHeader.IsTexture(cpu.Data)) continue;
            var h = TextureHeader.Parse(cpu.Data);
            if (h.Kind != 0 || !h.HasBase || h.Format != format || !h.Tiled || h.Levels < 2) continue;
            var t = new TextureAsset(cpu.Data, gpu.Data);
            if (t.Levels.Count != h.Levels || t.ExpectedGpuSize != gpu.Data.Length) continue;   // must hold the whole chain
            if (h.Width * h.Height > bestSize) { best = s; bestSize = h.Width * h.Height; }
        }
        if (best != 0 || !allowMipOnly) return best;
        // fallback (bundles whose textures all stream their top level, e.g. the vehicle-parts bundle 4e967e): any tiled
        // 2D texture of the format; Create rewrites size, level count, +0x2C (base included) and the whole GPU blob
        for (int s = 1; s <= caff.Symbols.Count; s++)
        {
            if (!AssetIds.DisplayName(caff.Symbols[s - 1]).StartsWith("aid_texture_")) continue;
            var cpu = caff.PartsOf(s).FirstOrDefault(p => caff.SectionOf(p).Name == ".data");
            var gpu = caff.PartsOf(s).FirstOrDefault(p => caff.SectionOf(p).Name == ".texturegpu");
            if (cpu == null || gpu == null || !TextureHeader.IsTexture(cpu.Data)) continue;
            var h = TextureHeader.Parse(cpu.Data);
            if (h.Kind == 0 && h.Format == format && h.Tiled && h.Count == 1) return s;
        }
        return 0;
    }

    public static int LevelsFor(int w, int h) => 1 + (int)Math.Log2(Math.Max(w, h));

    /// <summary>Creates texture <paramref name="name"/> (e.g. "aid_texture_banjox_seattle_asphalt") of size w x h
    /// (powers of two) from an RGBA image. Returns the new symbol id.</summary>
    public static int Create(CaffFile caff, string name, byte[] rgba, int imgW, int imgH, int w, int h, XenosFormat format = XenosFormat.DXT1)
    {
        if ((w & (w - 1)) != 0 || (h & (h - 1)) != 0 || w < 4 || h < 4 || w > 4096 || h > 4096) throw new ArgumentException("texture size must be a power of two between 4 and 4096");
        int tsym = FindTemplate(caff, format, allowMipOnly: true);
        if (tsym == 0) throw new InvalidDataException($"no {format} texture with a full mip chain in this bundle to use as a template");
        int sym = CaffEdit.CloneAsset(caff, tsym, name);
        var cpu = caff.PartsOf(sym).First(p => caff.SectionOf(p).Name == ".data");
        var gpu = caff.PartsOf(sym).First(p => caff.SectionOf(p).Name == ".texturegpu");
        int levels = LevelsFor(w, h);
        BE.W16(cpu.Data, 0x24, (ushort)w); BE.W16(cpu.Data, 0x26, (ushort)h);
        BE.W32(cpu.Data, 0x2C, 0xFFFFFFFF);
        cpu.Data[0x30] = (byte)levels;
        var hdr = TextureHeader.Parse(cpu.Data);
        var layout = XenosTexture.Layout(w, h, levels, hdr.Format, 0, levels - 1, hdr.Tiled);
        gpu.Data = TextureAsset.EncodeBlob(hdr, layout, rgba, imgW, imgH);
        gpu.Size = gpu.Data.Length;
        // sanity: the texture reads back with the whole chain
        var check = new TextureAsset(cpu.Data, gpu.Data);
        if (check.Levels.Count != levels || check.ExpectedGpuSize != gpu.Data.Length) throw new InvalidDataException("created texture does not lay out as expected");
        return sym;
    }
}
