using System.IO.Compression;

namespace NB.Core.Textures;

/// <summary>
/// RGBA8 image loading for the mod tool, without GDI+ so it runs on Windows and Linux: PNG (any colour type, 1-16 bits,
/// not interlaced) and TGA. Same API as NB Studio's ImageIO (whose TGA decoder and mip box filter this keeps).
/// </summary>
public static class ImageIO
{
    public static (byte[] Rgba, int W, int H) Load(string path) => Load(File.ReadAllBytes(path), path);

    public static (byte[] Rgba, int W, int H) Load(byte[] data, string nameHint)
    {
        if (Path.GetExtension(nameHint).Equals(".tga", StringComparison.OrdinalIgnoreCase)) return DecodeTga(data);
        if (data.Length >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') return DecodePng(data);
        throw new InvalidDataException($"{Path.GetFileName(nameHint)}: only PNG and TGA images are supported");
    }

    static uint U32(byte[] d, int o) => (uint)(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]);

    public static (byte[] Rgba, int W, int H) DecodePng(byte[] d)
    {
        int w = 0, h = 0, depth = 0, type = 0;
        byte[]? palette = null, trns = null;
        var idat = new MemoryStream();
        for (int p = 8; p + 12 <= d.Length;)
        {
            int len = (int)U32(d, p);
            string tag = System.Text.Encoding.ASCII.GetString(d, p + 4, 4);
            int body = p + 8;
            if (body + len > d.Length) throw new InvalidDataException("PNG: truncated chunk " + tag);
            switch (tag)
            {
                case "IHDR":
                    w = (int)U32(d, body); h = (int)U32(d, body + 4); depth = d[body + 8]; type = d[body + 9];
                    if (d[body + 12] != 0) throw new InvalidDataException("PNG: interlaced images are not supported (save it without interlacing)");
                    break;
                case "PLTE": palette = d[body..(body + len)]; break;
                case "tRNS": trns = d[body..(body + len)]; break;
                case "IDAT": idat.Write(d, body, len); break;
            }
            if (tag == "IEND") break;
            p = body + len + 4;
        }
        if (w <= 0 || h <= 0) throw new InvalidDataException("PNG: no image header");
        int channels = type switch { 0 => 1, 2 => 3, 3 => 1, 4 => 2, 6 => 4, _ => throw new InvalidDataException($"PNG: colour type {type}") };
        int bitsPerPixel = channels * depth;
        int stride = (w * bitsPerPixel + 7) / 8, bpp = Math.Max(1, bitsPerPixel / 8);
        idat.Position = 0;
        var raw = new MemoryStream();
        using (var z = new ZLibStream(idat, CompressionMode.Decompress)) z.CopyTo(raw);
        var src = raw.ToArray();
        if (src.Length < (stride + 1) * h) throw new InvalidDataException("PNG: image data too short");

        // unfilter
        var cur = new byte[stride];
        var prev = new byte[stride];
        var outp = new byte[w * h * 4];
        for (int y = 0; y < h; y++)
        {
            int f = src[y * (stride + 1)];
            Buffer.BlockCopy(src, y * (stride + 1) + 1, cur, 0, stride);
            for (int i = 0; i < stride; i++)
            {
                int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
                int add = f switch
                {
                    0 => 0, 1 => a, 2 => b, 3 => (a + b) / 2,
                    4 => Paeth(a, b, c),
                    _ => throw new InvalidDataException($"PNG: filter {f}"),
                };
                cur[i] = (byte)(cur[i] + add);
            }
            for (int x = 0; x < w; x++)
            {
                int o = (y * w + x) * 4;
                int Sample(int ch)
                {
                    if (depth == 8) return cur[x * channels + ch];
                    if (depth == 16) return cur[(x * channels + ch) * 2];
                    int bit = (x * channels + ch) * depth, v = cur[bit >> 3] >> (8 - depth - (bit & 7)) & ((1 << depth) - 1);
                    return type == 3 ? v : v * 255 / ((1 << depth) - 1);
                }
                int Raw16(int ch) => depth == 16 ? cur[(x * channels + ch) * 2] << 8 | cur[(x * channels + ch) * 2 + 1] : -1;
                switch (type)
                {
                    case 0:
                    {
                        byte g = (byte)Sample(0);
                        outp[o] = outp[o + 1] = outp[o + 2] = g; outp[o + 3] = 255;
                        if (trns is { Length: >= 2 } && (depth == 16 ? Raw16(0) : Sample(0)) == (depth == 16 ? trns[0] << 8 | trns[1] : trns[1] * 255 / ((1 << Math.Min(depth, 8)) - 1))) outp[o + 3] = 0;
                        break;
                    }
                    case 2:
                        outp[o] = (byte)Sample(0); outp[o + 1] = (byte)Sample(1); outp[o + 2] = (byte)Sample(2); outp[o + 3] = 255;
                        if (trns is { Length: >= 6 } && depth == 8 && outp[o] == trns[1] && outp[o + 1] == trns[3] && outp[o + 2] == trns[5]) outp[o + 3] = 0;
                        break;
                    case 3:
                    {
                        int i = Sample(0);
                        if (palette == null || i * 3 + 2 >= palette.Length) throw new InvalidDataException("PNG: palette index out of range");
                        outp[o] = palette[i * 3]; outp[o + 1] = palette[i * 3 + 1]; outp[o + 2] = palette[i * 3 + 2];
                        outp[o + 3] = trns != null && i < trns.Length ? trns[i] : (byte)255;
                        break;
                    }
                    case 4:
                        outp[o] = outp[o + 1] = outp[o + 2] = (byte)Sample(0); outp[o + 3] = (byte)Sample(1);
                        break;
                    case 6:
                        outp[o] = (byte)Sample(0); outp[o + 1] = (byte)Sample(1); outp[o + 2] = (byte)Sample(2); outp[o + 3] = (byte)Sample(3);
                        break;
                }
            }
            (cur, prev) = (prev, cur);
        }
        return (outp, w, h);
    }

