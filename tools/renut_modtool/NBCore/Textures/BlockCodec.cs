namespace NB.Core.Textures;

/// <summary>
/// Decoders/encoders for the block formats, working on little-endian linear block data
/// (after Xenos endian swap and untiling). Images are RGBA8, row-major.
/// Two-channel formats (DXN, CTX1) are normal maps: X in red, Y in green, Z rebuilt into blue.
/// </summary>
public static class BlockCodec
{
    public static byte[] Decode(ReadOnlySpan<byte> blocks, int w, int h, XenosFormat fmt)
    {
        var img = new byte[w * h * 4];
        var fi = FormatInfo.Of(fmt);
        int bw = (w + fi.BlockW - 1) / fi.BlockW, bh = (h + fi.BlockH - 1) / fi.BlockH;
        Span<byte> px = stackalloc byte[64];
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++)
            {
                var blk = blocks.Slice((by * bw + bx) * fi.BytesPerBlock, fi.BytesPerBlock);
                if (fi.BlockW == 1)
                {
                    int o = (by * w + bx) * 4;
                    DecodePixel(blk, fmt, img.AsSpan(o, 4));
                    continue;
                }
                DecodeBlock(blk, fmt, px);
                for (int y = 0; y < 4; y++)
                {
                    int iy = by * 4 + y; if (iy >= h) break;
                    for (int x = 0; x < 4; x++)
                    {
                        int ix = bx * 4 + x; if (ix >= w) break;
                        px.Slice((y * 4 + x) * 4, 4).CopyTo(img.AsSpan((iy * w + ix) * 4, 4));
                    }
                }
            }
        return img;
    }

    static void DecodePixel(ReadOnlySpan<byte> p, XenosFormat fmt, Span<byte> o)
    {
        switch (fmt)
        {
            case XenosFormat.k8: o[0] = o[1] = o[2] = p[0]; o[3] = 255; break;
            case XenosFormat.k8_8: o[0] = p[0]; o[1] = p[1]; o[2] = 0; o[3] = 255; break;
            case XenosFormat.k8_8_8_8: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3]; break; // little-endian A8R8G8B8
            case XenosFormat.k5_6_5:
            {
                int v = p[0] | p[1] << 8;
                o[0] = (byte)((v >> 11) * 255 / 31); o[1] = (byte)(((v >> 5) & 63) * 255 / 63); o[2] = (byte)((v & 31) * 255 / 31); o[3] = 255; break;
            }
            case XenosFormat.k4_4_4_4:
            {
                int v = p[0] | p[1] << 8;
                o[3] = (byte)((v >> 12) * 17); o[0] = (byte)(((v >> 8) & 15) * 17); o[1] = (byte)(((v >> 4) & 15) * 17); o[2] = (byte)((v & 15) * 17); break;
            }
            case XenosFormat.k1_5_5_5:
            {
                int v = p[0] | p[1] << 8;
                o[3] = (byte)((v >> 15) != 0 ? 255 : 0); o[0] = (byte)(((v >> 10) & 31) * 255 / 31); o[1] = (byte)(((v >> 5) & 31) * 255 / 31); o[2] = (byte)((v & 31) * 255 / 31); break;
            }
            default: throw new NotSupportedException(fmt.ToString());
        }
    }

    static void Rgb565(int c, out int r, out int g, out int b)
    {
        r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    }

    static void DecodeColorBlock(ReadOnlySpan<byte> b, Span<byte> px, bool forceFour)
    {
        int c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
        Span<int> pal = stackalloc int[16];
        Rgb565(c0, out pal[0], out pal[1], out pal[2]); pal[3] = 255;
        Rgb565(c1, out pal[4], out pal[5], out pal[6]); pal[7] = 255;
        if (c0 > c1 || forceFour)
        {
            for (int k = 0; k < 3; k++) { pal[8 + k] = (2 * pal[k] + pal[4 + k]) / 3; pal[12 + k] = (pal[k] + 2 * pal[4 + k]) / 3; }
            pal[11] = pal[15] = 255;
        }
        else
        {
            for (int k = 0; k < 3; k++) { pal[8 + k] = (pal[k] + pal[4 + k]) / 2; pal[12 + k] = 0; }
            pal[11] = 255; pal[15] = 0;
        }
        uint idx = (uint)(b[4] | b[5] << 8 | b[6] << 16 | b[7] << 24);
        for (int i = 0; i < 16; i++)
        {
            int s = (int)((idx >> (2 * i)) & 3) * 4;
            px[i * 4] = (byte)pal[s]; px[i * 4 + 1] = (byte)pal[s + 1]; px[i * 4 + 2] = (byte)pal[s + 2]; px[i * 4 + 3] = (byte)pal[s + 3];
        }
    }

    static void DecodeAlphaBlock(ReadOnlySpan<byte> b, Span<byte> outp, int channel)
    {
        int a0 = b[0], a1 = b[1];
        Span<int> pal = stackalloc int[8];
        pal[0] = a0; pal[1] = a1;
        if (a0 > a1) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
        else { for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5; pal[6] = 0; pal[7] = 255; }
        ulong bits = 0;
        for (int i = 0; i < 6; i++) bits |= (ulong)b[2 + i] << (8 * i);
        for (int i = 0; i < 16; i++) outp[i * 4 + channel] = (byte)pal[(int)((bits >> (3 * i)) & 7)];
    }

    static void DecodeBlock(ReadOnlySpan<byte> b, XenosFormat fmt, Span<byte> px)
    {
        switch (fmt)
        {
            case XenosFormat.DXT1: DecodeColorBlock(b, px, false); break;
            case XenosFormat.DXT2_3:
                DecodeColorBlock(b[8..], px, true);
                for (int i = 0; i < 16; i++) { int a = (b[i / 2] >> (4 * (i & 1))) & 15; px[i * 4 + 3] = (byte)(a * 17); }
                break;
            case XenosFormat.DXT4_5:
                DecodeColorBlock(b[8..], px, true);
                DecodeAlphaBlock(b, px, 3);
                break;
            case XenosFormat.DXN:
                DecodeAlphaBlock(b, px, 0);
                DecodeAlphaBlock(b[8..], px, 1);
                RebuildZ(px);
                break;
            case XenosFormat.DXT5A:
                DecodeAlphaBlock(b, px, 0);
                for (int i = 0; i < 16; i++) { px[i * 4 + 1] = px[i * 4 + 2] = px[i * 4]; px[i * 4 + 3] = 255; }
                break;
            case XenosFormat.DXT3A:
                for (int i = 0; i < 16; i++) { int a = (b[i / 2] >> (4 * (i & 1))) & 15; px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = (byte)(a * 17); px[i * 4 + 3] = 255; }
                break;
            case XenosFormat.CTX1:
            {
                // Two 8:8 endpoints (x, y) followed by 2-bit indices, four-colour mode only.
                int x0 = b[0], y0 = b[1], x1 = b[2], y1 = b[3];
                Span<int> pal = stackalloc int[8] { x0, y0, x1, y1, (2 * x0 + x1) / 3, (2 * y0 + y1) / 3, (x0 + 2 * x1) / 3, (y0 + 2 * y1) / 3 };
                uint idx = (uint)(b[4] | b[5] << 8 | b[6] << 16 | b[7] << 24);
                for (int i = 0; i < 16; i++)
                {
                    int s = (int)((idx >> (2 * i)) & 3) * 2;
                    px[i * 4] = (byte)pal[s]; px[i * 4 + 1] = (byte)pal[s + 1];
                }
                RebuildZ(px);
                break;
            }
            default: throw new NotSupportedException(fmt.ToString());
        }
    }

    static void RebuildZ(Span<byte> px)
    {
        for (int i = 0; i < 16; i++)
        {
            float x = px[i * 4] / 127.5f - 1, y = px[i * 4 + 1] / 127.5f - 1;
            float z = MathF.Sqrt(MathF.Max(0, 1 - x * x - y * y));
            px[i * 4 + 2] = (byte)Math.Clamp((int)MathF.Round((z + 1) * 127.5f), 0, 255);
            px[i * 4 + 3] = 255;
        }
    }

    // ------------------------------------------------------------------ encoding

    /// <summary>Encodes an RGBA8 image into linear little-endian blocks of the given format.</summary>
    public static byte[] Encode(ReadOnlySpan<byte> img, int w, int h, XenosFormat fmt)
    {
        var fi = FormatInfo.Of(fmt);
        int bw = (w + fi.BlockW - 1) / fi.BlockW, bh = (h + fi.BlockH - 1) / fi.BlockH;
        var outp = new byte[bw * bh * fi.BytesPerBlock];
        if (fi.BlockW == 1)
        {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    EncodePixel(img.Slice((y * w + x) * 4, 4), fmt, outp.AsSpan((y * w + x) * fi.BytesPerBlock, fi.BytesPerBlock));
            return outp;
        }
        Span<byte> px = stackalloc byte[64];
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++)
            {
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++)
                    {
                        int ix = Math.Min(bx * 4 + x, w - 1), iy = Math.Min(by * 4 + y, h - 1);
                        img.Slice((iy * w + ix) * 4, 4).CopyTo(px.Slice((y * 4 + x) * 4, 4));
                    }
                var dst = outp.AsSpan((by * bw + bx) * fi.BytesPerBlock, fi.BytesPerBlock);
                switch (fmt)
                {
                    case XenosFormat.DXT1: EncodeColorBlock(px, dst, true); break;
                    case XenosFormat.DXT2_3:
                        for (int i = 0; i < 8; i++) dst[i] = (byte)((px[(2 * i) * 4 + 3] >> 4) | (px[(2 * i + 1) * 4 + 3] >> 4) << 4);
                        EncodeColorBlock(px, dst[8..], false); break;
                    case XenosFormat.DXT4_5: EncodeAlphaBlock(px, 3, dst); EncodeColorBlock(px, dst[8..], false); break;
                    case XenosFormat.DXN: EncodeAlphaBlock(px, 0, dst); EncodeAlphaBlock(px, 1, dst[8..]); break;
                    case XenosFormat.DXT5A: EncodeAlphaBlock(px, 0, dst); break;
                    case XenosFormat.DXT3A:
                        for (int i = 0; i < 8; i++) dst[i] = (byte)((px[(2 * i) * 4] >> 4) | (px[(2 * i + 1) * 4] >> 4) << 4);
                        break;
                    case XenosFormat.CTX1: EncodeCtx1(px, dst); break;
                    default: throw new NotSupportedException(fmt.ToString());
                }
            }
        return outp;
    }

    static void EncodePixel(ReadOnlySpan<byte> p, XenosFormat fmt, Span<byte> o)
    {
        switch (fmt)
        {
            case XenosFormat.k8: o[0] = (byte)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8); break;
            case XenosFormat.k8_8: o[0] = p[0]; o[1] = p[1]; break;
            case XenosFormat.k8_8_8_8: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3]; break;
            case XenosFormat.k5_6_5: { int v = (p[0] >> 3) << 11 | (p[1] >> 2) << 5 | (p[2] >> 3); o[0] = (byte)v; o[1] = (byte)(v >> 8); break; }
            case XenosFormat.k4_4_4_4: { int v = (p[3] >> 4) << 12 | (p[0] >> 4) << 8 | (p[1] >> 4) << 4 | (p[2] >> 4); o[0] = (byte)v; o[1] = (byte)(v >> 8); break; }
            case XenosFormat.k1_5_5_5: { int v = (p[3] >= 128 ? 1 : 0) << 15 | (p[0] >> 3) << 10 | (p[1] >> 3) << 5 | (p[2] >> 3); o[0] = (byte)v; o[1] = (byte)(v >> 8); break; }
            default: throw new NotSupportedException(fmt.ToString());
        }
    }

    static int To565(float r, float g, float b)
    {
        int ri = Math.Clamp((int)MathF.Round(r * 31 / 255f), 0, 31);
        int gi = Math.Clamp((int)MathF.Round(g * 63 / 255f), 0, 63);
        int bi = Math.Clamp((int)MathF.Round(b * 31 / 255f), 0, 31);
        return ri << 11 | gi << 5 | bi;
    }

    /// <summary>DXT colour block: principal-axis endpoint fit, then two least-squares refinement passes.</summary>
    static void EncodeColorBlock(ReadOnlySpan<byte> px, Span<byte> dst, bool allowAlpha)
    {
        bool anyTransparent = false;
        if (allowAlpha) for (int i = 0; i < 16; i++) if (px[i * 4 + 3] < 128) { anyTransparent = true; break; }
        Span<float> c = stackalloc float[48];
        float mr = 0, mg = 0, mb = 0; int n = 0;
        for (int i = 0; i < 16; i++)
        {
            c[i * 3] = px[i * 4]; c[i * 3 + 1] = px[i * 4 + 1]; c[i * 3 + 2] = px[i * 4 + 2];
            if (anyTransparent && px[i * 4 + 3] < 128) continue;
            mr += c[i * 3]; mg += c[i * 3 + 1]; mb += c[i * 3 + 2]; n++;
        }
        if (n == 0) { dst.Clear(); dst[0] = 0; dst[2] = 1; for (int i = 4; i < 8; i++) dst[i] = 0xFF; return; } // fully transparent
        mr /= n; mg /= n; mb /= n;
        // covariance + power iteration for principal axis
        float xx = 0, xy = 0, xz = 0, yy = 0, yz = 0, zz = 0;
        for (int i = 0; i < 16; i++)
        {
            if (anyTransparent && px[i * 4 + 3] < 128) continue;
            float r = c[i * 3] - mr, g = c[i * 3 + 1] - mg, b = c[i * 3 + 2] - mb;
            xx += r * r; xy += r * g; xz += r * b; yy += g * g; yz += g * b; zz += b * b;
        }
        float ax = 1, ay = 1, az = 1;
        for (int it = 0; it < 8; it++)
        {
            float nx = xx * ax + xy * ay + xz * az, ny = xy * ax + yy * ay + yz * az, nz = xz * ax + yz * ay + zz * az;
            float len = MathF.Sqrt(nx * nx + ny * ny + nz * nz);
            if (len < 1e-6f) { ax = 0.577f; ay = 0.577f; az = 0.577f; break; }
            ax = nx / len; ay = ny / len; az = nz / len;
        }
        float tmin = float.MaxValue, tmax = float.MinValue;
        for (int i = 0; i < 16; i++)
        {
            if (anyTransparent && px[i * 4 + 3] < 128) continue;
            float t = (c[i * 3] - mr) * ax + (c[i * 3 + 1] - mg) * ay + (c[i * 3 + 2] - mb) * az;
            tmin = MathF.Min(tmin, t); tmax = MathF.Max(tmax, t);
        }
        Span<float> e = stackalloc float[6] { mr + ax * tmax, mg + ay * tmax, mb + az * tmax, mr + ax * tmin, mg + ay * tmin, mb + az * tmin };
        int steps = anyTransparent ? 3 : 4;
        Span<int> sel = stackalloc int[16];
        for (int pass = 0; pass < 3; pass++)
        {
            // assign indices along the endpoint line
            float dr = e[3] - e[0], dg = e[4] - e[1], db = e[5] - e[2];
            float dd = dr * dr + dg * dg + db * db;
            for (int i = 0; i < 16; i++)
            {
                float t = dd < 1e-6f ? 0 : ((c[i * 3] - e[0]) * dr + (c[i * 3 + 1] - e[1]) * dg + (c[i * 3 + 2] - e[2]) * db) / dd;
                sel[i] = Math.Clamp((int)MathF.Round(t * (steps - 1)), 0, steps - 1);
            }
            if (pass == 2) break;
            // least squares refit of endpoints
            float a2 = 0, b2 = 0, ab = 0; Span<float> ax3 = stackalloc float[3]; Span<float> bx3 = stackalloc float[3];
            for (int i = 0; i < 16; i++)
            {
                if (anyTransparent && px[i * 4 + 3] < 128) continue;
                float w1 = sel[i] / (float)(steps - 1), w0 = 1 - w1;
                a2 += w0 * w0; b2 += w1 * w1; ab += w0 * w1;
                for (int k = 0; k < 3; k++) { ax3[k] += w0 * c[i * 3 + k]; bx3[k] += w1 * c[i * 3 + k]; }
            }
            float det = a2 * b2 - ab * ab;
            if (MathF.Abs(det) < 1e-6f) break;
            for (int k = 0; k < 3; k++)
            {
                e[k] = Math.Clamp((ax3[k] * b2 - bx3[k] * ab) / det, 0, 255);
                e[3 + k] = Math.Clamp((bx3[k] * a2 - ax3[k] * ab) / det, 0, 255);
            }
        }
        int c0 = To565(e[0], e[1], e[2]), c1 = To565(e[3], e[4], e[5]);
        // Palette from quantized endpoints; pick best index per pixel.
        bool fourColour = !anyTransparent;
        if (fourColour && c0 < c1) (c0, c1) = (c1, c0);
        if (fourColour && c0 == c1) { if (c1 > 0) c1--; else c0++; }
        if (!fourColour && c0 > c1) (c0, c1) = (c1, c0);
        Span<byte> tmpBlk = stackalloc byte[8];
        tmpBlk[0] = (byte)c0; tmpBlk[1] = (byte)(c0 >> 8); tmpBlk[2] = (byte)c1; tmpBlk[3] = (byte)(c1 >> 8);
        Span<byte> pal = stackalloc byte[64];
        tmpBlk[4] = 0xE4; tmpBlk[5] = 0; tmpBlk[6] = 0; tmpBlk[7] = 0; // indices 0,1,2,3 for first 4 px
        DecodeColorBlock(tmpBlk, pal, false);
        uint idx = 0;
        for (int i = 0; i < 16; i++)
        {
            int best = 0;
            if (!fourColour && px[i * 4 + 3] < 128) best = 3;
            else
            {
                int bestD = int.MaxValue;
                for (int s = 0; s < (fourColour ? 4 : 3); s++)
                {
                    int d0 = pal[s * 4] - px[i * 4], d1 = pal[s * 4 + 1] - px[i * 4 + 1], d2 = pal[s * 4 + 2] - px[i * 4 + 2];
                    int d = d0 * d0 + d1 * d1 + d2 * d2;
                    if (d < bestD) { bestD = d; best = s; }
                }
            }
            idx |= (uint)best << (2 * i);
        }
        dst[0] = (byte)c0; dst[1] = (byte)(c0 >> 8); dst[2] = (byte)c1; dst[3] = (byte)(c1 >> 8);
        dst[4] = (byte)idx; dst[5] = (byte)(idx >> 8); dst[6] = (byte)(idx >> 16); dst[7] = (byte)(idx >> 24);
    }

    /// <summary>BC4-style block (DXT5 alpha / DXN channel / DXT5A) for one channel.</summary>
    static void EncodeAlphaBlock(ReadOnlySpan<byte> px, int ch, Span<byte> dst)
    {
        int mn = 255, mx = 0;
        for (int i = 0; i < 16; i++) { int v = px[i * 4 + ch]; mn = Math.Min(mn, v); mx = Math.Max(mx, v); }
        dst[0] = (byte)mx; dst[1] = (byte)mn;
        Span<int> pal = stackalloc int[8];
        pal[0] = mx; pal[1] = mn;
        if (mx > mn) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * mx + i * mn) / 7;
        else for (int i = 2; i < 8; i++) pal[i] = mx;
        ulong bits = 0;
        for (int i = 0; i < 16; i++)
        {
            int v = px[i * 4 + ch], best = 0, bd = int.MaxValue;
            for (int s = 0; s < 8; s++) { int d = Math.Abs(pal[s] - v); if (d < bd) { bd = d; best = s; } }
            bits |= (ulong)best << (3 * i);
        }
        for (int i = 0; i < 6; i++) dst[2 + i] = (byte)(bits >> (8 * i));
    }

    static void EncodeCtx1(ReadOnlySpan<byte> px, Span<byte> dst)
    {
        // Fit along the principal axis of the (x, y) point set.
        float mx = 0, my = 0;
        for (int i = 0; i < 16; i++) { mx += px[i * 4]; my += px[i * 4 + 1]; }
        mx /= 16; my /= 16;
        float sxx = 0, sxy = 0, syy = 0;
        for (int i = 0; i < 16; i++) { float x = px[i * 4] - mx, y = px[i * 4 + 1] - my; sxx += x * x; sxy += x * y; syy += y * y; }
        float ang = 0.5f * MathF.Atan2(2 * sxy, sxx - syy), ax = MathF.Cos(ang), ay = MathF.Sin(ang);
        float tmin = float.MaxValue, tmax = float.MinValue;
        for (int i = 0; i < 16; i++) { float t = (px[i * 4] - mx) * ax + (px[i * 4 + 1] - my) * ay; tmin = MathF.Min(tmin, t); tmax = MathF.Max(tmax, t); }
        int x0 = Math.Clamp((int)MathF.Round(mx + ax * tmax), 0, 255), y0 = Math.Clamp((int)MathF.Round(my + ay * tmax), 0, 255);
        int x1 = Math.Clamp((int)MathF.Round(mx + ax * tmin), 0, 255), y1 = Math.Clamp((int)MathF.Round(my + ay * tmin), 0, 255);
        Span<int> pal = stackalloc int[8] { x0, y0, x1, y1, (2 * x0 + x1) / 3, (2 * y0 + y1) / 3, (x0 + 2 * x1) / 3, (y0 + 2 * y1) / 3 };
        uint idx = 0;
        for (int i = 0; i < 16; i++)
        {
            int best = 0, bd = int.MaxValue;
            for (int s = 0; s < 4; s++) { int dx = pal[s * 2] - px[i * 4], dy = pal[s * 2 + 1] - px[i * 4 + 1]; int d = dx * dx + dy * dy; if (d < bd) { bd = d; best = s; } }
            idx |= (uint)best << (2 * i);
        }
        dst[0] = (byte)x0; dst[1] = (byte)y0; dst[2] = (byte)x1; dst[3] = (byte)y1;
        dst[4] = (byte)idx; dst[5] = (byte)(idx >> 8); dst[6] = (byte)(idx >> 16); dst[7] = (byte)(idx >> 24);
    }
}
