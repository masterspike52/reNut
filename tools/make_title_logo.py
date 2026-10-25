#!/usr/bin/env python3
"""Builds reNut's title logo (gpu/render/title_logo.cpp) from a PNG.

The game's logo is aid_texture_banjox_ui_logonew: 1280x720, BC2 (DXT3), one
level, in the frontend's stream bundle (Bundle/50/685374). reNut swaps its
blocks for these as the texture uploads. The PNG may be any 16:9 size: it is
scaled to 1280x720 (premultiplied, so its edges keep no dark fringe), encoded
to BC2 (host block order: 8 bytes of 4-bit alpha, then a 5:6:5 colour block)
and written, zstd-compressed, as a C++ byte array.

usage: python make_title_logo.py <logo.png> <out.inc>
       python make_title_logo.py --hash-original
         (the XXH3 of the game's own logo blocks, row after row, each row's
          seeded with the last: what title_logo.cpp recognises)
"""
import struct
import sys

import numpy as np

WIDTH, HEIGHT = 1280, 720
BW, BH = WIDTH // 4, HEIGHT // 4


def scale(path):
    from PIL import Image, ImageFilter
    im = Image.open(path).convert("RGBA")
    a = np.asarray(im, dtype=np.float32) / 255.0
    pre = a.copy()
    pre[..., :3] *= pre[..., 3:4]
    channels = [Image.fromarray(pre[..., c]).resize((WIDTH, HEIGHT), Image.LANCZOS) for c in range(4)]
    out = np.stack([np.asarray(c, dtype=np.float32) for c in channels], -1).clip(0.0, 1.0)
    alpha = out[..., 3:4]
    rgb = np.where(alpha > 1e-4, out[..., :3] / np.maximum(alpha, 1e-4), 0.0)
    # Transparent texels take their neighbours' colour, so filtering at the
    # edges blends towards the logo's own colours rather than black.
    def blur(channel):
        image = Image.fromarray(np.rint(channel * 255).astype(np.uint8), "L")
        return np.asarray(image.filter(ImageFilter.GaussianBlur(6)), dtype=np.float32) / 255.0
    blur_pre = np.stack([blur(out[..., c]) for c in range(4)], -1)
    blur_rgb = blur_pre[..., :3] / np.maximum(blur_pre[..., 3:4], 1e-4)
    rgb = np.where(alpha > 1e-4, rgb, blur_rgb).clip(0.0, 1.0)
    return rgb, alpha[..., 0]


def pack565(c):
    r = np.clip(np.rint(c[..., 0] * 31), 0, 31).astype(np.uint32)
    g = np.clip(np.rint(c[..., 1] * 63), 0, 63).astype(np.uint32)
    b = np.clip(np.rint(c[..., 2] * 31), 0, 31).astype(np.uint32)
    return (r << 11) | (g << 5) | b


def unpack565(v):
    return np.stack([(v >> 11) / 31.0, ((v >> 5) & 63) / 63.0, (v & 31) / 31.0], -1)


def encode(rgb, alpha):
    # Blocks: (BH, BW, 16 texels, channels).
    def blocks(x):
        x = x.reshape(BH, 4, BW, 4, *x.shape[2:])
        return np.moveaxis(x, 2, 1).reshape(BH, BW, 16, *x.shape[4:])
    px = blocks(rgb)
    al = blocks(alpha)
    weight = np.maximum(al, 0.02)[..., None]
    mean = (px * weight).sum(2, keepdims=True) / weight.sum(2, keepdims=True)
    d = px - mean
    cov = np.einsum("...ni,...nj->...ij", d * weight, d)
    # The principal axis by power iteration.
    axis = np.ones(mean.shape[:-2] + (3,), dtype=np.float32)
    for _ in range(8):
        axis = np.einsum("...ij,...j->...i", cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=-1, keepdims=True), 1e-8)
    proj = np.einsum("...ni,...i->...n", d, axis)
    lo = mean[..., 0, :] + axis * proj.min(-1, keepdims=True)
    hi = mean[..., 0, :] + axis * proj.max(-1, keepdims=True)
    best = None
    for _ in range(3):
        c0, c1 = pack565(hi), pack565(lo)
        e0, e1 = unpack565(c0), unpack565(c1)
        palette = np.stack([e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3], -2)
        dist = (((px[..., :, None, :] - palette[..., None, :, :]) ** 2).sum(-1)) * weight
        index = dist.argmin(-1)
        best = (c0, c1, index)
        # Refit the end points to the chosen indices (least squares).
        t = np.array([1.0, 0.0, 2 / 3, 1 / 3])[index]
        w = weight[..., 0]
        a11 = (w * t * t).sum(-1)
        a22 = (w * (1 - t) ** 2).sum(-1)
        a12 = (w * t * (1 - t)).sum(-1)
        b1 = (w[..., None] * t[..., None] * px).sum(-2)
        b2 = (w[..., None] * (1 - t)[..., None] * px).sum(-2)
        det = a11 * a22 - a12 * a12
        ok = np.abs(det) > 1e-8
        safe = np.where(ok, det, 1.0)[..., None]
        hi_new = (b1 * a22[..., None] - b2 * a12[..., None]) / safe
        lo_new = (b2 * a11[..., None] - b1 * a12[..., None]) / safe
        hi = np.where(ok[..., None], hi_new, hi).clip(0, 1)
        lo = np.where(ok[..., None], lo_new, lo).clip(0, 1)
    c0, c1, index = best
    # Four colours always (BC2): swap so c0 >= c1 isn't needed, but keep the
    # indices consistent with the palette above.
    a4 = np.clip(np.rint(al * 15), 0, 15).astype(np.uint64)
    alpha_bits = (a4 << (4 * np.arange(16, dtype=np.uint64))).sum(-1)
    index_bits = (index.astype(np.uint64) << (2 * np.arange(16, dtype=np.uint64))).sum(-1)
    out = bytearray()
    for y in range(BH):
        for x in range(BW):
            out += struct.pack("<QHHI", int(alpha_bits[y, x]), int(c0[y, x]), int(c1[y, x]),
                               int(index_bits[y, x]))
    return bytes(out)