    static int Paeth(int a, int b, int c)
    {
        int p = a + b - c, pa = Math.Abs(p - a), pb = Math.Abs(p - b), pc = Math.Abs(p - c);
        return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
    }

    /// <summary>Truevision TGA: uncompressed or RLE, 8-bit grey, 15/16/24/32-bit colour, 8-bit colour-mapped; either origin.</summary>
    public static (byte[] Rgba, int W, int H) DecodeTga(byte[] d)
    {
        if (d.Length < 18) throw new InvalidDataException("TGA file too short");
        int idLen = d[0], cmType = d[1], type = d[2];
        int cmFirst = d[3] | d[4] << 8, cmLen = d[5] | d[6] << 8, cmBits = d[7];
        int w = d[12] | d[13] << 8, h = d[14] | d[15] << 8, bpp = d[16], desc = d[17];
        bool rle = type >= 9, grey = type is 3 or 11, mapped = type is 1 or 9;
        if (type is not (1 or 2 or 3 or 9 or 10 or 11) || w == 0 || h == 0) throw new InvalidDataException($"unsupported TGA image type {type}");
        int p = 18 + idLen;
        byte[]? palette = null;
        int cmBytes = (cmBits + 7) / 8;
        if (cmType == 1) { palette = d[p..(p + cmLen * cmBytes)]; p += cmLen * cmBytes; }
        int px = (bpp + 7) / 8;
        var outp = new byte[w * h * 4];
        void Colour(ReadOnlySpan<byte> s, int bytes, bool isGrey, Span<byte> o)
        {
            if (isGrey) { o[0] = o[1] = o[2] = s[0]; o[3] = bytes > 1 ? s[1] : (byte)255; return; }
            if (bytes == 2) { int v = s[0] | s[1] << 8; o[0] = (byte)((v >> 10 & 31) * 255 / 31); o[1] = (byte)((v >> 5 & 31) * 255 / 31); o[2] = (byte)((v & 31) * 255 / 31); o[3] = 255; return; }
            o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = bytes == 4 ? s[3] : (byte)255;
        }
        void Pixel(ReadOnlySpan<byte> s, Span<byte> o)
        {
            if (mapped && palette != null) { int idx = (px == 2 ? s[0] | s[1] << 8 : s[0]) - cmFirst; Colour(palette.AsSpan(Math.Clamp(idx, 0, cmLen - 1) * cmBytes), cmBytes, false, o); }
            else Colour(s, px, grey, o);
        }
        int n = w * h, i = 0;
        var tmp = new byte[4];
        while (i < n)
        {
            if (!rle) { Pixel(d.AsSpan(p, px), outp.AsSpan(i * 4, 4)); p += px; i++; continue; }
            int hdr = d[p++], count = (hdr & 0x7F) + 1;
            if ((hdr & 0x80) != 0)
            {
                Pixel(d.AsSpan(p, px), tmp); p += px;
                for (int k = 0; k < count && i < n; k++, i++) tmp.CopyTo(outp, i * 4);
            }
            else for (int k = 0; k < count && i < n; k++, i++) { Pixel(d.AsSpan(p, px), outp.AsSpan(i * 4, 4)); p += px; }
        }
        bool topDown = (desc & 0x20) != 0, rightLeft = (desc & 0x10) != 0;
        if (!topDown || rightLeft)
        {
            var f = new byte[outp.Length];
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                {
                    int sy = topDown ? y : h - 1 - y, sx = rightLeft ? w - 1 - x : x;
                    Buffer.BlockCopy(outp, (sy * w + sx) * 4, f, (y * w + x) * 4, 4);
                }
            outp = f;
        }
        return (outp, w, h);
    }