def original_blocks():
    sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
    import dump_shaders as ds

    def tiled(x, y, pitch, l2):  # XGAddress2DTiledOffset (texture_layout.cpp TiledOffset2D)
        tpr = ((pitch + 31) & ~31) >> 5
        sh = l2 + 6
        tile = (((x >> 5) + (y >> 5) * tpr) << sh) & 0x1FFFFFFF
        micro = (((x & 7) + ((y << 2) & 0x18)) << sh) >> 6
        a = 2 * (tile + (micro & ~0xF)) + (micro & 0xF) + (((y >> 3) & 1) << sh) + ((y & 1) << 4)
        b = ((2 * ((x >> 3) + ((y >> 2) & 2))) & 6) + ((y >> 4) & 1)
        return ((b & 1) << 11) + (((a >> 6) & 7) << 8) + ((b & ~1) << 5) + ((a & ~0x1FF) << 3) + (a & 0x3F)

    import os
    bundle = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "Bundle", "50", "685374")
    for _, caff in ds.iter_caffs(bundle):
        for i in range(1, len(caff.files) + 1):
            a = caff.files[i - 1]["asset"]
            name = caff.symbols[a - 1].split(",")[0] if 0 < a <= len(caff.symbols) else ""
            if "aid_texture_banjox_ui_logonew\\" in name and caff.section_name(i) == ".texturegpu":
                raw = caff.read_file(i)
                rows = []
                for y in range(BH):
                    row = bytearray()
                    for x in range(BW):
                        o = tiled(x, y, BW, 4)
                        blk = bytearray(raw[o:o + 16])
                        blk[0::2], blk[1::2] = blk[1::2], blk[0::2]  # 8in16
                        row += blk
                    rows.append(bytes(row))
                return rows
    raise SystemExit("aid_texture_banjox_ui_logonew not found")


def main():
    if sys.argv[1:] == ["--hash-original"]:
        import xxhash
        h = 0
        for row in original_blocks():
            h = xxhash.xxh3_64_intdigest(row, seed=h)
        print("0x%016XULL" % h)
        return
    import zstandard
    rgb, alpha = scale(sys.argv[1])
    data = encode(rgb, alpha)
    packed = zstandard.ZstdCompressor(level=19).compress(data)
    with open(sys.argv[2], "w", newline="\n") as f:
        f.write("// Generated by tools/make_title_logo.py from %s: the title logo, 1280x720 BC2\n"
                "// blocks, zstd-compressed.\n" % sys.argv[1].replace("\\", "/").rsplit("/", 1)[-1])
        f.write("constexpr size_t kTitleLogoBytes = %d;\n" % len(data))
        f.write("constexpr unsigned char kTitleLogoZstd[] = {\n")
        for i in range(0, len(packed), 20):
            f.write("    " + ", ".join("0x%02X" % b for b in packed[i:i + 20]) + ",\n")
        f.write("};\n")
    print("%d bytes of blocks, %d compressed" % (len(data), len(packed)))


if __name__ == "__main__":
    main()