    /// <summary>Box-filter downsample by 2 (used to rebuild mip chains).</summary>
    public static (byte[] Rgba, int W, int H) Half(byte[] src, int w, int h)
    {
        int nw = Math.Max(1, w / 2), nh = Math.Max(1, h / 2);
        var o = new byte[nw * nh * 4];
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++)
                {
                    int x0 = Math.Min(x * 2, w - 1), x1 = Math.Min(x * 2 + 1, w - 1), y0 = Math.Min(y * 2, h - 1), y1 = Math.Min(y * 2 + 1, h - 1);
                    int s = src[(y0 * w + x0) * 4 + c] + src[(y0 * w + x1) * 4 + c] + src[(y1 * w + x0) * 4 + c] + src[(y1 * w + x1) * 4 + c];
                    o[(y * nw + x) * 4 + c] = (byte)((s + 2) / 4);
                }
        return (o, nw, nh);
    }

    /// <summary>Bilinear resize, edges clamped.</summary>
    public static (byte[] Rgba, int W, int H) Resize(byte[] src, int w, int h, int nw, int nh)
    {
        var o = new byte[nw * nh * 4];
        for (int y = 0; y < nh; y++)
        {
            float fy = Math.Clamp((y + 0.5f) * h / nh - 0.5f, 0, h - 1);
            int y0 = (int)fy, y1 = Math.Min(y0 + 1, h - 1);
            float ty = fy - y0;
            for (int x = 0; x < nw; x++)
            {
                float fx = Math.Clamp((x + 0.5f) * w / nw - 0.5f, 0, w - 1);
                int x0 = (int)fx, x1 = Math.Min(x0 + 1, w - 1);
                float tx = fx - x0;
                for (int c = 0; c < 4; c++)
                {
                    float top = src[(y0 * w + x0) * 4 + c] * (1 - tx) + src[(y0 * w + x1) * 4 + c] * tx;
                    float bottom = src[(y1 * w + x0) * 4 + c] * (1 - tx) + src[(y1 * w + x1) * 4 + c] * tx;
                    o[(y * nw + x) * 4 + c] = (byte)Math.Clamp((int)MathF.Round(top * (1 - ty) + bottom * ty), 0, 255);
                }
            }
        }
        return (o, nw, nh);
    }
}
